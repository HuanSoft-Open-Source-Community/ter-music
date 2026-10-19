/**
 * @file player_remote.c
 * @brief 门面的远端后端：把播放服务当 D-Bus 服务用（TUI 作为客户端）
 *
 * 架构边界（2026-09-15）：前端（TUI/CLI）拥有内容，后端（核心）只做播放。
 * 本文件是**前端侧**的播放面实现：连接核心的 `org.yxzl.ter_music.*` 接口，
 * 用 `Info.GetInfo` 拉快照、用 `Control.*` 下发传输命令、用 `Queue.*` 下发
 * 队列，并维护前端注册（`Control.Attach/Ping/Detach`）。
 *
 * 与 `player_remote_local.c` 的关系：两者是同一门面的两种后端，接口语义必须一致
 * （同样的快照字段、同样的乐观回显、同样的修订号语义）。`--frontend=remote`
 * 用本文件，`--frontend=local` 保留本地直调作为回归基线（M6 删除）。
 *
 * 断线与重连：
 *   - 连接失败或总线名字消失 → `player_remote_pump()` 返回 -1，界面显示断线浮层；
 *   - 重连采用指数退避（1/2/4/8/15/30 秒封顶），重连成功后重新握手与 Attach；
 *   - 断线期间**不清空快照**（保留最后一帧，界面不至于闪成空白）。
 *
 * @author 燕戏竹林 (yxz666xx@outlook.com)
 */

#include "player/player.h"
#include "player/player_backend.h"

#include "audio/equalizer.h"
#include "cli/cli.h"
#include "config/config_diff.h"
#include "config/config_json.h"
#include "core/core.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "media/rpc.h"
#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"
#include "util/json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define REMOTE_OBJECT_PATH   "/org/mpris/MediaPlayer2"
#define REMOTE_IFACE_INFO    "org.yxzl.ter_music.Info"
#define REMOTE_IFACE_CONTROL "org.yxzl.ter_music.Control"
#define REMOTE_IFACE_QUEUE   "org.yxzl.ter_music.Queue"
#define REMOTE_IFACE_LYRICS  "org.yxzl.ter_music.Lyrics"
#define REMOTE_IFACE_CONFIG  "org.yxzl.ter_music.Config"

/* 重连退避表（毫秒），最后一项封顶 */
static const int k_backoff_ms[] = { 1000, 2000, 4000, 8000, 15000, 30000 };
#define REMOTE_BACKOFF_COUNT (int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]))

/* 握手要求的方法：缺一即视为“核心过旧”。
 * 这里只列“没有它就没法用”的方法。Control.Quit **不入表**：它是退出时的
 * 尽力而为（要不要发只看前端自己的配置镜像，发送失败只记日志），列进来会让不带 Quit 的
 * 老核心直接连不上，把一个可选能力升级成硬性要求。 */
static const char *const k_required_methods[] = {
    "Info.GetInfo",
    "Control.Play", "Control.Pause", "Control.Stop", "Control.Next", "Control.Previous",
    "Control.SetVolume", "Control.SetSpeed", "Control.SetPlayMode", "Control.Attach",
    "Control.Ping", "Control.Detach",
    "Queue.Get", "Queue.Set", "Queue.PlayAt",
    /* 歌词面：面板的行数据全部来自 Lyrics.GetDocument，来源切换走 SetSource。
     * 缺了它们不能“安静地显示空歌词栏”，必须在接入时就明确失败。 */
    "Lyrics.GetDocument", "Lyrics.SetSource",
    NULL
};

typedef struct {
    int initialized;
    int connected;
    PlayerBackend requested;

    char bus_name[128];
    DBusConnection *connection;
    char token[128];
    int ping_interval_ms;

    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;
    char status_message[CORE_STATUS_MAX];

    uint64_t state_revision;
    uint64_t queue_revision;
    uint64_t lyrics_revision;
    uint64_t config_revision;
    uint64_t cover_revision;

    int queue_count;
    int queue_position;
    int next_ping_ms;

    /* 断线状态机 */
    int offline;
    int backoff_index;
    int reconnect_due_ms;

    /* 进度外推 */
    int anchor_position;
    uint64_t anchor_ms;

    /* 封面页缓存 */
    int cover_valid;
    int cover_cols;
    int cover_rows;
    int cover_charset;
    char cover_track_id[96];
    char cover_text[INFO_COVER_TEXT_MAX];

    /* 歌词文档缓存：本地后端直读进程内引擎内存，远端后端的等价物是这份
     * 缓存（界面按任意下标取行，不能每行一次 D-Bus）。归属由
     * lyrics_page_track_id 校验，失效点见 remote_store_track/lyrics。 */
    int lyrics_page_valid;
    int lyrics_page_count;
    int lyrics_total;
    char lyrics_page_track_id[96];
    uint64_t lyrics_retry_ms;       /* 上次拉取失败后的重试门槛（退避 1s） */
    LyricLine lyrics_lines[MAX_LYRIC_LINES];

    /* 队列页缓存（position→条目）：本地后端能就地读 bq，远端只能经 Queue.Get
     * 分页取。这里缓存一个连续窗口，供 queue_index_at / track_metadata 反查
     * 内容下标；窗口按 queue_revision 失效，改动队列的方法会顶修订号。 */
    BackendQueueEntry *queue_page_entries;
    uint64_t queue_page_revision;   /* 与缓存对应的 queue_revision */
    int queue_page_base;            /* 窗口起始队列位置 */
    int queue_page_count;           /* 窗口内已填充条目数 */
    int queue_page_cap;             /* 窗口已分配槽位 */

    /* 可视化快照缓存（Info.GetVisualizer）：界面每帧取数，这里带节流地拉取。
     * viz_active=0 时界面要求停止拉取（本期不做帧信号订阅）。 */
    int viz_active;
    int viz_valid;
    uint64_t viz_last_fetch_ms;
    uint64_t viz_revision;
    int viz_levels[VISUALIZER_BAND_COUNT];
    int viz_peaks[VISUALIZER_BAND_COUNT];

    /* 上次已知的核心配置：player_config_persist() 只把相对它的差异下发 */
    AppConfig synced_config;
    int config_synced;
} RemoteState;

const char *player_remote_play_mode_name_of(PlayMode mode, int use_english);

/* 队列页缓存前置声明（定义在队列章节，track_metadata 需要早于它取用） */
static int remote_queue_cache_at(int position, BackendQueueEntry *out);

static RemoteState g_remote;

static uint64_t remote_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ── 连接与握手 ─────────────────────────────────────────────────── */

/* 队列页缓存失效：释放窗口并复位，下一次读取重新按 Queue.Get 填充。
 * 断线、重连、以及任何改动队列内容的门面方法都调用它。 */
static void remote_queue_cache_reset(void)
{
    free(g_remote.queue_page_entries);
    g_remote.queue_page_entries = NULL;
    g_remote.queue_page_cap = 0;
    g_remote.queue_page_base = -1;
    g_remote.queue_page_count = 0;
    g_remote.queue_page_revision = 0;
}

static void remote_close_connection(void)
{
    if (g_remote.connection) {
        dbus_connection_close(g_remote.connection);
        dbus_connection_unref(g_remote.connection);
        g_remote.connection = NULL;
    }
    g_remote.token[0] = '\0';
    g_remote.connected = 0;
    remote_queue_cache_reset();
    g_remote.viz_valid = 0;
}

static void ensure_session_bus(void)
{
    if (getenv("DBUS_SESSION_BUS_ADDRESS")) {
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        char address[256];
        snprintf(address, sizeof(address), "unix:path=%s/bus", runtime);
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
    }
}

/* 发一次调用并等回复。
 * 参数用显式的 message_iter 逐个追加，**不用** dbus_message_append_args_valist：
 * 后者经 va_arg 取参数，传字符串字面量会被当成整数读出并崩溃（实测 SIGSEGV），
 * 显式迭代器没有这个陷阱，也更容易看出每个调用的真实签名。 */
static DBusMessage *remote_call_begin(const char *interface_name, const char *method,
                                      int timeout_ms, DBusMessage **message_out)
{
    *message_out = dbus_message_new_method_call(
        g_remote.bus_name[0] ? g_remote.bus_name : CLI_PRIMARY_BUS_NAME,
        REMOTE_OBJECT_PATH, interface_name, method);
    if (!*message_out) {
        return NULL;
    }
    (void)timeout_ms;
    return *message_out;
}

static DBusMessage *remote_call_finish(DBusMessage *message, int timeout_ms)
{
    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        g_remote.connection, message, timeout_ms > 0 ? timeout_ms : 2000, &error);
    dbus_message_unref(message);
    if (!reply) {
        if (dbus_error_is_set(&error)) {
            log_debug("player_remote", "Call failed: %s", error.message);
            dbus_error_free(&error);
        }
        return NULL;
    }
    return reply;
}

/* 无参数调用 */
static DBusMessage *remote_call0(const char *interface_name, const char *method, int timeout_ms)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    return remote_call_finish(message, timeout_ms);
}

/* 单个字符串参数 */
static DBusMessage *remote_call_s(const char *interface_name, const char *method,
                                  int timeout_ms, const char *arg)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    const char *text = arg ? arg : "";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text);
    return remote_call_finish(message, timeout_ms);
}

/* 单个 int32 参数 */
static DBusMessage *remote_call_i(const char *interface_name, const char *method,
                                  int timeout_ms, int arg)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_int32_t value = (dbus_int32_t)arg;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &value);
    return remote_call_finish(message, timeout_ms);
}

/* int32 + 字符串（Queue.InsertAfter / Append 的位置 + 载荷） */
static DBusMessage *remote_call_is(const char *interface_name, const char *method,
                                   int timeout_ms, int first, const char *second)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_int32_t value = (dbus_int32_t)first;
    const char *text = second ? second : "";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &value);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text);
    return remote_call_finish(message, timeout_ms);
}

/* int32 + int32（Queue.Get 的分页） */
static DBusMessage *remote_call_ii(const char *interface_name, const char *method,
                                   int timeout_ms, int first, int second)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_int32_t a = (dbus_int32_t)first;
    dbus_int32_t b = (dbus_int32_t)second;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &a);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &b);
    return remote_call_finish(message, timeout_ms);
}

/* 三个 int32（GetCoverArt 的列/行/字符集） */
static DBusMessage *remote_call_iii(const char *interface_name, const char *method,
                                    int timeout_ms, int a0, int a1, int a2)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_int32_t v0 = (dbus_int32_t)a0;
    dbus_int32_t v1 = (dbus_int32_t)a1;
    dbus_int32_t v2 = (dbus_int32_t)a2;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v0);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v1);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v2);
    return remote_call_finish(message, timeout_ms);
}

