/**
 * @file visualizer.c
 * @brief 频谱可视化、专辑封面盲文渲染
 *
 * 从 ui.c 拆分，负责控制栏区域的波形/封面显示，以及主界面右上角
 * 独立频谱窗格的绘制与刷新。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "player/player.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/cover_art.h"
#include "ui/menu_internal.h"
#include "ui/braille/braille_art.h"
#include "audio/audio.h"
#include "audio/visualizer.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

extern WINDOW *win_controls;
extern WINDOW *win_spectrum;

/* ============================================================
 * Spectrum glyph helpers
 * ============================================================ */

static const char *get_spectrum_glyph(int units)
{
    static const char *glyphs[]      = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    static const char ascii_glyphs[] = {' ', '.', ':', '-', '=', '+', '*', '#', '#'};

    if (units < 0) units = 0;
    if (units > 8) units = 8;

    if (use_ascii_fallback_ui()) {
        static char glyph_buf[2];
        glyph_buf[0] = ascii_glyphs[units];
        glyph_buf[1] = '\0';
        return glyph_buf;
    }

    return glyphs[units];
}

/* ============================================================
 * Album cover half-block rendering
 * ============================================================ */

static void render_album_cover(void)
{
    if (!win_controls || g_current_view != VIEW_MAIN) return;

    static int g_album_cover_enabled = 1;
    if (!g_album_cover_enabled || !g_app_config.show_album_cover) return;

    int h, w;
    getmaxyx(win_controls, h, w);

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);
    int viz_top = layout.visualizer_top;
    int viz_bottom = layout.visualizer_bottom;
    if (viz_top < 0 || viz_bottom < viz_top) return;
    int viz_height = viz_bottom - viz_top + 1;

    if (viz_height < BRAILLE_MIN_SIZE * BRAILLE_CELL_H) return;

    int optimal_size = calculate_optimal_cover_size(h);
    if (optimal_size < BRAILLE_MIN_SIZE) return;

    int cover_char_width = optimal_size * 2;

    /* constrain to window width: start_col(1) + cover + 4 cols gap ≤ w */
    int start_col = 1;
    int max_cover_width = w - start_col - 4;
    if (cover_char_width > max_cover_width) {
        cover_char_width = max_cover_width;
        if (cover_char_width < BRAILLE_MIN_SIZE * 2) return;
        optimal_size = cover_char_width / 2;
        if (optimal_size < BRAILLE_MIN_SIZE) return;
    }

    int min_spectrum_width = 20;
    if (w - cover_char_width - start_col - 4 < min_spectrum_width) return;

    char cover_path[MAX_PATH_LEN];
    /* 当前曲目的封面路径是**后端**的“当前曲目信息”，经门面取 */
    if (player_cover_path(cover_path, sizeof(cover_path)) != 0) return;

    const CoverArtGrid *cover = cover_art_get(COVER_ART_SLOT_CONTROLS,
                                              cover_path,
                                              cover_char_width, optimal_size);
    if (!cover) return;

    cover_art_draw(win_controls, start_col, viz_top, cover,
                   ui_pane_body_attr(LAYOUT_PANE_CONTROLS));
}

/* ============================================================
 * Visualizer area management
 * ============================================================ */

static void clear_visualizer_area(void)
{
    if (!win_controls) return;

    int h, w;
    getmaxyx(win_controls, h, w);

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);
    int viz_top = layout.visualizer_top;
    int viz_bottom = layout.visualizer_bottom;
    if (viz_top < 0 || viz_bottom < viz_top) return;

    for (int row = viz_top; row <= viz_bottom; row++) {
        mvwhline(win_controls, row, 1, ' ', w - 2);
    }
}

/* ============================================================
 * Wave particle visualizer
 * ============================================================ */

static void render_wave_particle_visualizer(int start_col, int graph_width)
{
    if (!win_controls || g_current_view != VIEW_MAIN) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h < 7 || w < 24) return;

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);
    int viz_top = layout.visualizer_top;
    int viz_bottom = layout.visualizer_bottom;
    if (viz_top < 0 || viz_bottom < viz_top) return;
    int viz_height = viz_bottom - viz_top + 1;
    if (viz_height < 2) return;

    if (graph_width <= 0) graph_width = w - 4;
    if (start_col <= 0)   start_col = 2;
    if (graph_width < 12) return;

    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    player_visualizer(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    (void)peaks;

    uint64_t now_ms = get_ui_time_ms();
    int inactive_decay = 0;
    int is_visualizer_active = 0;
    if (last_update_ms > 0 && now_ms > last_update_ms) {
        inactive_decay = (int)((now_ms - last_update_ms) / 90ULL);
        if ((now_ms - last_update_ms) < 250ULL &&
            (player_play_state() == PLAY_STATE_PLAYING || player_play_state() == PLAY_STATE_PAUSED)) {
            is_visualizer_active = 1;
        }
    }

    static int *column_units = NULL;
    static int column_units_cap = 0;
    if (graph_width > column_units_cap) {
        int *new_buf = realloc(column_units, (size_t)graph_width * sizeof(int));
        if (!new_buf) return;
        column_units = new_buf;
        column_units_cap = graph_width;
    }
    for (int col = 0; col < graph_width; col++) {
        double normalized = (graph_width <= 1)
            ? 0.0
            : ((double)col * (double)(VISUALIZER_BAND_COUNT - 1)) / (double)(graph_width - 1);
        int left = (int)normalized;
        int right = left + 1;
        if (right >= VISUALIZER_BAND_COUNT) right = VISUALIZER_BAND_COUNT - 1;
        double frac = normalized - (double)left;
        int blended_level = (int)lround(((double)levels[left] * (1.0 - frac)) + ((double)levels[right] * frac));

        int level = blended_level - inactive_decay * 7;
        if (level < 0) level = 0;
        if (is_visualizer_active && level > 0 && level < 3) level = 3;

        int units = (level * viz_height * 8 + 99) / 100;
        if (level > 0 && units == 0) units = 1;
        column_units[col] = units;
    }

    // Render columns
    for (int col = 0; col < graph_width; col++) {
        for (int row = viz_bottom; row >= viz_top; row--) {
            int row_from_bottom = viz_bottom - row;
            int units = column_units[col] - row_from_bottom * 8;
            if (units < 0) units = 0;
            if (units > 8) units = 8;
            mvwaddstr(win_controls, row, start_col + col, get_spectrum_glyph(units));
        }
    }
}

