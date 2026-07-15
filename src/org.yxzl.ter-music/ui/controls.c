/**
 * @file controls.c
 * @brief 控制栏按钮渲染及控制激活
 *
 * 从 ui.c 拆分，负责控制栏按钮的绘制、标签构建和控件激活逻辑。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menu_internal.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "playlist/playlist.h"
#include "config/config.h"
#include "logger/logger.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>

extern WINDOW *win_controls;

/* ── Control label i18n keys ── */
static const char *control_label_keys[] = {
    "controls.label.prev",
    "controls.label.play_pause",
    "controls.label.next",
    "controls.label.stop",
    "controls.label.loop",
    "controls.label.speed",
    "controls.label.volume",
    "controls.label.progress"
};

/* ── Speed state (defined in audio.c) ── */
extern float g_speed_ratios[];
extern int   g_speed_index;
extern int   g_speed_count;

/* ── Playlist tab mode (defined in playlist_render.c) ── */
extern int g_playlist_tab_mode;

/* ── Popup state ── */
PopupState g_popup = {0};
#define VOLUME_POPUP_STEP 5

/* Forward declarations */
static void popup_dismiss(void);
static void render_controls_popup(void);

/* ============================================================
 * Control label helpers
 * ============================================================ */

static const char *get_control_label(int index)
{
    if (index < 0 || index >= CONTROL_COUNT) return "";
    return i18n_get(control_label_keys[index]);
}

void build_control_label(int index, char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    dest[0] = '\0';
    if (index < 0 || index >= CONTROL_COUNT) return;

    if (index == CONTROL_IDX_LOOP) {
        snprintf(dest, dest_size, "%s:%s", get_control_label(index), get_play_mode_str());
        return;
    }
    if (index == CONTROL_IDX_SPEED) {
        snprintf(dest, dest_size, "%s:%.2fx", get_control_label(index), (double)g_playback_speed);
        return;
    }
    if (index == CONTROL_IDX_VOLUME) {
        snprintf(dest, dest_size, "%s:%d%%", get_control_label(index), get_volume_percent());
        return;
    }
    utf8_str_truncate(dest, get_control_label(index), (int)dest_size - 1);
}

/* ============================================================
 * Status line rendering
 * ============================================================ */

static pthread_mutex_t g_controls_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_controls_status_message[256] = "";
static time_t g_controls_status_time = 0;

void update_controls_status(const char *msg)
{
    if (msg && msg[0]) {
        log_debug("ui", "Controls status: %s", msg);
    }
    pthread_mutex_lock(&g_controls_status_mutex);
    snprintf(g_controls_status_message, sizeof(g_controls_status_message), "%s", msg ? msg : "");
    g_controls_status_time = time(NULL);
    pthread_mutex_unlock(&g_controls_status_mutex);
    request_ui_refresh(UI_DIRTY_CONTROLS);
}

void render_controls_status_line(void)
{
    if (!win_controls) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h <= 2 || w <= 3) return;

    char status_msg[sizeof(g_controls_status_message)];
    time_t status_time = 0;

    pthread_mutex_lock(&g_controls_status_mutex);
    snprintf(status_msg, sizeof(status_msg), "%s", g_controls_status_message);
    status_time = g_controls_status_time;
    pthread_mutex_unlock(&g_controls_status_mutex);

    int status_row = h - 2;
    mvwhline(win_controls, status_row, 1, ' ', w - 2);

    if (status_msg[0] != '\0' && (time(NULL) - status_time) < 3) {
        char display_msg[sizeof(g_controls_status_message)];
        format_display_text(display_msg, sizeof(display_msg), status_msg, w - 4, 0);
        mvwprintw(win_controls, status_row, 2, "%s", display_msg);
    }
}

/* ============================================================
 * Popup helpers
 * ============================================================ */

#define BASIC_MODE_COUNT 5
#define FOLDER_MODE_COUNT 4
#define ADVANCED_MODE_COUNT 8

static int calculate_available_play_modes(void)
{
    int count = BASIC_MODE_COUNT;
    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (g_current_play_index >= 0 || g_selected_index >= 0)
            count += FOLDER_MODE_COUNT;
    }
    if (g_app_config.advanced_play_modes_enabled)
        count += ADVANCED_MODE_COUNT;
    return count;
}

