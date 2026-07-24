/**
 * @file history.c
 * @brief 历史视图 — 目录历史浏览和管理
 *
 * 从 menus.c 拆分而来，负责历史页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "ui/dialog.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "config/config.h"
#include "playlist/playlist.h"
#include "library/library.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <time.h>
#include <stddef.h>

/* ============================================================
 * History view rendering
 * ============================================================ */

void render_history_content(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    mvprintw(start_y, content_start_x,
             i18n_get("history.title_fmt"),
             g_dir_history.count);
    mvprintw(start_y + 1, content_start_x, "----------------------------------------");
    start_y += 3;

    if (g_dir_history.count == 0) {
        mvprintw(start_y, content_start_x, "%s",
                 i18n_get("history.empty"));
        mvprintw(start_y + 1, content_start_x, "%s",
                 i18n_get("history.auto_record"));
    } else {
        int visible_lines = max_y - start_y - 2;

        if (g_content_selected_idx >= g_dir_history.count) {
            g_content_selected_idx = g_dir_history.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;

        if (g_content_selected_idx < g_history_content_offset) {
            g_history_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_history_content_offset + visible_lines) {
            g_history_content_offset = g_content_selected_idx - visible_lines + 1;
        }

        for (int i = 0; i < visible_lines && (g_history_content_offset + i) < g_dir_history.count; i++) {
            int idx = g_history_content_offset + i;
            DirHistoryEntry *entry = &g_dir_history.entries[idx];

            char time_str[64];
            struct tm *tm_info = localtime(&entry->open_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);

            char display_path[MAX_PATH_LEN];
            int path_width = max_x - menu_width - 25;
            if (path_width > MAX_PATH_LEN - 1) path_width = MAX_PATH_LEN - 1;
            utf8_str_truncate(display_path, entry->path, path_width);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                mvprintw(start_y + i, content_start_x, " %s [%s]", display_path, time_str);
                attroff(A_REVERSE);
            } else {
                mvprintw(start_y + i, content_start_x, " %s [%s]", display_path, time_str);
            }
        }

        int bottom_y = max_y - 3;
        mvprintw(bottom_y, content_start_x, "%s",
                 i18n_get("history.hint_manage"));
        mvprintw(bottom_y + 1, content_start_x, "%s",
                 i18n_get("history.hint_delete"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

/* ============================================================
 * History view input handling
 * ============================================================ */

void handle_history_input(int ch)
{
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = HISTORY_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_dir_history.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_history_content();
            }
            break;

        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= HISTORY_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_dir_history.count) g_content_selected_idx = 0;
                render_history_content();
            }
            break;

        case KEY_RIGHT:
        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            break;

        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            break;

        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                } else if (g_menu_selected_idx == 1) {
                    clear_dir_history();
                    g_content_selected_idx = 0;
                    show_status_message(i18n_get("history.cleared"));
                    render_menu_frame(i18n_get("menu.history"));
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                } else if (g_menu_selected_idx == HISTORY_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_dir_history.count > 0 && g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_dir_history.count) {

                    const char *path = g_dir_history.entries[g_content_selected_idx].path;
                    extern void stop_audio(void);
                    stop_audio();
                    int count = load_playlist(path);

                    if (count > 0) {
                        extern int g_selected_index;
                        g_selected_index = 0;
                        add_dir_history_entry(path);
                        exit_current_view();
                        show_status_message(i18n_get("status.folder_loaded"));
                    } else {
                        show_status_message(i18n_get("history.no_audio"));
                        render_history_content();
                    }
                }
            }
            break;

        case 'a':
        case 'A':
            if (g_focus_area == FOCUS_CONTENT &&
                g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 &&
                g_content_selected_idx < g_dir_history.count) {

                const char *path = g_dir_history.entries[g_content_selected_idx].path;
                int count = append_playlist(path);

                if (count > 0) {
                    add_dir_history_entry(path);
                    exit_current_view();
                    if (g_app_config.remember_last_path) {
                        snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", path);
                        save_config();
                    }
                    char msg[96];
                    snprintf(msg, sizeof(msg), "%s %d %s",
                             i18n_get("status.appended"), count,
                             i18n_get("history.tracks_to_queue"));
                    show_status_message(msg);
                } else {
                    show_status_message(i18n_get("dialog.no_new_audio"));
                    render_history_content();
                }
            }
            break;

        case 'd':
        case 'D':
            if (g_focus_area == FOCUS_CONTENT && g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_dir_history.count) {

                char removed_path[MAX_PATH_LEN] = "";
                strncpy(removed_path, g_dir_history.entries[g_content_selected_idx].path, MAX_PATH_LEN - 1);
                removed_path[MAX_PATH_LEN - 1] = '\0';

                for (int i = g_content_selected_idx; i < g_dir_history.count - 1; i++) {
                    g_dir_history.entries[i] = g_dir_history.entries[i + 1];
                }
                g_dir_history.count--;

                if (library_is_available())
                    library_dir_history_remove(removed_path);

                if (g_content_selected_idx >= g_dir_history.count) {
                    g_content_selected_idx = g_dir_history.count - 1;
                }

                show_status_message(i18n_get("history.entry_removed"));
                render_history_content();
            }
            break;

        case 'c':
        case 'C':
            if (g_focus_area == FOCUS_CONTENT) {
                clear_dir_history();
                g_content_selected_idx = 0;
                show_status_message(i18n_get("history.cleared"));
                render_history_content();
            }
            break;

        case 'r':
        case 'R':
            /* NOTE: 'R' for rename is handled in playlist context only.
             * This case is defined but does nothing for history. */
            break;
    }
}
