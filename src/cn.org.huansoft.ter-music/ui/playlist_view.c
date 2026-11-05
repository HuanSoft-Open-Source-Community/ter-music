/**
 * @file playlist_view.c
 * @brief 歌单管理视图 — 歌单列表与歌曲浏览
 *
 * 从 menus.c 拆分而来，负责歌单页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "player/player.h"
#include "ui/dialog.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "config/config.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>

/* ============================================================
 * Playlist view internal state
 *
 * 全部为本文件私有；视图切换由 menus.c 调用 reset_playlist_view()，
 * 歌单列表/曲目列表的滚动偏移也在这里统一复位。
 * ============================================================ */

static int g_playlist_view_mode = 0;           /* 0=list, 1=tracks */
static int g_playlist_selected_playlist = -1;
static int g_playlist_track_offset = 0;
static int g_playlist_content_offset = 0;      /* 歌单列表滚动偏移 */

/* ============================================================
 * Playlist view rendering
 * ============================================================ */

/* 渲染期记录的列表几何。本视图有两种列表：模式 0 是歌单列表，
 * 模式 1 是某个歌单的曲目，二者的行号含义完全不同，所以各记一组。
 * 每帧开头先全部作废，只有真正画出列表的那个分支填回自己那组，
 * 命中判定按当前模式取"当帧有效的那组"，绝不把歌单列表的行当成曲目的行。
 * top<0 表示这一帧没画列表（空列表或选中的歌单已失效）。 */
static int g_pl_list_top = -1;
static int g_pl_list_visible = 0;
static int g_pl_tracks_top = -1;
static int g_pl_tracks_visible = 0;

