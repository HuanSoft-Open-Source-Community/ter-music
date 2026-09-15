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
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/remote_view.h"

#include <ncursesw/ncurses.h>

#include "i18n/i18n.h"
#include "logger/logger.h"
#include "player/player.h"
#include "remote/remote.h"
#include "remote/remote_cache.h"
#include "remote/remote_store.h"
#include "ui/dialog.h"
#include "ui/menu_internal.h"
#include "ui/menus.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Forward declarations for remote helpers */
void remote_view_render(void);
void remote_view_handle_input(int ch);
void remote_enter_list_mode(void);  /* non-static: declared in menu_internal.h */
static void remote_start_add(void);
static void remote_start_edit(int conn_idx);
static void remote_delete_connection(int conn_idx);
static void remote_start_browse(int conn_idx);
static void remote_refresh_entries(void);

/* ============================================================
 * Remote device — navigation / lifecycle
 * ============================================================ */

void remote_enter_list_mode(void)
{
    g_remote_mode = 0;
    g_remote_selected = 0;
    g_remote_selected_conn = -1;
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
            g_remote_mode = 0;
            g_remote_selected = 0;
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
                render_settings_content();
            }
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
static int g_remote_check_pending = 0;   /* 本次列目录是“测试连接”而非“浏览目录” */
static int g_remote_download_seen = 0;   /* 本次 UI 会话里见过“下载中”状态 */

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

    const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);
    if (remote_cache_start_listing(conn, g_remote_current_path) != 0) {
        show_status_message(i18n_get("general.loading"));
        return;
    }
    show_status_message(i18n_get("general.connecting"));
    refresh();
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
    const char *err = remote_cache_status()->message;

    if (is_error) {
        if (was_check) {
            char buf[320];
            snprintf(buf, sizeof(buf), "%s%s%s",
                     i18n_get("remote.refresh_failed"),
                     (err && err[0]) ? err : "",
                     i18n_get("remote.press_any_key"));
            show_status_message(buf);
        } else if (err && err[0]) {
            char buf[320];
            snprintf(buf, sizeof(buf), "%s: %s", i18n_get("remote.list_failed"), err);
            show_status_message(buf);
        } else {
            show_status_message(i18n_get("remote.list_failed"));
        }
        return;
    }

    if (was_check) {
        if (entries) {
            remote_free_entries(entries, count);
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "%s %d %s",
                 i18n_get("remote.connection_ok"), count, i18n_get("remote.entries"));
        show_status_message(buf);
        render_settings_content();
        return;
    }

    g_remote_entries = entries;
    g_remote_entry_count = count;
    g_remote_entry_offset = 0;
    rerender_remote_view();
}


/* ── 缓存淘汰的保护集 ──────────────────────────────────────────────
 * 正在播放的曲目、核心播放列表里的曲目、收藏与历史引用的文件都不能被淘汰，
 * 否则用户会看到"列表里有、一播就失败"的条目。上游（核心）是这些状态的
 * 拥有者，故这里经门面把它们收集起来，再交给 remote_cache_evict()。 */
#define REMOTE_PROTECT_MAX 256

static int remote_collect_protected(char **paths, char **storage, int max)
{
    int count = 0;

    const InfoTrack *track = player_track();
    if (track && track->valid && track->path[0] && count < max) {
        storage[count] = strdup(track->path);
        if (storage[count]) {
            paths[count] = storage[count];
            count++;
        }
    }

    int favorites = player_favorites_count();
    for (int i = 0; i < favorites && count < max; i++) {
        Track fav;
        memset(&fav, 0, sizeof(fav));
        if (player_favorites_get(i, &fav) == 0 && fav.path[0]) {
            storage[count] = strdup(fav.path);
            if (storage[count]) {
                paths[count] = storage[count];
                count++;
            }
        }
    }

    int history = player_history_count();
    for (int i = 0; i < history && count < max; i++) {
        HistoryEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (player_history_get(i, &entry) == 0 && entry.path[0]) {
            storage[count] = strdup(entry.path);
            if (storage[count]) {
                paths[count] = storage[count];
                count++;
            }
        }
    }

    /* 播放列表可能很长：只登记缓存里的路径（远程缓存根目录下的文件） */
    const char *cache_root = remote_cache_dir();
    int total = player_playlist_count();
    for (int i = 0; i < total && count < max; i++) {
        Track item;
        memset(&item, 0, sizeof(item));
        if (player_track_metadata(i, &item) != 0 || !item.path[0]) {
            continue;
        }
        if (cache_root && strncmp(item.path, cache_root, strlen(cache_root)) != 0) {
            continue;
        }
        storage[count] = strdup(item.path);
        if (storage[count]) {
            paths[count] = storage[count];
            count++;
        }
    }

    return count;
}

