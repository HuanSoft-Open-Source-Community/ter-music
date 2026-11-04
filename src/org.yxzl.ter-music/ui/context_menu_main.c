/**
 * @file context_menu_main.c
 * @brief 主视图列表窗格的右键菜单（含文件/队列模式切换）
 *
 * 菜单只负责"在指针位置列出操作"，执行全部复用 handle_main_action() 与
 * ui_toggle_playlist_tab_mode()，与键盘走同一条路径。位置敏感的队列操作
 * 携带打开时的队列版本做竞态校验：列表一变就提示并拒绝，绝不对错行下手。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/context_menu.h"

#include "i18n/i18n.h"
#include "keymap/actions.h"
#include "library/browser/browser.h"
#include "logger/logger.h"
#include "player/player.h"
#include "playlist/playlist.h"
#include "ui/menu_internal.h"
#include "ui/menus.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

/* 列表窗格当前模式（定义在 ui/playlist_render.c）。 */
extern int g_playlist_tab_mode;

/* ============================================================
 * 状态提示（主视图走控制区状态行，其余走页面状态）
 * ============================================================ */

static void menu_status(const char *text)
{
    if (!text) {
        return;
    }
    if (g_current_view == VIEW_MAIN) {
        update_controls_status(text);
    } else {
        show_status_message(text);
    }
}

static const char *action_debug_name(ActionId action)
{
    const KeyActionDef *def = actions_def(action);
    return (def && def->id) ? def->id : "?";
}

/* ============================================================
 * 回调类条目
 * ============================================================ */

static void noop_callback(const UIContextMenuContext *ctx, intptr_t value)
{
    (void)ctx;
    (void)value;
}

static void mode_toggle_callback(const UIContextMenuContext *ctx, intptr_t value)
{
    (void)ctx;
    (void)value;
    if (!ui_toggle_playlist_tab_mode()) {
        menu_status(i18n_get("context.stale"));
    }
}

static void add_to_playlist_callback(const UIContextMenuContext *ctx,
                                     intptr_t value)
{
    if (!ctx || !ctx->valid || ctx->track_index < 0) {
        ui_context_menu_report_stale("playlist.add");
        return;
    }
    if (value < 0 || (int)value >= g_playlist_manager.count) {
        ui_context_menu_report_stale("playlist.add");
        return;
    }

    Track track;
    memset(&track, 0, sizeof(track));
    if (get_track_metadata(ctx->track_index, &track) != 0) {
        ui_context_menu_report_stale("playlist.add");
        return;
    }

    int rc = add_track_to_playlist((int)value, &track);
    if (rc == 0) {
        menu_status(i18n_get("playlist_mgr.added_to_playlist"));
    } else if (rc == -3) {
        menu_status(i18n_get("playlist_mgr.playlist_full"));
    } else {
        menu_status(i18n_get("playlist_mgr.already_in_playlist"));
    }
}

/* ============================================================
 * 子菜单：队列 / 播放控制 / 播放模式 / 加入歌单
 * ============================================================ */

static void queue_menu_build(UIContextMenuLevel *level,
                             const UIContextMenuContext *ctx)
{
    ui_ctx_menu_set_title(level, i18n_get("context.menu.queue"));

    const int queue_count = player_queue_count();
    const int has_track = ctx && ctx->valid && ctx->track_index >= 0;
    const int has_playing = player_track_index() >= 0;

    ui_ctx_menu_add_action(level, ACTION_QUEUE_INSERT_NEXT,
                           has_track && has_playing && queue_count > 0, 0);
    ui_ctx_menu_add_action(level, ACTION_QUEUE_APPEND, has_track, 0);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action(level, ACTION_QUEUE_REMOVE,
                           ctx && ctx->valid && queue_count > 0, 0);
    ui_ctx_menu_add_action(level, ACTION_QUEUE_MOVE_UP,
                           ctx && ctx->valid && queue_count > 1, 0);
    ui_ctx_menu_add_action(level, ACTION_QUEUE_MOVE_DOWN,
                           ctx && ctx->valid && queue_count > 1, 0);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action(level, ACTION_QUEUE_CLEAR, queue_count > 0, 0);
}

static void playmode_menu_build(UIContextMenuLevel *level,
                                const UIContextMenuContext *ctx)
{
    (void)ctx;
    ui_ctx_menu_set_title(level, i18n_get("context.menu.playmode"));
    const int mode = (int)player_play_mode();
    ui_ctx_menu_add_action(level, ACTION_PLAYMODE_SEQUENTIAL,
                           mode == PLAY_MODE_SEQUENTIAL, 1);
    ui_ctx_menu_add_action(level, ACTION_PLAYMODE_SINGLE,
                           mode == PLAY_MODE_SINGLE_REPEAT, 1);
    ui_ctx_menu_add_action(level, ACTION_PLAYMODE_LIST,
                           mode == PLAY_MODE_LIST_REPEAT, 1);
    ui_ctx_menu_add_action(level, ACTION_PLAYMODE_SHUFFLE,
                           mode == PLAY_MODE_SHUFFLE_REPEAT, 1);
    ui_ctx_menu_add_action(level, ACTION_PLAYMODE_FOLDER,
                           mode == PLAY_MODE_FOLDER_SEQUENTIAL, 1);
}