/* 单个 int64 参数（Control.SeekTo 的微秒） */
static DBusMessage *remote_call_x(const char *interface_name, const char *method,
                                  int timeout_ms, int64_t arg)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_int64_t value = (dbus_int64_t)arg;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT64, &value);
    return remote_call_finish(message, timeout_ms);
}

/* 单个 double 参数（Control.SetSpeed） */
static DBusMessage *remote_call_d(const char *interface_name, const char *method,
                                  int timeout_ms, double arg)
{
    if (!g_remote.connection) {
        return NULL;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(interface_name, method, timeout_ms, &message)) {
        return NULL;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    double value = arg;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_DOUBLE, &value);
    return remote_call_finish(message, timeout_ms);
}

/* 读取字符串返回值（调用方负责 unref 回复） */
static int remote_reply_string(DBusMessage *reply, char *out, size_t out_size)
{
    if (!reply || !out || out_size == 0) {
        return -1;
    }
    const char *text = NULL;
    if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) {
        return -1;
    }
    snprintf(out, out_size, "%s", text ? text : "");
    return 0;
}

static int remote_reply_bool(DBusMessage *reply, int *out)
{
    if (!reply) {
        return -1;
    }
    dbus_bool_t value = FALSE;
    if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID)) {
        return -1;
    }
    if (out) {
        *out = value ? 1 : 0;
    }
    return 0;
}

static int remote_reply_int(DBusMessage *reply, int *out)
{
    if (!reply) {
        return -1;
    }
    dbus_int32_t value = 0;
    if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID)) {
        return -1;
    }
    if (out) {
        *out = (int)value;
    }
    return 0;
}

/* 握手：校验 api_version 与必需方法；成功时完成 Attach */
static int remote_handshake(void)
{
    char json[INFO_JSON_MAX];
    DBusMessage *reply = remote_call0(REMOTE_IFACE_INFO, "GetInfo", 3000);
    if (!reply) {
        log_warn("player_remote_remote", "Handshake failed: no reply from '%s'", g_remote.bus_name);
        return -1;
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }

    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));

    long version = 0;
    if (json_get_path(&reader, "core.api_version", &value) == 0) {
        version = json_value_int(&value, 0);
    }
    if (version < TER_MUSIC_API_VERSION) {
        log_error("player_remote_remote", "Core is too old: api_version=%ld, need >= %d",
                  version, TER_MUSIC_API_VERSION);
        return -1;
    }

    /* core.methods 必须包含所需方法（缺失即核心过旧）。
     * 注意：json_get_path 会推进传入的 reader（成功路径不回写），因此每次
     * 从同一个 reader 取不同路径前要重新初始化。 */
    json_reader_init(&reader, json, strlen(json));
    int have_methods = (json_get_path(&reader, "core.methods", &value) == 0 &&
                        value.type == JSON_VALUE_ARRAY);
    if (!have_methods) {
        /* 老核心不带方法集：只能靠 api_version 判定，记一条日志 */
        log_warn("player_remote", "Core does not declare core.methods; relying on api_version");
    }
    if (have_methods) {
        for (int i = 0; k_required_methods[i] != NULL; i++) {
            int found = 0;
            JsonReader array = reader;
            JsonValue element;
            if (json_reader_enter(&array, &value) == 0) {
                while (json_array_next(&array, &element) == 1) {
                    char name[128];
                    name[0] = '\0';
                    if (element.type == JSON_VALUE_STRING) {
                        json_value_string(&element, name, sizeof(name));
                    }
                    if (element.type == JSON_VALUE_STRING &&
                        strcmp(name, k_required_methods[i]) == 0) {
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) {
                log_error("player_remote_remote", "Core is missing required method %s",
                          k_required_methods[i]);
                return -1;
            }
        }
    }

    /* Attach：登记本前端并取得心跳间隔 */
    /* 注意：参数经 va_arg 取用，字符串**必须**先落到指针变量再传入，
     * 直接传字面量在 x86-64 上会被当成整数读出并崩溃（实测 SIGSEGV）。 */
    const char *role = "tui";
    reply = remote_call_s(REMOTE_IFACE_CONTROL, "Attach", 3000, role);
    if (!reply) {
        log_warn("player_remote_remote", "Attach failed");
        return -1;
    }
    char attached[512];
    rc = remote_reply_string(reply, attached, sizeof(attached));
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }

    JsonReader att_reader;
    json_reader_init(&att_reader, attached, strlen(attached));
    if (json_get_path(&att_reader, "token", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, g_remote.token, sizeof(g_remote.token));
    }
    g_remote.ping_interval_ms = RPC_PING_INTERVAL_MS;
    if (json_get_path(&att_reader, "ping_interval_ms", &value) == 0) {
        long interval = json_value_int(&value, RPC_PING_INTERVAL_MS);
        if (interval >= 500 && interval <= 60000) {
            g_remote.ping_interval_ms = (int)interval;
        }
    }

    log_info("player_remote_remote", "Attached to '%s' (api_version=%ld, ping=%dms, token=%s)",
             g_remote.bus_name, version, g_remote.ping_interval_ms,
             g_remote.token[0] ? g_remote.token : "-");
    return 0;
}

static int remote_try_connect(void)
{
    ensure_session_bus();
    remote_close_connection();

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!connection) {
        if (dbus_error_is_set(&error)) {
            log_debug("player_remote_remote", "Session bus unavailable: %s", error.message);
            dbus_error_free(&error);
        }
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    g_remote.connection = connection;

    if (remote_handshake() != 0) {
        remote_close_connection();
        return -1;
    }

    g_remote.connected = 1;
    g_remote.backoff_index = 0;
    g_remote.next_ping_ms = (int)(remote_now_ms() + (uint64_t)g_remote.ping_interval_ms);
    /* 接回来的可能是另一个核心进程（重启过）：歌词缓存不再可信 */
    g_remote.lyrics_page_valid = 0;
    g_remote.lyrics_retry_ms = 0;
    return 0;
}

/* ── 快照拉取 ───────────────────────────────────────────────────── */

static void remote_store_track(const char *json)
{
    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));

    InfoTrack track;
    memset(&track, 0, sizeof(track));
    track.queue_position = -1;

    /* 队列长度与游标是后端队列的事实，无论是否有当前曲目都如实报告 */
    if (json_get_path(&reader, "track.playlist_count", &value) == 0) {
        track.playlist_total = (int)json_value_int(&value, 0);
    }
    if (json_get_path(&reader, "track.queue_count", &value) == 0) {
        track.queue_count = (int)json_value_int(&value, 0);
    }
    if (json_get_path(&reader, "track.queue_position", &value) == 0 &&
        value.type == JSON_VALUE_NUMBER) {
        track.queue_position = (int)json_value_int(&value, -1) - 1;   /* 1 基 → 0 基 */
    }
    if (json_get_path(&reader, "track.id", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, track.track_id, sizeof(track.track_id));
    }
    if (json_get_path(&reader, "track.path", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, track.path, sizeof(track.path));
    }
    if (json_get_path(&reader, "track.title", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, track.title, sizeof(track.title));
    }
    if (json_get_path(&reader, "track.artist", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, track.artist, sizeof(track.artist));
    }
    if (json_get_path(&reader, "track.album", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, track.album, sizeof(track.album));
    }

    /* 有效性判定：核心不给 track.valid 键（见 info_append_track_object，键集只有
     * id/index/number/...），改用真实字段——track.index 非 null 或 track.id 非 null
     * 即存在当前曲目。这两个键只在核心侧 t->valid 时才写成非 null。 */
    json_reader_init(&reader, json, strlen(json));
    int index_present = (json_get_path(&reader, "track.index", &value) == 0 &&
                         value.type == JSON_VALUE_NUMBER);
    track.valid = (index_present || track.track_id[0]) ? 1 : 0;

    /* 界面要的是**内容物理下标**（player_track_index 的门面契约）：核心的
     * track.index 是后端队列位（g_current_play_index），不能直接喂给界面。
     * 用当前曲目路径反查前端内容列表，与本地后端 queue_index_at 同一口径。 */
    track.index = -1;
    if (track.valid && track.path[0]) {
        track.index = playlist_find_track_index_by_path(track.path);
    }

    /* 封面文件路径：核心在 Info 的 cover.art_url（file:// URI）里给出，
     * 还原成本地路径供界面渲染盲文封面（前后端同机，路径可直接读）。 */
    json_reader_init(&reader, json, strlen(json));
    if (json_get_path(&reader, "cover.art_url", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        char art_url[INFO_URI_MAX];
        art_url[0] = '\0';
        if (json_value_string(&value, art_url, sizeof(art_url)) > 0) {
            info_uri_to_path(art_url, track.cover_path, sizeof(track.cover_path));
        }
    }
    track.has_cover = track.cover_path[0] ? 1 : 0;

    if (track.queue_count != g_remote.queue_count ||
        track.queue_position != g_remote.queue_position) {
        g_remote.queue_count = track.queue_count;
        g_remote.queue_position = track.queue_position;
        g_remote.queue_revision++;
    }

    /* 曲目标识变化才重算：否则每轮都会把 state_revision 顶上去 */
    if (track.valid != g_remote.track.valid ||
        strcmp(track.track_id, g_remote.track.track_id) != 0 ||
        track.index != g_remote.track.index) {
        g_remote.cover_valid = 0;
        g_remote.lyrics_page_valid = 0;
        g_remote.state_revision++;
        g_remote.lyrics_revision++;
    }

    g_remote.track = track;
}

