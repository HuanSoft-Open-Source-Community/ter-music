/**
 * @file remote_view.c
 * @brief 设置页的“远程设备”页（前端功能：服务器管理 + 浏览 + 下载播放）
 *
 * 远程音乐源由**前端**负责（核心只播放本地文件，见 scripts/test/
 * check-core-purity.sh）：
 *   - 服务器列表与密码存在前端自有的 <configdir>/remote.xml（remote_store）；
 *   - 目录浏览与曲目下载由后台线程完成（remote_cache），不阻塞 UI；
 *   - 每下载完成一首，经 player 门面把**本地缓存路径**交给核心播放。
 *
 * t4 体验层：
 *   - 进度/失败/结束摘要每帧渲染，主界面状态栏也会同步进度；
 *   - 列目录失败进入错误态（含本地化原因与“重试列目录”），不再永久“加载中”；
 *   - 进行中的会话可取消，失败的曲目可重试（缓存命中使重试成本很低）；
 *   - 删除连接/清理缓存需要二次确认；保存/读取 remote.xml 失败会明确提示；
 *   - 表单支持协议枚举、端口校验、TLS/自签名开关、SFTP known_hosts/口令、
 *     SMB 共享名+子路径语义；非法输入不能保存；
 *   - 缓存用量/清理/上限在设置页可见可用，清理保护播放中/队列/收藏/历史。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/remote_view.h"

#include <ncursesw/ncurses.h>

#include "app/open.h"
#include "i18n/i18n.h"
#include "library/library.h"
#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"
#include "logger/logger.h"
#include "player/player.h"
#include "remote/remote.h"
#include "remote/remote_cache.h"
#include "remote/remote_store.h"
#include "ui/dialog.h"
#include "ui/keymap.h"
#include "ui/layout.h"
#include "ui/menu_internal.h"
#include "ui/menus.h"
#include "ui/ui.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * Remote device UI state
 * ============================================================ */

static int g_remote_mode           = 0;   // 0=list, 1=actions, 2=form, 3=browse
static int g_remote_selected       = 0;
static int g_remote_selected_conn  = -1;
static RemoteDirEntry *g_remote_entries = NULL;
static int g_remote_entry_count    = 0;
static int g_remote_entry_offset   = 0;
static char g_remote_current_path[1024] = "";
static RemoteConnectionConfig g_remote_form_config;
static int g_remote_form_editing_idx = -1;

/* 后台任务 / 会话状态快照（UI 自己的镜像，避免每帧调用缓存 API） */
static int g_remote_check_pending = 0;   /* 本次列目录是“测试连接”而非“浏览目录” */
static int g_remote_download_seen = 0;   /* 本次 UI 会话里见过“下载中”状态 */
static int g_remote_session_pending = 0; /* 本进程发起、尚未结算的下载会话 */
/* 终结状态只结算一次；新会话（LISTING/DOWNLOADING）会重新武装。用于
 * “会话在首帧前就跑完、没采样到 DOWNLOADING”的快速会话（t5 V-02）。 */
static int g_remote_terminal_handled = 0;
static int g_remote_content_started = 0;
static int g_remote_listing_failed = 0;
static char g_remote_last_error[256] = "";
static int g_remote_load_failed = 0;     /* remote.xml 读取失败（损坏/无权限） */
static int g_remote_last_session_failed = 0;
static int g_remote_last_session_conn = -1;
static char g_remote_last_session_path[sizeof(g_remote_current_path)] = "";
static char g_remote_last_summary[256] = "";
static int g_remote_summary_valid = 0;
static int g_remote_last_reported_done = -1;
static uint64_t g_remote_last_progress_ms = 0;
static long long g_remote_cache_usage = -1;
static uint64_t g_remote_cache_usage_ms = 0;

/* 主视图的远程状态行（t5 V-03）：会话在后台进行、加载后自动回主视图时，
 * 状态栏要能看到进度/失败/摘要，而不是只闪一下或完全不画。 */
static char g_remote_main_status_text[400] = "";
static uint64_t g_remote_main_status_until = 0;
static uint64_t g_remote_main_status_paint_ms = 0;

/* 二次确认（删除连接 / 清理缓存） */
typedef enum {
    REMOTE_CONFIRM_NONE = 0,
    REMOTE_CONFIRM_DELETE,
    REMOTE_CONFIRM_CLEAN_CACHE
} RemoteConfirmKind;
static RemoteConfirmKind g_remote_confirm = REMOTE_CONFIRM_NONE;
static int g_remote_confirm_conn = -1;

/* Forward declarations for remote helpers */
void remote_view_render(void);
int remote_view_handle_input(int ch);
void remote_enter_list_mode(void);  /* non-static: declared in menu_internal.h */
static void remote_start_add(void);
static void remote_start_edit(int conn_idx);
static void remote_delete_connection(int conn_idx);
static void remote_start_browse(int conn_idx);
static void remote_refresh_entries(void);

/* ============================================================
 * 小工具：时间 / 错误分类 / 本地化
 * ============================================================ */

static uint64_t remote_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef enum {
    REMOTE_ERR_NONE = 0,
    REMOTE_ERR_AUTH,
    REMOTE_ERR_CONNECT,
    REMOTE_ERR_NOT_FOUND,
    REMOTE_ERR_TIMEOUT,
    REMOTE_ERR_TLS,
    REMOTE_ERR_PROTOCOL,
    REMOTE_ERR_SMBCLIENT,
    REMOTE_ERR_SMB_PATH,
    REMOTE_ERR_KNOWN_HOSTS,
    REMOTE_ERR_TOO_LARGE,
    REMOTE_ERR_NO_AUDIO,
    REMOTE_ERR_CACHE_LIMIT,
    REMOTE_ERR_PARSE,
    REMOTE_ERR_CANCELLED,
    REMOTE_ERR_UNKNOWN
} RemoteErrorKind;

/* remote 引擎的英文错误（t3）→ 用户可读分类。分类只看关键字，绝不把
 * 英文原文当作唯一信息（审计 F-21）。 */
static RemoteErrorKind remote_error_kind(const char *raw)
{
    if (!raw || !raw[0]) return REMOTE_ERR_NONE;
    if (strstr(raw, "smbclient")) return REMOTE_ERR_SMBCLIENT;
    if (strstr(raw, "SMB path") || strstr(raw, "passed safely") ||
        strstr(raw, "unsafe for smbclient")) {
        return REMOTE_ERR_SMB_PATH;
    }
    if (strstr(raw, "known_hosts") || strstr(raw, "host key") ||
        strstr(raw, "hostkey")) {
        return REMOTE_ERR_KNOWN_HOSTS;
    }
    if (strstr(raw, "Authentication failed") || strstr(raw, "HTTP 401") ||
        strstr(raw, "LOGON_FAILURE") || strstr(raw, "Login denied")) {
        return REMOTE_ERR_AUTH;
    }
    if (strstr(raw, "TLS certificate") || strstr(raw, "SSL") ||
        strstr(raw, "peer certificate")) {
        return REMOTE_ERR_TLS;
    }
    if (strstr(raw, "Cannot connect") || strstr(raw, "resolve host") ||
        strstr(raw, "Couldn't connect")) {
        return REMOTE_ERR_CONNECT;
    }
    if (strstr(raw, "timed out") || strstr(raw, "timeout")) return REMOTE_ERR_TIMEOUT;
    if (strstr(raw, "Unsupported") || strstr(raw, "not supported") ||
        strstr(raw, "bad/illegal format")) {
        return REMOTE_ERR_PROTOCOL;
    }
    if (strstr(raw, "not found") || strstr(raw, "Not found") ||
        strstr(raw, "HTTP 404") || strstr(raw, "No such file")) {
        return REMOTE_ERR_NOT_FOUND;
    }
    if (strstr(raw, "too large") || strstr(raw, "too big") ||
        strstr(raw, "FILESIZE")) {
        return REMOTE_ERR_TOO_LARGE;
    }
    if (strstr(raw, "no audio files") || strstr(raw, "no track")) return REMOTE_ERR_NO_AUDIO;
    if (strstr(raw, "cache limit")) return REMOTE_ERR_CACHE_LIMIT;
    if (strstr(raw, "parse") || strstr(raw, "Parse")) return REMOTE_ERR_PARSE;
    if (strstr(raw, "cancelled") || strstr(raw, "Cancelled")) return REMOTE_ERR_CANCELLED;
    return REMOTE_ERR_UNKNOWN;
}

static const char *remote_error_i18n_key(RemoteErrorKind kind)
{
    switch (kind) {
        case REMOTE_ERR_AUTH:        return "remote.err.auth";
        case REMOTE_ERR_CONNECT:     return "remote.err.connect";
        case REMOTE_ERR_NOT_FOUND:   return "remote.err.not_found";
        case REMOTE_ERR_TIMEOUT:     return "remote.err.timeout";
        case REMOTE_ERR_TLS:         return "remote.err.tls";
        case REMOTE_ERR_PROTOCOL:    return "remote.err.protocol";
        case REMOTE_ERR_SMBCLIENT:   return "remote.err.smbclient";
        case REMOTE_ERR_SMB_PATH:    return "remote.err.smb_path";
        case REMOTE_ERR_KNOWN_HOSTS: return "remote.err.known_hosts";
        case REMOTE_ERR_TOO_LARGE:   return "remote.err.too_large";
        case REMOTE_ERR_NO_AUDIO:    return "remote.err.no_audio";
        case REMOTE_ERR_CACHE_LIMIT: return "remote.err.cache_limit";
        case REMOTE_ERR_PARSE:       return "remote.err.parse";
        case REMOTE_ERR_CANCELLED:   return "remote.err.cancelled";
        case REMOTE_ERR_UNKNOWN:
        default:                     return "remote.err.unknown";
    }
}