static PlayMode get_available_play_mode_at(int index)
{
    if (index < BASIC_MODE_COUNT)
        return (PlayMode)index;

    int folder_offset = BASIC_MODE_COUNT;
    int advanced_offset = folder_offset + FOLDER_MODE_COUNT;

    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER &&
        (g_current_play_index >= 0 || g_selected_index >= 0)) {
        if (index >= folder_offset && index < folder_offset + FOLDER_MODE_COUNT)
            return (PlayMode)(index - folder_offset + PLAY_MODE_FOLDER_SEQUENTIAL);
        advanced_offset = folder_offset + FOLDER_MODE_COUNT;
    } else {
        advanced_offset = folder_offset;
    }

    if (index >= advanced_offset)
        return (PlayMode)(index - advanced_offset + PLAY_MODE_ALBUM_SEQUENTIAL);

    return PLAY_MODE_SEQUENTIAL;
}

static void calculate_popup_dimensions(PopupState *popup, WINDOW *parent)
{
    if (!parent) return;
    int parent_h, parent_w;
    getmaxyx(parent, parent_h, parent_w);
    int popup_h = popup->option_count + 2;
    /* Cap to a reasonable max, no longer constrained by parent height */
    if (popup_h > 20) popup_h = 20;
    int popup_w = 26;
    popup->height = popup_h;
    popup->width = popup_w;
    /* Position relative to parent window's top-left corner on screen */
    int parent_screen_y, parent_screen_x;
    getbegyx(parent, parent_screen_y, parent_screen_x);
    popup->start_row = parent_screen_y + (parent_h - popup_h) / 2;
    popup->start_col = parent_screen_x + (parent_w - popup_w) / 2;
    if (popup->start_row < 1) popup->start_row = 1;
    if (popup->start_col < 1) popup->start_col = 1;
    /* Clamp bottom edge so the popup doesn't overflow the screen */
    int screen_h, screen_w;
    getmaxyx(stdscr, screen_h, screen_w);
    if (popup->start_row + popup->height > screen_h)
        popup->start_row = screen_h - popup->height;
    if (popup->start_row < 1) popup->start_row = 1;
}

static void destroy_popup_window(PopupState *popup)
{
    if (popup->popup_win) {
        WINDOW *w = (WINDOW *)popup->popup_win;
        /* 仅释放窗口，不刷新—下层窗口将在后续全量重绘中恢复 */
        delwin(w);
        popup->popup_win = NULL;
    }
}

static void create_popup_window(PopupState *popup)
{
    destroy_popup_window(popup); /* clean up any lingering window */
    WINDOW *w = newwin(popup->height, popup->width, popup->start_row, popup->start_col);
    if (!w) return;
    popup->popup_win = (void *)w;
}

static void build_popup_option_text(PopupType type, int index,
                                     char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    switch (type) {
        case POPUP_LOOP_MODE: {
            PlayMode mode = get_available_play_mode_at(index);
            snprintf(dest, dest_size, "%s", play_mode_display_name(mode, 0));
            break;
        }
        case POPUP_SPEED: {
            float speeds[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
            if (index >= 0 && index < 6)
                snprintf(dest, dest_size, "%.2fx", (double)speeds[index]);
            break;
        }
        case POPUP_VOLUME: {
            int vol = index * VOLUME_POPUP_STEP;
            snprintf(dest, dest_size, "%d%%", vol);
            break;
        }
        default: dest[0] = '\0';
    }
}

static void apply_popup_selection(void)
{
    switch (g_popup.type) {
        case POPUP_LOOP_MODE:
            set_play_mode(get_available_play_mode_at(g_popup.selected_index));
            break;
        case POPUP_SPEED: {
            g_speed_index = g_popup.selected_index;
            g_playback_speed = g_speed_ratios[g_speed_index];
            g_app_config.default_playback_speed = g_playback_speed;
            save_config();
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: %.2fx",
                     i18n_get("controls.label.speed"), (double)g_playback_speed);
            update_controls_status(msg);
            apply_playback_speed_change();
            break;
        }
        case POPUP_VOLUME: {
            int new_vol = g_popup.selected_index * VOLUME_POPUP_STEP;
            set_volume_percent(new_vol);
            break;
        }
        default: break;
    }
}