static void remote_store_playback(const char *json)
{
    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));

    InfoPlayback playback;
    memset(&playback, 0, sizeof(playback));
    playback.state = PLAY_STATE_STOPPED;
    playback.speed = 1.0f;

    char text[64];
    if (json_get_path(&reader, "playback.state", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        json_value_string(&value, text, sizeof(text));
        if (strcmp(text, "playing") == 0) {
            playback.state = PLAY_STATE_PLAYING;
        } else if (strcmp(text, "paused") == 0) {
            playback.state = PLAY_STATE_PAUSED;
        } else {
            playback.state = PLAY_STATE_STOPPED;
        }
    }
    if (json_get_path(&reader, "playback.position_ms", &value) == 0) {
        playback.position_seconds = (int)(json_value_int(&value, 0) / 1000);
    }
    if (json_get_path(&reader, "playback.duration_ms", &value) == 0) {
        playback.duration_seconds = (int)(json_value_int(&value, 0) / 1000);
    }
    if (json_get_path(&reader, "playback.volume_percent", &value) == 0) {
        playback.volume_percent = (int)json_value_int(&value, 0);
    }
    if (json_get_path(&reader, "playback.speed", &value) == 0) {
        playback.speed = (float)json_value_double(&value, 1.0);
    }
    if (json_get_path(&reader, "playback.play_mode", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        json_value_string(&value, text, sizeof(text));
        int mode = info_play_mode_from_id(text);
        if (mode >= 0) {
            playback.play_mode = (PlayMode)mode;
        }
    }
    if (json_get_path(&reader, "playback.can_seek", &value) == 0) {
        playback.can_seek = json_value_bool(&value, 0) ? 1 : 0;
    }

    if (playback.state != g_remote.playback.state ||
        playback.duration_seconds != g_remote.playback.duration_seconds ||
        playback.volume_percent != g_remote.playback.volume_percent ||
        playback.play_mode != g_remote.playback.play_mode ||
        playback.can_seek != g_remote.playback.can_seek ||
        (playback.speed - g_remote.playback.speed > 0.001f) ||
        (g_remote.playback.speed - playback.speed > 0.001f)) {
        g_remote.state_revision++;
    }

    /* 进度不参与“变化”判定（每次拉取都会动），只做锚点 */
    g_remote.playback = playback;
    g_remote.anchor_ms = remote_now_ms();
    g_remote.anchor_position = playback.position_seconds;
}

static void remote_store_lyrics(const char *json)
{
    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));

    InfoLyrics lyrics;
    memset(&lyrics, 0, sizeof(lyrics));
    lyrics.current_index = -1;
    lyrics.next_index = -1;

    if (json_get_path(&reader, "lyrics.has_lyrics", &value) == 0) {
        lyrics.has_lyrics = json_value_bool(&value, 0) ? 1 : 0;
    }
    if (json_get_path(&reader, "lyrics.has_timestamps", &value) == 0) {
        lyrics.has_timestamps = json_value_bool(&value, 0) ? 1 : 0;
    }
    if (json_get_path(&reader, "lyrics.current.index", &value) == 0 &&
        value.type == JSON_VALUE_NUMBER) {
        lyrics.current_index = (int)json_value_int(&value, -1);
    }
    if (json_get_path(&reader, "lyrics.current.text", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        json_value_string(&value, lyrics.current_text, sizeof(lyrics.current_text));
    }
    if (json_get_path(&reader, "lyrics.next.text", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        json_value_string(&value, lyrics.next_text, sizeof(lyrics.next_text));
    }
    char source[32];
    /* json_value_string 返回**写入字节数**：写成 `== 0` 会让映射永不执行，
     * 于是界面的 Ctrl+L → Tab 根据 `player_lyrics_source()` 判断时永远看到
     * AUTO(0)，只会请求内嵌歌词——来源切换在远端模式下静默失效。 */
    if (json_get_path(&reader, "lyrics.source", &value) == 0 &&
        value.type == JSON_VALUE_STRING &&
        json_value_string(&value, source, sizeof(source)) > 0) {
        for (int s = LYRICS_SOURCE_AUTO; s <= LYRICS_SOURCE_EXTERNAL; s++) {
            if (strcmp(source, info_lyrics_source_id(s)) == 0) {
                lyrics.source = s;
                break;
            }
        }
    }

    if (lyrics.current_index != g_remote.lyrics.current_index ||
        lyrics.has_lyrics != g_remote.lyrics.has_lyrics ||
        lyrics.source != g_remote.lyrics.source ||
        strcmp(lyrics.current_text, g_remote.lyrics.current_text) != 0) {
        g_remote.lyrics_revision++;
    }

    /* 歌词**身份**变化（有没有 / 来源 / 有无时间戳）意味着行数据也换了，
     * 缓存必须失效重取。这是修复竞态的关键：play_audio() 里的 load_lyrics()
     * 在播放命令返回之后才执行，前端可能先轮询到 has_lyrics=false；靠这次
     * 0→1 跳变，面板才能在第一帧取到行数据。 */
    if (lyrics.has_lyrics != g_remote.lyrics.has_lyrics ||
        lyrics.source != g_remote.lyrics.source ||
        lyrics.has_timestamps != g_remote.lyrics.has_timestamps) {
        g_remote.lyrics_page_valid = 0;
    }
    g_remote.lyrics = lyrics;
}

static int remote_refresh_snapshot(void)
{
    char json[INFO_JSON_MAX];
    DBusMessage *reply = remote_call0(REMOTE_IFACE_INFO, "GetInfo", 2000);
    if (!reply) {
        return -1;
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }

    remote_store_track(json);
    remote_store_playback(json);
    remote_store_lyrics(json);

    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));
    char status[CORE_STATUS_MAX] = "";
    if (json_get_path(&reader, "status.message", &value) == 0 &&
        value.type == JSON_VALUE_STRING) {
        json_value_string(&value, status, sizeof(status));
    }
    if (strcmp(status, g_remote.status_message) != 0) {
        snprintf(g_remote.status_message, sizeof(g_remote.status_message), "%s", status);
        g_remote.state_revision++;
    }
    return 0;
}

/* ── 生命周期 ───────────────────────────────────────────────────── */

int player_remote_init(PlayerBackend backend, const char *bus_name)
{
    if (backend != PLAYER_BACKEND_REMOTE) {
        log_error("player_remote_remote", "player_remote only implements the remote backend");
        return -1;
    }

    memset(&g_remote, 0, sizeof(g_remote));
    g_remote.requested = backend;
    g_remote.track.queue_position = -1;
    g_remote.lyrics.current_index = -1;
    g_remote.lyrics.next_index = -1;
    g_remote.queue_page_base = -1;
    g_remote.viz_active = 1;   /* 默认允许按需拉取；界面可用 set_visualizer_active 关闭 */
    snprintf(g_remote.bus_name, sizeof(g_remote.bus_name), "%s",
             (bus_name && bus_name[0]) ? bus_name : CLI_PRIMARY_BUS_NAME);
    g_remote.initialized = 1;
    g_remote.offline = 1;

    if (remote_try_connect() != 0) {
        g_remote.offline = 1;
        g_remote.reconnect_due_ms = (int)(remote_now_ms() + k_backoff_ms[0]);
        log_warn("player_remote_remote", "Core '%s' is not reachable yet", g_remote.bus_name);
        return -1;
    }

    remote_refresh_snapshot();
    g_remote.state_revision++;
    g_remote.queue_revision++;
    g_remote.lyrics_revision++;
    return 0;
}

/* 让核心自己收尾退出（Control.Quit）：要不要走这里由**前端自己的配置镜像**
 * preferences.stop_core_on_exit 判定，本函数不参与取值，且只在调用方确认已接入核心
 * （握手完成并持有 Attach 令牌）之后才被调用。核心的 Quit 处理器置退出标志后回 true，
 * 主循环收尾会先持久化会话再停播——前端**不得**改发 Control.Stop（那会半途停播
 * 却不退进程）。语义与 cli_client_quit() 一致：核心可能在回复送达前就关掉了连接
 * （它正在退出），所以“没回复/连接断开类错误”同样算成功；其它错误只记一条日志。
 * 只发一次同步调用，不等进程消亡、不轮询总线名消失，绝不阻塞前端退出。 */
static void remote_quit_core(void)
{
    if (!g_remote.connection) {
        return;
    }
    DBusMessage *message = NULL;
    if (!remote_call_begin(REMOTE_IFACE_CONTROL, "Quit", 1500, &message)) {
        return;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        g_remote.connection, message, 1500, &error);
    dbus_message_unref(message);
    if (reply) {
        dbus_message_unref(reply);
        return;
    }

    if (dbus_error_is_set(&error)) {
        int going_away = dbus_error_has_name(&error, DBUS_ERROR_NO_REPLY) ||
                         dbus_error_has_name(&error, DBUS_ERROR_DISCONNECTED) ||
                         dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN) ||
                         dbus_error_has_name(&error, DBUS_ERROR_NAME_HAS_NO_OWNER);
        if (!going_away) {
            log_warn("player_remote", "Control.Quit failed: %s",
                     error.message ? error.message : "unknown D-Bus error");
        }
        dbus_error_free(&error);
    }
}

void player_remote_shutdown(void)
{
    /* 幂等：initialized 在收尾末尾清零，重复调用不再发任何消息 */
    if (!g_remote.initialized) {
        return;
    }

    /* 接入门控：本前端从没成功接入过核心（握手失败、离线退避中、Attach 未成功），
     * 那个总线名对应的核心不由本前端登记，退出时 Quit 和 Detach 都不该发——
     * 一条总线消息都不发，只收掉自己的连接。 */
    if (!g_remote.connected || !g_remote.token[0]) {
        remote_close_connection();
        g_remote.initialized = 0;
        return;
    }

    /* 分流开关取自前端自己的配置镜像：启动时 load_config() 已读盘，设置页的改动
     * 也已通过 player_config_persist() 即时下发核心，退出路径不再额外打总线。 */
    if (g_app_config.stop_core_on_exit) {
        /* 1 = 退出即终止核心：进程将亡，Detach 没有意义，直接 Quit */
        remote_quit_core();
    } else {
        /* 0 = “关掉 TUI，音乐继续”：只注销前端注册 */
        DBusMessage *reply = remote_call_s(REMOTE_IFACE_CONTROL, "Detach", 1000, g_remote.token);
        if (reply) {
            dbus_message_unref(reply);
        }
    }
    remote_close_connection();
    g_remote.initialized = 0;
}

int player_remote_pump(void)
{
    if (!g_remote.initialized) {
        return -1;
    }

    uint64_t now = remote_now_ms();

    if (!g_remote.connected) {
        if ((int)now < g_remote.reconnect_due_ms) {
            return -1;
        }
        /* 指数退避重连；成功后重新握手并 Attach */
        if (remote_try_connect() == 0) {
            remote_refresh_snapshot();
            g_remote.state_revision++;
            /* 通知前端：核心（可能是新起的）里没有队列，需要前端补推 */
            player_notify_reconnected();
            return 0;
        }
        g_remote.offline = 1;
        if (g_remote.backoff_index < REMOTE_BACKOFF_COUNT - 1) {
            g_remote.backoff_index++;
        }
        g_remote.reconnect_due_ms = (int)(now + (uint64_t)k_backoff_ms[g_remote.backoff_index]);
        return -1;
    }

    /* 心跳：按核心给的间隔 Ping，核心据此判定前端在线 */
    if ((int)now >= g_remote.next_ping_ms) {
        g_remote.next_ping_ms = (int)(now + (uint64_t)g_remote.ping_interval_ms);
        if (g_remote.token[0]) {
            DBusMessage *reply = remote_call_s(REMOTE_IFACE_CONTROL, "Ping", 1500, g_remote.token);
            if (!reply) {
                log_warn("player_remote_remote", "Heartbeat lost; the core may be gone");
                remote_close_connection();
                g_remote.offline = 1;
                g_remote.backoff_index = 0;
                g_remote.reconnect_due_ms = (int)(now + k_backoff_ms[0]);
                return -1;
            }
            dbus_message_unref(reply);
        }
    }

    if (remote_refresh_snapshot() != 0) {
        log_warn("player_remote_remote", "Snapshot failed; dropping the connection");
        remote_close_connection();
        g_remote.offline = 1;
        g_remote.backoff_index = 0;
        g_remote.reconnect_due_ms = (int)(now + k_backoff_ms[0]);
        return -1;
    }
    return 0;
}