/* 本地化错误文案；未知错误附带英文详情，保证“英文原文不是唯一信息”。 */
static void remote_localized_error(const char *raw, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    RemoteErrorKind kind = remote_error_kind(raw);
    if (kind == REMOTE_ERR_NONE) {
        out[0] = '\0';
        return;
    }
    const char *text = i18n_get(remote_error_i18n_key(kind));
    if (kind == REMOTE_ERR_UNKNOWN && raw && raw[0]) {
        snprintf(out, out_size, "%s %s", text, raw);
    } else {
        snprintf(out, out_size, "%s", text);
    }
}

static void remote_remember_error(const char *raw)
{
    snprintf(g_remote_last_error, sizeof(g_remote_last_error), "%s", raw ? raw : "");
}

/* ============================================================
 * 后台任务状态 / 进度文案
 * ============================================================ */

static int remote_job_busy(const RemoteCacheStatus *status)
{
    return status && (status->state == REMOTE_CACHE_LISTING ||
                      status->state == REMOTE_CACHE_DOWNLOADING);
}

/* “下载中 3/5，失败 1”之类的可判定进度（F-21/F-25）。 */
static void remote_format_status(const RemoteCacheStatus *status, char *buf, size_t size)
{
    if (!status || !buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    switch (status->state) {
        case REMOTE_CACHE_LISTING:
            snprintf(buf, size, "%s", i18n_get("remote.status.listing"));
            break;
        case REMOTE_CACHE_DOWNLOADING:
            snprintf(buf, size, i18n_get("remote.status.downloading"),
                     status->done, status->total, status->failed);
            break;
        case REMOTE_CACHE_DONE:
            if (status->truncated) {
                /* t5 V-02：截断必须显式可见（仅加载 N/M），不能只写 summary。 */
                snprintf(buf, size, i18n_get("remote.truncated"),
                         status->done, status->total);
            } else if (status->total == 0 && !g_remote_summary_valid) {
                snprintf(buf, size, "%s", i18n_get("remote.status.listing_done"));
            } else {
                snprintf(buf, size, i18n_get("remote.status.done"),
                         status->done, status->total, status->failed);
            }
            break;
        case REMOTE_CACHE_FAILED: {
            char localized[320];
            remote_localized_error(g_remote_last_error[0] ? g_remote_last_error
                                                          : status->message,
                                   localized, sizeof(localized));
            snprintf(buf, size, "%s", localized[0] ? localized
                                                  : i18n_get("remote.status.failed"));
            break;
        }
        case REMOTE_CACHE_CANCELLED:
            snprintf(buf, size, "%s", i18n_get("remote.status.cancelled"));
            break;
        default:
            if (g_remote_summary_valid) {
                snprintf(buf, size, "%s", g_remote_last_summary);
            }
            break;
    }
}

/* 主界面也同步下载进度（每 700ms 或 done 变化时刷新状态栏）。 */
static void remote_sync_progress_message(const RemoteCacheStatus *status)
{
    if (!status || status->state != REMOTE_CACHE_DOWNLOADING) {
        return;
    }
    uint64_t now = remote_now_ms();
    if (status->done == g_remote_last_reported_done &&
        now - g_remote_last_progress_ms < 700) {
        return;
    }
    g_remote_last_reported_done = status->done;
    g_remote_last_progress_ms = now;
    char text[160];
    snprintf(text, sizeof(text), i18n_get("remote.status.downloading"),
             status->done, status->total, status->failed);
    show_status_message(text);
}

/* 记住一条主视图可见的远程状态，保持 hold_ms 毫秒（t5 V-03）。 */
static void remote_set_main_status(const char *text, uint64_t hold_ms)
{
    if (!text || !text[0]) {
        return;
    }
    snprintf(g_remote_main_status_text, sizeof(g_remote_main_status_text), "%s", text);
    g_remote_main_status_until = remote_now_ms() + hold_ms;
}

/* 主视图状态行：任务进行中周期性刷新进度；结束后保留摘要若干秒。
 * 不覆盖其它模块刚设置的 3 秒内新消息。 */
static void remote_paint_main_status(void)
{
    if (!g_remote_main_status_text[0]) {
        return;
    }
    uint64_t now = remote_now_ms();
    if (now > g_remote_main_status_until) {
        g_remote_main_status_text[0] = '\0';
        return;
    }

    RemoteCacheStatus status;
    remote_cache_get_status(&status);
    uint64_t interval = remote_job_busy(&status) ? 400u : 1000u;
    if (now - g_remote_main_status_paint_ms < interval) {
        return;
    }
    g_remote_main_status_paint_ms = now;

    const char *current = get_status_message();
    if (current[0] && strcmp(current, g_remote_main_status_text) != 0 &&
        time(NULL) - get_status_message_time() < 3) {
        return;   /* 用户/其它模块的新消息优先 */
    }
    show_status_message(g_remote_main_status_text);
    if (g_current_view == VIEW_MAIN) {
        repaint_status_message();
        refresh();
    }
}

/* ============================================================
 * Remote device — navigation / lifecycle
 * ============================================================ */

void remote_enter_list_mode(void)
{
    g_remote_mode = 0;
    g_remote_selected = 0;
    g_remote_selected_conn = -1;
    g_remote_confirm = REMOTE_CONFIRM_NONE;
    g_remote_confirm_conn = -1;
    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;
    g_remote_current_path[0] = '\0';
}

static void remote_go_back(void)
{
    switch (g_remote_mode) {
        case 0:
            g_focus_area = FOCUS_SIDEBAR;
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            break;
        case 1:
            g_remote_confirm = REMOTE_CONFIRM_NONE;
            g_remote_confirm_conn = -1;
            g_remote_mode = 0;
            g_remote_selected = 0;
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            break;
        case 3:
            if (g_remote_current_path[0] && strcmp(g_remote_current_path, "/") != 0) {
                char *last_slash = strrchr(g_remote_current_path, '/');
                if (last_slash && last_slash != g_remote_current_path) {
                    *last_slash = '\0';
                } else if (last_slash == g_remote_current_path) {
                    g_remote_current_path[1] = '\0';
                } else {
                    g_remote_current_path[0] = '\0';
                }
                g_remote_selected = 0;
                remote_refresh_entries();
            } else {
                g_remote_mode = 1;
                g_remote_selected = 0;
                if (g_remote_entries) {
                    remote_free_entries(g_remote_entries, g_remote_entry_count);
                    g_remote_entries = NULL;
                }
                g_remote_entry_count = 0;
                g_remote_listing_failed = 0;
                render_settings_content();
            }
            break;
        default:
            break;
    }
}

static void rerender_remote_view(void)
{
    render_menu_frame(i18n_get("menu.settings"));
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

/* 列目录改走后台线程：结果在 remote_view_tick() 里取回，UI 不阻塞 */
static void remote_refresh_entries(void)
{
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= remote_store_count()) {
        return;
    }
    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;
    g_remote_check_pending = 0;
    g_remote_listing_failed = 0;
    g_remote_last_error[0] = '\0';
    g_remote_summary_valid = 0;

    const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);
    if (remote_cache_start_listing(conn, g_remote_current_path) != 0) {
        snprintf(g_remote_last_summary, sizeof(g_remote_last_summary),
                 "%s", i18n_get("remote.err.busy"));
        g_remote_summary_valid = 1;
        show_status_message(g_remote_last_summary);
        return;
    }
    show_status_message(i18n_get("remote.status.listing"));
    refresh();
}

/* 远程设备页当前是否可见（设置页 + 侧栏“远程设备”项） */
static int remote_view_is_active(void)
{
    return g_current_view == VIEW_SETTINGS && g_menu_selected_idx == SETTINGS_PAGE_REMOTE;
}

int remote_view_form_active(void)
{
    return g_remote_mode == 2;
}

/* UI 线程取回列目录结果（后台线程不碰 UI） */
static void remote_drain_listing(void)
{
    RemoteDirEntry *entries = NULL;
    int count = 0;
    int is_error = 0;
    if (!remote_cache_take_listing(&entries, &count, &is_error)) {
        return;
    }

    int was_check = g_remote_check_pending;
    g_remote_check_pending = 0;
    RemoteCacheStatus cache_status;
    remote_cache_get_status(&cache_status);

    if (is_error) {
        remote_remember_error(cache_status.message);
        g_remote_listing_failed = 1;
        char localized[320];
        remote_localized_error(cache_status.message, localized, sizeof(localized));
        snprintf(g_remote_last_summary, sizeof(g_remote_last_summary), "%s",
                 localized[0] ? localized : i18n_get("remote.list_failed"));
        g_remote_summary_valid = 1;
        if (was_check) {
            char buf[420];
            snprintf(buf, sizeof(buf), "%s%s",
                     i18n_get("remote.refresh_failed"),
                     localized[0] ? localized : "");
            show_status_message(buf);
        } else {
            show_status_message(g_remote_last_summary);
        }
        if (remote_view_is_active()) {
            render_settings_content();
        }
        return;
    }

    g_remote_listing_failed = 0;
    g_remote_last_error[0] = '\0';
    g_remote_summary_valid = 0;

    if (was_check) {
        if (entries) {
            remote_free_entries(entries, count);
        }
        char buf[200];
        snprintf(buf, sizeof(buf), "%s %d %s",
                 i18n_get("remote.connection_ok"), count, i18n_get("remote.entries"));
        show_status_message(buf);
        if (remote_view_is_active()) {
            render_settings_content();
        }
        return;
    }

    g_remote_entries = entries;
    g_remote_entry_count = count;
    g_remote_entry_offset = 0;
    if (remote_view_is_active()) {
        rerender_remote_view();
    }
}

