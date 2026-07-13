/**
 * @file progress_ui.c
 * @brief 进度条更新和快进快退
 *
 * 从 ui.c 拆分，负责进度条的增量重绘和播放列表滚动偏移计算。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menu_internal.h"
#include "audio/audio.h"
#include "audio/progress/progress.h"
#include "search/search.h"
#include "playlist/playlist.h"
#include "ui/lyrics.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>


extern WINDOW *win_controls;
extern int g_playlist_tab_mode;
extern int g_queue_selected_index;

#define UI_PROGRESS_REFRESH_MS 80

/* ============================================================
 * Seek relative
 * ============================================================ */

void seek_relative_seconds(int delta_seconds)
{
    if (delta_seconds == 0 || g_play_state == PLAY_STATE_STOPPED || g_total_duration <= 0) return;
    int new_pos = g_current_position + delta_seconds;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > g_total_duration) new_pos = g_total_duration;
    if (new_pos != g_current_position) seek_audio((double)new_pos);
}

/* ============================================================
 * Playlist scroll offset
 * ============================================================ */

int get_playlist_scroll_offset(void)
{
    if (!win_playlist) return 0;

    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;

    int content_height = h - 2;
    int visible_lines = content_height - 6;
    if (visible_lines <= 0) return 0;

    if (g_search_state.active || g_search_state.in_progress) {
        int offset = 0;
        if (g_search_state.selected_index >= visible_lines)
            offset = g_search_state.selected_index - visible_lines + 1;
        return offset;
    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        int offset = 0;
        if (g_queue_selected_index >= visible_lines)
            offset = g_queue_selected_index - visible_lines + 1;
        return offset;
    } else {
        int offset = 0;
        if (g_selected_index >= visible_lines)
            offset = g_selected_index - visible_lines + 1;
        return offset;
    }
}

/* ============================================================
 * Progress bar update (incremental)
 * ============================================================ */

void update_progress_bar(void)
{
    static uint64_t last_refresh_ms = 0;
    static int last_position = -1;
    static int last_duration = -1;
    static PlayState last_state = PLAY_STATE_STOPPED;

    if (g_play_state == PLAY_STATE_STOPPED || g_total_duration <= 0 || !win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    if (g_play_state == PLAY_STATE_PLAYING && progress_tracker_is_ready()) {
        int tracked_position = progress_tracker_get_position_seconds();
        if (tracked_position < 0) tracked_position = 0;
        if (g_total_duration > 0 && tracked_position > g_total_duration)
            tracked_position = g_total_duration;
        g_current_position = tracked_position;
    }

    /* 弹出菜单时仅保留逻辑计算，暂停进度条 UI 渲染 */
    if (g_popup.active) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h < 5 || w < 20) return;

    int current_pos = g_current_position;
    if (current_pos < 0) current_pos = 0;
    if (current_pos > g_total_duration) current_pos = g_total_duration;

    uint64_t now_ms = get_ui_time_ms();
    int position_changed = (current_pos != last_position);
    int force_redraw = position_changed || g_total_duration != last_duration || g_play_state != last_state;
    if (!force_redraw && (now_ms - last_refresh_ms) < UI_PROGRESS_REFRESH_MS) return;

    int progress_percent = (current_pos * 100) / g_total_duration;
    if (progress_percent > 100) progress_percent = 100;

    int current_min = current_pos / 60;
    int current_sec = current_pos % 60;
    int total_min = g_total_duration / 60;
    int total_sec = g_total_duration % 60;
    current_min %= 100;
    total_min %= 100;

    int is_progress_selected = (g_current_control_idx == CONTROL_IDX_PROGRESS && g_control_focus == 1);

    int progress_row = get_controls_progress_row(h);
    if (progress_row < 1 || progress_row >= h - 1) return;

    wmove(win_controls, progress_row, 1);
    for (int i = 1; i < w - 1 && i < 512; i++) {
        waddch(win_controls, ' ');
    }

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

    if (is_progress_selected) wattroff(win_controls, A_REVERSE | A_BOLD);

    mvwaddstr(win_controls, progress_row, 0, "\xe2\x94\x82");
    mvwaddstr(win_controls, progress_row, w - 1, "\xe2\x94\x82");

    wrefresh(win_controls);

    if (position_changed) {
        update_lyrics_display();
    }

    static uint64_t last_spectrum_refresh_ms = 0;
    if (g_current_view == VIEW_MAIN && (now_ms - last_spectrum_refresh_ms >= 150ULL)) {
        request_ui_refresh(UI_DIRTY_LYRICS);
        last_spectrum_refresh_ms = now_ms;
    }

    last_refresh_ms = now_ms;
    last_position = current_pos;
    last_duration = g_total_duration;
    last_state = g_play_state;
}