int player_remote_is_connected(void)
{
    return g_remote.connected;
}

PlayerBackend player_remote_backend(void)
{
    return PLAYER_BACKEND_REMOTE;
}

int player_remote_is_offline(void)
{
    return g_remote.offline || !g_remote.connected;
}

int player_remote_reconnect_in_ms(void)
{
    if (g_remote.connected) {
        return 0;
    }
    int now = (int)remote_now_ms();
    return g_remote.reconnect_due_ms > now ? g_remote.reconnect_due_ms - now : 0;
}

int player_remote_restart_core(void)
{
    if (!g_remote.initialized) {
        return -1;
    }

    /* 先断开旧连接，再立刻试一次（核心可能只是短暂无响应） */
    remote_close_connection();
    g_remote.backoff_index = 0;
    g_remote.reconnect_due_ms = (int)remote_now_ms();
    if (player_remote_pump() == 0) {
        return 0;
    }

    /* 核心确实不在了：拉起一个新的后台核心，再给它一点时间上线。
     * 界面在事件循环里调用本函数，其余工作由后续的 player_pump() 继续。 */
    if (cli_in_sandbox()) {
        if (cli_client_activate_instance(g_remote.bus_name[0] ? g_remote.bus_name : NULL)
            != CLI_EXIT_OK) {
            return -1;
        }
    } else {
        char pid_text[32] = "";
        if (daemon_start_background(NULL, 0, 0, pid_text, sizeof(pid_text)) != CLI_EXIT_OK) {
            return -1;
        }
    }

    /* 不在这里等待：把下次重连提前，由事件循环的心跳把它接上 */
    g_remote.reconnect_due_ms = (int)remote_now_ms() + 300;
    g_remote.offline = 1;
    return -1;   /* 尚未连上；界面据 offline 状态继续提示，后续 pump 会自动接入 */
}

/* ── 修订号与快照 ───────────────────────────────────────────────── */

uint64_t player_remote_state_revision(void)    { return g_remote.state_revision; }
uint64_t player_remote_queue_revision(void)    { return g_remote.queue_revision; }
uint64_t player_remote_lyrics_revision(void)   { return g_remote.lyrics_revision; }
uint64_t player_remote_config_revision(void)   { return g_remote.config_revision; }
uint64_t player_remote_cover_revision(void)    { return g_remote.cover_revision; }

const InfoTrack    *player_remote_track(void)    { return &g_remote.track; }
const InfoLyrics   *player_remote_lyrics(void)   { return &g_remote.lyrics; }
const char *player_remote_status_message(void)   { return g_remote.status_message; }

const InfoPlayback *player_remote_playback(void)
{
    /* 位置按本地单调时钟外推：远端快照最慢每轮一次，外推让进度条平滑 */
    static InfoPlayback playback;
    playback = g_remote.playback;
    if (playback.state == PLAY_STATE_PLAYING && playback.duration_seconds > 0) {
        uint64_t elapsed = remote_now_ms() - g_remote.anchor_ms;
        int position = g_remote.anchor_position + (int)(elapsed / 1000);
        if (position > playback.duration_seconds) {
            position = playback.duration_seconds;
        }
        playback.position_seconds = position;
    }
    return &playback;
}

int player_remote_track_index(void)      { return g_remote.track.valid ? g_remote.track.index : -1; }
int player_remote_position_seconds(void) { return player_remote_playback()->position_seconds; }
int player_remote_duration_seconds(void) { return g_remote.playback.duration_seconds; }
int player_remote_play_state(void)       { return (int)g_remote.playback.state; }
int player_remote_play_mode(void)        { return (int)g_remote.playback.play_mode; }
int player_remote_volume_percent(void)   { return g_remote.playback.volume_percent; }
float player_remote_speed(void)          { return g_remote.playback.speed; }

int player_remote_track_metadata(int index, Track *out)
{
    /* 远端不做内容查询：元数据来自队列页缓存（Queue.Get 带回的条目）。
     * 这里的 index 是**队列位置**（调用方按队列长度遍历即可，整表下发时
     * 与内容下标一一对应；Shuffle 后仍能给出核心实际持有的那一行）。 */
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    BackendQueueEntry entry;
    if (remote_queue_cache_at(index, &entry) != 0 || !entry.path[0]) {
        return -1;
    }
    snprintf(out->path, sizeof(out->path), "%s", entry.path);
    snprintf(out->title, sizeof(out->title), "%s", entry.title);
    snprintf(out->artist, sizeof(out->artist), "%s", entry.artist);
    snprintf(out->album, sizeof(out->album), "%s", entry.album);
    return 0;
}

/* ── transport ──────────────────────────────────────────────────── */

static int remote_bool_call(const char *interface_name, const char *method)
{
    DBusMessage *reply = remote_call0(interface_name, method, 2000);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    return rc == 0 && ok ? 0 : -1;
}

void player_remote_play(int track_index)
{
    if (track_index >= 0) {
        /* 越界保护（对齐本地后端 player_local_play 的下标校验）：PlayAt 的入参
         * 是队列位置，越界时不发命令，避免核心回 OutOfRange 后界面看似“点了没反应”。
         * 队列内容变化后的正常播放入口走 play_track_index()（路径反查位置再播）。 */
        if (track_index >= g_remote.queue_count) {
            return;
        }
        DBusMessage *reply = remote_call_i(REMOTE_IFACE_QUEUE, "PlayAt", 3000, track_index);
        if (reply) {
            dbus_message_unref(reply);
        }
    } else {
        remote_bool_call(REMOTE_IFACE_CONTROL, "Play");
    }
    remote_refresh_snapshot();
}

void player_remote_pause(void)         { remote_bool_call(REMOTE_IFACE_CONTROL, "Pause"); remote_refresh_snapshot(); }
void player_remote_resume(void)        { remote_bool_call(REMOTE_IFACE_CONTROL, "Play"); remote_refresh_snapshot(); }
void player_remote_play_pause(void)    { remote_bool_call(REMOTE_IFACE_CONTROL, "PlayPause"); remote_refresh_snapshot(); }
void player_remote_stop(void)          { remote_bool_call(REMOTE_IFACE_CONTROL, "Stop"); remote_refresh_snapshot(); }
void player_remote_next(void)          { remote_bool_call(REMOTE_IFACE_CONTROL, "Next"); remote_refresh_snapshot(); }
void player_remote_prev(void)          { remote_bool_call(REMOTE_IFACE_CONTROL, "Previous"); remote_refresh_snapshot(); }

void player_remote_seek_seconds(int seconds)
{
    DBusMessage *reply = remote_call_x(REMOTE_IFACE_CONTROL, "SeekTo", 2000, (int64_t)seconds * 1000000LL);
    if (reply) {
        dbus_message_unref(reply);
    }
    remote_refresh_snapshot();
}

void player_remote_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* 乐观回显：立刻改本地快照，界面无需等下一次拉取 */
    if (percent != g_remote.playback.volume_percent) {
        g_remote.playback.volume_percent = percent;
        g_remote.state_revision++;
    }

    DBusMessage *reply = remote_call_i(REMOTE_IFACE_CONTROL, "SetVolume", 2000, percent);
    if (reply) {
        dbus_message_unref(reply);
    }
}

void player_remote_adjust_volume(int delta)
{
    player_remote_set_volume(player_remote_volume_percent() + delta);
}

void player_remote_set_speed(float rate)
{
    if (rate != g_remote.playback.speed) {
        g_remote.playback.speed = rate;
        g_remote.state_revision++;
    }
    DBusMessage *reply = remote_call_d(REMOTE_IFACE_CONTROL, "SetSpeed", 2000, (double)rate);
    if (reply) {
        dbus_message_unref(reply);
    }
}

void player_remote_set_play_mode(PlayMode mode)
{
    DBusMessage *reply = remote_call_i(REMOTE_IFACE_CONTROL, "SetPlayMode", 2000, mode);
    if (reply) {
        dbus_message_unref(reply);
    }
    remote_refresh_snapshot();
}

void player_remote_cycle_play_mode(void)
{
    PlayMode next = (PlayMode)(((int)g_remote.playback.play_mode + 1) % 5);
    player_remote_set_play_mode(next);
}

/* 倍速档位：核心侧是固定的档位表（Control.SetSpeed 接受任意值），
 * 前端渲染菜单时用同一张表，避免两边档位不一致 */
static const float k_speed_steps[] = { 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };
#define REMOTE_SPEED_STEP_COUNT (int)(sizeof(k_speed_steps) / sizeof(k_speed_steps[0]))

const float *player_remote_speed_steps(int *out_count)
{
    if (out_count) {
        *out_count = REMOTE_SPEED_STEP_COUNT;
    }
    return k_speed_steps;
}

int player_remote_speed_step_count(void)
{
    return REMOTE_SPEED_STEP_COUNT;
}

int player_remote_speed_index(void)
{
    float current = g_remote.playback.speed;
    int best = 0;
    for (int i = 1; i < REMOTE_SPEED_STEP_COUNT; i++) {
        if (fabsf(k_speed_steps[i] - current) < fabsf(k_speed_steps[best] - current)) {
            best = i;
        }
    }
    return best;
}

void player_remote_set_speed_index(int index)
{
    if (index < 0 || index >= REMOTE_SPEED_STEP_COUNT) {
        return;
    }
    player_remote_set_speed(k_speed_steps[index]);
}

const char *player_remote_play_mode_name(int use_english)
{
    return player_remote_play_mode_name_of((PlayMode)player_remote_play_mode(), use_english);
}


