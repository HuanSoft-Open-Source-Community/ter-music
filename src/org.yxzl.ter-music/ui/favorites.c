/**
 * @file favorites.c
 * @brief 收藏视图 — 收藏歌曲浏览和管理
 *
 * 从 menus.c 拆分而来，负责收藏页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>

/* ============================================================
 * Favorites view rendering
 * ============================================================ */

void render_favorites_content(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));

    mvprintw(start_y, content_start_x,
             i18n_get("favorites.title_fmt"),
             g_favorites.count);
    mvprintw(start_y + 1, content_start_x, "----------------------------------------");
    start_y += 3;

    if (g_favorites.count == 0) {
        mvprintw(start_y, content_start_x, "%s",
                 i18n_get("favorites.empty"));
        mvprintw(start_y + 1, content_start_x, "%s",
                 i18n_get("favorites.hint_f"));
    } else {
        int visible_lines = max_y - start_y - 2;

        if (g_content_selected_idx >= g_favorites.count) {
            g_content_selected_idx = g_favorites.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;

        if (g_content_selected_idx < g_favorites_content_offset) {
            g_favorites_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_favorites_content_offset + visible_lines) {
            g_favorites_content_offset = g_content_selected_idx - visible_lines + 1;
        }

        for (int i = 0; i < visible_lines && (g_favorites_content_offset + i) < g_favorites.count; i++) {
            int idx = g_favorites_content_offset + i;
            Track *t = &g_favorites.tracks[idx];

            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            char display_title[MAX_META_LEN + 32];
            char display_artist[MAX_META_LEN + 32];
            int title_width  = (max_x - menu_width - 10) * 3 / 5;
            int artist_width = (max_x - menu_width - 10) * 2 / 5;

            utf8_str_truncate(truncated_title, t->title, title_width > 0 ? title_width : 30);
            utf8_str_truncate(truncated_artist, t->artist, artist_width > 0 ? artist_width : 20);
            utf8_str_pad(display_title, sizeof(display_title), truncated_title, title_width > 0 ? title_width : 30);
            utf8_str_pad(display_artist, sizeof(display_artist), truncated_artist, artist_width > 0 ? artist_width : 20);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                mvprintw(start_y + i, content_start_x, " %s - %s", display_title, display_artist);
                attroff(A_REVERSE);
            } else {
                mvprintw(start_y + i, content_start_x, " %s - %s", display_title, display_artist);
            }
        }

        int bottom_y = max_y - 3;
        mvprintw(bottom_y, content_start_x, "%s",
                 i18n_get("favorites.hint_manage"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    refresh();
}

/* ============================================================
 * Favorites view input handling
 * ============================================================ */

void handle_favorites_input(int ch)
{
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = FAVORITES_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_favorites.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_favorites_content();
            }
            break;

        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= FAVORITES_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_favorites.count) g_content_selected_idx = 0;
                render_favorites_content();
            }
            break;

        case KEY_RIGHT:
        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            break;

        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            break;

        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                    render_favorites_content();
                } else if (g_menu_selected_idx == FAVORITES_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_favorites.count > 0 && g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_favorites.count) {

                    Track *t = &g_favorites.tracks[g_content_selected_idx];
                    int found = playlist_find_track_index_by_path(t->path);

                    if (found >= 0) {
                        play_audio(found);
                        exit_current_view();
                    } else {
                        show_status_message(i18n_get("playlist_mgr.track_not_in_queue"));
                    }
                }
            }
            break;

        case 'd':
        case 'D':
            if (g_focus_area == FOCUS_CONTENT && g_favorites.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_favorites.count) {

                remove_from_favorites(g_content_selected_idx);
                if (g_content_selected_idx >= g_favorites.count) {
                    g_content_selected_idx = g_favorites.count - 1;
                }
                show_status_message(i18n_get("favorites.removed"));
                render_favorites_content();
            }
            break;
    }
}