/* ── 缓存淘汰的保护集 ──────────────────────────────────────────────
 * 正在播放的曲目、核心播放列表里的曲目、收藏与历史引用的文件都不能被淘汰，
 * 否则用户会看到"列表里有、一播就失败"的条目。上游（核心）是这些状态的
 * 拥有者，故这里经门面把它们收集起来，再交给 remote_cache_evict()。
 *
 * 动态数组：不再有 256 条硬上限（审计 F-19），队列有多长就保护多长。 */
typedef struct {
    char **paths;
    char **storage;
    int count;
    int capacity;
} RemoteProtectSet;

static void protect_add(RemoteProtectSet *set, const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    if (set->count >= set->capacity) {
        int next = set->capacity == 0 ? 128 : set->capacity * 2;
        char **grown_paths = realloc(set->paths, sizeof(char *) * (size_t)next);
        if (!grown_paths) {
            return;
        }
        set->paths = grown_paths;
        char **grown_storage = realloc(set->storage, sizeof(char *) * (size_t)next);
        if (!grown_storage) {
            return;
        }
        set->storage = grown_storage;
        set->capacity = next;
    }
    char *copy = strdup(path);
    if (!copy) {
        return;
    }
    set->storage[set->count] = copy;
    set->paths[set->count] = copy;
    set->count++;
}

static void protect_free(RemoteProtectSet *set)
{
    for (int i = 0; i < set->count; i++) {
        free(set->storage[i]);
    }
    free(set->paths);
    free(set->storage);
    memset(set, 0, sizeof(*set));
}

static void remote_collect_protected(RemoteProtectSet *set)
{
    const InfoTrack *track = player_track();
    if (track && track->valid && track->path[0]) {
        protect_add(set, track->path);
    }

    /* 收藏与历史是前端自有内容（library/），直接读内容库 */
    Track favorites[64];
    int favorites_count = library_favorites_get_all(favorites, 64);
    for (int i = 0; i < favorites_count; i++) {
        protect_add(set, favorites[i].path);
    }

    HistoryEntry history[64];
    int history_count = library_history_get_all(history, 64);
    for (int i = 0; i < history_count; i++) {
        protect_add(set, history[i].path);
    }

    /* 播放列表可能很长：只登记缓存里的路径（远程缓存根目录下的文件）。
     * remote 下元数据来自核心队列页（player_track_metadata 以队列位置为入参），
     * 故按队列长度遍历，拿到的正是核心当前持有的那批路径。 */
    const char *cache_root = remote_cache_dir();
    int total = player_queue_page_count();
    for (int i = 0; i < total; i++) {
        Track item;
        memset(&item, 0, sizeof(item));
        if (player_track_metadata(i, &item) != 0 || !item.path[0]) {
            continue;
        }
        if (cache_root && strncmp(item.path, cache_root, strlen(cache_root)) != 0) {
            continue;
        }
        protect_add(set, item.path);
    }
}

static void remote_evict_protected(void)
{
    RemoteProtectSet set;
    memset(&set, 0, sizeof(set));
    remote_collect_protected(&set);
    remote_cache_evict((const char *const *)set.paths, set.count);
    protect_free(&set);
}

/* ── 缓存用量 / 清理 / 上限（F-14 设置页入口） ───────────────────── */

static long long remote_cache_usage_cached(void)
{
    uint64_t now = remote_now_ms();
    if (g_remote_cache_usage < 0 || now - g_remote_cache_usage_ms > 3000) {
        g_remote_cache_usage = remote_cache_usage_bytes();
        g_remote_cache_usage_ms = now;
    }
    return g_remote_cache_usage;
}

static void remote_cache_usage_invalidate(void)
{
    g_remote_cache_usage = -1;
    g_remote_cache_usage_ms = 0;
}

static void remote_format_cache_usage(char *buf, size_t size)
{
    long long used_mb = remote_cache_usage_cached() / (1024LL * 1024LL);
    int limit_mb = remote_store_cache_limit_mb();
    if (limit_mb > 0) {
        char limit[48];
        snprintf(limit, sizeof(limit), "%d MB", limit_mb);
        snprintf(buf, size, i18n_get("remote.cache_usage"), used_mb, limit);
    } else {
        snprintf(buf, size, i18n_get("remote.cache_usage"), used_mb,
                 i18n_get("remote.cache_unlimited"));
    }
}

static void remote_do_clean_cache(void)
{
    RemoteProtectSet set;
    memset(&set, 0, sizeof(set));
    remote_collect_protected(&set);
    int deleted = remote_cache_purge((const char *const *)set.paths, set.count);
    protect_free(&set);

    remote_cache_usage_invalidate();
    char message[160];
    snprintf(message, sizeof(message), i18n_get("remote.cache_cleaned"), deleted);
    show_status_message(message);
    g_remote_confirm = REMOTE_CONFIRM_NONE;
    g_remote_confirm_conn = -1;
}

static void remote_prompt_cache_limit(void)
{
    int max_x = getmaxx(stdscr);
    int content_start_x = (max_x / 4) + 2;
    char buf[16];
    int limit = remote_store_cache_limit_mb();
    if (limit > 0) {
        snprintf(buf, sizeof(buf), "%d", limit);
    } else {
        buf[0] = '\0';
    }

    curs_set(1);
    noecho();
    prompt_text_input(stdscr, 4, content_start_x,
                      i18n_get("remote.cache_limit_prompt"), buf, sizeof(buf), 1, 0, 1);
    curs_set(0);
    noecho();

    if (!buf[0]) {
        return;
    }
    char *end = NULL;
    long value = strtol(buf, &end, 10);
    if (end == buf || (end && *end != '\0') || value < 0 || value > 1024 * 1024) {
        show_status_message(i18n_get("remote.cache_limit_invalid"));
        rerender_remote_view();
        return;
    }

    remote_store_set_cache_limit_mb((int)value);
    if (remote_store_save() != 0) {
        show_status_message(i18n_get("remote.save_failed"));
        rerender_remote_view();
        return;
    }
    /* 上限变更立即生效：按新上限淘汰未保护文件。 */
    remote_evict_protected();
    remote_cache_usage_invalidate();
    char message[160];
    snprintf(message, sizeof(message), i18n_get("remote.cache_limit_set"), (int)value);
    show_status_message(message);
    rerender_remote_view();
}

/* ============================================================
 * 会话跟踪 / 重试
 * ============================================================ */

static void remote_begin_session(int conn_idx, const char *path, int autoplay)
{
    if (conn_idx < 0 || conn_idx >= remote_store_count()) {
        return;
    }
    const RemoteConnectionConfig *conn = remote_store_get(conn_idx);
    if (!conn) {
        return;
    }
    if (remote_cache_start_session(conn, path, autoplay) != 0) {
        snprintf(g_remote_last_summary, sizeof(g_remote_last_summary), "%s",
                 i18n_get("remote.err.busy"));
        g_remote_summary_valid = 1;
        show_status_message(g_remote_last_summary);
        return;
    }
    g_remote_last_session_conn = conn_idx;
    snprintf(g_remote_last_session_path, sizeof(g_remote_last_session_path), "%s",
             path ? path : "");
    g_remote_last_session_failed = 0;
    g_remote_session_pending = 1;
    g_remote_download_seen = 0;
    g_remote_last_error[0] = '\0';
    g_remote_summary_valid = 0;
    g_remote_last_reported_done = -1;

    char msg[200];
    snprintf(msg, sizeof(msg), "%s: %s", i18n_get("remote.status.listing"), conn->name);
    show_status_message(msg);
    remote_set_main_status(msg, 2000);
}

static void remote_retry_session(void)
{
    if (g_remote_last_session_conn < 0 ||
        g_remote_last_session_conn >= remote_store_count()) {
        show_status_message(i18n_get("remote.err.unknown"));
        return;
    }
    remote_begin_session(g_remote_last_session_conn, g_remote_last_session_path, 0);
    show_status_message(i18n_get("remote.retry_started"));
}