const char *player_remote_play_mode_name_of(PlayMode mode, int use_english)
{
    /* 显示名在前端本地渲染（i18n 归前端）；模块出处与本地后端一致 */
    (void)use_english;
    switch (mode) {
        case PLAY_MODE_SEQUENTIAL:      return i18n_get("play_mode.seq");
        case PLAY_MODE_SINGLE_REPEAT:   return i18n_get("play_mode.single_repeat");
        case PLAY_MODE_LIST_REPEAT:     return i18n_get("play_mode.list_repeat");
        case PLAY_MODE_SHUFFLE_ONCE:    return i18n_get("play_mode.shuffle_once");
        case PLAY_MODE_SHUFFLE_REPEAT:  return i18n_get("play_mode.shuffle_repeat");
        case PLAY_MODE_FOLDER_SEQUENTIAL: return i18n_get("play_mode.folder_seq");
        case PLAY_MODE_FOLDER_REPEAT:   return i18n_get("play_mode.folder_repeat");
        case PLAY_MODE_FOLDER_SHUFFLE:  return i18n_get("play_mode.folder_shuffle");
        case PLAY_MODE_FOLDER_SHUFFLE_REPEAT: return i18n_get("play_mode.folder_shuffle_repeat");
        case PLAY_MODE_ALBUM_SEQUENTIAL: return i18n_get("play_mode.album_seq");
        case PLAY_MODE_ALBUM_REPEAT:    return i18n_get("play_mode.album_repeat");
        case PLAY_MODE_ALBUM_SHUFFLE:   return i18n_get("play_mode.album_shuffle");
        case PLAY_MODE_ALBUM_SHUFFLE_REPEAT: return i18n_get("play_mode.album_shuffle_repeat");
        case PLAY_MODE_ARTIST_SEQUENTIAL: return i18n_get("play_mode.artist_seq");
        case PLAY_MODE_ARTIST_REPEAT:   return i18n_get("play_mode.artist_repeat");
        case PLAY_MODE_ARTIST_SHUFFLE:  return i18n_get("play_mode.artist_shuffle");
        case PLAY_MODE_ARTIST_SHUFFLE_REPEAT: return i18n_get("play_mode.artist_shuffle_repeat");
        default:                        return i18n_get("play_mode.seq");
    }
}

/* ── 队列（远端：内容由前端下发） ───────────────────────────────── */

int player_remote_queue_count(void)    { return g_remote.queue_count; }
int player_remote_queue_position(void) { return g_remote.queue_position; }

/* 队列页缓存窗口大小：一页 ≤256 条，配合 Queue.Get 的接收缓冲（RPC_PAYLOAD_MAX/2）
 * 让单页载荷稳定在 256 KB 以内；块对齐取页，顺扫时同一块只拉一次。 */
#define QUEUE_CACHE_WINDOW 256

/* 确保缓存窗口覆盖 position 并把该位置条目拷到 *out。
 * 未命中即按需拉取 position 所在块（Queue.Get）。成功 0，越界/失败 -1。 */
static int remote_queue_cache_at(int position, BackendQueueEntry *out)
{
    if (!g_remote.connected || position < 0 || position >= g_remote.queue_count) {
        return -1;
    }

    /* 队列修订号变了：整块作废，重新分配窗口 */
    if (g_remote.queue_page_revision != g_remote.queue_revision ||
        !g_remote.queue_page_entries) {
        remote_queue_cache_reset();
        g_remote.queue_page_entries =
            malloc(sizeof(BackendQueueEntry) * (size_t)QUEUE_CACHE_WINDOW);
        if (!g_remote.queue_page_entries) {
            return -1;
        }
        g_remote.queue_page_cap = QUEUE_CACHE_WINDOW;
        g_remote.queue_page_base = -1;
        g_remote.queue_page_count = 0;
        g_remote.queue_page_revision = g_remote.queue_revision;
    }

    int block = (position / QUEUE_CACHE_WINDOW) * QUEUE_CACHE_WINDOW;
    int window_end = g_remote.queue_page_base + g_remote.queue_page_count;
    if (g_remote.queue_page_base < 0 ||
        position < g_remote.queue_page_base ||
        position >= window_end) {
        int want = QUEUE_CACHE_WINDOW;
        int remaining = g_remote.queue_count - block;
        if (want > remaining) {
            want = remaining;
        }
        int got = player_remote_queue_page(block, want, g_remote.queue_page_entries,
                                           g_remote.queue_page_cap);
        g_remote.queue_page_base = block;
        g_remote.queue_page_count = got;
    }

    if (g_remote.queue_page_base < 0 ||
        position < g_remote.queue_page_base ||
        position >= g_remote.queue_page_base + g_remote.queue_page_count) {
        return -1;
    }
    if (out) {
        *out = g_remote.queue_page_entries[position - g_remote.queue_page_base];
    }
    return 0;
}

int player_remote_queue_index_at(int position)
{
    /* 与本地后端同一口径：队列位置 → 条目路径 → 反查内容物理下标。
     * 路径来自 Queue.Get 的页缓存（Shuffle/InsertAfter 后仍能映射回正确行）。 */
    BackendQueueEntry entry;
    if (remote_queue_cache_at(position, &entry) != 0 || !entry.path[0]) {
        return -1;
    }
    return playlist_find_track_index_by_path(entry.path);
}

int player_remote_queue_is_active(void) { return g_remote.queue_count > 0 && g_remote.queue_position >= 0; }

int player_remote_queue_play(int position)
{
    DBusMessage *reply = remote_call_i(REMOTE_IFACE_QUEUE, "PlayAt", 3000, position);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    remote_refresh_snapshot();
    return rc == 0 && ok ? 0 : -1;
}

/* 队列内容的下发只有一条路径：player_remote_queue_push()（整表重推）。
 * 单条增删在远端后端里通过“推这一条”实现。 */
static int remote_queue_insert_json(const char *payload, int position)
{
    const char *method = (position >= 0) ? "InsertAfter" : "Append";
    DBusMessage *reply;
    if (position >= 0) {
        reply = remote_call_is(REMOTE_IFACE_QUEUE, method, 3000, position, payload);
    } else {
        reply = remote_call_s(REMOTE_IFACE_QUEUE, method, 3000, payload);
    }
    int written = 0;
    int rc = remote_reply_int(reply, &written);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.queue_revision++;
    return rc == 0 && written > 0 ? 0 : -1;
}

/* 单条曲目的队列载荷由内容侧渲染（这里只负责把它交给核心） */
extern char *playlist_queue_render_entry(int index);

int player_remote_queue_append(int track_index)
{
    char *payload = playlist_queue_render_entry(track_index);
    if (!payload) {
        return -1;
    }
    int rc = remote_queue_insert_json(payload, -1);
    free(payload);
    remote_refresh_snapshot();
    return rc;
}

int player_remote_queue_insert_after(int track_index)
{
    char *payload = playlist_queue_render_entry(track_index);
    if (!payload) {
        return -1;
    }
    int rc = remote_queue_insert_json(payload, g_remote.queue_position);
    free(payload);
    remote_refresh_snapshot();
    return rc;
}

static int remote_queue_int_method(const char *method, int position)
{
    DBusMessage *reply = remote_call_i(REMOTE_IFACE_QUEUE, method, 2000, position);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.queue_revision++;
    remote_refresh_snapshot();
    return rc == 0 && ok ? 0 : -1;
}

int player_remote_queue_remove_at(int position) { return remote_queue_int_method("RemoveAt", position); }
int player_remote_queue_move_up(int position)   { return remote_queue_int_method("MoveUp", position); }
int player_remote_queue_move_down(int position) { return remote_queue_int_method("MoveDown", position); }

int player_remote_queue_clear(void)
{
    int rc = remote_bool_call(REMOTE_IFACE_QUEUE, "Clear");
    remote_queue_cache_reset();
    g_remote.queue_revision++;
    remote_refresh_snapshot();
    return rc;
}

int player_remote_queue_shuffle(void)
{
    int rc = remote_bool_call(REMOTE_IFACE_QUEUE, "Shuffle");
    /* 洗牌只动后端顺序、未必改 queue_count/游标，快照不一定顶修订号，
     * 这里显式让页缓存作废并重取快照。 */
    remote_queue_cache_reset();
    g_remote.queue_revision++;
    remote_refresh_snapshot();
    return rc;
}

/* 分块下发的回调：首块走 Queue.Set（整表替换），其余走 Queue.Append 追加。
 * 任一块失败返回非 0，playlist_queue_foreach_chunk 即中止——核心此时停在
 * 已成功下发的合法前缀上。 */
static int remote_push_chunk_cb(const char *chunk_json, int chunk_index,
                                int entries_in_chunk, void *user)
{
    (void)entries_in_chunk;
    (void)user;
    const char *method = (chunk_index == 0) ? "Set" : "Append";
    DBusMessage *reply = remote_call_s(REMOTE_IFACE_QUEUE, method, 5000, chunk_json);
    int written = 0;
    int rc = remote_reply_int(reply, &written);
    if (reply) {
        dbus_message_unref(reply);
    }
    return (rc == 0 && written >= 0) ? 0 : -1;
}

int player_remote_queue_push(void)
{
    if (!g_remote.connected) {
        return -1;
    }
    remote_queue_cache_reset();

    /* 内容为空：下发空表清空核心队列，避免留着上一次的内容 */
    if (playlist_count() <= 0) {
        DBusMessage *reply = remote_call_s(REMOTE_IFACE_QUEUE, "Set", 5000,
                                           "{\"entries\":[]}");
        int written = 0;
        int rc = remote_reply_int(reply, &written);
        if (reply) {
            dbus_message_unref(reply);
        }
        g_remote.queue_revision++;
        remote_refresh_snapshot();
        return (rc == 0 && written >= 0) ? 0 : -1;
    }

    /* 整表分块下发：首块 Set、余块 Append（单块 ≤ BQ_SET_MAX，规避核心对
     * >500 单发 Queue.Set 返回 InvalidArgs）。返回实际下发条目数或 -1。 */
    int total = playlist_queue_foreach_chunk(remote_push_chunk_cb, NULL);
    g_remote.queue_revision++;
    remote_refresh_snapshot();
    return total;
}

int player_remote_queue_find(const char *path)
{
    if (!path || !path[0]) {
        return -1;
    }

    /* 逐页扫描后端队列（页上限 RPC_PAGE_MAX）。
     * 缓冲约 120 KB：一页 500 行 × 约 240 B 的载荷放得下，同时避免在栈上
     * 开 256 KB（沙箱栈 8 MB，每次栈审计都要求静态帧远小于该值）。 */
    char json[RPC_PAYLOAD_MAX / 2];
    int offset = 0;
    while (offset < g_remote.queue_count) {
        DBusMessage *reply = remote_call_ii(REMOTE_IFACE_QUEUE, "Get", 3000,
                                            offset, RPC_PAGE_MAX);
        if (!reply) {
            return -1;
        }
        int rc = remote_reply_string(reply, json, sizeof(json));
        dbus_message_unref(reply);
        if (rc != 0) {
            return -1;
        }

        JsonReader reader;
        JsonValue rows;
        json_reader_init(&reader, json, strlen(json));
        if (json_get_path(&reader, "rows", &rows) != 0 ||
            json_reader_enter(&reader, &rows) != 0) {
            return -1;
        }

        int in_page = 0;
        JsonValue row;
        while (json_array_next(&reader, &row) == 1) {
            JsonReader row_reader = reader;
            JsonValue value;
            if (json_reader_enter(&row_reader, &row) != 0) {
                continue;
            }
            if (json_get_path(&row_reader, "path", &value) == 0 &&
                value.type == JSON_VALUE_STRING) {
                char row_path[MAX_PATH_LEN];
                json_value_string(&value, row_path, sizeof(row_path));
                if (strcmp(row_path, path) == 0) {
                    long position = 0;
                    if (json_get_path(&row_reader, "position", &value) == 0) {
                        position = json_value_int(&value, 0);
                    }
                    return (int)position;
                }
            }
            in_page++;
        }
        if (in_page == 0) {
            break;
        }
        offset += in_page;
    }
    return -1;
}

