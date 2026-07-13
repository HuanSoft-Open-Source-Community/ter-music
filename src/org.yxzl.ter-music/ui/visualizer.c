/**
 * @file visualizer.c
 * @brief 频谱可视化、专辑封面盲文渲染
 *
 * 从 ui.c 拆分，负责控制栏区域的频谱可视化与专辑封面显示。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menu_internal.h"
#include "ui/braille/braille_art.h"
#include "audio/audio.h"
#include "config/config.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

extern WINDOW *win_controls;

/* ── Braille album art buffer (set from ui.c, used here) ── */
extern char g_braille_art_buffer[8192];
extern int g_album_cover_size;

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
 * Album cover braille rendering
 * ============================================================ */

static void render_album_cover(void)
{
    if (!win_controls || g_current_view != VIEW_MAIN) return;

    static int g_album_cover_enabled = 1;
    if (!g_album_cover_enabled || !g_app_config.show_album_cover) return;

    int h, w;
    getmaxyx(win_controls, h, w);

    int viz_top    = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);
    int viz_height = viz_bottom - viz_top + 1;

    if (viz_height < BRAILLE_MIN_SIZE * BRAILLE_CELL_H) return;

    int optimal_size = calculate_optimal_cover_size(h);
    if (optimal_size < BRAILLE_MIN_SIZE) return;

    int cover_char_width = optimal_size * 2;

    /* constrain to window width: start_col(0) + cover + 4 cols gap ≤ w */
    int start_col = 0;
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
    if (get_current_album_cover_path(cover_path, sizeof(cover_path)) != 0) return;

    if (g_album_cover_size != optimal_size || g_braille_art_buffer[0] == '\0') {
        g_album_cover_size = optimal_size;
        if (generate_braille_art_dynamic(cover_path, BRAILLE_DEFAULT_THRESHOLD,
                                          cover_char_width, optimal_size,
                                          g_braille_art_buffer, sizeof(g_braille_art_buffer)) != 0) {
            g_braille_art_buffer[0] = '\0';
            return;
        }
    }

    char *lines[BRAILLE_MAX_SIZE];
    int line_count = get_braille_art_lines(g_braille_art_buffer, lines, BRAILLE_MAX_SIZE);

    int start_row = viz_top;

    for (int i = 0; i < line_count && i < optimal_size; i++) {
        if (start_row + i < viz_bottom) {
            mvwprintw(win_controls, start_row + i, start_col, "%s", lines[i]);
        }
        free(lines[i]);
    }
}

/* ============================================================
 * Visualizer area management
 * ============================================================ */

static void clear_visualizer_area(void)
{
    if (!win_controls) return;

    int h, w;
    getmaxyx(win_controls, h, w);

    int button_row = get_controls_button_row(h);
    int viz_top    = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);

    int separator_row = viz_top - 1;
    if (separator_row > button_row && separator_row < h - 1) {
        for (int x = 1; x < w - 1; x++)
            mvwaddstr(win_controls, separator_row, x, "\xe2\x94\x80"); /* ─ */
        mvwaddstr(win_controls, separator_row, 0, "\xe2\x94\x82");     /* │ */
        mvwaddstr(win_controls, separator_row, w - 1, "\xe2\x94\x82"); /* │ */
    }

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

    int button_row = get_controls_button_row(h);
    int viz_top    = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);
    int viz_height = viz_bottom - viz_top + 1;
    if (viz_height < 2) return;

    if (graph_width <= 0) graph_width = w - 4;
    if (start_col <= 0)   start_col = 2;
    if (graph_width < 12) return;

    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    get_visualizer_snapshot(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    (void)peaks;

    uint64_t now_ms = get_ui_time_ms();
    int inactive_decay = 0;
    int is_visualizer_active = 0;
    if (last_update_ms > 0 && now_ms > last_update_ms) {
        inactive_decay = (int)((now_ms - last_update_ms) / 90ULL);
        if ((now_ms - last_update_ms) < 250ULL &&
            (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED)) {
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

    // Smoothing
    if (graph_width >= 3) {
        static int *smoothed_units = NULL;
        static int smoothed_units_cap = 0;
        if (graph_width > smoothed_units_cap) {
            int *new_buf = realloc(smoothed_units, (size_t)graph_width * sizeof(int));
            if (!new_buf) return;
            smoothed_units = new_buf;
            smoothed_units_cap = graph_width;
        }
        smoothed_units[0] = (column_units[0] * 3 + column_units[1]) / 4;
        for (int col = 1; col < graph_width - 1; col++) {
            smoothed_units[col] = (column_units[col - 1] + column_units[col] * 2 + column_units[col + 1]) / 4;
        }
        smoothed_units[graph_width - 1] = (column_units[graph_width - 2] + column_units[graph_width - 1] * 3) / 4;

        for (int col = 0; col < graph_width; col++) {
            column_units[col] = smoothed_units[col];
        }
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
 * Visualizer entry point
 * ============================================================ */

void render_visualizer_with_album_cover(void)
{
    if (!win_controls || g_current_view != VIEW_MAIN) return;

    int h, w;
    getmaxyx(win_controls, h, w);

    if (h < 9) return;

    clear_visualizer_area();

    if (g_app_config.show_album_cover) {
        render_album_cover();
    }

    render_wave_particle_visualizer(2, w - 4);
}