/* UI 线程把已下载好的本地路径交给核心（核心只看到本地文件） */
static void remote_drain_downloads(void)
{
    char path[MAX_PATH_LEN];
    int first = 0;
    int last = 0;
    while (remote_cache_take_ready(path, sizeof(path), &first, &last)) {
        (void)last;
        /* 新下载会话的首个 ready 项：重置“已替换内容”标记。首曲失败时，
         * 下一次循环仍会走替换分支，而不是把新会话追加到旧内容尾部。 */
        if (first) {
            g_remote_content_started = 0;
        }
        int replacing = !g_remote_content_started;
        int autoplay = (replacing && remote_cache_session_autoplay()) ? 1 : 0;

        /* 第一首只装载缓存文件本身，不能走 app_open_path()：后者会把
         * 缓存父目录整体扫描成内容，旧会话残留的缓存文件会混进本次队列。
         * 后续曲目追加到同一内容列表。 */
        int loaded;
        if (replacing) {
            loaded = (load_single_file(path) > 0);
        } else {
            loaded = (append_playlist(path) > 0);
        }
        if (!loaded) {
            log_warn("remote_view", "Failed to load cache path '%s'", path);
            continue;
        }

        if (replacing) {
            /* 首曲整表下发；后续用 Append，避免每下载一首就把核心游标
             * 拉回 0，也避免 O(n²) 的整表编码。 */
            player_queue_push();
            g_remote_content_started = 1;
        } else if (player_backend() == PLAYER_BACKEND_REMOTE) {
            /* remote 后端：append_playlist() 只更新本进程镜像，必须显式
             * Append 才能到达核心。 */
            int index = playlist_count() - 1;
            if (index >= 0 && player_queue_append(index) == 0) {
                /* appended */
            } else {
                /* 未连接/追加失败：下次重连钩子会整表补推。 */
                log_warn("remote_view", "Could not append '%s' to the core queue", path);
            }
        } else {
            /* local 后端：append_playlist() 内部已 playlist_queue_sync()
             * 整表同步，再 Append 会重复入队（审计 F-15）。 */
        }

        if (autoplay) {
            ui_play_track_index(0);
        }
    }

    RemoteCacheStatus status;
    remote_cache_get_status(&status);

    /* 主界面状态栏同步进度：用户离开设置页也能判断是否还在下载。 */
    remote_sync_progress_message(&status);

    char error[256];
    while (remote_cache_take_error(error, sizeof(error))) {
        remote_remember_error(error);
        char localized[360];
        remote_localized_error(error, localized, sizeof(localized));
        char buf[400];
        snprintf(buf, sizeof(buf), "%s %s", i18n_get("remote.download_failed"),
                 localized[0] ? localized : error);
        show_status_message(buf);
        remote_set_main_status(buf, 8000);
    }

    /* 会话收尾：保存失败数用于“重试失败曲目”，并给出结束摘要。
     * 本进程发起的会话用 pending 结算，避免“会话在一帧内跑完、没被采样到
     * DOWNLOADING”时丢失失败数（重试入口就不会出现）。 */
    int terminal = (status.state == REMOTE_CACHE_DONE ||
                    status.state == REMOTE_CACHE_FAILED ||
                    status.state == REMOTE_CACHE_CANCELLED);
    int has_download_data = (status.total > 0 || status.done > 0 ||
                             status.failed > 0 || status.truncated);
    int session_done = terminal && !g_remote_terminal_handled &&
                       (g_remote_download_seen || g_remote_session_pending ||
                        has_download_data);
    if (status.state == REMOTE_CACHE_LISTING ||
        status.state == REMOTE_CACHE_DOWNLOADING) {
        g_remote_terminal_handled = 0;
        if (status.state == REMOTE_CACHE_DOWNLOADING) {
            g_remote_download_seen = 1;
        }
    } else if (session_done) {
        int was_cancelled = (status.state == REMOTE_CACHE_CANCELLED);
        g_remote_terminal_handled = 1;
        g_remote_download_seen = 0;
        g_remote_session_pending = 0;
        g_remote_last_session_failed = status.failed;
        if (status.failed > 0) {
            remote_remember_error(status.message);
        }
        if (!was_cancelled) {
            remote_evict_protected();
            remote_cache_usage_invalidate();
            if (status.done == 0 && g_remote_last_error[0]) {
                /* 列目录/鉴权等失败时 done=0，不能误报“目录没有音频”。 */
                char localized[360];
                remote_localized_error(g_remote_last_error, localized, sizeof(localized));
                snprintf(g_remote_last_summary, sizeof(g_remote_last_summary), "%s",
                         localized[0] ? localized : i18n_get("remote.status.failed"));
            } else if (status.done == 0) {
                snprintf(g_remote_last_summary, sizeof(g_remote_last_summary), "%s",
                         i18n_get("remote.no_audio"));
            } else if (status.truncated) {
                snprintf(g_remote_last_summary, sizeof(g_remote_last_summary),
                         i18n_get("remote.truncated"), status.done, status.total);
            } else {
                snprintf(g_remote_last_summary, sizeof(g_remote_last_summary),
                         i18n_get("remote.session_summary"),
                         status.done, status.total, status.failed);
            }
            g_remote_summary_valid = 1;
            show_status_message(g_remote_last_summary);
            remote_set_main_status(g_remote_last_summary, 8000);
            if (status.failed > 0) {
                char localized[360];
                remote_localized_error(g_remote_last_error, localized, sizeof(localized));
                if (localized[0]) {
                    show_status_message(localized);
                    remote_set_main_status(localized, 8000);
                }
            }
        } else {
            snprintf(g_remote_last_summary, sizeof(g_remote_last_summary), "%s",
                     i18n_get("remote.status.cancelled"));
            g_remote_summary_valid = 1;
        }
        if (!remote_job_busy(&status) && remote_view_is_active()) {
            render_settings_content();
        }
    }
}

/* UI 主循环每帧调用：取回后台线程的结果 */
void remote_view_tick(void)
{
    remote_drain_listing();
    remote_drain_downloads();

    /* 后台任务进行中：把可判定进度写进主视图状态行（t5 V-03）。 */
    RemoteCacheStatus status;
    remote_cache_get_status(&status);
    if (remote_job_busy(&status)) {
        char line[400];
        remote_format_status(&status, line, sizeof(line));
        remote_set_main_status(line, 1500);
    }
    remote_paint_main_status();
}

static void remote_start_browse(int conn_idx)
{
    g_remote_selected_conn = conn_idx;
    g_remote_mode = 3;
    g_remote_selected = 0;
    g_remote_confirm = REMOTE_CONFIRM_NONE;

    const RemoteConnectionConfig *conn = remote_store_get(conn_idx);
    strncpy(g_remote_current_path, conn->base_path, sizeof(g_remote_current_path) - 1);
    g_remote_current_path[sizeof(g_remote_current_path) - 1] = '\0';

    remote_refresh_entries();
    rerender_remote_view();
}

static void remote_refresh_connection(void)
{
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= remote_store_count()) return;
    const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);

    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;
    g_remote_check_pending = 1;
    g_remote_listing_failed = 0;

    show_status_message(i18n_get("remote.status.listing"));
    render_settings_content();
    refresh();

    if (remote_cache_start_listing(conn, conn->base_path) != 0) {
        g_remote_check_pending = 0;
        show_status_message(i18n_get("remote.err.busy"));
    }
}

/* 打开远程目录：前端下载到本地缓存，逐首交给核心播放。
 * 目录列取与下载都在后台线程进行，本函数立刻返回，用户可继续操作。 */
static void remote_open_directory(void)
{
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= remote_store_count()) return;
    remote_begin_session(g_remote_selected_conn, g_remote_current_path, 1);
    exit_current_view();
}

/* ============================================================
 * Remote device — form (add / edit)
 * ============================================================ */

/* 表单字段按协议动态显隐；visible 顺序就是屏幕顺序。 */
typedef enum {
    RF_NAME = 0,
    RF_PROTOCOL,
    RF_HOST,
    RF_PORT,
    RF_USERNAME,
    RF_PASSWORD,
    RF_TLS,
    RF_INSECURE_TLS,
    RF_PRIVATE_KEY,
    RF_KEY_PASSPHRASE,
    RF_KNOWN_HOSTS,
    RF_BASE_PATH,
    RF_COUNT
} RemoteFormField;

typedef struct {
    RemoteFormField fields[RF_COUNT];
    int count;
} RemoteFormLayout;

static int remote_protocol_has_tls(int protocol)
{
    return protocol == REMOTE_PROTOCOL_FTP || protocol == REMOTE_PROTOCOL_WEBDAV ||
           protocol == REMOTE_PROTOCOL_HTTP;
}

static void remote_form_layout(RemoteFormLayout *layout)
{
    layout->count = 0;
    layout->fields[layout->count++] = RF_NAME;
    layout->fields[layout->count++] = RF_PROTOCOL;
    layout->fields[layout->count++] = RF_HOST;
    layout->fields[layout->count++] = RF_PORT;
    layout->fields[layout->count++] = RF_USERNAME;
    layout->fields[layout->count++] = RF_PASSWORD;
    if (remote_protocol_has_tls(g_remote_form_config.protocol)) {
        layout->fields[layout->count++] = RF_TLS;
        if (g_remote_form_config.use_tls) {
            layout->fields[layout->count++] = RF_INSECURE_TLS;
        }
    }
    if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) {
        layout->fields[layout->count++] = RF_PRIVATE_KEY;
        layout->fields[layout->count++] = RF_KEY_PASSPHRASE;
        layout->fields[layout->count++] = RF_KNOWN_HOSTS;
    }
    layout->fields[layout->count++] = RF_BASE_PATH;
}

static const char *remote_form_protocol_hint(void)
{
    if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SMB) {
        return "remote.smb_path_hint";
    }
    if (remote_protocol_has_tls(g_remote_form_config.protocol)) {
        return "remote.tls_hint";
    }
    return "remote.form_hint";
}