int handle_popup_input(int ch)
{
    if (!g_popup.active) return 0;

    switch (ch) {
        case KEY_UP:
            if (g_popup.selected_index > 0)
                g_popup.selected_index--;
            render_controls_popup();  /* only redraw popup, not full controls */
            return 1;
        case KEY_DOWN:
            if (g_popup.selected_index < g_popup.option_count - 1)
                g_popup.selected_index++;
            render_controls_popup();  /* only redraw popup, not full controls */
            return 1;
        case ' ':
        case 10:
            apply_popup_selection();
            popup_dismiss();
            return 1;
        case 27:
        case KEY_LEFT:
            popup_dismiss();
            return 1;
    }
    return 0;
}

/* ============================================================
 * Control activation
 * ============================================================ */

void activate_current_control(void)
{
    switch (g_current_control_idx) {
        case CONTROL_IDX_PREV:
            prev_track();
            break;
        case CONTROL_IDX_PLAY_PAUSE: {
            PlayState current_state = g_play_state;
            int is_thread_running = g_play_thread_running;

            if (current_state == PLAY_STATE_PLAYING && is_thread_running) {
                pause_audio();
            } else if (current_state == PLAY_STATE_PAUSED && is_thread_running) {
                resume_audio();
            } else if (current_state == PLAY_STATE_STOPPED) {
                int playlist_total = playlist_count();
                if (playlist_is_loaded() && playlist_total > 0) {
                    int target_index = (g_current_play_index >= 0)
                        ? g_current_play_index
                        : g_selected_index;
                    if (g_sort_state.active && g_current_play_index < 0) {
                        target_index = g_sort_state.sorted_indices[g_selected_index];
                    }
                    /* Tree mode: translate visible index to track index */
                    if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
                        && g_current_play_index < 0) {
                        int ti = get_visible_node_track_index(g_selected_index);
                        if (ti >= 0) target_index = ti;
                    }
                    if (target_index >= 0 && target_index < playlist_total) {
                        play_audio(target_index);
                    }
                }
            }
            break;
        }
        case CONTROL_IDX_NEXT:
            next_track();
            break;
        case CONTROL_IDX_STOP:
            stop_audio();
            break;
        case CONTROL_IDX_LOOP:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_LOOP_MODE;
                g_popup.option_count = calculate_available_play_modes();
                /* Find the popup index that maps to the current play mode */
                g_popup.selected_index = 0;
                for (int i = 0; i < g_popup.option_count; i++) {
                    if (get_available_play_mode_at(i) == g_play_mode) {
                        g_popup.selected_index = i;
                        break;
                    }
                }
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_SPEED:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_SPEED;
                g_popup.selected_index = g_speed_index;
                g_popup.option_count = g_speed_count;
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_VOLUME:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_VOLUME;
                g_popup.selected_index = get_volume_percent() / VOLUME_POPUP_STEP;
                g_popup.option_count = 100 / VOLUME_POPUP_STEP + 1;
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_PROGRESS:
            break;
    }
}

/* ============================================================
 * Popup rendering
 * ============================================================ */