static void remote_evict_protected(void)
{
    char *paths[REMOTE_PROTECT_MAX];
    char *storage[REMOTE_PROTECT_MAX];
    memset(paths, 0, sizeof(paths));
    memset(storage, 0, sizeof(storage));

    int count = remote_collect_protected(paths, storage, REMOTE_PROTECT_MAX);
    remote_cache_evict((const char *const *)paths, count);

    for (int i = 0; i < count; i++) {
        free(storage[i]);
    }
}

/* UI 线程把已下载好的本地路径交给核心（核心只看到本地文件） */
static void remote_drain_downloads(void)
{
    char path[MAX_PATH_LEN];
    int first = 0;
    int last = 0;
    while (remote_cache_take_ready(path, sizeof(path), &first, &last)) {
        int autoplay = (first && remote_cache_session_autoplay()) ? 1 : 0;
        if (player_playlist_load(path, first ? 0 : 1, autoplay) != 0) {
            log_warn("remote_view", "Core refused cache path '%s'", path);
        }
    }

    const RemoteCacheStatus *status = remote_cache_status();
    char error[256];
    while (remote_cache_take_error(error, sizeof(error))) {
        char buf[320];
        snprintf(buf, sizeof(buf), "%s%s", i18n_get("remote.download_failed"), error);
        show_status_message(buf);
    }

    /* 会话收尾提示：只有真正下载过的会话才提示（浏览目录不算） */
    if (status->state == REMOTE_CACHE_DOWNLOADING) {
        g_remote_download_seen = 1;
    } else if (g_remote_download_seen &&
               (status->state == REMOTE_CACHE_DONE || status->state == REMOTE_CACHE_FAILED)) {
        g_remote_download_seen = 0;
        remote_evict_protected();
        if (status->done == 0) {
            show_status_message(i18n_get("remote.no_audio"));
        } else if (status->failed > 0) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s (%d/%d)",
                     i18n_get("remote.partial"), status->done, status->total);
            show_status_message(msg);
        }
    }
}

/* UI 主循环每帧调用：取回后台线程的结果 */
void remote_view_tick(void)
{
    remote_drain_listing();
    remote_drain_downloads();
}

static void remote_start_browse(int conn_idx)
{
    g_remote_selected_conn = conn_idx;
    g_remote_mode = 3;
    g_remote_selected = 0;

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

    show_status_message(i18n_get("general.connecting"));
    render_settings_content();
    refresh();

    if (remote_cache_start_listing(conn, conn->base_path) != 0) {
        g_remote_check_pending = 0;
        show_status_message(i18n_get("general.loading"));
    }
}

/* 打开远程目录：前端下载到本地缓存，逐首交给核心播放。
 * 目录列取与下载都在后台线程进行，本函数立刻返回，用户可继续操作。 */
static void remote_open_directory(void)
{
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= remote_store_count()) return;
    const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);

    if (remote_cache_start_session(conn, g_remote_current_path, 1) != 0) {
        show_status_message(i18n_get("general.loading"));
        return;
    }


    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %s", i18n_get("general.loading"), conn->name);
    show_status_message(msg);
    exit_current_view();
}

/* ============================================================
 * Remote device — form (add / edit)
 * ============================================================ */

static int remote_form_field_count(void)
{
    return (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) ? 8 : 7;
}

static void remote_form_field_label(int field_idx, char *buf, size_t size)
{
    switch (field_idx) {
        case 0: snprintf(buf, size, "%s:", i18n_get("remote.field.name")); break;
        case 1: snprintf(buf, size, "%s:", i18n_get("remote.field.protocol")); break;
        case 2: snprintf(buf, size, "%s:", i18n_get("remote.field.host")); break;
        case 3: snprintf(buf, size, "%s:", i18n_get("remote.field.port")); break;
        case 4: snprintf(buf, size, "%s:", i18n_get("remote.field.username")); break;
        case 5: snprintf(buf, size, "%s:", i18n_get("remote.field.password")); break;
        case 6:
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP)
                snprintf(buf, size, "%s:", i18n_get("remote.field.private_key"));
            else
                snprintf(buf, size, "%s:", i18n_get("remote.field.base_path"));
            break;
        case 7: snprintf(buf, size, "%s:", i18n_get("remote.field.base_path")); break;
    }
}

