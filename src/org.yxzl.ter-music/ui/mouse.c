/**
 * @file mouse.c
 * @brief 鼠标事件处理
 *
 * 从 ui.c 拆分，处理主视图中的鼠标事件，包括窗口坐标转换、
 * 播放列表行命中、控制栏按钮命中、歌词行命中和功能键提示条命中。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "playlist/playlist.h"
#include "search/search.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "ui/lyrics.h"
#include "config/config.h"
#include <ncursesw/ncurses.h>
#include <pthread.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

/* Playlist tab mode and queue selection (defined in playlist_render.c) */
extern int g_playlist_tab_mode;
extern int g_queue_selected_index;

/* Layout helpers (defined in layout.c) */
extern int calculate_lyrics_content_top(int h, int w);
extern int get_controls_button_row(int height);

/* ============================================================
 * Mouse event helpers
 * ============================================================ */

int is_primary_mouse_click(const MEVENT *event)
{
    if (!event) return 0;
    if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED)) return 1;
    return (event->bstate & BUTTON1_RELEASED) != 0 && (event->bstate & BUTTON1_PRESSED) == 0;
}

int translate_screen_to_window(WINDOW *win, int screen_y, int screen_x, int *window_y, int *window_x)
{
    if (!win) return 0;
    int beg_y, beg_x, height, width;
    getbegyx(win, beg_y, beg_x);
    getmaxyx(win, height, width);

    if (screen_y < beg_y || screen_y >= beg_y + height ||
        screen_x < beg_x || screen_x >= beg_x + width) return 0;
    if (window_y) *window_y = screen_y - beg_y;
    if (window_x) *window_x = screen_x - beg_x;
    return 1;
}

/* ============================================================
 * Playlist row hit detection
 * ============================================================ */

int get_playlist_index_from_window_row(int window_y, int *display_index, int *actual_index)
{
    if (!win_playlist) return 0;

    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;

    int content_height = h - 2;
    int visible_lines = content_height - 6;
    if (visible_lines <= 0 || window_y < 1 || window_y >= 1 + visible_lines) return 0;

    int clicked_display_index = get_playlist_scroll_offset() + (window_y - 1);

    if (g_search_state.active || g_search_state.in_progress) {
        pthread_mutex_lock(&g_search_mutex);
        int snap_count = g_search_state.result_count;
        int idx = (snap_count > 0 && clicked_display_index >= 0 && clicked_display_index < snap_count)
                  ? g_search_state.result_indices[clicked_display_index] : -1;
        pthread_mutex_unlock(&g_search_mutex);
        if (idx < 0) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = idx;
        return 1;
    }

    if (g_sort_state.active) {
        if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = g_sort_state.sorted_indices[clicked_display_index];
        return 1;
    }

    /* Tree mode: index is a visible line, actual is the track index */
    else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (clicked_display_index < 0 || clicked_display_index >= playlist_visible_count()) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = get_visible_node_track_index(clicked_display_index);
        return 1;
    }

    if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) return 0;
    if (display_index) *display_index = clicked_display_index;
    if (actual_index)  *actual_index = clicked_display_index;
    return 1;
}

/* ============================================================
 * Control button hit detection
 * ============================================================ */

int get_control_index_from_window_point(int window_y, int window_x)
{
    if (!win_controls) return -1;

    int h, w;
    getmaxyx(win_controls, h, w);

    int button_start_row = get_controls_button_row(h);
    int button_end_row = h - 2;

    if (window_y < button_start_row || window_y > button_end_row) return -1;

    int available_width = w - 2;
    int current_col = 0;
    int is_first_in_row = 1;
    int current_row = button_start_row;

    for (int i = 0; i < CONTROL_COUNT - 1; i++) {
        char display_label[32];
        build_control_label(i, display_label, sizeof(display_label));
        int button_width = utf8_str_width(display_label) + 4;

        if (is_first_in_row) {
            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT - 1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;
                if (row_width + temp_button_width > available_width && row_button_count > 0) break;
                row_width += temp_button_width;
                row_button_count++;
            }
            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
            is_first_in_row = 0;
        }

        if (!is_first_in_row && current_col + button_width > w - 1) {
            current_row++;
            if (current_row > button_end_row) break;

            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT - 1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;
                if (row_width + temp_button_width > available_width && row_button_count > 0) break;
                row_width += temp_button_width;
                row_button_count++;
            }
            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
        }

        if (window_y == current_row && window_x >= current_col && window_x < current_col + button_width)
            return i;

        current_col += button_width;
    }

    return -1;
}

/* ============================================================
 * Lyric line hit detection
 * ============================================================ */

int get_lyric_index_from_window_row(int window_y, int *lyric_index, double *timestamp)
{
    if (!win_lyrics || !g_app_config.show_lyrics_panel) return 0;

    int h, w;
    getmaxyx(win_lyrics, h, w);

    pthread_mutex_lock(&g_lyrics.lock);

    if (!g_lyrics.has_lyrics || g_lyrics.count == 0 || g_lyrics.current_index < 0) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int content_top = calculate_lyrics_content_top(h, w);
    int visible_lines = h - content_top - 1;
    if (visible_lines <= 0 || window_y < content_top || window_y >= content_top + visible_lines) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int current_center_idx = (g_lyric_cursor_mode && g_lyrics.cursor_index >= 0)
        ? g_lyrics.cursor_index
        : g_lyrics.current_index;

    int start_idx = current_center_idx - (visible_lines / 2);
    if (start_idx < 0) start_idx = 0;
    if (start_idx + visible_lines > g_lyrics.count) start_idx = g_lyrics.count - visible_lines;
    if (start_idx < 0) start_idx = 0;

    int clicked_lyric_index = start_idx + (window_y - content_top);
    if (clicked_lyric_index < 0 || clicked_lyric_index >= g_lyrics.count) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    if (lyric_index) *lyric_index = clicked_lyric_index;
    if (timestamp)   *timestamp = g_lyrics.lines[clicked_lyric_index].timestamp;

    pthread_mutex_unlock(&g_lyrics.lock);
    return 1;
}

