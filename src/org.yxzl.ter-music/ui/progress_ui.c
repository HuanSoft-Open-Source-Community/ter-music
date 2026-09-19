/**
 * @file progress_ui.c
 * @brief 进度条更新和快进快退
 *
 * 从 ui.c 拆分，负责进度条的增量重绘。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "player/player.h"
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

#define UI_PROGRESS_REFRESH_MS 80

/* ============================================================
 * Seek relative
 * ============================================================ */

void seek_relative_seconds(int delta_seconds)
{
    if (delta_seconds == 0 || player_play_state() == PLAY_STATE_STOPPED || player_duration_seconds() <= 0) return;
    int new_pos = player_position_seconds() + delta_seconds;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > player_duration_seconds()) new_pos = player_duration_seconds();
    if (new_pos != player_position_seconds()) player_seek_seconds((int)(new_pos));
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

    if (player_play_state() == PLAY_STATE_STOPPED || player_duration_seconds() <= 0 || !win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    if (player_play_state() == PLAY_STATE_PLAYING && progress_tracker_is_ready()) {
        int tracked_position = progress_tracker_get_position_seconds();
        if (tracked_position < 0) tracked_position = 0;
        if (player_duration_seconds() > 0 && tracked_position > player_duration_seconds())
            tracked_position = player_duration_seconds();
        /* 位置由播放线程/门面维护：界面只读取（本地后端的外推也一样） */
        (void)tracked_position;
    }

    /* 弹出菜单时仅保留逻辑计算，暂停进度条 UI 渲染 */
    if (g_popup.active) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h < 5 || w < 20) return;

    int current_pos = player_position_seconds();
    if (current_pos < 0) current_pos = 0;
    if (current_pos > player_duration_seconds()) current_pos = player_duration_seconds();

    uint64_t now_ms = get_ui_time_ms();
    int position_changed = (current_pos != last_position);
    int force_redraw = position_changed || player_duration_seconds() != last_duration || player_play_state() != last_state;
    if (!force_redraw && (now_ms - last_refresh_ms) < UI_PROGRESS_REFRESH_MS) return;

    int progress_percent = (current_pos * 100) / player_duration_seconds();
    if (progress_percent > 100) progress_percent = 100;

    int current_min = current_pos / 60;
    int current_sec = current_pos % 60;
    int total_min = player_duration_seconds() / 60;
    int total_sec = player_duration_seconds() % 60;
    current_min %= 100;
    total_min %= 100;

    int progress_row = get_controls_progress_row(h);
    if (progress_row < 1 || progress_row >= h - 1) return;

    wmove(win_controls, progress_row, 1);
    for (int i = 1; i < w - 1 && i < 512; i++) {
        waddch(win_controls, ' ');
    }

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

    wrefresh(win_controls);

    if (position_changed) {
        render_lyrics();
    }

    static uint64_t last_spectrum_refresh_ms = 0;
    if (g_current_view == VIEW_MAIN && (now_ms - last_spectrum_refresh_ms >= 150ULL)) {
        request_ui_refresh(UI_DIRTY_LYRICS);
        last_spectrum_refresh_ms = now_ms;
    }

    last_refresh_ms = now_ms;
    last_position = current_pos;
    last_duration = player_duration_seconds();
    last_state = player_play_state();
}