static void remote_form_value_text(int field_idx, char *buf, size_t size)
{
    const RemoteConnectionConfig *rc = &g_remote_form_config;
    buf[0] = '\0';
    switch (field_idx) {
        case 0: snprintf(buf, size, "%s", rc->name); break;
        case 1: snprintf(buf, size, "%s", remote_protocol_name(rc->protocol)); break;
        case 2: snprintf(buf, size, "%s", rc->host); break;
        case 3: if (rc->port > 0) snprintf(buf, size, "%d", rc->port); break;
        case 4: snprintf(buf, size, "%s", rc->username); break;
        case 5:
            if (rc->password[0]) {
                int n = (int)strlen(rc->password);
                if (n > 50) n = 50;
                memset(buf, '*', (size_t)n);
                buf[n] = '\0';
            }
            break;
        case 6:
            if (rc->protocol == REMOTE_PROTOCOL_SFTP)
                snprintf(buf, size, "%s", rc->private_key_path);
            else
                snprintf(buf, size, "%s", rc->base_path);
            break;
        case 7: snprintf(buf, size, "%s", rc->base_path); break;
    }
}

static void remote_form_edit_field(int field_idx)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int content_start_x = (max_x / 4) + 2;
    int form_start_y = 4;

    curs_set(1);
    noecho();

    char label[32];
    remote_form_field_label(field_idx, label, sizeof(label));

    switch (field_idx) {
        case 0:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.name,
                              sizeof(g_remote_form_config.name), 1, 0, 1);
            break;
        case 1: {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", g_remote_form_config.protocol);
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, buf, sizeof(buf), 1, 0, 1);
            int val = atoi(buf);
            if (val >= 0 && val <= 4) g_remote_form_config.protocol = val;
            break;
        }
        case 2:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.host,
                              sizeof(g_remote_form_config.host), 1, 0, 1);
            break;
        case 3: {
            char buf[16];
            if (g_remote_form_config.port > 0)
                snprintf(buf, sizeof(buf), "%d", g_remote_form_config.port);
            else
                buf[0] = '\0';
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, buf, sizeof(buf), 1, 0, 1);
            g_remote_form_config.port = buf[0] ? (int)strtol(buf, NULL, 10) : 0;
            break;
        }
        case 4:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.username,
                              sizeof(g_remote_form_config.username), 1, 0, 1);
            break;
        case 5:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.password,
                              sizeof(g_remote_form_config.password), 1, 1, 1);
            break;
        case 6:
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) {
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, g_remote_form_config.private_key_path,
                                  sizeof(g_remote_form_config.private_key_path), 1, 0, 1);
            } else {
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, g_remote_form_config.base_path,
                                  sizeof(g_remote_form_config.base_path), 1, 0, 1);
                if (!g_remote_form_config.base_path[0])
                    strncpy(g_remote_form_config.base_path, "/",
                            sizeof(g_remote_form_config.base_path) - 1);
            }
            break;
        case 7:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.base_path,
                              sizeof(g_remote_form_config.base_path), 1, 0, 1);
            if (!g_remote_form_config.base_path[0])
                strncpy(g_remote_form_config.base_path, "/",
                        sizeof(g_remote_form_config.base_path) - 1);
            break;
    }

    curs_set(0);
    noecho();
}