/* ============================================================
 * Main-view spectrum pane
 * ============================================================ */

/* 频谱独立窗格刷新上限：40ms ≈ 25 FPS。远端唯一新帧仍受
 * RPC_VISUALIZER_INTERVAL_MS (50ms) 限制，约 20 FPS。 */
#define SPECTRUM_REFRESH_MS 40

void render_spectrum_pane(void)
{
    if (!win_spectrum || g_current_view != VIEW_MAIN) return;

    int h, w;
    getmaxyx(win_spectrum, h, w);
    if (h < 1 || w < 1) return;
    ui_pane_begin(LAYOUT_PANE_SPECTRUM, win_spectrum, i18n_get("controls.spectrum"));
    if (h < 3 || w < 8) {
        wrefresh(win_spectrum);
        return;
    }

    int graph_top = 1;
    int graph_bottom = h - 2;
    int graph_height = graph_bottom - graph_top + 1;
    int graph_width = w - 4;
    if (graph_height < 1 || graph_width < 4) {
        wrefresh(win_spectrum);
        return;
    }

    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    player_visualizer(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    (void)peaks;

    uint64_t now_ms = get_ui_time_ms();
    int inactive_decay = 0;
    int is_visualizer_active = 0;
    if (last_update_ms > 0 && now_ms > last_update_ms) {
        inactive_decay = (int)((now_ms - last_update_ms) / 90ULL);
        if ((now_ms - last_update_ms) < 250ULL &&
            (player_play_state() == PLAY_STATE_PLAYING || player_play_state() == PLAY_STATE_PAUSED)) {
            is_visualizer_active = 1;
        }
    }

    static int *column_units = NULL;
    static int column_units_cap = 0;
    if (graph_width > column_units_cap) {
        int *new_buf = realloc(column_units, (size_t)graph_width * sizeof(int));
        if (!new_buf) return;
        column_units = new_buf;
        column_units_cap = graph_width;
    }

    for (int col = 0; col < graph_width; col++) {
        double normalized = (graph_width <= 1)
            ? 0.0
            : ((double)col * (double)(VISUALIZER_BAND_COUNT - 1)) / (double)(graph_width - 1);
        int left = (int)normalized;
        int right = left + 1;
        if (right >= VISUALIZER_BAND_COUNT) {
            right = VISUALIZER_BAND_COUNT - 1;
        }
        double frac = normalized - (double)left;
        int blended_level = (int)lround(((double)levels[left] * (1.0 - frac)) +
                                        ((double)levels[right] * frac));
        int level = blended_level - inactive_decay * 7;
        if (level < 0) {
            level = 0;
        }
        if (is_visualizer_active && level > 0 && level < 3) {
            level = 3;
        }

        int units = (level * graph_height * 8 + 99) / 100;
        if (level > 0 && units == 0) {
            units = 1;
        }
        column_units[col] = units;
    }

    for (int col = 0; col < graph_width; col++) {
        for (int row = graph_bottom; row >= graph_top; row--) {
            int row_from_bottom = graph_bottom - row;
            int units = column_units[col] - row_from_bottom * 8;
            if (units < 0) units = 0;
            if (units > 8) units = 8;
            mvwaddstr(win_spectrum, row, 2 + col, get_spectrum_glyph(units));
        }
    }

    wrefresh(win_spectrum);
}

void spectrum_tick(void)
{
    if (!win_spectrum || g_current_view != VIEW_MAIN) return;

    static uint64_t last_render_ms = 0;
    static uint64_t fps_window_ms = 0;
    static unsigned long long frames = 0;

    uint64_t now_ms = get_ui_time_ms();
    if (last_render_ms != 0 && now_ms - last_render_ms < SPECTRUM_REFRESH_MS) {
        return;
    }
    last_render_ms = now_ms;

    render_spectrum_pane();

    if (fps_window_ms == 0) {
        fps_window_ms = now_ms;
    }
    frames++;
    if (now_ms - fps_window_ms >= 1000ULL) {
        log_debug("spectrum", "fps=%llu", frames);
        frames = 0;
        fps_window_ms = now_ms;
    }
}

/* ============================================================
 * Visualizer entry point
 * ============================================================ */

void render_visualizer_with_album_cover(void)
{
    if (!win_controls || g_current_view != VIEW_MAIN) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    wattrset(win_controls, ui_pane_body_attr(LAYOUT_PANE_CONTROLS));

    if (h < 9 || w < 24) return;

    clear_visualizer_area();

    /* 波形先画、封面后画：封面是不透明矩形，盖住左下方的波形，
     * 否则波形会把控制区的彩色封面重新刷成空白。 */
    render_wave_particle_visualizer(2, w - 4);

    if (g_app_config.show_album_cover) {
        render_album_cover();
    }
}