static void playback_menu_build(UIContextMenuLevel *level,
                                const UIContextMenuContext *ctx)
{
    (void)ctx;
    ui_ctx_menu_set_title(level, i18n_get("context.menu.playback"));
    const int can_skip = playlist_is_loaded() && !g_lyric_cursor_mode;
    ui_ctx_menu_add_action(level, ACTION_TRACK_PREV, can_skip, 0);
    ui_ctx_menu_add_action(level, ACTION_TRACK_NEXT, can_skip, 0);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action(level, ACTION_VOLUME_UP, 1, 0);
    ui_ctx_menu_add_action(level, ACTION_VOLUME_DOWN, 1, 0);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action(level, ACTION_FOCUS_CONTROLS, 1, 0);
    ui_ctx_menu_add_action(level, ACTION_FOCUS_LIST, 1, 0);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_submenu(level, i18n_get("context.menu.playmode"),
                            playmode_menu_build, 1);
}

static void playlist_menu_build(UIContextMenuLevel *level,
                                const UIContextMenuContext *ctx)
{
    ui_ctx_menu_set_title(level, i18n_get("context.menu.playlist"));

    if (!ctx || !ctx->valid || ctx->track_index < 0) {
        ui_ctx_menu_add_callback(level, i18n_get("context.playlist.none"),
                                 "playlist.none", noop_callback, 0, 0, 0);
    } else if (g_playlist_manager.count <= 0) {
        ui_ctx_menu_add_callback(level, i18n_get("context.playlist.none"),
                                 "playlist.none", noop_callback, 0, 0, 0);
    } else {
        for (int i = 0; i < g_playlist_manager.count; i++) {
            ui_ctx_menu_add_callback(level,
                                     g_playlist_manager.playlists[i].name,
                                     "playlist.add", add_to_playlist_callback,
                                     (intptr_t)i, 1, 0);
        }
    }

    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action_label(level, i18n_get("context.playlist.manage"),
                                 ACTION_VIEW_PLAYLIST, 1, 0);
}

/* ============================================================
 * 根菜单
 * ============================================================ */

void main_list_context_menu_build(UIContextMenuLevel *level,
                                  const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("context.title.list"));

    const int has_track = ctx->valid && ctx->track_index >= 0;
    const int has_row = ctx->valid;

    ui_ctx_menu_add_action(level, ACTION_LIST_ACTIVATE, has_row, 0);

    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_submenu(level, i18n_get("context.menu.queue"),
                            queue_menu_build, 1);
    ui_ctx_menu_add_action(level, ACTION_FAVORITE_TOGGLE, has_track, 0);
    ui_ctx_menu_add_submenu(level, i18n_get("context.menu.playlist"),
                            playlist_menu_build, 1);
    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_action(level, ACTION_FOLDER_OPEN, 1, 0);
    ui_ctx_menu_add_action(level, ACTION_FOLDER_APPEND, 1, 0);
    ui_ctx_menu_add_action(level, ACTION_SEARCH_OPEN, 1, 0);
    ui_ctx_menu_add_action(level, ACTION_HISTORY_SHOW,
                           g_play_history.count > 0 && !g_lyric_cursor_mode, 0);

    ui_ctx_menu_add_separator(level);
    /* 必需条目：文件模式 ↔ 队列模式。标签随当前模式变化。 */
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        ui_ctx_menu_add_callback(level, i18n_get("context.mode.to_file"),
                                 "mode.toggle", mode_toggle_callback, 0,
                                 playlist_is_loaded(), 1);
    } else {
        ui_ctx_menu_add_callback(level, i18n_get("context.mode.to_queue"),
                                 "mode.toggle", mode_toggle_callback, 0,
                                 playlist_is_loaded(), 0);
    }
    ui_ctx_menu_add_action(level, ACTION_LIBRARY_TOGGLE, 1,
                           g_library_state.active ? 1 : 0);

    ui_ctx_menu_add_separator(level);
    ui_ctx_menu_add_submenu(level, i18n_get("context.menu.playback"),
                            playback_menu_build, 1);
    ui_context_menu_add_pages_submenu(level);
}

/* ============================================================
 * 执行
 * ============================================================ */

static int action_is_queue_position_sensitive(ActionId action)
{
    switch (action) {
        case ACTION_QUEUE_REMOVE:
        case ACTION_QUEUE_MOVE_UP:
        case ACTION_QUEUE_MOVE_DOWN:
            return 1;
        default:
            return 0;
    }
}

int main_list_context_dispatch(ActionId action, const UIContextMenuContext *ctx)
{
    if (!ctx) {
        return 0;
    }

    /* 队列被别的前端改过（版本变化）时，位置敏感动作拒绝执行。 */
    if ((action_is_queue_position_sensitive(action) ||
         (action == ACTION_LIST_ACTIVATE && ctx->queue_mode)) &&
        player_queue_revision() != ctx->queue_revision) {
        ui_context_menu_report_stale(action_debug_name(action));
        return 1;
    }

    int rc = handle_main_action(action);
    if (rc == 2) {
        /* 菜单不含退出项；真收到也按已处理返回，交由调用方决定。 */
        log_debug("context_menu", "main action requested quit");
    }
    return 1;
}