int player_remote_queue_page_count(void) { return g_remote.queue_count; }

int player_remote_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap)
{
    if (!out || out_cap <= 0 || count <= 0) {
        return 0;
    }

    /* 单页 JSON 的接收缓冲：与 Queue.Get 的分页上限同量级（见上面的说明） */
    char json[RPC_PAYLOAD_MAX / 2];
    DBusMessage *reply = remote_call_ii(REMOTE_IFACE_QUEUE, "Get", 3000, offset, count);
    if (!reply) {
        return 0;
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return 0;
    }

    JsonReader reader;
    JsonValue rows;
    json_reader_init(&reader, json, strlen(json));
    if (json_get_path(&reader, "rows", &rows) != 0 ||
        json_reader_enter(&reader, &rows) != 0) {
        return 0;
    }

    int written = 0;
    JsonValue row;
    while (written < out_cap && json_array_next(&reader, &row) == 1) {
        JsonReader row_reader = reader;
        JsonValue value;
        if (json_reader_enter(&row_reader, &row) != 0) {
            continue;
        }
        BackendQueueEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (json_get_path(&row_reader, "path", &value) == 0 && value.type == JSON_VALUE_STRING) {
            json_value_string(&value, entry.path, sizeof(entry.path));
        }
        if (json_get_path(&row_reader, "title", &value) == 0 && value.type == JSON_VALUE_STRING) {
            json_value_string(&value, entry.title, sizeof(entry.title));
        }
        if (json_get_path(&row_reader, "artist", &value) == 0 && value.type == JSON_VALUE_STRING) {
            json_value_string(&value, entry.artist, sizeof(entry.artist));
        }
        if (json_get_path(&row_reader, "album", &value) == 0 && value.type == JSON_VALUE_STRING) {
            json_value_string(&value, entry.album, sizeof(entry.album));
        }
        if (json_get_path(&row_reader, "is_cue", &value) == 0) {
            entry.is_cue = json_value_bool(&value, 0) ? 1 : 0;
        }
        if (entry.path[0] == '\0') {
            continue;
        }
        out[written++] = entry;
    }
    return written;
}

/* ── 歌词 / 封面 / 可视化 ───────────────────────────────────────── */

/* 拉取一页歌词文档到 out（绝对下标 offset 起），并带回整篇元信息。
 * @return 本页行数；-1 = 调用或解析失败 */
static int remote_lyrics_fetch_page(int offset, LyricLine *out, int out_cap,
                                    int *out_total, int *out_has_lyrics,
                                    char *out_track_id, size_t out_track_id_size)
{
    /* 单页 200 行 × 每行 ~300 B ≈ 60 KB，与 Queue.Get 的分页同一量级；
     * 与 player_remote_queue_find 一样用接收缓冲，不在栈上开 256 KB。 */
    char json[RPC_PAYLOAD_MAX / 2];
    DBusMessage *reply = remote_call_ii(REMOTE_IFACE_LYRICS, "GetDocument", 3000,
                                        offset, PLAYER_LYRIC_PAGE_MAX);
    if (!reply) {
        return -1;
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }

    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));

    if (out_total && json_get_path(&reader, "total", &value) == 0) {
        *out_total = (int)json_value_int(&value, 0);
    }
    if (out_has_lyrics) {
        *out_has_lyrics = 0;
        if (json_get_path(&reader, "has_lyrics", &value) == 0) {
            *out_has_lyrics = json_value_bool(&value, 0) ? 1 : 0;
        }
    }
    if (out_track_id && out_track_id_size > 0) {
        out_track_id[0] = '\0';
        if (json_get_path(&reader, "track_id", &value) == 0 &&
            value.type == JSON_VALUE_STRING) {
            json_value_string(&value, out_track_id, out_track_id_size);
        }
    }

    JsonReader lines_reader;
    JsonValue lines;
    json_reader_init(&lines_reader, json, strlen(json));
    if (json_get_path(&lines_reader, "lines", &lines) != 0 ||
        json_reader_enter(&lines_reader, &lines) != 0) {
        return 0;
    }

    int in_page = 0;
    JsonValue row;
    while (json_array_next(&lines_reader, &row) == 1) {
        if (offset + in_page >= out_cap) {
            /* 核心的行数上限（MAX_LYRIC_LINES）以内不该发生；超出即丢弃本页余量 */
            break;
        }
        JsonReader row_reader = lines_reader;
        JsonValue field;
        if (json_reader_enter(&row_reader, &row) != 0) {
            continue;
        }

        LyricLine line;
        memset(&line, 0, sizeof(line));
        if (json_get_path(&row_reader, "timestamp", &field) == 0) {
            /* 纯文本歌词的 timestamp 是 null：json_value_double 回落 0.0 */
            line.timestamp = json_value_double(&field, 0.0);
        }
        if (json_get_path(&row_reader, "text", &field) == 0 &&
            field.type == JSON_VALUE_STRING) {
            json_value_string(&field, line.text, sizeof(line.text));
        }
        out[offset + in_page] = line;
        in_page++;
    }
    return in_page;
}

/* 确保整篇歌词文档已在本地缓存里（每页 ≤PLAYER_LYRIC_PAGE_MAX 行，分页拉完）。
 * 界面要“按任意下标取行”，本地后端是内存直读，远端后端等价物就是这份缓存。
 * @return 0 = 可用；-1 = 无法提供（未连接 / 核心没有歌词 / 拉取失败） */
static int remote_lyrics_ensure_loaded(void)
{
    if (!g_remote.connected) {
        return -1;
    }
    /* 核心说没有歌词：不必发调用，也**不缓存“没有”**——load_lyrics() 发生在
     * 播放命令返回之后，缓存住空结果就会永远停在空歌词栏。 */
    if (!g_remote.lyrics.has_lyrics) {
        return -1;
    }
    if (g_remote.lyrics_page_valid &&
        strcmp(g_remote.lyrics_page_track_id, g_remote.track.track_id) == 0) {
        return 0;
    }

    uint64_t now = remote_now_ms();
    if (now < g_remote.lyrics_retry_ms) {
        return -1;      /* 失败退避：核心挂死时不让渲染路径每帧阻塞 */
    }

    /* 页数护栏：核心的 count 不会超过 MAX_LYRIC_LINES，多给一轮余量防呆 */
    const int max_pages = (MAX_LYRIC_LINES / PLAYER_LYRIC_PAGE_MAX) + 2;
    int offset = 0;
    int loaded = 0;
    int total = 0;
    int has = 0;
    char track_id[96] = "";
    char first_track_id[96] = "";

    for (int page = 0; page < max_pages; page++) {
        int in_page = remote_lyrics_fetch_page(offset, g_remote.lyrics_lines,
                                               MAX_LYRIC_LINES, &total, &has,
                                               track_id, sizeof(track_id));
        if (in_page < 0) {
            log_debug("player_remote", "Lyrics.GetDocument failed at offset %d", offset);
            g_remote.lyrics_retry_ms = now + 1000;
            return -1;
        }
        if (offset == 0 && !has) {
            /* 轮询与取文档之间核心换了曲：这次结果不可信，不缓存 */
            g_remote.lyrics_retry_ms = now + 1000;
            return -1;
        }
        /* 多页之间换曲会把两首歌的行拼在一起：宁可这次不给，也不缓存错行 */
        if (page == 0) {
            snprintf(first_track_id, sizeof(first_track_id), "%s", track_id);
        } else if (strcmp(first_track_id, track_id) != 0) {
            log_debug("player_remote", "Track changed while paging lyrics; discarding");
            g_remote.lyrics_retry_ms = now + 1000;
            return -1;
        }

        loaded += in_page;
        offset += in_page;
        if (in_page == 0 || loaded >= total || loaded >= MAX_LYRIC_LINES) {
            break;
        }
    }

    if (loaded <= 0) {
        g_remote.lyrics_retry_ms = now + 1000;
        return -1;
    }

    g_remote.lyrics_total = total;
    g_remote.lyrics_page_count = loaded;
    snprintf(g_remote.lyrics_page_track_id, sizeof(g_remote.lyrics_page_track_id), "%s",
             track_id[0] ? track_id : g_remote.track.track_id);
    g_remote.lyrics_page_valid = 1;
    g_remote.lyrics_retry_ms = 0;

    log_info("player_remote", "Pulled %d lyric lines from the core (source=%s, track=%s)",
             loaded, info_lyrics_source_id(g_remote.lyrics.source),
             g_remote.lyrics_page_track_id);
    return 0;
}

int player_remote_lyrics_document(int offset, int count, PlayerLyricsDoc *out)
{
    if (!out || offset < 0 || count <= 0) {
        return -1;
    }
    if (count > PLAYER_LYRIC_PAGE_MAX) {
        count = PLAYER_LYRIC_PAGE_MAX;      /* 与本地后端一致：夹取而非报错 */
    }
    memset(out, 0, sizeof(*out));

    out->offset = offset;
    out->has_lyrics = g_remote.lyrics.has_lyrics;
    out->has_timestamps = g_remote.lyrics.has_timestamps;
    out->source = g_remote.lyrics.source;
    out->current_index = g_remote.lyrics.has_lyrics ? g_remote.lyrics.current_index : -1;
    out->revision = g_remote.lyrics_revision;
    snprintf(out->track_id, sizeof(out->track_id), "%s", g_remote.track.track_id);

    if (remote_lyrics_ensure_loaded() != 0) {
        return 0;
    }

    out->total = g_remote.lyrics_total;
    int written = 0;
    for (int i = 0; i < count; i++) {
        int index = offset + i;
        if (index >= g_remote.lyrics_page_count) {
            break;
        }
        out->lines[written++] = g_remote.lyrics_lines[index];
    }
    out->count = written;
    return written;
}