static void remote_form_field_label(RemoteFormField field, char *buf, size_t size)
{
    switch (field) {
        case RF_NAME:        snprintf(buf, size, "%s:", i18n_get("remote.field.name")); break;
        case RF_PROTOCOL:    snprintf(buf, size, "%s:", i18n_get("remote.field.protocol")); break;
        case RF_HOST:        snprintf(buf, size, "%s:", i18n_get("remote.field.host")); break;
        case RF_PORT:        snprintf(buf, size, "%s:", i18n_get("remote.field.port")); break;
        case RF_USERNAME:    snprintf(buf, size, "%s:", i18n_get("remote.field.username")); break;
        case RF_PASSWORD:    snprintf(buf, size, "%s:", i18n_get("remote.field.password")); break;
        case RF_TLS:         snprintf(buf, size, "%s:", i18n_get("remote.field.tls")); break;
        case RF_INSECURE_TLS:snprintf(buf, size, "%s:", i18n_get("remote.field.insecure_tls")); break;
        case RF_PRIVATE_KEY: snprintf(buf, size, "%s:", i18n_get("remote.field.private_key")); break;
        case RF_KEY_PASSPHRASE: snprintf(buf, size, "%s:", i18n_get("remote.field.key_passphrase")); break;
        case RF_KNOWN_HOSTS: snprintf(buf, size, "%s:", i18n_get("remote.field.known_hosts")); break;
        case RF_BASE_PATH:
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SMB) {
                snprintf(buf, size, "%s:", i18n_get("remote.field.smb_share"));
            } else {
                snprintf(buf, size, "%s:", i18n_get("remote.field.base_path"));
            }
            break;
        default: buf[0] = '\0'; break;
    }
}

static void remote_form_value_text(RemoteFormField field, char *buf, size_t size)
{
    const RemoteConnectionConfig *rc = &g_remote_form_config;
    buf[0] = '\0';
    switch (field) {
        case RF_NAME: snprintf(buf, size, "%s", rc->name); break;
        case RF_PROTOCOL: snprintf(buf, size, "%s", remote_protocol_name(rc->protocol)); break;
        case RF_HOST: snprintf(buf, size, "%s", rc->host); break;
        case RF_PORT: if (rc->port > 0) snprintf(buf, size, "%d", rc->port); break;
        case RF_USERNAME: snprintf(buf, size, "%s", rc->username); break;
        case RF_PASSWORD:
            if (rc->password[0]) {
                int n = (int)strlen(rc->password);
                if (n > 50) n = 50;
                memset(buf, '*', (size_t)n);
                buf[n] = '\0';
            }
            break;
        case RF_TLS:
            snprintf(buf, size, "%s", i18n_get(rc->use_tls ? "remote.value_on"
                                                             : "remote.value_off"));
            break;
        case RF_INSECURE_TLS:
            snprintf(buf, size, "%s", i18n_get(rc->insecure_tls ? "remote.value_on"
                                                                 : "remote.value_off"));
            break;
        case RF_PRIVATE_KEY: snprintf(buf, size, "%s", rc->private_key_path); break;
        case RF_KEY_PASSPHRASE:
            if (rc->has_key_passphrase && rc->key_passphrase[0]) {
                int n = (int)strlen(rc->key_passphrase);
                if (n > 50) n = 50;
                memset(buf, '*', (size_t)n);
                buf[n] = '\0';
            }
            break;
        case RF_KNOWN_HOSTS: snprintf(buf, size, "%s", rc->known_hosts_path); break;
        case RF_BASE_PATH: snprintf(buf, size, "%s", rc->base_path); break;
        default: break;
    }
}

static void remote_form_cycle_protocol(int delta)
{
    int next = (g_remote_form_config.protocol + delta + 5) % 5;
    g_remote_form_config.protocol = next;
    if (!remote_protocol_has_tls(next)) {
        g_remote_form_config.use_tls = 0;
        g_remote_form_config.insecure_tls = 0;
    }
    RemoteFormLayout layout;
    remote_form_layout(&layout);
    if (g_remote_selected >= layout.count) {
        g_remote_selected = layout.count - 1;
    }
}

/* 把 SMB 的“共享名/子路径”规范化：去掉 smb:// 前缀与首尾斜杠。 */
static void remote_form_normalize_smb_path(char *path, size_t size)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", path);
    char *start = buffer;
    const char *scheme = strstr(buffer, "://");
    if (scheme) {
        start = strchr(scheme + 3, '/');
        if (!start) {
            start = buffer + strlen(buffer);
        }
    }
    while (*start == '/') start++;
    size_t len = strlen(start);
    while (len > 0 && start[len - 1] == '/') {
        start[--len] = '\0';
    }
    snprintf(path, size, "%s", start);
}

static void remote_form_edit_field(RemoteFormField field)
{
    int form_start_y = 4;
    int max_x = getmaxx(stdscr);
    int content_start_x = (max_x / 4) + 2;
    char label[48];
    remote_form_field_label(field, label, sizeof(label));

    curs_set(1);
    noecho();

    switch (field) {
        case RF_NAME:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.name,
                              sizeof(g_remote_form_config.name), 1, 0, 1);
            break;
        case RF_PROTOCOL:
            /* 协议是枚举：ENTER 与 +/- 一样按顺序切换，不再要求输入数字。 */
            remote_form_cycle_protocol(1);
            break;
        case RF_HOST:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.host,
                              sizeof(g_remote_form_config.host), 1, 0, 1);
            break;
        case RF_PORT: {
            char buf[16];
            if (g_remote_form_config.port > 0) {
                snprintf(buf, sizeof(buf), "%d", g_remote_form_config.port);
            } else {
                buf[0] = '\0';
            }
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, buf, sizeof(buf), 1, 0, 1);
            if (!buf[0]) {
                g_remote_form_config.port = 0;   /* 空 = 使用默认端口 */
            } else {
                char *end = NULL;
                long value = strtol(buf, &end, 10);
                if (end == buf || (end && *end != '\0') || value < 1 || value > 65535) {
                    show_status_message(i18n_get("remote.invalid_port"));
                } else {
                    g_remote_form_config.port = (int)value;
                }
            }
            break;
        }
        case RF_USERNAME:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.username,
                              sizeof(g_remote_form_config.username), 1, 0, 1);
            break;
        case RF_PASSWORD:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.password,
                              sizeof(g_remote_form_config.password), 1, 1, 1);
            break;
        case RF_PRIVATE_KEY:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.private_key_path,
                              sizeof(g_remote_form_config.private_key_path), 1, 0, 1);
            break;
        case RF_KEY_PASSPHRASE:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.key_passphrase,
                              sizeof(g_remote_form_config.key_passphrase), 1, 1, 1);
            g_remote_form_config.has_key_passphrase =
                g_remote_form_config.key_passphrase[0] != '\0';
            break;
        case RF_KNOWN_HOSTS:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.known_hosts_path,
                              sizeof(g_remote_form_config.known_hosts_path), 1, 0, 1);
            break;
        case RF_BASE_PATH:
            prompt_text_input(stdscr, form_start_y, content_start_x,
                              label, g_remote_form_config.base_path,
                              sizeof(g_remote_form_config.base_path), 1, 0, 1);
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SMB) {
                remote_form_normalize_smb_path(g_remote_form_config.base_path,
                                               sizeof(g_remote_form_config.base_path));
            } else if (!g_remote_form_config.base_path[0]) {
                snprintf(g_remote_form_config.base_path,
                         sizeof(g_remote_form_config.base_path), "/");
            }
            break;
        default:
            break;
    }

    curs_set(0);
    noecho();
}

static int remote_form_validate(char *message, size_t size)
{
    if (!g_remote_form_config.name[0]) {
        snprintf(message, size, "%s", i18n_get("remote.name_required"));
        return 0;
    }
    if (!g_remote_form_config.host[0]) {
        snprintf(message, size, "%s", i18n_get("remote.host_required"));
        return 0;
    }
    if (g_remote_form_config.port != 0 &&
        (g_remote_form_config.port < 1 || g_remote_form_config.port > 65535)) {
        snprintf(message, size, "%s", i18n_get("remote.invalid_port"));
        return 0;
    }
    if (g_remote_form_config.protocol < REMOTE_PROTOCOL_SMB ||
        g_remote_form_config.protocol > REMOTE_PROTOCOL_HTTP) {
        snprintf(message, size, "%s", i18n_get("remote.err.protocol"));
        return 0;
    }
    if (!remote_protocol_has_tls(g_remote_form_config.protocol)) {
        g_remote_form_config.use_tls = 0;
        g_remote_form_config.insecure_tls = 0;
    }
    if (!g_remote_form_config.base_path[0]) {
        snprintf(message, size, "%s", i18n_get("remote.invalid_base_path"));
        return 0;
    }
    return 1;
}

static void remote_form_save(void)
{
    char message[256];
    if (!remote_form_validate(message, sizeof(message))) {
        show_status_message(message);
        rerender_remote_view();
        return;
    }

    if (g_remote_form_editing_idx >= 0) {
        if (remote_store_upsert(g_remote_form_editing_idx, &g_remote_form_config) != 0) {
            show_status_message(i18n_get("remote.save_failed"));
            rerender_remote_view();
            return;
        }
    } else {
        if (remote_store_count() >= MAX_REMOTE_CONNECTIONS) {
            show_status_message(i18n_get("remote.connections_full"));
            rerender_remote_view();
            return;
        }
        if (remote_store_upsert(-1, &g_remote_form_config) != 0) {
            show_status_message(i18n_get("remote.save_failed"));
            rerender_remote_view();
            return;
        }
    }

    if (remote_store_save() != 0) {
        show_status_message(i18n_get("remote.save_failed"));
        rerender_remote_view();
        return;
    }
    show_status_message(i18n_get("remote.connection_saved"));
    g_remote_mode = 0;
    g_remote_selected = g_remote_form_editing_idx >= 0
        ? g_remote_form_editing_idx
        : (remote_store_count() - 1);
    g_remote_form_editing_idx = -1;
    rerender_remote_view();
}