static void remote_form_save(void)
{
    if (!g_remote_form_config.name[0]) {
        show_status_message(i18n_get("remote.name_required"));
        rerender_remote_view();
        return;
    }
    if (!g_remote_form_config.host[0]) {
        show_status_message(i18n_get("remote.host_required"));
        rerender_remote_view();
        return;
    }
    if (!g_remote_form_config.base_path[0]) {
        strncpy(g_remote_form_config.base_path, "/",
                sizeof(g_remote_form_config.base_path) - 1);
    }

    if (g_remote_form_editing_idx >= 0) {
        remote_store_upsert(g_remote_form_editing_idx, &g_remote_form_config);
    } else {
        if (remote_store_count() >= MAX_REMOTE_CONNECTIONS) {
            show_status_message(i18n_get("remote.connections_full"));
            rerender_remote_view();
            return;
        }
        remote_store_upsert(-1, &g_remote_form_config);
    }

    remote_store_save();
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
    remote_store_save();
    show_status_message(i18n_get("remote.connection_deleted"));

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

void remote_view_render(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int content_start_x = (max_x / 4) + 2;
    int y = 2;

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    for (int row = 2; row < max_y - 2; row++) {
        move(row, content_start_x);
        clrtoeol();
    }

    if (g_remote_mode == 0) {
        mvprintw(y++, content_start_x, "%s",
                 i18n_get("remote.hint_manage"));
        y++;

        int count = remote_store_count();
        char header[64];
        snprintf(header, sizeof(header), "%s (%d)",
                 i18n_get("remote.saved_connections"), count);
        mvprintw(y++, content_start_x, "%s", header);
        y++;

        for (int i = 0; i < count && y < max_y - 3; i++) {
            const RemoteConnectionConfig *rc = remote_store_get(i);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %-20s [%s] %s",
                     rc->name, remote_protocol_name(rc->protocol), rc->host);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
        }

        y++;
        if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attron(A_REVERSE);
        mvprintw(y++, content_start_x, "  %s", i18n_get("remote.add_new"));
        if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attroff(A_REVERSE);

    } else if (g_remote_mode == 1) {
        int conn_idx = g_remote_selected_conn;
        if (conn_idx < 0 || conn_idx >= remote_store_count()) {
            mvprintw(y++, content_start_x, "%s", i18n_get("general.no_connection"));
            attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
            return;
        }
        const RemoteConnectionConfig *rc = remote_store_get(conn_idx);

        mvprintw(y++, content_start_x, "%s: %s [%s]",
                 i18n_get("remote.connection"), rc->name, remote_protocol_name(rc->protocol));
        y++;

        const char *actions[] = {
            i18n_get("remote.browse"),
            i18n_get("remote.load_to_playlist"),
            i18n_get("remote.refresh"),
            i18n_get("remote.edit"),
            i18n_get("remote.delete")
        };
        int action_count = 5;

        for (int i = 0; i < action_count; i++) {
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %s", actions[i]);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
        }

    } else if (g_remote_mode == 2) {
        int field_count = remote_form_field_count();
        const char *title_fmt = g_remote_form_editing_idx >= 0
            ? i18n_get("remote.edit_hint")
            : i18n_get("remote.add_hint");
        mvprintw(y++, content_start_x, "%s", title_fmt);
        y++;

        char label[32], value[256];
        for (int i = 0; i < field_count && y < max_y - 2; i++) {
            remote_form_field_label(i, label, sizeof(label));
            remote_form_value_text(i, value, sizeof(value));
            move(y, content_start_x);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            printw("  %-14s %s", label, value);
            clrtoeol();
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
            y++;
        }

        if (y < max_y - 2) {
            mvprintw(y, content_start_x, "%s",
                     i18n_get("remote.edit_keys"));
        }

    } else if (g_remote_mode == 3) {
        if (g_remote_selected_conn < 0) {
            mvprintw(y++, content_start_x, "%s", i18n_get("general.no_connection"));
            attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
            return;
        }
        const RemoteConnectionConfig *conn = remote_store_get(g_remote_selected_conn);

        char header[256];
        snprintf(header, sizeof(header), "%s: %s > %s",
                 i18n_get("remote.browse_short"), conn->name,
                 g_remote_current_path[0] ? g_remote_current_path : "/");
        mvprintw(y++, content_start_x, "%s", header);
        y++;

        if (!g_remote_entries) {
            mvprintw(y++, content_start_x, "%s", i18n_get("general.loading"));
        } else if (g_remote_entry_count == 0) {
            mvprintw(y++, content_start_x, "%s", i18n_get("general.empty_dir"));
        } else {
            int display_count = g_remote_entry_count;
            int max_display = max_y - y - 3;
            if (display_count > max_display) display_count = max_display;
            if (g_remote_entry_offset > g_remote_entry_count - display_count)
                g_remote_entry_offset = g_remote_entry_count - display_count;
            if (g_remote_entry_offset < 0) g_remote_entry_offset = 0;

            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %s",
                     i18n_get("remote.load_hint"));
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attroff(A_REVERSE);

            for (int i = g_remote_entry_offset; i < g_remote_entry_count && y < max_y - 3; i++) {
                const RemoteDirEntry *e = &g_remote_entries[i];
                int is_sel = (g_focus_area == FOCUS_CONTENT && g_remote_selected == i + 1);
                if (is_sel) attron(A_REVERSE);
                if (e->is_dir) {
                    mvprintw(y++, content_start_x, "  [%s] %s",
                             i18n_get("remote.dir"), e->name);
                } else {
                    mvprintw(y++, content_start_x, "  %s", e->name);
                }
                if (is_sel) attroff(A_REVERSE);
            }
        }

        if (y < max_y - 2) {
            mvprintw(y, content_start_x, "%s",
                     i18n_get("remote.browse_hint"));
        }
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

/* ============================================================
 * Remote device — input handling
 * ============================================================ */

void remote_view_handle_input(int ch)
{
    int conn_count = remote_store_count();

    if (g_remote_mode == 0) {
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = conn_count;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected > conn_count) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                if (g_remote_selected >= 0 && g_remote_selected < conn_count) {
                    g_remote_selected_conn = g_remote_selected;
                    g_remote_mode = 1;
                    g_remote_selected = 0;
                    rerender_remote_view();
                } else {
                    remote_start_add();
                }
                break;
            case KEY_LEFT:
            case 27:
                remote_go_back();
                break;
        }
    } else if (g_remote_mode == 1) {
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = 4;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected > 4) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                if (g_remote_selected == 0) {
                    remote_start_browse(g_remote_selected_conn);
                } else if (g_remote_selected == 1) {
                    if (g_remote_selected_conn >= 0 && g_remote_selected_conn < remote_store_count()) {
                        const RemoteConnectionConfig *c = remote_store_get(g_remote_selected_conn);
                        strncpy(g_remote_current_path, c->base_path, sizeof(g_remote_current_path) - 1);
                        g_remote_current_path[sizeof(g_remote_current_path) - 1] = '\0';
                        remote_open_directory();
                    }
                } else if (g_remote_selected == 2) {
                    remote_refresh_connection();
                } else if (g_remote_selected == 3) {
                    remote_start_edit(g_remote_selected_conn);
                } else if (g_remote_selected == 4) {
                    remote_delete_connection(g_remote_selected_conn);
                }
                break;
            case KEY_LEFT:
            case 27:
                remote_go_back();
                break;
        }
    } else if (g_remote_mode == 2) {
        int field_count = remote_form_field_count();
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = field_count - 1;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= field_count) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                remote_form_edit_field(g_remote_selected);
                render_settings_content();
                break;
            case '+':
            case '=':
                if (g_remote_selected == 1) {
                    g_remote_form_config.protocol = (g_remote_form_config.protocol + 1) % 5;
                    if (g_remote_selected >= remote_form_field_count())
                        g_remote_selected = remote_form_field_count() - 1;
                    render_settings_content();
                }
                break;
            case '-':
            case '_':
                if (g_remote_selected == 1) {
                    g_remote_form_config.protocol = (g_remote_form_config.protocol + 4) % 5;
                    if (g_remote_selected >= remote_form_field_count())
                        g_remote_selected = remote_form_field_count() - 1;
                    render_settings_content();
                }
                break;
            case 's':
            case 'S':
                remote_form_save();
                break;
            case KEY_LEFT:
            case 27:
                remote_form_cancel();
                break;
        }
    } else if (g_remote_mode == 3) {
        int total_items = 1 + g_remote_entry_count;

        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = total_items - 1;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= total_items) g_remote_selected = 0;
                render_settings_content();
                break;
            case KEY_RIGHT:
                if (g_remote_selected > 0) {
                    int entry_idx = g_remote_selected - 1;
                    if (entry_idx >= 0 && entry_idx < g_remote_entry_count &&
                        g_remote_entries[entry_idx].is_dir) {
                        size_t cur_len = strlen(g_remote_current_path);
                        if (cur_len > 0 && g_remote_current_path[cur_len - 1] != '/') {
                            strncat(g_remote_current_path, "/",
                                    sizeof(g_remote_current_path) - cur_len - 1);
                        }
                        strncat(g_remote_current_path, g_remote_entries[entry_idx].name,
                                sizeof(g_remote_current_path) - strlen(g_remote_current_path) - 1);
                        g_remote_selected = 0;
                        g_remote_entry_offset = 0;
                        remote_refresh_entries();
                        rerender_remote_view();
                    }
                }
                break;
            case 10:
            case ' ':
                if (g_remote_selected == 0) {
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
                                    sizeof(g_remote_current_path) - strlen(g_remote_current_path) - 1);
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
            case KEY_LEFT:
                remote_go_back();
                break;
            case 27:
                remote_go_back();
                break;
        }
    }
}

/* ── 生命周期（main.c 启动 / ui.c 退出） ───────────────────────────── */

void remote_view_init(void)
{
    /* 前端自有的 <configdir>/remote.xml：核心不认识远程音乐源 */
    remote_store_load();
    remote_cache_init();
    /* 启动时按上限淘汰过期缓存（保护集：当前曲目、播放列表内的缓存文件、
     * 收藏与历史引用的路径都不会被删除） */
    remote_evict_protected();
}

void remote_view_shutdown(void)
{
    remote_cache_shutdown();
}