void render_playlist_manager_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

    /* 本帧先作废两组几何，避免上一模式的旧行号被误用 */
    g_pl_list_top = -1;
    g_pl_list_visible = 0;
    g_pl_tracks_top = -1;
    g_pl_tracks_visible = 0;

    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));

    if (g_playlist_view_mode == 0) {
        menu_content_clear_rows(&area, 2, max_y - 3);

        char header[256];
        snprintf(header, sizeof(header), i18n_get("playlist_mgr.playlists_fmt"),
                 g_playlist_manager.count);
        menu_content_puts(&area, start_y, header);
        menu_content_puts(&area, start_y + 1, "----------------------------------------");
        start_y += 3;

        if (g_playlist_manager.count == 0) {
            menu_content_puts(&area, start_y, i18n_get("playlist_mgr.no_playlists"));
            menu_content_puts(&area, start_y + 1, i18n_get("playlist_mgr.create_hint"));
        } else {
            int visible_lines = max_y - start_y - 3;
            if (visible_lines < 1) visible_lines = 1;

            /* 歌单列表按选中项推导滚动偏移；渲染循环与鼠标命中共用
             * g_playlist_content_offset，避免“画到哪”与“点到哪”漂移。 */
            if (g_content_selected_idx >= g_playlist_manager.count) {
                g_content_selected_idx = g_playlist_manager.count - 1;
            }
            if (g_content_selected_idx < 0) g_content_selected_idx = 0;
            if (g_content_selected_idx < g_playlist_content_offset) {
                g_playlist_content_offset = g_content_selected_idx;
            } else if (g_content_selected_idx >=
                       g_playlist_content_offset + visible_lines) {
                g_playlist_content_offset =
                    g_content_selected_idx - visible_lines + 1;
            }
            if (g_playlist_content_offset < 0) {
                g_playlist_content_offset = 0;
            }
            if (g_playlist_content_offset > g_playlist_manager.count - visible_lines) {
                g_playlist_content_offset = g_playlist_manager.count - visible_lines;
            }
            if (g_playlist_content_offset < 0) {
                g_playlist_content_offset = 0;
            }

            g_pl_list_top = start_y;
            g_pl_list_visible = visible_lines;

            for (int i = 0; i < visible_lines; i++) {
                int idx = g_playlist_content_offset + i;
                if (idx >= g_playlist_manager.count) break;
                UserPlaylist *pl = &g_playlist_manager.playlists[idx];
                char display_name[MAX_PLAYLIST_NAME_LEN + 8];
                utf8_str_pad(display_name, sizeof(display_name), pl->name, 30);

                char line[MAX_PLAYLIST_NAME_LEN + 128];
                snprintf(line, sizeof(line), i18n_get("playlist_mgr.track_count_fmt"),
                         display_name, pl->track_count);

                if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                    attron(A_REVERSE);
                    menu_content_puts(&area, start_y + i, line);
                    attroff(A_REVERSE);
                } else {
                    menu_content_puts(&area, start_y + i, line);
                }
            }

            int bottom_y = max_y - 3;
            menu_content_puts(&area, bottom_y, i18n_get("playlist_mgr.hint_manage"));
        }
    } else {
        if (g_playlist_selected_playlist >= 0 &&
            g_playlist_selected_playlist < g_playlist_manager.count) {
            UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];

            menu_content_clear_rows(&area, 2, max_y - 3);

            char header[256];
            snprintf(header, sizeof(header), i18n_get("playlist_mgr.playlist_tracks_fmt"),
                     pl->name, pl->track_count);
            menu_content_puts(&area, start_y, header);
            menu_content_puts(&area, start_y + 1, "----------------------------------------");
            start_y += 3;

            if (pl->track_count == 0) {
                menu_content_puts(&area, start_y, i18n_get("playlist_mgr.playlist_empty"));
                menu_content_puts(&area, start_y + 1, i18n_get("playlist_mgr.add_hint"));
            } else {
                int visible_lines = max_y - start_y - 3;

                /* 渲染期记下曲目列表几何（滚动偏移 g_playlist_track_offset 在
                 * 下面这段钳制里已经和选中项同步过，命中判定只需加上行号）。 */
                g_pl_tracks_top = start_y;
                g_pl_tracks_visible = visible_lines;

                if (g_content_selected_idx >= pl->track_count) {
                    g_content_selected_idx = pl->track_count - 1;
                }
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;

                if (g_content_selected_idx < g_playlist_track_offset) {
                    g_playlist_track_offset = g_content_selected_idx;
                } else if (g_content_selected_idx >= g_playlist_track_offset + visible_lines) {
                    g_playlist_track_offset = g_content_selected_idx - visible_lines + 1;
                }

                for (int i = 0; i < visible_lines &&
                     (g_playlist_track_offset + i) < pl->track_count; i++) {
                    int idx = g_playlist_track_offset + i;
                    Track *t = &pl->tracks[idx];

                    char truncated_title[MAX_META_LEN];
                    char display_title[MAX_META_LEN + 32];
                    int title_width = max_x - menu_width - 30;
                    utf8_str_truncate(truncated_title, t->title,
                                      title_width > 0 ? title_width : 30);
                    utf8_str_pad(display_title, sizeof(display_title),
                                 truncated_title, title_width > 0 ? title_width : 30);

                    char line[MAX_META_LEN * 2 + 64];
                    snprintf(line, sizeof(line), " %s - %s", display_title, t->artist);

                    if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                        attron(A_REVERSE);
                        menu_content_puts(&area, start_y + i, line);
                        attroff(A_REVERSE);
                    } else {
                        menu_content_puts(&area, start_y + i, line);
                    }
                }

                int bottom_y = max_y - 3;
                menu_content_puts(&area, bottom_y, i18n_get("playlist_mgr.hint_tracks"));
            }
        }
    }

    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    repaint_status_message();
    refresh();
}

/* ============================================================
 * Playlist view input handling
 * ============================================================ */

/* 新建歌单：侧栏"新建"动作与右键菜单共用同一段输入框逻辑。 */
static void playlist_view_create(void)
{
    noecho();
    curs_set(1);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int menu_width = max_x / 4;

    const char *create_prompt = i18n_get("playlist_mgr.create_prompt");
    char name[MAX_PLAYLIST_NAME_LEN];
    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                      create_prompt, name, sizeof(name), 1, 0, 0);

    noecho();
    curs_set(0);

    if (strlen(name) > 0) {
        int result = create_user_playlist(name);
        if (result == 0) {
            show_status_message(i18n_get("playlist_mgr.created"));
        } else if (result == -2) {
            show_status_message(i18n_get("playlist_mgr.limit_reached"));
        } else {
            show_status_message(i18n_get("playlist_mgr.create_failed"));
        }
    }

    render_menu_frame(i18n_get("menu.playlists"));
    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
    render_playlist_manager_content();
    render_menu_hint_bar();
}