static void remote_form_cancel(void)
{
    g_remote_mode = 0;
    g_remote_selected = g_remote_form_editing_idx >= 0
        ? g_remote_form_editing_idx
        : remote_store_count();
    g_remote_form_editing_idx = -1;
    rerender_remote_view();
}

static void remote_start_add(void)
{
    memset(&g_remote_form_config, 0, sizeof(g_remote_form_config));
    g_remote_form_config.protocol = REMOTE_PROTOCOL_FTP;
    g_remote_form_editing_idx = -1;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_start_edit(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= remote_store_count()) return;
    const RemoteConnectionConfig *stored = remote_store_get(conn_idx);
    if (!stored) return;
    g_remote_form_config = *stored;
    g_remote_form_editing_idx = conn_idx;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_delete_connection(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= remote_store_count()) return;
    if (remote_store_remove(conn_idx) != 0) return;
    if (remote_store_save() != 0) {
        show_status_message(i18n_get("remote.save_failed"));
    } else {
        show_status_message(i18n_get("remote.connection_deleted"));
    }

    g_remote_confirm = REMOTE_CONFIRM_NONE;
    g_remote_confirm_conn = -1;
    g_remote_mode = 0;
    if (g_remote_selected >= remote_store_count()) {
        g_remote_selected = remote_store_count() > 0
            ? remote_store_count() - 1 : 0;
    }
    rerender_remote_view();
}

/* ============================================================
 * Remote device — rendering
 * ============================================================ */

/* ── 渲染期行映射（供鼠标命中） ───────────────────────────────────
 * 远程页四种模式（连接列表 / 连接动作菜单 / 表单 / 目录浏览）的可选项
 * 行号推导各不相同，还有目录模式的滚动偏移。与其在鼠标层再抄一套公式，
 * 不如画到哪一行就记下"这一行对应哪个 g_remote_selected 值"。
 * 每帧开头清空，所以映射永远等于屏幕上看到的那一帧。 */
#define REMOTE_MAP_MAX_ROWS 1024
static int g_remote_row_map[REMOTE_MAP_MAX_ROWS];

static void remote_map_clear(void)
{
    for (int i = 0; i < REMOTE_MAP_MAX_ROWS; i++) g_remote_row_map[i] = -1;
}

static void remote_map_row(int screen_y, int selection_value)
{
    if (screen_y < 0 || screen_y >= REMOTE_MAP_MAX_ROWS) return;
    g_remote_row_map[screen_y] = selection_value;
}

/* 屏幕行 → 该模式下的选中值（与 g_remote_selected 同一套编号）；
 * 落在不可选的行（标题、提示、空白）返回 -1。仅本模块用。 */
static int remote_view_hit_index(int screen_y)
{
    if (screen_y < 0 || screen_y >= REMOTE_MAP_MAX_ROWS) return -1;
    return g_remote_row_map[screen_y];
}

/* 底部状态行：进度 / 失败原因 / 结束摘要（F-21/F-25）。 */
static void remote_render_status_line(const MenuContentArea *area, int y,
                                      const RemoteCacheStatus *status)
{
    if (y >= area->max_y - 2) {
        return;
    }
    char line[400];
    remote_format_status(status, line, sizeof(line));
    if (!line[0]) {
        return;
    }
    char text[448];
    snprintf(text, sizeof(text), "  %s", line);
    menu_content_puts(area, y, text);
}

/* ============================================================
 * 连接动作菜单（按当前状态动态增减取消/重试项）
 * ============================================================ */

typedef enum {
    REMOTE_MENU_BROWSE = 0,
    REMOTE_MENU_LOAD,
    REMOTE_MENU_REFRESH,
    REMOTE_MENU_EDIT,
    REMOTE_MENU_DELETE,
    REMOTE_MENU_CANCEL,
    REMOTE_MENU_RETRY_LISTING,
    REMOTE_MENU_RETRY_FAILED,
    REMOTE_MENU_COUNT
} RemoteMenuAction;

typedef struct {
    RemoteMenuAction actions[REMOTE_MENU_COUNT];
    int count;
} RemoteMenuLayout;

static void remote_menu_layout(RemoteMenuLayout *layout, const RemoteCacheStatus *status)
{
    layout->count = 0;
    layout->actions[layout->count++] = REMOTE_MENU_BROWSE;
    layout->actions[layout->count++] = REMOTE_MENU_LOAD;
    layout->actions[layout->count++] = REMOTE_MENU_REFRESH;
    layout->actions[layout->count++] = REMOTE_MENU_EDIT;
    layout->actions[layout->count++] = REMOTE_MENU_DELETE;
    if (remote_job_busy(status)) {
        layout->actions[layout->count++] = REMOTE_MENU_CANCEL;
    }
    if (g_remote_listing_failed) {
        layout->actions[layout->count++] = REMOTE_MENU_RETRY_LISTING;
    }
    if (g_remote_last_session_failed > 0 &&
        g_remote_last_session_conn == g_remote_selected_conn) {
        layout->actions[layout->count++] = REMOTE_MENU_RETRY_FAILED;
    }
}

static const char *remote_menu_label(RemoteMenuAction action, char *buf, size_t size)
{
    switch (action) {
        case REMOTE_MENU_BROWSE: return i18n_get("remote.browse");
        case REMOTE_MENU_LOAD: return i18n_get("remote.load_to_playlist");
        case REMOTE_MENU_REFRESH: return i18n_get("remote.refresh");
        case REMOTE_MENU_EDIT: return i18n_get("remote.edit");
        case REMOTE_MENU_DELETE: return i18n_get("remote.delete");
        case REMOTE_MENU_CANCEL: return i18n_get("remote.cancel");
        case REMOTE_MENU_RETRY_LISTING: return i18n_get("remote.retry_listing");
        case REMOTE_MENU_RETRY_FAILED:
            snprintf(buf, size, i18n_get("remote.retry_failed"),
                     g_remote_last_session_failed);
            return buf;
        default: return "";
    }
}

/* ============================================================
 * 渲染
 * ============================================================ */