int player_remote_lyrics_reload_source(int source)
{
    /* Lyrics.SetSource：核心切换来源并重新加载（与界面的 Ctrl+L → Tab 同一条路） */
    DBusMessage *reply = remote_call_i(REMOTE_IFACE_LYRICS, "SetSource", 3000, source);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.lyrics_page_valid = 0;
    g_remote.lyrics_retry_ms = 0;
    remote_refresh_snapshot();

    /* 歌词来源偏好落库（Task 5.7）：local 由 main.c 注册的 lyrics_set_source_hook
     * 在引擎换源时写内容库；remote 下核心不写库，来源偏好归前端内容，故在 SetSource
     * 成功后由前端对"当前曲目"自行 library_set_lyrics_source（与 local 语义一致）。 */
    if (rc == 0 && ok && g_remote.track.path[0]) {
        library_set_lyrics_source(g_remote.track.path, source);
    }

    return rc == 0 && ok ? 0 : -1;
}

int player_remote_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps)
{
    int has = g_remote.lyrics.has_lyrics;
    if (out_current) *out_current = has ? g_remote.lyrics.current_index : -1;
    if (out_next)    *out_next = has ? g_remote.lyrics.next_index : -1;
    if (out_has_timestamps) *out_has_timestamps = g_remote.lyrics.has_timestamps;
    return has ? 1 : 0;
}

int player_remote_lyrics_highlight_count(void)
{
    /* 与核心 lyrics_tick() 同一口径：同一时间戳的多行一起高亮（最多两行），
     * 纯文本歌词固定停在第 0 行（返回 0，界面会夹取为 1）。 */
    if (remote_lyrics_ensure_loaded() != 0) {
        return 1;
    }
    int index = g_remote.lyrics.current_index;
    if (index < 0 || index >= g_remote.lyrics_page_count) {
        return 1;
    }
    if (!g_remote.lyrics.has_timestamps) {
        return 0;
    }
    if (index + 1 < g_remote.lyrics_page_count &&
        g_remote.lyrics_lines[index + 1].timestamp ==
        g_remote.lyrics_lines[index].timestamp) {
        return 2;
    }
    return 1;
}

int player_remote_lyrics_source(void) { return g_remote.lyrics.source; }

int player_remote_lyrics_total(void)
{
    /* 返回**可服务的**行数：界面按 i < total 逐行取行，保证每个 line_at(i) 都成功 */
    if (remote_lyrics_ensure_loaded() != 0) {
        return 0;
    }
    return g_remote.lyrics_page_count;
}

int player_remote_lyrics_line_at(int index, LyricLine *out)
{
    if (!out || index < 0) {
        return -1;
    }
    if (remote_lyrics_ensure_loaded() != 0 || index >= g_remote.lyrics_page_count) {
        return -1;
    }
    *out = g_remote.lyrics_lines[index];
    return 0;
}

int player_remote_cover_rows(int cols, int rows, int charset, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (g_remote.cover_valid && g_remote.cover_cols == cols &&
        g_remote.cover_rows == rows && g_remote.cover_charset == charset &&
        strcmp(g_remote.cover_track_id, g_remote.track.track_id) == 0) {
        snprintf(out, out_size, "%s", g_remote.cover_text);
        return out[0] ? 1 : 0;
    }

    const char *charset_name = info_cover_charset_id(charset);
    /* GetCoverArt(cols, rows, charset)：字符集是字符串，先落到指针变量 */
    DBusMessage *cover_msg = NULL;
    DBusMessage *reply = NULL;
    if (remote_call_begin(REMOTE_IFACE_INFO, "GetCoverArt", 3000, &cover_msg)) {
        DBusMessageIter iter;
        dbus_message_iter_init_append(cover_msg, &iter);
        dbus_int32_t v_cols = (dbus_int32_t)cols;
        dbus_int32_t v_rows = (dbus_int32_t)rows;
        const char *v_charset = charset_name ? charset_name : "braille";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v_cols);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v_rows);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &v_charset);
        reply = remote_call_finish(cover_msg, 3000);
    }
    if (!reply) {
        return 0;
    }
    char text[INFO_COVER_TEXT_MAX];
    int rc = remote_reply_string(reply, text, sizeof(text));
    dbus_message_unref(reply);
    if (rc != 0 || text[0] == '\0') {
        return 0;
    }

    snprintf(g_remote.cover_text, sizeof(g_remote.cover_text), "%s", text);
    g_remote.cover_cols = cols;
    g_remote.cover_rows = rows;
    g_remote.cover_charset = charset;
    snprintf(g_remote.cover_track_id, sizeof(g_remote.cover_track_id), "%s",
             g_remote.track.track_id);
    g_remote.cover_valid = 1;
    g_remote.cover_revision++;

    snprintf(out, out_size, "%s", g_remote.cover_text);
    return 1;
}

int player_remote_cover_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!g_remote.track.valid) {
        return -1;
    }
    /* 封面文件路径由核心在 Info 快照里给出（art_url → 本地路径） */
    snprintf(out, out_size, "%s", g_remote.track.cover_path);
    return out[0] ? 0 : -1;
}

/* 拉取一帧可视化快照到缓存（Info.GetVisualizer）。JSON 字段：
 * revision / bands / levels[] / peaks[]（见 rpc_info.c 的 GetVisualizer）。 */
static void remote_visualizer_fetch(void)
{
    char json[4096];
    DBusMessage *reply = remote_call0(REMOTE_IFACE_INFO, "GetVisualizer", 1000);
    g_remote.viz_last_fetch_ms = remote_now_ms();
    if (!reply) {
        return;   /* 失败保留上一帧，界面不至于突然归零 */
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return;
    }

    JsonReader reader;
    JsonValue value;
    json_reader_init(&reader, json, strlen(json));
    uint64_t revision = g_remote.viz_revision;
    if (json_get_path(&reader, "revision", &value) == 0) {
        revision = (uint64_t)json_value_int(&value, (long long)revision);
    }

    int new_levels[VISUALIZER_BAND_COUNT] = {0};
    int new_peaks[VISUALIZER_BAND_COUNT] = {0};

    json_reader_init(&reader, json, strlen(json));
    if (json_get_path(&reader, "levels", &value) == 0 && value.type == JSON_VALUE_ARRAY) {
        JsonReader arr = reader;
        JsonValue el;
        int i = 0;
        if (json_reader_enter(&arr, &value) == 0) {
            while (i < VISUALIZER_BAND_COUNT && json_array_next(&arr, &el) == 1) {
                new_levels[i++] = (int)json_value_int(&el, 0);
            }
        }
    }

    json_reader_init(&reader, json, strlen(json));
    if (json_get_path(&reader, "peaks", &value) == 0 && value.type == JSON_VALUE_ARRAY) {
        JsonReader arr = reader;
        JsonValue el;
        int i = 0;
        if (json_reader_enter(&arr, &value) == 0) {
            while (i < VISUALIZER_BAND_COUNT && json_array_next(&arr, &el) == 1) {
                new_peaks[i++] = (int)json_value_int(&el, 0);
            }
        }
    }

    memcpy(g_remote.viz_levels, new_levels, sizeof(new_levels));
    memcpy(g_remote.viz_peaks, new_peaks, sizeof(new_peaks));
    g_remote.viz_revision = revision;
    g_remote.viz_valid = 1;
}

void player_remote_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms)
{
    if (max_levels <= 0) {
        return;
    }

    /* 界面每帧取数：带节流地拉核心（≥RPC_VISUALIZER_INTERVAL_MS 才再发一次），
     * 本期不做帧信号订阅，渲染路径按需同步拉取即可。 */
    if (g_remote.connected && g_remote.viz_active) {
        uint64_t now = remote_now_ms();
        if (!g_remote.viz_valid || (now - g_remote.viz_last_fetch_ms) >= RPC_VISUALIZER_INTERVAL_MS) {
            remote_visualizer_fetch();
        }
    }

    int copy = max_levels < VISUALIZER_BAND_COUNT ? max_levels : VISUALIZER_BAND_COUNT;
    if (levels) {
        if (g_remote.viz_valid) {
            memcpy(levels, g_remote.viz_levels, (size_t)copy * sizeof(int));
        } else {
            memset(levels, 0, (size_t)copy * sizeof(int));
        }
        for (int i = copy; i < max_levels; i++) levels[i] = 0;
    }
    if (peaks) {
        if (g_remote.viz_valid) {
            memcpy(peaks, g_remote.viz_peaks, (size_t)copy * sizeof(int));
        } else {
            memset(peaks, 0, (size_t)copy * sizeof(int));
        }
        for (int i = copy; i < max_levels; i++) peaks[i] = 0;
    }
    if (last_update_ms) {
        *last_update_ms = g_remote.viz_last_fetch_ms ? g_remote.viz_last_fetch_ms
                                                     : remote_now_ms();
    }
}

void player_remote_set_visualizer_active(int active)
{
    /* 界面进入/离开可视化面板时调用：0 时停止拉取并作废缓存。
     * 本期经 Info.GetVisualizer 按需取数，不订阅 VisualizerFrame 信号。 */
    g_remote.viz_active = active ? 1 : 0;
    if (!active) {
        g_remote.viz_valid = 0;
    }
}

/* ── 均衡器（远端：经 Config.Set 写入，核心热应用） ─────────────── */

static int remote_config_patch(const char *patch_json)
{
    DBusMessage *reply = remote_call_s(REMOTE_IFACE_CONFIG, "Set", 2000, patch_json);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.config_revision++;
    return rc == 0 && ok ? 0 : -1;
}

/* 核心是配置的拥有者：把当前镜像记为"已同步"基线 */
static void remote_config_mark_synced(void)
{
    g_remote.synced_config = g_app_config;
    g_remote.config_synced = 1;
}