/* 动作入口：键盘解析出的动作与鼠标滚轮/单击共用同一条路径。
 * （鼠标绝不合成按键码：用户可改键，合成按键会在改键后失灵或做错事。） */
int handle_playlist_action(ActionId action)
{
    switch (action) {
        case ACTION_PLAYLIST_VIEW_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = PLAYLIST_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            } else {
                if (g_playlist_view_mode == 0) {
                    g_content_selected_idx--;
                    if (g_content_selected_idx < 0)
                        g_content_selected_idx = g_playlist_manager.count - 1;
                    if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                } else {
                    g_content_selected_idx--;
                    if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                    if (g_playlist_selected_playlist >= 0) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                    }
                }
                render_playlist_manager_content();
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= PLAYLIST_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            } else {
                if (g_playlist_view_mode == 0) {
                    g_content_selected_idx++;
                    if (g_content_selected_idx >= g_playlist_manager.count) g_content_selected_idx = 0;
                } else {
                    g_content_selected_idx++;
                    if (g_playlist_selected_playlist >= 0) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                    }
                }
                render_playlist_manager_content();
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                render_playlist_manager_content();
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_playlist_view_mode == 1) {
                    g_playlist_view_mode = 0;
                    g_content_selected_idx = 0;
                    render_playlist_manager_content();
                } else {
                    g_focus_area = FOCUS_SIDEBAR;
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                }
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_ACTIVATE:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                } else if (g_menu_selected_idx == 1) {
                    playlist_view_create();
                } else if (g_menu_selected_idx == PLAYLIST_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_playlist_view_mode == 0) {
                    if (g_content_selected_idx >= 0 &&
                        g_content_selected_idx < g_playlist_manager.count) {
                        g_playlist_selected_playlist = g_content_selected_idx;
                        g_playlist_view_mode = 1;
                        g_content_selected_idx = 0;
                        g_playlist_track_offset = 0;
                        render_playlist_manager_content();
                    }
                } else {
                    if (g_playlist_selected_playlist >= 0 &&
                        g_playlist_selected_playlist < g_playlist_manager.count) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= 0 &&
                            g_content_selected_idx < pl->track_count) {
                            Track *t = &pl->tracks[g_content_selected_idx];
                            int found = playlist_find_track_index_by_path(t->path);
                            /* 直播不在当前列表的歌单曲目（Task 5.2）：追加到前端
                             * 内容列表尾部后再按内容下标播放（ui_play_track_index 会
                             * 在需要时整表重推给核心），复用既有内容→队列桥。 */
                            if (found < 0 && append_playlist(t->path) > 0) {
                                found = playlist_find_track_index_by_path(t->path);
                            }
                            if (found >= 0) {
                                ui_play_track_index(found);
                                exit_current_view();
                            } else {
                                show_status_message(i18n_get("playlist_mgr.track_not_in_queue"));
                            }
                        }
                    }
                }
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_DELETE:
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_playlist_view_mode == 0) {
                    if (g_content_selected_idx >= 0 &&
                        g_content_selected_idx < g_playlist_manager.count) {
                        delete_user_playlist(g_content_selected_idx);
                        if (g_content_selected_idx >= g_playlist_manager.count) {
                            g_content_selected_idx = g_playlist_manager.count - 1;
                        }
                        show_status_message(i18n_get("playlist_mgr.deleted"));
                        render_playlist_manager_content();
                    }
                } else {
                    if (g_playlist_selected_playlist >= 0 &&
                        g_playlist_selected_playlist < g_playlist_manager.count) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= 0 &&
                            g_content_selected_idx < pl->track_count &&
                            remove_track_from_playlist(g_playlist_selected_playlist,
                                                       g_content_selected_idx) == 0) {
                            if (g_content_selected_idx >= pl->track_count) {
                                g_content_selected_idx = pl->track_count - 1;
                            }
                            show_status_message(i18n_get("playlist_mgr.track_removed"));
                        } else {
                            show_status_message(i18n_get("playlist_mgr.track_remove_failed"));
                        }
                        render_playlist_manager_content();
                    }
                }
            }
            return 1;

        case ACTION_PLAYLIST_VIEW_RENAME:
            if (g_focus_area == FOCUS_CONTENT && g_playlist_view_mode == 0) {
                if (g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_playlist_manager.count) {
                    noecho();
                    curs_set(1);

                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    int menu_width = max_x / 4;

                    const char *rename_prompt = i18n_get("playlist_mgr.rename_prompt");
                    char new_name[MAX_PLAYLIST_NAME_LEN];
                    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                                      rename_prompt, new_name, sizeof(new_name), 1, 0, 0);

                    noecho();
                    curs_set(0);

                    if (strlen(new_name) > 0) {
                        int result = rename_user_playlist(g_content_selected_idx, new_name);
                        if (result == 0) {
                            show_status_message(i18n_get("playlist_mgr.renamed"));
                        } else {
                            show_status_message(i18n_get("playlist_mgr.rename_failed"));
                        }
                    }

                    render_menu_frame(i18n_get("menu.playlists"));
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                    render_menu_hint_bar();
                }
            }
            return 1;

        default:
            return 0;
    }
}

