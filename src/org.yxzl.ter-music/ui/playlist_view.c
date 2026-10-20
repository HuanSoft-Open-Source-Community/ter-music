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
 * g_playlist_view_mode / g_playlist_selected_playlist /
 * g_playlist_track_offset are non-static (extern in
 * menu_internal.h) so menus.c can reset them in switch_to_view
 * and handle ESC-in-track-view navigation.
 * ============================================================ */

int g_playlist_view_mode = 0;           /* 0=list, 1=tracks */
int g_playlist_selected_playlist = -1;
int g_playlist_track_offset = 0;

/* ============================================================
 * Playlist view rendering
 * ============================================================ */

void render_playlist_manager_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

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

            for (int i = 0; i < visible_lines && i < g_playlist_manager.count; i++) {
                UserPlaylist *pl = &g_playlist_manager.playlists[i];
                char display_name[MAX_PLAYLIST_NAME_LEN + 8];
                utf8_str_pad(display_name, sizeof(display_name), pl->name, 30);

                char line[MAX_PLAYLIST_NAME_LEN + 128];
                snprintf(line, sizeof(line), i18n_get("playlist_mgr.track_count_fmt"),
                         display_name, pl->track_count);

                if (i == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
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
    refresh();
}

/* ============================================================
 * Playlist view input handling
 * ============================================================ */

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

    ActionId action = keymap_resolve(CTX_PLAYLIST_VIEW, ch);

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
                        remove_track_from_playlist(g_playlist_selected_playlist,
                                                   g_content_selected_idx);
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                        show_status_message(i18n_get("playlist_mgr.track_removed"));
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

/* Reset internal state for view switching */
void reset_playlist_view(void)
{
    g_playlist_view_mode = 0;
    g_playlist_selected_playlist = -1;
    g_playlist_track_offset = 0;
}
