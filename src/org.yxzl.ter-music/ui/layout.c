/**
 * @file layout.c
 * @brief 窗口布局创建及辅助布局计算
 *
 * 从 ui.c 拆分，负责主视图三个子窗口的创建、尺寸计算和分隔线绘制。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "config/config.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

/* ============================================================
 * Layout coordinate helpers
 * ============================================================ */

int get_controls_progress_row(int height)
{
    if (height >= 9) return 1;
    int row = height / 2 - 2;
    if (row < 1) row = 1;
    return row;
}

int get_controls_button_row(int height)
{
    if (height >= 9) return 2;
    int row = height / 2;
    if (row < 2) row = 2;
    return row;
}

int get_controls_visualizer_top(int height)
{
    if (height >= 9) return 4;
    return get_controls_button_row(height) + 1;
}

int get_controls_visualizer_bottom(int height)
{
    return height - 3;
}

int get_corner_spectrum_height(int h)
{
    if (h >= 28) return 5;
    if (h >= 22) return 4;
    if (h >= 17) return 3;
    if (h >= 13) return 2;
    return 1;
}

int calculate_lyrics_content_top(int h, int w)
{
    int spectrum_height = get_corner_spectrum_height(h);
    int graph_top = 1;
    int graph_bottom = graph_top + spectrum_height - 1;
    if (graph_bottom >= h - 2 || w < 16) return 1;
    return graph_bottom + 1;
}

/* ============================================================
 * Main layout creation
 * ============================================================ */

void create_layout(void)
{
    // 初始化视图状态（仅在第一次调用时）
    static int initialized = 0;
    if (!initialized) {
        g_current_view = VIEW_MAIN;
        g_menu_selected_idx = 0;
        initialized = 1;
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    // 边界检查
    if (max_y < 8) max_y = 8;
    if (max_x < 20) max_x = 20;

    int lyrics_width;
    if (max_x >= 160) {
        lyrics_width = max_x / 3;
    } else if (max_x >= 120) {
        lyrics_width = (max_x * 3) / 8;
    } else {
        lyrics_width = (max_x * 2) / 5;
    }
    if (lyrics_width < 28) lyrics_width = 28;
    if (lyrics_width > max_x - 44) lyrics_width = max_x - 44;
    if (lyrics_width < 10) lyrics_width = 10;

    int main_width;
    if (g_app_config.show_lyrics_panel) {
        main_width = max_x - lyrics_width - 1;
    } else {
        lyrics_width = 0;
        main_width = max_x - 1;
    }
    if (main_width < 10) main_width = 10;

    int total_inner_height = max_y - 3;
    if (total_inner_height < 3) total_inner_height = 3;

    // 控制区域高度
    int controls_height;
    if (max_y >= 34)       controls_height = 7;
    else if (max_y >= 24)  controls_height = 6;
    else                   controls_height = 5;

    int min_controls      = 4;
    int min_playlist_info = 2;
    int min_playlist      = 1;

    int playlist_height = total_inner_height - controls_height;

    if (playlist_height < min_playlist_info) {
        playlist_height = min_playlist_info;
        controls_height = total_inner_height - playlist_height;
    }
    if (controls_height < min_controls) {
        controls_height = min_controls;
        playlist_height = total_inner_height - controls_height;
    }
    if (playlist_height < min_playlist) {
        playlist_height = min_playlist;
        controls_height = total_inner_height - playlist_height;
    }
    if (controls_height < 1) controls_height = 1;
    if (playlist_height < 1) playlist_height = 1;

    int total_used = controls_height + playlist_height;
    if (total_used > total_inner_height) {
        playlist_height = total_inner_height - controls_height;
        if (playlist_height < 1) {
            playlist_height = 1;
            controls_height = total_inner_height - playlist_height;
        }
    }

    // 1. 播放列表窗口 (左上)
    win_playlist = newwin(playlist_height, main_width, 1, 1);
    wattron(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    rounded_box(win_playlist);
    wattroff(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    wbkgd(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    mvwprintw(win_playlist, 0, 2, "%s", ui_text(" 播放列表 ", " Playlist "));
    wrefresh(win_playlist);

    // 2. 控制栏窗口 (左下)
    win_controls = newwin(controls_height, main_width, 1 + playlist_height, 1);
    wattron(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    rounded_box(win_controls);
    const char *focus_hint = g_control_focus
        ? ui_text("[控件焦点]", "[Ctrl Focus]")
        : ui_text("[列表焦点]", "[List Focus]");
    const char *lyric_hint = g_lyric_cursor_mode
        ? ui_text("[D:退出定位]", "[D:Exit Seek]")
        : ui_text("[D:歌词定位]", "[D:Lyric Seek]");
    char controls_header[160];
    snprintf(controls_header, sizeof(controls_header), "%s %s %s %s %s",
             ui_text("控制区", "Controls"),
             ui_text("[空格:执行]", "[Space:Run]"),
             ui_text("[C:控件]", "[C:Ctrl]"),
             ui_text("[L:列表]", "[L:List]"),
             focus_hint);
    wattroff(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    wbkgd(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    mvwprintw(win_controls, 0, 2, " %s %s", controls_header, lyric_hint);
    wrefresh(win_controls);

    // 3. 歌词侧栏窗口 (右侧)
    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        int lyrics_height = max_y - 3;
        if (lyrics_height < 3) lyrics_height = 3;
        win_lyrics = newwin(lyrics_height, lyrics_width, 1, 1 + main_width);
        wattron(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));
        rounded_box(win_lyrics);
        wattroff(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));
        wbkgd(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));
        mvwprintw(win_lyrics, 0, 2, "%s", ui_text(" 歌词 ", " Lyrics "));
        mvwprintw(win_lyrics, 2, 2, "%s", ui_text("未加载歌词。", "No lyrics loaded."));
        wrefresh(win_lyrics);
    } else {
        win_lyrics = NULL;
    }

    // 绘制分隔线
    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        int vline_len = max_y - 3;
        if (vline_len < 1) vline_len = 1;
        for (int i = 0; i < vline_len; i++)
            mvaddstr(1 + i, 1 + main_width, "\xe2\x94\x82"); /* │ */
    }

    int hline_len = main_width;
    if (hline_len < 1) hline_len = 1;
    for (int i = 0; i < hline_len; i++)
        mvaddstr(1 + playlist_height, 1 + i, "\xe2\x94\x80"); /* ─ */

    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        mvaddstr(1 + playlist_height, 1 + main_width, "\xe2\x94\xbc"); /* ┼ */
    }

    refresh();
    render_menu_hint_bar();
}