/* ============================================================
 * Menu hint bar click (F-key from bottom bar)
 * ============================================================ */

int get_menu_hint_fkey_from_column(int screen_x)
{
    static const char *menu_labels_zh[] = {
        "F1:主页", "F2:设置", "F3:历史", "F4:歌单",
        "F5:收藏", "F6:信息", "F7:中/EN", "F8:帮助", "F9:退出"
    };
    static const char *menu_labels_en[] = {
        "F1:Home", "F2:Settings", "F3:History", "F4:Playlists",
        "F5:Favorites", "F6:Info", "F7:Lang", "F8:Help", "F9:Quit"
    };

    const char **labels = use_english_ui() ? menu_labels_en : menu_labels_zh;
    int col = 2;

    for (int i = 0; i < 9; i++) {
        int width = utf8_str_width(labels[i]);
        if (screen_x >= col && screen_x < col + width)
            return KEY_F(i + 1);
        col += width + 2;
    }
    return 0;
}

int handle_menu_hint_bar_click(const MEVENT *event)
{
    if (!event || !is_primary_mouse_click(event)) return 0;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    if (event->y == max_y - 1) {
        int fkey = get_menu_hint_fkey_from_column(event->x);
        if (fkey != 0) {
            handle_function_keys(fkey);
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * Main view mouse event dispatch
 * ============================================================ */

int handle_main_view_mouse_event(const MEVENT *event)
{
    if (!event || !is_primary_mouse_click(event)) return 0;

    int window_y, window_x;

    /* Popup overlay check — popup spans multiple windows */
    {
        int ret = popup_handle_mouse_click(event->y, event->x);
        if (ret > 0) return 1;  /* click inside popup, fully handled */
        /* ret == 0 → dismissed; ret < 0 → no popup; both: continue */
    }

    // Playlist click
    if (translate_screen_to_window(win_playlist, event->y, event->x, &window_y, &window_x)) {
        int display_index = -1, actual_index = -1;
        (void)window_x;

        if (!get_playlist_index_from_window_row(window_y, &display_index, &actual_index))
            return 0;

        g_control_focus = 0;
        if (g_search_state.active || g_search_state.in_progress) {
            if (g_search_state.in_progress) {
                search_async_cancel();
                pthread_mutex_lock(&g_search_mutex);
                g_search_state.in_progress = 0;
                pthread_mutex_unlock(&g_search_mutex);
            }
            g_search_state.selected_index = display_index;
            play_audio(actual_index);
            g_search_state.active = 0;
            g_selected_index = g_sort_state.active ? 0 : actual_index;
            render_playlist_content();
        } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
            g_queue_selected_index = display_index;
            if (display_index >= 0 && display_index < g_play_queue.count) {
                g_play_queue.current_position = display_index;
                play_audio(g_play_queue.indices[display_index]);
            }
        } else {
            g_selected_index = display_index;
            /* Tree mode: toggle directory or play file */
            if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
                && !g_search_state.active) {
                int node_type = get_visible_node_type(display_index);
                if (node_type == TREE_NODE_DIRECTORY) {
                    int tree_idx = get_visible_node_tree_index(display_index);
                    playlist_toggle_directory_expand(tree_idx);
                    if (g_selected_index >= playlist_visible_count())
                        g_selected_index = playlist_visible_count() - 1;
                    if (g_selected_index < 0) g_selected_index = 0;
                    render_playlist_content();
                    return 1;
                }
                int track_idx = get_visible_node_track_index(display_index);
                if (track_idx >= 0) play_audio(track_idx);
            } else {
                play_audio(actual_index);
            }
        }
        render_controls();
        return 1;
    }

    // Controls click
    if (translate_screen_to_window(win_controls, event->y, event->x, &window_y, &window_x)) {

        int control_index = get_control_index_from_window_point(window_y, window_x);
        if (control_index < 0) return 0;

        g_control_focus = 1;
        g_current_control_idx = control_index;

        if (control_index != CONTROL_IDX_VOLUME && control_index != CONTROL_IDX_PROGRESS) {
            activate_current_control();
            render_playlist_content();
        }
        render_controls();
        return 1;
    }

    // Lyrics click
    if (translate_screen_to_window(win_lyrics, event->y, event->x, &window_y, &window_x)) {
        int lyric_index = -1;
        double target_timestamp = 0.0;
        (void)window_x;

        if (!get_lyric_index_from_window_row(window_y, &lyric_index, &target_timestamp))
            return 0;

        pthread_mutex_lock(&g_lyrics.lock);
        g_lyrics.cursor_index = lyric_index;
        g_lyric_cursor_index = lyric_index;
        pthread_mutex_unlock(&g_lyrics.lock);

        seek_audio(target_timestamp);
        render_lyrics();
        return 1;
    }

    return 0;
}
