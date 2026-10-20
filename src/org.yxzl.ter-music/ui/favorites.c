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
#include "player/player.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>

/* ============================================================
 * Favorites view rendering
 * ============================================================ */

void render_favorites_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

    menu_content_clear_rows(&area, 2, max_y - 3);

    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));

    char header[256];
    snprintf(header, sizeof(header), i18n_get("favorites.title_fmt"),
             g_favorites.count);
    menu_content_puts(&area, start_y, header);
    menu_content_puts(&area, start_y + 1, "----------------------------------------");
    start_y += 3;

    if (g_favorites.count == 0) {
        menu_content_puts(&area, start_y, i18n_get("favorites.empty"));
        menu_content_puts(&area, start_y + 1, i18n_get("favorites.hint_f"));
    } else {
        int visible_lines = max_y - start_y - 3;

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

            char line[MAX_META_LEN * 2 + 64];
            snprintf(line, sizeof(line), " %s - %s", display_title, display_artist);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                menu_content_puts(&area, start_y + i, line);
                attroff(A_REVERSE);
            } else {
                menu_content_puts(&area, start_y + i, line);
            }
        }

        int bottom_y = max_y - 3;
        menu_content_puts(&area, bottom_y, i18n_get("favorites.hint_manage"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    refresh();
}

/* ============================================================
 * Favorites view input handling
 * ============================================================ */

int handle_favorites_input(int ch)
{
    ActionId action = keymap_resolve(CTX_FAVORITES, ch);

    switch (action) {
        case ACTION_FAVORITES_UP:
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
            return 1;

        case ACTION_FAVORITES_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= FAVORITES_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_favorites.count) g_content_selected_idx = 0;
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_ACTIVATE:
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

                    /* 直播不在当前列表的曲目（Task 5.2）：追加到前端内容列表尾部，
                     * 再经 ui_play_track_index 反查队列位置播放（其内部会在需要时
                     * 整表重推给核心）；复用既有内容→队列桥，不新增核心方法。 */
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
            return 1;

        case ACTION_FAVORITES_REMOVE:
            if (g_focus_area == FOCUS_CONTENT && g_favorites.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_favorites.count) {

                remove_from_favorites(g_content_selected_idx);
                if (g_content_selected_idx >= g_favorites.count) {
                    g_content_selected_idx = g_favorites.count - 1;
                }
                show_status_message(i18n_get("favorites.removed"));
                render_favorites_content();
            }
            return 1;

        default:
            return 0;
    }
}