/* 把前端对镜像的改动换算成最小补丁下发；无变化则不发任何消息 */
int player_remote_config_persist(void)
{
    if (!g_remote.connected) {
        return -1;
    }

    /* 首次下发还没有基线（Task 5.6）：refresh 会用核心配置整体覆盖 g_app_config
     * 镜像，若直接 refresh→mark_synced→return，调用方刚做的改动会在算 diff 之前
     * 被冲掉。这里先记下当前镜像（= 启动镜像 + 本次改动），refresh 建立核心基线后
     * 把该笔改动重新施加回镜像，再按"相对核心基线的差异"下发。
     * 与核心侧"Config.Set 不回卷运行时模式/倍速"协同：diff 若含 mode/speed，
     * 核心按自身运行时值处理，不会被前端旧值卷回（见 rpc_config.c）。 */
    if (!g_remote.config_synced) {
        AppConfig desired = g_app_config;   /* 调用方本次改动后的本地镜像 */

        /* refresh 成功会顺带把 g_app_config 与 synced_config 都置为核心基线 */
        if (player_remote_config_refresh() != 0) {
            /* 取不到核心基线：标记为已同步并保留用户改动，下次再试，不吞改动 */
            remote_config_mark_synced();
            return 0;
        }

        g_app_config = desired;             /* 重新施加本次改动 */
        char patch[4096];
        size_t len = config_diff_json(&g_remote.synced_config, &g_app_config,
                                      patch, sizeof(patch));
        if (len > 2 && remote_config_patch(patch) != 0) {
            log_warn("player_remote", "First config persist rejected: %.200s", patch);
            return -1;
        }
        remote_config_mark_synced();
        return 0;
    }

    char patch[4096];
    size_t len = config_diff_json(&g_remote.synced_config, &g_app_config, patch, sizeof(patch));
    if (len <= 2) {
        return 0;   /* "{}"：没有变化 */
    }
    if (remote_config_patch(patch) != 0) {
        log_warn("player_remote", "Config persist rejected: %.200s", patch);
        return -1;
    }
    remote_config_mark_synced();
    return 0;
}

int player_remote_eq_enabled(void) { return g_app_config.eq_enabled; }

/* 均衡器属于配置（equalizer 分区）：远端模式统一"改镜像 → persist 差异"，
 * 不再手写补丁串——原先的 eq_enabled/eq_preamp/eq_band_gains/eq_preset 键都
 * 不在配置 schema 里，Config.Set 会整包拒绝。 */
void player_remote_eq_set_enabled(int enabled)
{
    g_app_config.eq_enabled = enabled ? 1 : 0;
    player_remote_config_persist();
}

void player_remote_eq_set_band_gain(int band, float gain)
{
    if (band < 0 || band >= EQ_BAND_COUNT) {
        return;
    }
    g_app_config.eq_band_gains[band] = (int)gain;
    player_remote_config_persist();
}

float player_remote_eq_get_band_gain(int band)
{
    if (band < 0 || band >= EQ_BAND_COUNT) {
        return 0.0f;
    }
    return g_app_config.eq_band_gains[band];
}

void player_remote_eq_set_preamp(float preamp)
{
    g_app_config.eq_preamp = (int)preamp;
    player_remote_config_persist();
}

void player_remote_eq_apply_preset(int preset)
{
    const int *gains = eq_preset_gains(preset);
    if (!gains) {
        return;
    }
    for (int b = 0; b < EQ_BAND_COUNT; b++) {
        g_app_config.eq_band_gains[b] = gains[b];
    }
    g_app_config.eq_enabled = 1;
    player_remote_config_persist();
}

/* ── 配置 ───────────────────────────────────────────────────────── */

int player_remote_config_refresh(void)
{
    char json[INFO_JSON_MAX];
    DBusMessage *reply = remote_call0(REMOTE_IFACE_CONFIG, "GetAll", 3000);
    if (!reply) {
        return -1;
    }
    int rc = remote_reply_string(reply, json, sizeof(json));
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }
    /* 回填镜像（GetAll 顶层带 version 键，config_apply_json 容忍它） */
    char reason[256];
    if (config_apply_json(json, reason, sizeof(reason)) != 0) {
        log_warn("player_remote", "Config mirror rejected the core payload: %s", reason);
        return -1;
    }
    remote_config_mark_synced();
    g_remote.config_revision++;
    return 0;
}

int player_remote_config_apply_json(const char *patch_json)
{
    return remote_config_patch(patch_json);
}

int player_remote_config_set_int(const char *key, int value)
{
    char patch[256];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":%d}}", key, value);
    return remote_config_patch(patch);
}

int player_remote_config_set_string(const char *key, const char *value)
{
    char patch[512];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":\"%s\"}}", key,
             value ? value : "");
    return remote_config_patch(patch);
}

int player_remote_config_set_float(const char *key, float value)
{
    char patch[256];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":%.3f}}", key, (double)value);
    return remote_config_patch(patch);
}

int player_remote_config_reload(void)
{
    DBusMessage *reply = remote_call0(REMOTE_IFACE_CONTROL, "ReloadConfig", 3000);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.config_revision++;
    return rc == 0 && ok ? 0 : -1;
}

int player_remote_config_reset(void)
{
    DBusMessage *reply = remote_call0(REMOTE_IFACE_CONFIG, "Reset", 3000);
    int ok = 0;
    int rc = remote_reply_bool(reply, &ok);
    if (reply) {
        dbus_message_unref(reply);
    }
    g_remote.config_revision++;
    return rc == 0 && ok ? 0 : -1;
}

#else /* !HAVE_DBUS */

/* 没有 D-Bus 时远端后端不可用：所有调用都明确失败，界面据此提示。 */

int player_remote_init(PlayerBackend backend, const char *bus_name)
{
    (void)backend;
    (void)bus_name;
    log_error("player_remote_remote", "Built without D-Bus: the remote backend is unavailable");
    return -1;
}

void player_remote_shutdown(void) {}
int player_remote_pump(void) { return -1; }
int player_remote_is_connected(void) { return 0; }
PlayerBackend player_remote_backend(void) { return PLAYER_BACKEND_REMOTE; }
int player_remote_restart_core(void) { return -1; }
int player_remote_is_offline(void) { return 1; }
int player_remote_reconnect_in_ms(void) { return 0; }

uint64_t player_remote_state_revision(void)  { return 0; }
uint64_t player_remote_queue_revision(void)  { return 0; }
uint64_t player_remote_lyrics_revision(void) { return 0; }
uint64_t player_remote_config_revision(void) { return 0; }
uint64_t player_remote_cover_revision(void)  { return 0; }

const InfoTrack    *player_remote_track(void)    { return NULL; }
const InfoLyrics   *player_remote_lyrics(void)   { return NULL; }
const InfoPlayback *player_remote_playback(void){ return NULL; }
const char *player_remote_status_message(void)   { return ""; }

int player_remote_track_index(void) { return -1; }
int player_remote_track_metadata(int index, Track *out) { (void)index; (void)out; return -1; }
int player_remote_position_seconds(void) { return 0; }
int player_remote_duration_seconds(void) { return 0; }
int player_remote_play_state(void) { return PLAY_STATE_STOPPED; }
int player_remote_play_mode(void) { return PLAY_MODE_SEQUENTIAL; }
int player_remote_volume_percent(void) { return 0; }
float player_remote_speed(void) { return 1.0f; }

void player_remote_play(int track_index) { (void)track_index; }
void player_remote_pause(void) {}
void player_remote_resume(void) {}
void player_remote_play_pause(void) {}
void player_remote_stop(void) {}
void player_remote_next(void) {}
void player_remote_prev(void) {}
void player_remote_seek_seconds(int seconds) { (void)seconds; }
void player_remote_set_volume(int percent) { (void)percent; }
void player_remote_adjust_volume(int delta) { (void)delta; }
void player_remote_set_speed(float rate) { (void)rate; }
void player_remote_set_play_mode(PlayMode mode) { (void)mode; }
void player_remote_cycle_play_mode(void) {}
const char *player_remote_play_mode_name(int use_english) { (void)use_english; return ""; }
const char *player_remote_play_mode_name_of(PlayMode mode, int use_english)
{
    (void)mode; (void)use_english; return "";
}
const float *player_remote_speed_steps(int *out_count) { if (out_count) *out_count = 0; return NULL; }
int player_remote_speed_step_count(void) { return 0; }
int player_remote_speed_index(void) { return 0; }
void player_remote_set_speed_index(int index) { (void)index; }

int player_remote_eq_enabled(void) { return 0; }
void player_remote_eq_set_enabled(int enabled) { (void)enabled; }
void player_remote_eq_set_band_gain(int band, float gain) { (void)band; (void)gain; }
float player_remote_eq_get_band_gain(int band) { (void)band; return 0.0f; }
void player_remote_eq_set_preamp(float preamp) { (void)preamp; }
void player_remote_eq_apply_preset(int preset) { (void)preset; }

int player_remote_queue_count(void) { return 0; }
int player_remote_queue_position(void) { return -1; }
int player_remote_queue_index_at(int position) { (void)position; return -1; }
int player_remote_queue_play(int position) { (void)position; return -1; }
int player_remote_queue_append(int track_index) { (void)track_index; return -1; }
int player_remote_queue_insert_after(int track_index) { (void)track_index; return -1; }
int player_remote_queue_remove_at(int position) { (void)position; return -1; }
int player_remote_queue_move_up(int position) { (void)position; return -1; }
int player_remote_queue_move_down(int position) { (void)position; return -1; }
int player_remote_queue_clear(void) { return -1; }
int player_remote_queue_shuffle(void) { return -1; }
int player_remote_queue_is_active(void) { return 0; }
int player_remote_queue_push(void) { return -1; }
int player_remote_queue_find(const char *path) { (void)path; return -1; }
int player_remote_queue_page_count(void) { return 0; }
int player_remote_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap)
{
    (void)offset; (void)count; (void)out; (void)out_cap; return 0;
}

int player_remote_lyrics_document(int offset, int count, PlayerLyricsDoc *out)
{
    (void)offset; (void)count; (void)out; return -1;
}
int player_remote_lyrics_reload_source(int source) { (void)source; return -1; }
int player_remote_lyrics_highlight(int *a, int *b, int *c) { (void)a; (void)b; (void)c; return 0; }
int player_remote_lyrics_highlight_count(void) { return 0; }
int player_remote_lyrics_source(void) { return LYRICS_SOURCE_AUTO; }
int player_remote_lyrics_total(void) { return 0; }
int player_remote_lyrics_line_at(int index, LyricLine *out) { (void)index; (void)out; return -1; }

int player_remote_cover_rows(int cols, int rows, int charset, char *out, size_t out_size)
{
    (void)cols; (void)rows; (void)charset; (void)out; (void)out_size; return 0;
}
int player_remote_cover_path(char *out, size_t out_size) { (void)out; (void)out_size; return -1; }
void player_remote_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last)
{
    (void)levels; (void)peaks; (void)max_levels; (void)last;
}
void player_remote_set_visualizer_active(int active) { (void)active; }

int player_remote_config_refresh(void) { return -1; }
int player_remote_config_apply_json(const char *patch_json) { (void)patch_json; return -1; }
int player_remote_config_set_int(const char *key, int value) { (void)key; (void)value; return -1; }
int player_remote_config_set_string(const char *key, const char *value)
{
    (void)key; (void)value; return -1;
}
int player_remote_config_set_float(const char *key, float value) { (void)key; (void)value; return -1; }
int player_remote_config_reload(void) { return -1; }
int player_remote_config_reset(void) { return -1; }

#endif /* HAVE_DBUS */
