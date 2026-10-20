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
#include "player/player.h"
#include "ui/dialog.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
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
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

    menu_content_clear_rows(&area, 2, max_y - 2);

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    char header[256];
    snprintf(header, sizeof(header), i18n_get("history.title_fmt"),
             g_dir_history.count);
    menu_content_puts(&area, start_y, header);
    menu_content_puts(&area, start_y + 1, "----------------------------------------");
    start_y += 3;

    if (g_dir_history.count == 0) {
        menu_content_puts(&area, start_y, i18n_get("history.empty"));
        menu_content_puts(&area, start_y + 1, i18n_get("history.auto_record"));
    } else {
        int visible_lines = max_y - start_y - 3;

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

            char line[MAX_PATH_LEN + 96];
            snprintf(line, sizeof(line), " %s [%s]", display_path, time_str);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                menu_content_puts(&area, start_y + i, line);
                attroff(A_REVERSE);
            } else {
                menu_content_puts(&area, start_y + i, line);
            }
        }

        int bottom_y = max_y - 3;
        menu_content_puts(&area, bottom_y, i18n_get("history.hint_manage"));
        menu_content_puts(&area, bottom_y + 1, i18n_get("history.hint_delete"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

/* ============================================================
 * History view input handling
 * ============================================================ */

int handle_history_input(int ch)
{
    ActionId action = keymap_resolve(CTX_HISTORY, ch);

    switch (action) {
        case ACTION_HISTORY_UP:
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
            return 1;

        case ACTION_HISTORY_DOWN:
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
            return 1;

        case ACTION_HISTORY_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_ACTIVATE:
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
                    render_menu_hint_bar();
                } else if (g_menu_selected_idx == HISTORY_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_dir_history.count > 0 && g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_dir_history.count) {

                    const char *path = g_dir_history.entries[g_content_selected_idx].path;
                    player_stop();
                    int count = load_playlist(path);
                    /* 目录历史加载的内容需经门面下发给核心（remote 下 load_playlist
                     * 只写进程内镜像，不会把路径交给核心）（Task 4.3）。 */
                    if (count > 0) {
                        player_queue_push();
                    }

                    if (count > 0) {
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
            return 1;

        case ACTION_HISTORY_QUEUE_APPEND:
            if (g_focus_area == FOCUS_CONTENT &&
                g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 &&
                g_content_selected_idx < g_dir_history.count) {

                const char *path = g_dir_history.entries[g_content_selected_idx].path;
                int count = append_playlist(path);

                if (count > 0) {
                    /* 追加的内容也要经门面下发给核心（remote 下 append_playlist
                     * 只写进程内镜像）（Task 4.3）。 */
                    player_queue_push();
                    add_dir_history_entry(path);
                    exit_current_view();
                    if (g_app_config.remember_last_path) {
                        snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", path);
                        player_config_persist();
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
            return 1;

        case ACTION_HISTORY_DELETE:
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
            return 1;

        case ACTION_HISTORY_CLEAR:
            if (g_focus_area == FOCUS_CONTENT) {
                clear_dir_history();
                g_content_selected_idx = 0;
                show_status_message(i18n_get("history.cleared"));
                render_history_content();
            }
            return 1;

        default:
            return 0;
    }
}