int handle_playlist_input(int ch)
{
    /* ESC in track view mode goes back to playlist list */
    if (ch == 27) {
        if (g_playlist_view_mode == 1) {
            g_playlist_view_mode = 0;
            g_content_selected_idx = g_playlist_selected_playlist;
            render_playlist_manager_content();
            return 1;
        }
        exit_current_view();
        return 1;
    }

    return handle_playlist_action(keymap_resolve(CTX_PLAYLIST_VIEW, ch));
}

/* ============================================================
 * Playlist view mouse handling
 * ============================================================ */

/* 内容区某屏幕行在当前模式下对应哪一条：模式 0 是歌单、模式 1 是该歌单的曲目。
 * 用的是当帧真正画出来的那一组几何，两种行号绝不会互相冒用；不在列表上返回 -1。 */
static int playlist_index_at_row(int screen_row)
{
    if (g_playlist_view_mode == 0) {
        if (g_pl_list_top < 0 || g_pl_list_visible <= 0) return -1;

        int line = screen_row - g_pl_list_top;
        if (line < 0 || line >= g_pl_list_visible) return -1;

        /* 模式 0 与渲染使用同一份滚动偏移，行号不是下标。 */
        int idx = g_playlist_content_offset + line;
        if (idx < 0 || idx >= g_playlist_manager.count) return -1;
        return idx;
    }

    if (g_pl_tracks_top < 0 || g_pl_tracks_visible <= 0) return -1;

    int line = screen_row - g_pl_tracks_top;
    if (line < 0 || line >= g_pl_tracks_visible) return -1;

    if (g_playlist_selected_playlist < 0 ||
        g_playlist_selected_playlist >= g_playlist_manager.count) return -1;
    const UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];

    int idx = g_playlist_track_offset + line;
    if (idx < 0 || idx >= pl->track_count) return -1;
    return idx;
}

/* ============================================================
 * Right-click context menu
 * ============================================================ */

static void playlist_create_callback(const UIContextMenuContext *ctx,
                                     intptr_t value)
{
    (void)ctx;
    (void)value;
    playlist_view_create();
}

void playlist_view_context_menu_build(UIContextMenuLevel *level,
                                      const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.playlist"));

    if (ctx->area == UI_CTX_AREA_SIDEBAR) {
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_ACTIVATE,
                               ctx->valid, 0);
        if (ctx->index == 1) {
            ui_ctx_menu_add_callback(level, i18n_get("context.playlist.create"),
                                     "playlist.create", playlist_create_callback,
                                     0, 1, 0);
        }
    } else if (g_playlist_view_mode == 0) {
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_ACTIVATE,
                               ctx->valid, 0);
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_RENAME,
                               ctx->valid, 0);
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_DELETE,
                               ctx->valid, 0);
        ui_ctx_menu_add_callback(level, i18n_get("context.playlist.create"),
                                 "playlist.create", playlist_create_callback,
                                 0, 1, 0);
    } else {
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_ACTIVATE,
                               ctx->valid, 0);
        ui_ctx_menu_add_action_label(level,
                                     i18n_get("context.playlist.remove_track"),
                                     ACTION_PLAYLIST_VIEW_DELETE,
                                     ctx->valid, 0);
        ui_ctx_menu_add_action(level, ACTION_PLAYLIST_VIEW_LEFT, 1, 0);
    }

    ui_ctx_menu_add_separator(level);
    ui_context_menu_add_pages_submenu(level);
}