void remote_view_render(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int y = 2;

    remote_map_clear();
    /* 设置右栏的“远程设备”内容与其它设置正文同色：当前生效的播放列表色（t28） */
    int content_attr = ui_pane_body_attr(LAYOUT_PANE_PLAYLIST);
    attron(content_attr);

    menu_content_clear_rows(&area, 2, max_y - 3);

    RemoteCacheStatus status;
    remote_cache_get_status(&status);

    if (g_remote_mode == 0) {
        menu_content_puts(&area, y++, i18n_get("remote.hint_manage"));
        if (g_remote_load_failed && y < max_y - 3) {
            menu_content_puts(&area, y++, i18n_get("remote.load_failed"));
        }
        y++;

        int count = remote_store_count();
        char header[96];
        snprintf(header, sizeof(header), "%s (%d)",
                 i18n_get("remote.saved_connections"), count);
        menu_content_puts(&area, y++, header);
        y++;

        for (int i = 0; i < count && y < max_y - 3; i++) {
            const RemoteConnectionConfig *rc = remote_store_get(i);
            char line[640];
            snprintf(line, sizeof(line), "  %-20s [%s%s] %s",
                     rc->name, remote_protocol_name(rc->protocol),
                     rc->use_tls ? "+TLS" : "", rc->host);
            remote_map_row(y, i);   /* 第 i 条已存连接 */
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            menu_content_puts(&area, y++, line);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
        }

        y++;
        if (y <= max_y - 3) {
            char add_line[512];
            snprintf(add_line, sizeof(add_line), "  %s", i18n_get("remote.add_new"));
            remote_map_row(y, count);   /* "新建连接"行＝列表尾下标 */
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attron(A_REVERSE);
            menu_content_puts(&area, y, add_line);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attroff(A_REVERSE);
            y++;
        }

        /* 缓存管理（F-14）：用量 + 清理 + 上限，均有真实入口。 */
        if (y < max_y - 3) {
            y++;
            menu_content_puts(&area, y++, i18n_get("remote.cache_title"));
            char usage[200];
            remote_format_cache_usage(usage, sizeof(usage));
            char usage_line[240];
            snprintf(usage_line, sizeof(usage_line), "  %s", usage);
            if (y < max_y - 3) {
                menu_content_puts(&area, y++, usage_line);
            }
            if (y < max_y - 3) {
                menu_content_puts(&area, y++,
                                  i18n_get("remote.cache_prefetch_unavailable"));
            }
            if (y < max_y - 3) {
                int clean_index = count + 1;
                char clean_line[240];
                snprintf(clean_line, sizeof(clean_line), "  %s",
                         i18n_get("remote.cache_clean"));
                remote_map_row(y, clean_index);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == clean_index) {
                    attron(A_REVERSE);
                }
                menu_content_puts(&area, y++, clean_line);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == clean_index) {
                    attroff(A_REVERSE);
                }
            }
            if (y < max_y - 3) {
                int limit_index = count + 2;
                char limit_line[240];
                snprintf(limit_line, sizeof(limit_line), "  %s",
                         i18n_get("remote.cache_limit_field"));
                remote_map_row(y, limit_index);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == limit_index) {
                    attron(A_REVERSE);
                }
                menu_content_puts(&area, y++, limit_line);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == limit_index) {
                    attroff(A_REVERSE);
                }
            }
        }

        if (g_remote_confirm == REMOTE_CONFIRM_CLEAN_CACHE && y < max_y - 3) {
            y++;
            menu_content_puts(&area, y++, i18n_get("remote.cache_clean_confirm"));
        }
        remote_render_status_line(&area, y, &status);

    } else if (g_remote_mode == 1) {
        int conn_idx = g_remote_selected_conn;
        if (conn_idx < 0 || conn_idx >= remote_store_count()) {
            menu_content_puts(&area, y++, i18n_get("general.no_connection"));
            attroff(content_attr);
            return;
        }
        const RemoteConnectionConfig *rc = remote_store_get(conn_idx);

        char title[640];
        snprintf(title, sizeof(title), "%s: %s [%s%s]",
                 i18n_get("remote.connection"), rc->name,
                 remote_protocol_name(rc->protocol), rc->use_tls ? "+TLS" : "");
        menu_content_puts(&area, y++, title);
        y++;

        if (g_remote_confirm == REMOTE_CONFIRM_DELETE) {
            char confirm[640];
            snprintf(confirm, sizeof(confirm), i18n_get("remote.delete_confirm"),
                     rc->name);
            menu_content_puts(&area, y++, confirm);
            menu_content_puts(&area, y++, i18n_get("remote.confirm_keys"));
            remote_render_status_line(&area, y, &status);
        } else {
            RemoteMenuLayout layout;
            remote_menu_layout(&layout, &status);
            for (int i = 0; i < layout.count && y <= max_y - 3; i++) {
                char label[160];
                const char *text = remote_menu_label(layout.actions[i], label, sizeof(label));
                char action_line[640];
                snprintf(action_line, sizeof(action_line), "  %s", text);
                remote_map_row(y, i);   /* 连接动作菜单的第 i 项 */
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
                menu_content_puts(&area, y++, action_line);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
            }
            y++;
            if (y < max_y - 2) {
                menu_content_puts(&area, y, i18n_get("remote.actions_hint"));
            }
            remote_render_status_line(&area, y + 1, &status);
        }

    } else if (g_remote_mode == 2) {
        RemoteFormLayout layout;
        remote_form_layout(&layout);
        const char *title_fmt = g_remote_form_editing_idx >= 0
            ? i18n_get("remote.edit_hint")
            : i18n_get("remote.add_hint");
        menu_content_puts(&area, y++, title_fmt);
        y++;

        char label[48], value[256];
        for (int i = 0; i < layout.count && y < max_y - 3; i++) {
            remote_form_field_label(layout.fields[i], label, sizeof(label));
            remote_form_value_text(layout.fields[i], value, sizeof(value));
            char line[640];
            snprintf(line, sizeof(line), "  %-18s %s", label, value);
            remote_map_row(y, i);   /* 表单的第 i 个可见字段 */
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            menu_content_puts(&area, y, line);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
            y++;
        }

        if (y < max_y - 2) {
            menu_content_puts(&area, y++, i18n_get(remote_form_protocol_hint()));
        }
        if (y < max_y - 2) {
            menu_content_puts(&area, y++, i18n_get("remote.form_hint"));
        }
        remote_render_status_line(&area, y, &status);

    } else if (g_remote_mode == 3) {
        if (g_remote_selected_conn < 0) {
            menu_content_puts(&area, y++, i18n_get("general.no_connection"));
            attroff(content_attr);
            return;
        }
        const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);

        char header[1088];
        snprintf(header, sizeof(header), "%s: %s > %s",
                 i18n_get("remote.browse_short"), conn->name,
                 g_remote_current_path[0] ? g_remote_current_path : "/");
        menu_content_puts(&area, y++, header);
        y++;

        if (g_remote_listing_failed) {
            char localized[360];
            remote_localized_error(g_remote_last_error, localized, sizeof(localized));
            menu_content_puts(&area, y++, localized[0] ? localized
                                                       : i18n_get("remote.list_failed"));
            if (y < max_y - 3) {
                char retry_line[240];
                snprintf(retry_line, sizeof(retry_line), "  %s",
                         i18n_get("remote.retry_listing"));
                remote_map_row(y, 0);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attron(A_REVERSE);
                menu_content_puts(&area, y++, retry_line);
                if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attroff(A_REVERSE);
            }
        } else if (!g_remote_entries) {
            menu_content_puts(&area, y++, i18n_get("remote.status.listing"));
        } else if (g_remote_entry_count == 0) {
            menu_content_puts(&area, y++, i18n_get("general.empty_dir"));
        } else {
            int max_display = max_y - y - 3;
            if (max_display < 1) max_display = 1;
            int entry_rows = max_display - 1;   /* 第一行是“加载整个目录” */
            if (entry_rows < 1) entry_rows = 1;

            /* 选中项跟随滚动窗口：否则 ↑/↓ 会移到一个没画出来的条目。 */
            if (g_remote_selected == 0) {
                g_remote_entry_offset = 0;
            } else {
                int sel_entry = g_remote_selected - 1;
                if (sel_entry < g_remote_entry_offset) {
                    g_remote_entry_offset = sel_entry;
                } else if (sel_entry >= g_remote_entry_offset + entry_rows) {
                    g_remote_entry_offset = sel_entry - entry_rows + 1;
                }
            }
            if (g_remote_entry_offset < 0) g_remote_entry_offset = 0;
            if (g_remote_entry_offset > g_remote_entry_count - entry_rows) {
                g_remote_entry_offset = g_remote_entry_count - entry_rows;
            }
            if (g_remote_entry_offset < 0) g_remote_entry_offset = 0;

            char load_hint[512];
            snprintf(load_hint, sizeof(load_hint), "  %s", i18n_get("remote.load_hint"));
            remote_map_row(y, 0);   /* 目录浏览里第 0 项＝"加载整个目录到播放列表" */
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attron(A_REVERSE);
            menu_content_puts(&area, y++, load_hint);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attroff(A_REVERSE);

            for (int i = g_remote_entry_offset;
                 i < g_remote_entry_count && y < max_y - 3; i++) {
                const RemoteDirEntry *e = &g_remote_entries[i];
                int is_sel = (g_focus_area == FOCUS_CONTENT && g_remote_selected == i + 1);
                char line[640];
                if (e->is_dir) {
                    snprintf(line, sizeof(line), "  [%s] %s",
                             i18n_get("remote.dir"), e->name);
                } else {
                    snprintf(line, sizeof(line), "  %s", e->name);
                }
                /* 条目本身从选中值 1 起（0 被"加载整个目录"占了） */
                remote_map_row(y, i + 1);
                if (is_sel) attron(A_REVERSE);
                menu_content_puts(&area, y++, line);
                if (is_sel) attroff(A_REVERSE);
            }
        }

        remote_render_status_line(&area, y, &status);
        if (y + 1 < max_y - 2) {
            menu_content_puts(&area, y + 1,
                              remote_job_busy(&status) ? i18n_get("remote.cancel_hint")
                                                       : i18n_get("remote.browse_hint"));
        }
    }

    attroff(content_attr);
    refresh();
}

/* ============================================================
 * Remote device — input handling
 * ============================================================ */

int remote_view_handle_input(int ch)
{
    /* Esc 是系统保留键：沿用各模式的返回语义；任务进行中则先取消任务。 */
    if (ch == 27) {
        if (g_remote_mode == 2) {
            remote_form_cancel();
        } else if (g_remote_confirm != REMOTE_CONFIRM_NONE) {
            g_remote_confirm = REMOTE_CONFIRM_NONE;
            g_remote_confirm_conn = -1;
            rerender_remote_view();
        } else {
            RemoteCacheStatus status;
            remote_cache_get_status(&status);
            if (remote_job_busy(&status)) {
                remote_cache_cancel();
                show_status_message(i18n_get("remote.cancelled"));
                rerender_remote_view();
            } else {
                remote_go_back();
            }
        }
        return 1;
    }

    return remote_view_handle_action(keymap_resolve(CTX_REMOTE, ch));
}

/* 动作入口：键盘解析出的动作与鼠标滚轮共用同一条路径。
 * 鼠标不合成按键码——用户可以改键，合成键在改键后会失灵或做错事。 */