static void render_controls_popup(void)
{
    if (!g_popup.active) return;
    WINDOW *popup_win = (WINDOW *)g_popup.popup_win;
    if (!popup_win) return;

    int popup_h = g_popup.height;
    int popup_w = g_popup.width;
    int visible_options = popup_h - 2;

    /* Scroll offset: keep selected_index visible */
    int scroll_offset = 0;
    if (g_popup.selected_index >= visible_options)
        scroll_offset = g_popup.selected_index - visible_options + 1;

    /* Step 1 — clear window (default background, no color pair) */
    werase(popup_win);

    /* Step 2 — draw border and title with HIGHLIGHT color pair */
    wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    rounded_box(popup_win);

    const char *title = "";
    switch (g_popup.type) {
        case POPUP_LOOP_MODE: title = i18n_get("popup.play_mode"); break;
        case POPUP_SPEED:     title = i18n_get("popup.speed"); break;
        case POPUP_VOLUME:    title = i18n_get("popup.volume"); break;
        default: break;
    }
    if (title[0])
        mvwprintw(popup_win, 0, 2, "%s", title);

    /* Step 3 — fill all option rows with HIGHLIGHT background */
    for (int i = 0; i < visible_options; i++)
        mvwprintw(popup_win, i + 1, 1, " %-*s ", popup_w - 4, "");

    /* Step 4 — draw option text with HIGHLIGHT color pair (same as the
       fill background), selected item gets A_REVERSE for contrast. */
    for (int i = 0; i < visible_options && (scroll_offset + i) < g_popup.option_count; i++) {
        int opt_idx = scroll_offset + i;
        char option_text[48];
        build_popup_option_text(g_popup.type, opt_idx, option_text, sizeof(option_text));

        if (opt_idx == g_popup.selected_index)
            wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_REVERSE);
        else
            wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        mvwprintw(popup_win, i + 1, 1, " %-*s ", popup_w - 4, option_text);
    }

    /* Step 5 — scroll indicator arrows */
    wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    if (scroll_offset > 0)
        mvwaddch(popup_win, 1, popup_w < 3 ? 1 : popup_w - 3, '^');
    if (scroll_offset + visible_options < g_popup.option_count)
        mvwaddch(popup_win, popup_h - 2 < 1 ? 1 : popup_h - 2,
                 popup_w < 3 ? 1 : popup_w - 3, 'v');

    /* Step 6 — cleanup and flush */
    wattrset(popup_win, A_NORMAL);
    wrefresh(popup_win);
}

/* ============================================================
 * Popup dismiss — force full refresh of all UI elements
 * ============================================================ */

static void popup_dismiss(void)
{
    if (!g_popup.active) return;
    destroy_popup_window(&g_popup);
    g_popup.active = 0;

    /* Step 1 — overwrite popup content in curscr's gap areas (areas
     * between sub-windows) by copying stdscr's clean background to
     * curscr.  wrefresh(popup_win) called wnoutrefresh(popup_win)
     * which wrote into curscr; the gap rows are never covered by the
     * sub-window wnoutrefresh calls below, so we must scrub them here
     * before doupdate() sees them. */
    redrawwin(stdscr);
    wnoutrefresh(stdscr);

    /* Step 2 — force full re-render of playlist and controls so
     * their sub-window content overwrites the remaining areas. */
    redrawwin(win_playlist);
    redrawwin(win_controls);
    render_playlist_content();
    render_controls();
    render_menu_hint_bar();
}

/* ============================================================
 * Popup mouse handling
 * ============================================================ */

int popup_handle_mouse_click(int screen_y, int screen_x)
{
    if (!g_popup.active || !g_popup.popup_win)
        return -1;

    int py = g_popup.start_row;
    int px = g_popup.start_col;
    int ph = g_popup.height;
    int pw = g_popup.width;
    int visible_options = ph - 2;

    /* Scroll offset applied to the popup */
    int scroll_offset = 0;
    if (g_popup.selected_index >= visible_options)
        scroll_offset = g_popup.selected_index - visible_options + 1;

    if (screen_y >= py && screen_y < py + ph &&
        screen_x >= px && screen_x < px + pw) {
        /* Click inside popup — select option */
        int option_row = screen_y - py - 1;
        int opt_idx = scroll_offset + option_row;

        if (option_row >= 0 && option_row < visible_options &&
            opt_idx >= 0 && opt_idx < g_popup.option_count) {
            g_popup.selected_index = opt_idx;
            apply_popup_selection();
        }

        popup_dismiss();
        return 1;  /* fully handled */
    }

    /* Click outside popup — dismiss, caller should continue */
    popup_dismiss();
    return 0;  /* dismissed, may continue with normal handling */
}

/* ============================================================
 * Controls rendering
 * ============================================================ */