int playlist_view_context_dispatch(ActionId action,
                                   const UIContextMenuContext *ctx)
{
    if (!ctx) {
        return 0;
    }
    return handle_playlist_action(action);
}

static void playlist_view_open_context_menu(const UIMouseEvent *ev,
                                            const UIMouseDecision *d)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_PLAYLIST;
    ctx.target = d->target;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        ctx.area = UI_CTX_AREA_SIDEBAR;
        if (d->row >= 0 && d->row < PLAYLIST_ITEM_COUNT) {
            g_focus_area = FOCUS_SIDEBAR;
            g_menu_selected_idx = d->row;
            ctx.valid = 1;
            ctx.index = d->row;
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items,
                                PLAYLIST_ITEM_COUNT);
            render_playlist_manager_content();
        }
    } else {
        ctx.area = UI_CTX_AREA_CONTENT;
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items,
                                PLAYLIST_ITEM_COUNT);
        }
        int idx = playlist_index_at_row(ev->y);
        if (idx >= 0) {
            g_content_selected_idx = idx;
            ctx.valid = 1;
            ctx.index = idx;
            render_playlist_manager_content();
        }
    }

    (void)ui_context_menu_open(playlist_view_context_menu_build,
                               playlist_view_context_dispatch,
                               &ctx, ev->y, ev->x);
}

int playlist_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        playlist_view_open_context_menu(ev, d);
        return 1;
    }

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (d->row < 0 || d->row >= PLAYLIST_ITEM_COUNT) return 1;

        /* 滚轮落在侧栏这一列：移动侧栏选中项。动作内部按 g_focus_area 分流，
         * 所以先把焦点带到侧栏（否则滚的是内容列）。 */
        if (ev->wheel) {
            g_focus_area = FOCUS_SIDEBAR;
            return handle_playlist_action(ev->wheel < 0 ? ACTION_PLAYLIST_VIEW_UP
                                                         : ACTION_PLAYLIST_VIEW_DOWN);
        }
        if (!ev->click) return 1;

        g_focus_area = FOCUS_SIDEBAR;
        g_menu_selected_idx = d->row;
        render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
        render_playlist_manager_content();

        /* 单击即选中并执行：进入列表 / 新建歌单 / 返回主界面 */
        return handle_playlist_action(ACTION_PLAYLIST_VIEW_ACTIVATE);
    }

    /* 内容区 */
    if (ev->wheel) {
        /* 列表高亮只在焦点位于内容区时画得出来，
         * 所以滚轮要先把焦点带到指针所在这一列，再走键盘那条上下动作。 */
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
        }
        return handle_playlist_action(ev->wheel < 0 ? ACTION_PLAYLIST_VIEW_UP
                                                    : ACTION_PLAYLIST_VIEW_DOWN);
    }
    if (!ev->click) return 1;

    /* 内容区几何记的是**屏幕行**（menu_content_puts 的行号就是 stdscr 行号），
     * 而 d->row 是内容区矩形的本地行号（该矩形 top=1），两者差一行，
     * 所以这里用 ev->y；侧栏那一路才用 d->row（侧栏 top=2 与 start_y 同源）。 */
    const int idx = playlist_index_at_row(ev->y);
    if (idx < 0) return 1;   /* 标题行、分隔线、底部提示行：只认领不动作 */

    /* 把焦点带到指针这一列：一是选中高亮只在焦点为内容区时才画得出来，
     * 二是 ACTION_PLAYLIST_VIEW_ACTIVATE 自己按 g_focus_area 分流，
     * 焦点若还留在侧栏，点击会去执行侧栏那一条（新建歌单/返回）而不是进入歌单。 */
    if (g_focus_area != FOCUS_CONTENT) {
        g_focus_area = FOCUS_CONTENT;
        render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
    }
    g_content_selected_idx = idx;
    render_playlist_manager_content();

    return handle_playlist_action(ACTION_PLAYLIST_VIEW_ACTIVATE);
}

/* Reset internal state for view switching */
void reset_playlist_view(void)
{
    g_playlist_view_mode = 0;
    g_playlist_selected_playlist = -1;
    g_playlist_track_offset = 0;
    g_playlist_content_offset = 0;
}