int remote_view_handle_action(ActionId action)
{
    int conn_count = remote_store_count();
    RemoteCacheStatus status;
    remote_cache_get_status(&status);

    if (g_remote_mode == 0) {
        int total = conn_count + 3;   /* 连接 + 新建 + 清理缓存 + 缓存上限 */
        switch (action) {
            case ACTION_REMOTE_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = total - 1;
                render_settings_content();
                break;
            case ACTION_REMOTE_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= total) g_remote_selected = 0;
                render_settings_content();
                break;
            case ACTION_REMOTE_ACTIVATE:
                if (g_remote_confirm == REMOTE_CONFIRM_CLEAN_CACHE) {
                    remote_do_clean_cache();
                    rerender_remote_view();
                } else if (g_remote_selected >= 0 && g_remote_selected < conn_count) {
                    g_remote_selected_conn = g_remote_selected;
                    g_remote_mode = 1;
                    g_remote_selected = 0;
                    rerender_remote_view();
                } else if (g_remote_selected == conn_count) {
                    remote_start_add();
                } else if (g_remote_selected == conn_count + 1) {
                    g_remote_confirm = REMOTE_CONFIRM_CLEAN_CACHE;
                    rerender_remote_view();
                } else if (g_remote_selected == conn_count + 2) {
                    remote_prompt_cache_limit();
                }
                break;
            case ACTION_REMOTE_LEFT:
                remote_go_back();
                break;
            default:
                break;
        }
    } else if (g_remote_mode == 1) {
        if (g_remote_confirm == REMOTE_CONFIRM_DELETE) {
            if (action == ACTION_REMOTE_ACTIVATE) {
                remote_delete_connection(g_remote_confirm_conn);
            } else if (action == ACTION_REMOTE_LEFT) {
                g_remote_confirm = REMOTE_CONFIRM_NONE;
                g_remote_confirm_conn = -1;
                rerender_remote_view();
            }
            return (action != ACTION_NONE && actions_context(action) == CTX_REMOTE) ? 1 : 0;
        }

        RemoteMenuLayout layout;
        remote_menu_layout(&layout, &status);
        switch (action) {
            case ACTION_REMOTE_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = layout.count - 1;
                render_settings_content();
                break;
            case ACTION_REMOTE_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= layout.count) g_remote_selected = 0;
                render_settings_content();
                break;
            case ACTION_REMOTE_ACTIVATE:
                if (g_remote_selected < 0 || g_remote_selected >= layout.count) {
                    break;
                }
                switch (layout.actions[g_remote_selected]) {
                    case REMOTE_MENU_BROWSE:
                        remote_start_browse(g_remote_selected_conn);
                        break;
                    case REMOTE_MENU_LOAD:
                        if (g_remote_selected_conn >= 0 &&
                            g_remote_selected_conn < remote_store_count()) {
                            const RemoteConnectionConfig *c =
                                remote_store_get(g_remote_selected_conn);
                            strncpy(g_remote_current_path, c->base_path,
                                    sizeof(g_remote_current_path) - 1);
                            g_remote_current_path[sizeof(g_remote_current_path) - 1] = '\0';
                            remote_open_directory();
                        }
                        break;
                    case REMOTE_MENU_REFRESH:
                        remote_refresh_connection();
                        break;
                    case REMOTE_MENU_EDIT:
                        remote_start_edit(g_remote_selected_conn);
                        break;
                    case REMOTE_MENU_DELETE:
                        g_remote_confirm = REMOTE_CONFIRM_DELETE;
                        g_remote_confirm_conn = g_remote_selected_conn;
                        rerender_remote_view();
                        break;
                    case REMOTE_MENU_CANCEL:
                        remote_cache_cancel();
                        show_status_message(i18n_get("remote.cancelled"));
                        rerender_remote_view();
                        break;
                    case REMOTE_MENU_RETRY_LISTING:
                        remote_refresh_entries();
                        show_status_message(i18n_get("remote.retry_listing_started"));
                        rerender_remote_view();
                        break;
                    case REMOTE_MENU_RETRY_FAILED:
                        remote_retry_session();
                        rerender_remote_view();
                        break;
                    default:
                        break;
                }
                break;
            case ACTION_REMOTE_LEFT:
                remote_go_back();
                break;
            default:
                break;
        }
    } else if (g_remote_mode == 2) {
        RemoteFormLayout layout;
        remote_form_layout(&layout);
        switch (action) {
            case ACTION_REMOTE_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = layout.count - 1;
                render_settings_content();
                break;
            case ACTION_REMOTE_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= layout.count) g_remote_selected = 0;
                render_settings_content();
                break;
            case ACTION_REMOTE_ACTIVATE:
                if (g_remote_selected >= 0 && g_remote_selected < layout.count) {
                    remote_form_edit_field(layout.fields[g_remote_selected]);
                    render_settings_content();
                }
                break;
            case ACTION_REMOTE_ADJUST_UP:
            case ACTION_REMOTE_ADJUST_DOWN: {
                int delta = (action == ACTION_REMOTE_ADJUST_UP) ? 1 : -1;
                if (g_remote_selected >= 0 && g_remote_selected < layout.count) {
                    RemoteFormField field = layout.fields[g_remote_selected];
                    if (field == RF_PROTOCOL) {
                        remote_form_cycle_protocol(delta);
                    } else if (field == RF_TLS) {
                        g_remote_form_config.use_tls = !g_remote_form_config.use_tls;
                        if (!g_remote_form_config.use_tls) {
                            g_remote_form_config.insecure_tls = 0;
                        }
                    } else if (field == RF_INSECURE_TLS) {
                        g_remote_form_config.insecure_tls =
                            !g_remote_form_config.insecure_tls;
                    }
                    RemoteFormLayout updated;
                    remote_form_layout(&updated);
                    if (g_remote_selected >= updated.count) {
                        g_remote_selected = updated.count - 1;
                    }
                    render_settings_content();
                }
                break;
            }
            case ACTION_REMOTE_SAVE:
                remote_form_save();
                break;
            case ACTION_REMOTE_LEFT:
                remote_form_cancel();
                break;
            default:
                break;
        }
    } else if (g_remote_mode == 3) {
        int total_items = g_remote_listing_failed ? 1 : 1 + g_remote_entry_count;

        switch (action) {
            case ACTION_REMOTE_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = total_items - 1;
                render_settings_content();
                break;
            case ACTION_REMOTE_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= total_items) g_remote_selected = 0;
                render_settings_content();
                break;
            case ACTION_REMOTE_RIGHT:
                if (!g_remote_listing_failed && g_remote_selected > 0) {
                    int entry_idx = g_remote_selected - 1;
                    if (entry_idx >= 0 && entry_idx < g_remote_entry_count &&
                        g_remote_entries[entry_idx].is_dir) {
                        size_t cur_len = strlen(g_remote_current_path);
                        if (cur_len > 0 && g_remote_current_path[cur_len - 1] != '/') {
                            strncat(g_remote_current_path, "/",
                                    sizeof(g_remote_current_path) - cur_len - 1);
                        }
                        strncat(g_remote_current_path, g_remote_entries[entry_idx].name,
                                sizeof(g_remote_current_path) -
                                    strlen(g_remote_current_path) - 1);
                        g_remote_selected = 0;
                        g_remote_entry_offset = 0;
                        remote_refresh_entries();
                        rerender_remote_view();
                    }
                }
                break;
            case ACTION_REMOTE_ACTIVATE:
                if (g_remote_listing_failed) {
                    remote_refresh_entries();
                    rerender_remote_view();
                } else if (g_remote_selected == 0) {
                    remote_open_directory();
                } else {
                    int entry_idx = g_remote_selected - 1;
                    if (entry_idx >= 0 && entry_idx < g_remote_entry_count) {
                        if (g_remote_entries[entry_idx].is_dir) {
                            size_t cur_len = strlen(g_remote_current_path);
                            if (cur_len > 0 && g_remote_current_path[cur_len - 1] != '/') {
                                strncat(g_remote_current_path, "/",
                                        sizeof(g_remote_current_path) - cur_len - 1);
                            }
                            strncat(g_remote_current_path, g_remote_entries[entry_idx].name,
                                    sizeof(g_remote_current_path) -
                                        strlen(g_remote_current_path) - 1);
                            g_remote_selected = 0;
                            g_remote_entry_offset = 0;
                            remote_refresh_entries();
                            rerender_remote_view();
                        } else {
                            remote_open_directory();
                        }
                    }
                }
                break;
            case ACTION_REMOTE_LEFT:
                remote_go_back();
                break;
            default:
                break;
        }
    } else {
        return 0;
    }

    /* 本上下文的动作即使当前模式无对应操作也视为已消费（与旧行为一致），
     * 其他上下文的动作（含 global 回退）交回上层分发。 */
    return (action != ACTION_NONE && actions_context(action) == CTX_REMOTE) ? 1 : 0;
}

/* ── 鼠标 ────────────────────────────────────────────────────────
 *
 * 远程页的行号在各模式（连接列表 / 动作菜单 / 表单 / 目录浏览）里推导
 * 方式都不同，还有目录模式的滚动偏移，所以选中状态只在本模块内解释：
 * 渲染时记下"这一行=哪个选中值"，命中就照那张表走。
 *
 * 约定与设置页其它分区一致：单击=选中并执行（进入连接 / 执行动作 /
 * 编辑字段 / 打开目录）；右键菜单路径只传 activate=0，仅选中不执行。
 * g_remote_selected 是本模块私有状态，因此设置页只传屏幕行，不碰它。 */
int remote_view_handle_click(int screen_y, int activate)
{
    int hit = remote_view_hit_index(screen_y);
    if (hit < 0) {
        return 0;   /* 标题、提示、空白行：不可选 */
    }

    g_remote_selected = hit;
    render_settings_content();

    if (activate) {
        return remote_view_handle_action(ACTION_REMOTE_ACTIVATE);
    }
    return 1;
}

/* ── 生命周期（main.c 启动 / ui.c 退出） ───────────────────────────── */

void remote_view_init(void)
{
    /* 前端自有的 <configdir>/remote.xml：核心不认识远程音乐源 */
    g_remote_load_failed = remote_store_load() < 0;
    remote_cache_init();
    remote_cache_usage_invalidate();
}

/* player_init() 之后才调用：此时核心已接上，保护集能拿到当前曲目与整条
 * 核心队列（F-19）。启动淘汰本身按 mtime 从旧到新，保护集内的文件不动。 */
void remote_view_startup_evict(void)
{
    remote_evict_protected();
}

void remote_view_shutdown(void)
{
    remote_cache_shutdown();
}