void render_controls(void)
{
    if (!win_controls) return;

    werase(win_controls);
    wattron(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    rounded_box(win_controls);

    const char *focus_hint = g_control_focus
        ? i18n_get("controls.ctrl_focus")
        : i18n_get("controls.list_focus");
    const char *lyric_hint = g_lyric_cursor_mode
        ? i18n_get("controls.d_exit_seek")
        : i18n_get("controls.d_lyric_seek");
    char controls_header[160];
    snprintf(controls_header, sizeof(controls_header), "%s %s %s %s %s %s",
             i18n_get("controls.controls_area"),
             i18n_get("controls.space_run"),
             i18n_get("controls.c_ctrl"),
             i18n_get("controls.l_list"),
             i18n_get("controls.tab_view"),
             focus_hint);
    mvwprintw(win_controls, 0, 2, " %s %s", controls_header, lyric_hint);

    wattroff(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    wbkgd(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    int h, w;
    getmaxyx(win_controls, h, w);

    // Progress bar
    if (g_play_state != PLAY_STATE_STOPPED && g_total_duration > 0) {
        int progress_row = get_controls_progress_row(h);

        if (h >= 5 && w >= 20) {
            int current_pos = g_current_position;
            if (current_pos < 0) current_pos = 0;
            if (current_pos > g_total_duration) current_pos = g_total_duration;

            int progress_percent = (current_pos * 100) / g_total_duration;
            if (progress_percent > 100) progress_percent = 100;

            int current_min = current_pos / 60;
            int current_sec = current_pos % 60;
            int total_min = g_total_duration / 60;
            int total_sec = g_total_duration % 60;
            current_min %= 100;
            total_min %= 100;

            int is_progress_selected = (g_current_control_idx == CONTROL_IDX_PROGRESS && g_control_focus == 1);
            if (is_progress_selected) wattron(win_controls, A_REVERSE | A_BOLD);

            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
                     current_min, current_sec, total_min, total_sec);
            mvwprintw(win_controls, progress_row, 2, "%s", time_str);

            int time_width = 13;
            int percent_width = 4;
            int padding = 6;
            int progress_bar_width = w - time_width - percent_width - padding - 4;
            if (progress_bar_width < 10) progress_bar_width = 10;

            int progress_start_col = 2 + time_width + 1;

            mvwprintw(win_controls, progress_row, progress_start_col, "[");

            int filled_width = (progress_bar_width * progress_percent) / 100;
            if (filled_width > progress_bar_width) filled_width = progress_bar_width;

            for (int i = 0; i < progress_bar_width && (progress_start_col + 1 + i) < w - 2; i++) {
                char c = '-';
                if (i < filled_width) c = '=';
                else if (i == filled_width && progress_percent < 100) c = '>';
                mvwaddch(win_controls, progress_row, progress_start_col + 1 + i, c);
            }

            mvwprintw(win_controls, progress_row, progress_start_col + 1 + progress_bar_width, "]");
            mvwprintw(win_controls, progress_row, progress_start_col + 2 + progress_bar_width, "%d%%", progress_percent);

            mvwaddstr(win_controls, progress_row, 0, "\xe2\x94\x82");
            mvwaddstr(win_controls, progress_row, w - 1, "\xe2\x94\x82");

            if (is_progress_selected) wattroff(win_controls, A_REVERSE | A_BOLD);
        }
    }

    // Control buttons
    int button_start_row = get_controls_button_row(h);
    int current_row = button_start_row;
    int available_width = w - 2;
    int current_col = 0;
    int is_first_in_row = 1;

    for (int i = 0; i < CONTROL_COUNT - 1; i++) {
        char display_label[32];
        build_control_label(i, display_label, sizeof(display_label));
        int len = utf8_str_width(display_label);
        int button_width = len + 4;

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
            if (current_row >= h - 2) break;

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

        if (i == g_current_control_idx && g_control_focus == 1) {
            wattron(win_controls, A_REVERSE | A_BOLD);
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
            wattroff(win_controls, A_REVERSE | A_BOLD);
        } else {
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
        }
        current_col += button_width;
    }

    render_controls_status_line();
    render_visualizer_with_album_cover();

    /* Refresh controls base window first, then overlay popup on top */
    wnoutrefresh(win_controls);
    if (g_popup.active && g_popup.popup_win) {
        render_controls_popup();
    }
    doupdate();
}
