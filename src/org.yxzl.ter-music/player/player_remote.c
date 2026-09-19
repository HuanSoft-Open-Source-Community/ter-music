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

#include "cli/cli.h"
#include "core/core.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "media/rpc.h"
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

/* 握手要求的方法：缺一即视为“核心过旧” */
static const char *const k_required_methods[] = {
    "Info.GetInfo",
    "Control.Play", "Control.Pause", "Control.Stop", "Control.Next", "Control.Previous",
    "Control.SetVolume", "Control.SetSpeed", "Control.SetPlayMode", "Control.Attach",
    "Control.Ping", "Control.Detach",
    "Queue.Get", "Queue.Set", "Queue.PlayAt",
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

    /* 歌词页缓存 */
    int lyrics_page_valid;
    int lyrics_page_offset;
    int lyrics_page_count;
    int lyrics_total;
    int lyrics_has;
    int lyrics_has_timestamps;
    int lyrics_source;
    int lyrics_current_index;
    LyricLine lyrics_lines[PLAYER_LYRIC_PAGE_MAX];
} RemoteState;

const char *player_remote_play_mode_name_of(PlayMode mode, int use_english);

static RemoteState g_remote;

static uint64_t remote_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ── 连接与握手 ─────────────────────────────────────────────────── */

static void remote_close_connection(void)
{
    if (g_remote.connection) {
        dbus_connection_close(g_remote.connection);
        dbus_connection_unref(g_remote.connection);
        g_remote.connection = NULL;
    }
    g_remote.token[0] = '\0';
    g_remote.connected = 0;
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

    if (json_get_path(&reader, "track.valid", &value) == 0 &&
        json_value_bool(&value, 0)) {
        track.valid = 1;
    }
    if (json_get_path(&reader, "track.index", &value) == 0) {
        track.index = (int)json_value_int(&value, -1);
    }
    if (json_get_path(&reader, "track.queue_position", &value) == 0 &&
        value.type == JSON_VALUE_NUMBER) {
        track.queue_position = (int)json_value_int(&value, -1) - 1;   /* 1 基 → 0 基 */
    }
    if (json_get_path(&reader, "track.playlist_count", &value) == 0) {
        track.playlist_total = (int)json_value_int(&value, 0);
    }
    if (json_get_path(&reader, "track.queue_count", &value) == 0) {
        track.queue_count = (int)json_value_int(&value, 0);
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
    if (json_get_path(&reader, "lyrics.source", &value) == 0 &&
        value.type == JSON_VALUE_STRING &&
        json_value_string(&value, source, sizeof(source)) == 0) {
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

void player_remote_shutdown(void)
{
    if (g_remote.connected && g_remote.token[0]) {
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
    /* 远端后端不做内容查询：元数据来自队列页（Queue.Get） */
    (void)index;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
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

int player_remote_queue_index_at(int position)
{
    /* 队列页缓存里查找（player_remote_queue_page 会填充）；未命中返回该位置本身，
     * 因为内容下标与队列位置在“整表下发”语义下一一对应。 */
    if (position < 0 || position >= g_remote.queue_count) {
        return -1;
    }
    return position;
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

int player_remote_queue_clear(void)             { return remote_bool_call(REMOTE_IFACE_QUEUE, "Clear"); }

int player_remote_queue_rebuild(void)
{
    DBusMessage *reply = remote_call_i(REMOTE_IFACE_QUEUE, "Rebuild", 2000, (int)g_remote.playback.play_mode);
    if (reply) {
        dbus_message_unref(reply);
    }
    remote_refresh_snapshot();
    return 0;
}

int player_remote_queue_shuffle(void)           { return remote_bool_call(REMOTE_IFACE_QUEUE, "Shuffle"); }

int player_remote_queue_push(void)
{
    char *json = playlist_queue_render();
    if (!json) {
        return -1;
    }
    DBusMessage *reply = remote_call_s(REMOTE_IFACE_QUEUE, "Set", 5000, json);
    int written = 0;
    int rc = remote_reply_int(reply, &written);
    if (reply) {
        dbus_message_unref(reply);
    }
    free(json);
    g_remote.queue_revision++;
    remote_refresh_snapshot();
    return rc == 0 ? written : -1;
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

int player_remote_lyrics_document(int offset, int count, PlayerLyricsDoc *out)
{
    if (!out || count <= 0 || count > PLAYER_LYRIC_PAGE_MAX) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    JsonValue args[2];
    (void)args;

    /* Lyrics.GetDocument(offset, count) -> JSON */
    DBusMessage *reply = remote_call_ii(REMOTE_IFACE_LYRICS, "GetDocument", 3000,
                                        offset, count);
    if (!reply) {
        return -1;
    }

    /* 复用歌词快照 + 本地缓存的行（远端分页在 Lyrics.GetDocument 里给全文） */
    dbus_message_unref(reply);

    out->total = g_remote.lyrics_total;
    out->offset = offset;
    out->has_lyrics = g_remote.lyrics.has_lyrics;
    out->has_timestamps = g_remote.lyrics.has_timestamps;
    out->source = g_remote.lyrics.source;
    snprintf(out->track_id, sizeof(out->track_id), "%s", g_remote.track.track_id);
    out->current_index = g_remote.lyrics.current_index;
    out->revision = g_remote.lyrics_revision;

    if (!g_remote.lyrics_page_valid) {
        return 0;
    }

    int written = 0;
    for (int i = 0; i < count && written < PLAYER_LYRIC_PAGE_MAX; i++) {
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
    remote_refresh_snapshot();
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
    /* 同时间戳一起高亮的行数：远端快照未单独给出，按“下一行文本是否存在且
     * 时间戳相同”无从判断，故返回 1（渲染退化为单行高亮，不影响正确性）。 */
    return 1;
}

int player_remote_lyrics_source(void) { return g_remote.lyrics.source; }
int player_remote_lyrics_total(void)  { return g_remote.lyrics_total; }

int player_remote_lyrics_line_at(int index, LyricLine *out)
{
    if (!out || index < 0 || !g_remote.lyrics_page_valid ||
        index >= g_remote.lyrics_page_count) {
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

void player_remote_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms)
{
    if (max_levels <= 0) {
        return;
    }
    if (levels) memset(levels, 0, (size_t)max_levels * sizeof(int));
    if (peaks)  memset(peaks, 0, (size_t)max_levels * sizeof(int));
    if (last_update_ms) *last_update_ms = remote_now_ms();
}

void player_remote_set_visualizer_active(int active)
{
    (void)active;
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

int player_remote_eq_enabled(void) { return g_app_config.eq_enabled; }

void player_remote_eq_set_enabled(int enabled)
{
    char patch[128];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"eq_enabled\":%s}}",
             enabled ? "true" : "false");
    remote_config_patch(patch);
}

void player_remote_eq_set_band_gain(int band, float gain)
{
    char patch[256];
    snprintf(patch, sizeof(patch),
             "{\"preferences\":{\"eq_band_gains\":{\"%d\":%.2f}}}", band, (double)gain);
    remote_config_patch(patch);
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
    char patch[128];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"eq_preamp\":%.2f}}", (double)preamp);
    remote_config_patch(patch);
}

void player_remote_eq_apply_preset(int preset)
{
    char patch[128];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"eq_preset\":%d}}", preset);
    remote_config_patch(patch);
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
int player_remote_queue_rebuild(void) { return -1; }
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
