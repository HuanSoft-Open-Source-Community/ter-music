/**
 * @file cover_art_cells.c
 * @brief TUI 字符封面的纯配色/字形实现
 *
 * 彩色模式：每个字符格统一输出 ▀；前景 = 上半像素颜色，背景 = 下半像素
 * 颜色。上下同色时视觉上就是全块。单色/ASCII 回退按亮度生成字形。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/cover_art_cells.h"

#include <math.h>
#include <string.h>

typedef struct {
    int r, g, b;
    int index;      /* 终端调色板索引（8 色：0..7；256 色：16..255） */
} PaletteEntry;

#define COVER_ART_MAX_CELLS (COVER_ART_MAX_COLS * COVER_ART_MAX_ROWS)
#define COVER_ART_PALETTE_MAX 240

static int luma(int r, int g, int b)
{
    return (299 * r + 587 * g + 114 * b) / 1000;
}

/* 构建候选调色板：
 *  - 256 色：xterm 16..231 的 6x6x6 色立方 + 232..255 的 24 级灰阶；
 *  - 8 色：ANSI 0..7。
 * 返回调色板条目数。 */
static int build_palette(int tier, PaletteEntry *pal)
{
    if (tier == 256) {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        int n = 0;
        for (int ri = 0; ri < 6; ri++) {
            for (int gi = 0; gi < 6; gi++) {
                for (int bi = 0; bi < 6; bi++) {
                    pal[n].r = levels[ri];
                    pal[n].g = levels[gi];
                    pal[n].b = levels[bi];
                    pal[n].index = 16 + 36 * ri + 6 * gi + bi;
                    n++;
                }
            }
        }
        for (int i = 0; i < 24; i++) {
            int v = 8 + 10 * i;
            pal[n].r = v;
            pal[n].g = v;
            pal[n].b = v;
            pal[n].index = 232 + i;
            n++;
        }
        return n;
    }

    if (tier == 8) {
        static const int ansi[8][3] = {
            {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
            {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
        };
        for (int i = 0; i < 8; i++) {
            pal[i].r = ansi[i][0];
            pal[i].g = ansi[i][1];
            pal[i].b = ansi[i][2];
            pal[i].index = i;
        }
        return 8;
    }

    return 0;
}

/* 在 sel[] 选中的调色板条目中找最接近 (r,g,b) 的位置（返回 sel 下标）。 */
static int nearest_selected(const PaletteEntry *pal, const int *sel, int nsel,
                            int r, int g, int b)
{
    int best = 0;
    long best_dist = -1;

    for (int i = 0; i < nsel; i++) {
        const PaletteEntry *p = &pal[sel[i]];
        long dr = (long)r - p->r;
        long dg = (long)g - p->g;
        long db = (long)b - p->b;
        long dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

/* 用选中的调色板生成彩色单元格：每格 ▀，fg=上半色，bg=下半色。 */
static int fill_color_cells(const unsigned char *rgb, int cols, int rows,
                            const PaletteEntry *pal, const int *sel, int nsel,
                            CoverArtGrid *out)
{
    short pair_fg[COVER_ART_MAX_CELLS];
    short pair_bg[COVER_ART_MAX_CELLS];
    int pair_count = 0;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const unsigned char *top =
                rgb + (((size_t)(2 * y) * cols) + (size_t)x) * 3;
            const unsigned char *bottom =
                rgb + (((size_t)(2 * y + 1) * cols) + (size_t)x) * 3;

            int ti = nearest_selected(pal, sel, nsel, top[0], top[1], top[2]);
            int bi = nearest_selected(pal, sel, nsel,
                                      bottom[0], bottom[1], bottom[2]);
            short fg = (short)pal[sel[ti]].index;
            short bg = (short)pal[sel[bi]].index;

            CoverArtCell *cell = &out->cells[y][x];
            cell->glyph = COVER_GLYPH_UPPER;
            cell->fg = fg;
            cell->bg = bg;
            cell->pair = 0;

            int found = 0;
            for (int k = 0; k < pair_count; k++) {
                if (pair_fg[k] == fg && pair_bg[k] == bg) {
                    found = 1;
                    break;
                }
            }
            if (!found && pair_count < COVER_ART_MAX_CELLS) {
                pair_fg[pair_count] = fg;
                pair_bg[pair_count] = bg;
                pair_count++;
            }
        }
    }

    out->colorized = 1;
    out->pair_count = pair_count;
    return pair_count;
}

/* 单色/ASCII 回退：按亮度生成 █/▀/▄/空格 或 #/空格。 */
static void fill_mono_cells(const unsigned char *rgb, int cols, int rows,
                            int unicode, CoverArtGrid *out)
{
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const unsigned char *top =
                rgb + (((size_t)(2 * y) * cols) + (size_t)x) * 3;
            const unsigned char *bottom =
                rgb + (((size_t)(2 * y + 1) * cols) + (size_t)x) * 3;

            int top_luma = luma(top[0], top[1], top[2]);
            int bottom_luma = luma(bottom[0], bottom[1], bottom[2]);
            int top_bright = top_luma >= COVER_ART_MONO_THRESHOLD;
            int bottom_bright = bottom_luma >= COVER_ART_MONO_THRESHOLD;

            unsigned char glyph;
            if (!unicode) {
                int average = (top_luma + bottom_luma) / 2;
                glyph = average >= COVER_ART_MONO_THRESHOLD
                            ? COVER_GLYPH_ASCII : COVER_GLYPH_SPACE;
            } else if (top_bright && bottom_bright) {
                glyph = COVER_GLYPH_FULL;
            } else if (top_bright) {
                glyph = COVER_GLYPH_UPPER;
            } else if (bottom_bright) {
                glyph = COVER_GLYPH_LOWER;
            } else {
                glyph = COVER_GLYPH_SPACE;
            }

            CoverArtCell *cell = &out->cells[y][x];
            cell->glyph = glyph;
            cell->fg = -1;
            cell->bg = -1;
            cell->pair = 0;
        }
    }

    out->colorized = 0;
    out->pair_count = 0;
}

int cover_art_tier_from_caps(int has_colors, int colors, int unicode)
{
    if (!has_colors || !unicode || colors < 8) {
        return 0;
    }
    if (colors < 256) {
        return 8;
    }
    return 256;
}

void cover_art_pair_plan(int color_pairs, CoverArtPairPlan *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    int total = color_pairs - COVER_ART_PAIR_BASE;
    if (total < 0) {
        total = 0;
    }
    if (total > COVER_ART_PAIR_CAP) {
        total = COVER_ART_PAIR_CAP;
    }

    out->info_base = COVER_ART_PAIR_BASE;
    out->controls_base = out->info_base;

    if (total < COVER_ART_VINYL_MIN_TOTAL) {
        /* 色对太少：保住信息/控制区封面的旧比例，黑胶走字符动画。 */
        out->color_vinyl = 0;
        out->info_budget = total * 2 / 3;
        out->controls_budget = total - out->info_budget;
        out->controls_base = out->info_base + out->info_budget;
        return;
    }

    /* 足够多：固定 12 对给黑胶碟面/频谱柱，剩下 info:controls:label = 2:1:1。 */
    out->color_vinyl = 1;
    out->fixed_budget = COVER_ART_VINYL_FIXED_PAIRS;

    int cover_total = total - out->fixed_budget;
    out->info_budget = cover_total / 2;
    out->controls_budget = cover_total / 4;
    out->label_budget = cover_total - out->info_budget - out->controls_budget;

    out->controls_base = out->info_base + out->info_budget;
    out->label_base = out->controls_base + out->controls_budget;
    out->fixed_base = out->label_base + out->label_budget;
}

/* 只保留实际出现过的颜色，按频次降序；减少颜色时优先保留高频色。 */
static int select_used_by_frequency(const int *freq, int palette_size, int *used)
{
    int used_count = 0;
    for (int i = 0; i < palette_size; i++) {
        if (freq[i] > 0) {
            used[used_count++] = i;
        }
    }
    for (int i = 1; i < used_count; i++) {
        int key = used[i];
        int j = i - 1;
        while (j >= 0 && freq[used[j]] < freq[key]) {
            used[j + 1] = used[j];
            j--;
        }
        used[j + 1] = key;
    }
    return used_count;
}

int cover_art_cells_build(const unsigned char *rgb, int cols, int rows,
                          int tier, int pair_budget, int unicode,
                          CoverArtGrid *out)
{
    if (!rgb || !out ||
        cols < 1 || cols > COVER_ART_MAX_COLS ||
        rows < 1 || rows > COVER_ART_MAX_ROWS) {
        return -1;
    }
    if (tier != 0 && tier != 8 && tier != 256) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->cols = cols;
    out->rows = rows;

    if (tier == 0 || !unicode || pair_budget < 4) {
        fill_mono_cells(rgb, cols, rows, unicode, out);
        return 0;
    }

    PaletteEntry pal[COVER_ART_PALETTE_MAX];
    int palette_size = build_palette(tier, pal);
    if (palette_size < 2) {
        fill_mono_cells(rgb, cols, rows, unicode, out);
        return 0;
    }

    /* 全调色板映射 + 频次统计（上下两个半像素都算）。 */
    int all[COVER_ART_PALETTE_MAX];
    int freq[COVER_ART_PALETTE_MAX];
    for (int i = 0; i < palette_size; i++) {
        all[i] = i;
        freq[i] = 0;
    }
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const unsigned char *top =
                rgb + (((size_t)(2 * y) * cols) + (size_t)x) * 3;
            const unsigned char *bottom =
                rgb + (((size_t)(2 * y + 1) * cols) + (size_t)x) * 3;
            freq[nearest_selected(pal, all, palette_size,
                                  top[0], top[1], top[2])]++;
            freq[nearest_selected(pal, all, palette_size,
                                  bottom[0], bottom[1], bottom[2])]++;
        }
    }

    /* 只用实际出现过的颜色，按频次降序。 */
    int used[COVER_ART_PALETTE_MAX];
    int used_count = select_used_by_frequency(freq, palette_size, used);
    if (used_count == 0) {
        fill_mono_cells(rgb, cols, rows, unicode, out);
        return 0;
    }

    int sel[COVER_ART_PALETTE_MAX];
    int selected_count = used_count;
    for (int i = 0; i < selected_count; i++) {
        sel[i] = used[i];
    }

    int pair_count = fill_color_cells(rgb, cols, rows, pal, sel,
                                      selected_count, out);
    while (pair_count > pair_budget && selected_count > 2) {
        selected_count--;
        for (int i = 0; i < selected_count; i++) {
            sel[i] = used[i];
        }
        pair_count = fill_color_cells(rgb, cols, rows, pal, sel,
                                      selected_count, out);
    }

    if (pair_count > pair_budget) {
        fill_mono_cells(rgb, cols, rows, unicode, out);
    }
    return 0;
}

/* ============================================================
 * 黑胶中心标签：旋转圆形裁剪
 * ============================================================ */

typedef struct {
    const unsigned char *src;
    int src_w;
    int src_h;
    int cols;
    int rows;
    double cx;
    double cy;
    double ca;
    double sa;
} LabelSampler;

static void label_grid_clear(CoverArtGrid *out, int cols, int rows)
{
    memset(out, 0, sizeof(*out));
    out->cols = cols;
    out->rows = rows;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            out->cells[y][x].fg = -1;
            out->cells[y][x].bg = -1;
        }
    }
}

/* 格心落在标签内切圆内才可见（旋转不改变圆，掩码也不随角度变）。 */
static int label_cell_inside(const LabelSampler *s, int x, int y)
{
    double nx = ((double)x - s->cx) / s->cx;
    double ny = ((double)y - s->cy) / s->cy;
    return (nx * nx + ny * ny) <= 1.0 + 1e-9;
}

/* 采样一个半格的源图颜色：先把标签坐标归一化到 [-1,1] 的内切圆，
 * 再旋转、映射回源图；越界样本夹到源图边缘。 */
static void label_sample_half(const LabelSampler *s, int x, int y,
                              double vertical, unsigned char out[3])
{
    double nx = ((double)x - s->cx) / s->cx;
    double ny = (((double)y + vertical) - s->cy) / s->cy;
    double rx = nx * s->ca - ny * s->sa;
    double ry = nx * s->sa + ny * s->ca;
    double u = (rx + 1.0) * 0.5 * (double)(s->src_w - 1);
    double v = (ry + 1.0) * 0.5 * (double)(s->src_h - 1);
    int sx = (int)lround(u);
    int sy = (int)lround(v);

    if (sx < 0) {
        sx = 0;
    }
    if (sx >= s->src_w) {
        sx = s->src_w - 1;
    }
    if (sy < 0) {
        sy = 0;
    }
    if (sy >= s->src_h) {
        sy = s->src_h - 1;
    }

    const unsigned char *p =
        s->src + ((size_t)sy * (size_t)s->src_w + (size_t)sx) * 3;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

/* 选色统计用未旋转的圆形裁剪：旋转只挪位置，颜色集合保持不变。 */
static void label_collect_frequencies(const LabelSampler *s,
                                      const PaletteEntry *pal, int palette_size,
                                      int *freq)
{
    int all[COVER_ART_PALETTE_MAX];
    for (int i = 0; i < palette_size; i++) {
        all[i] = i;
        freq[i] = 0;
    }

    for (int y = 0; y < s->rows; y++) {
        for (int x = 0; x < s->cols; x++) {
            if (!label_cell_inside(s, x, y)) {
                continue;
            }
            unsigned char top[3];
            unsigned char bottom[3];
            label_sample_half(s, x, y, -0.25, top);
            label_sample_half(s, x, y, 0.25, bottom);
            freq[nearest_selected(pal, all, palette_size,
                                  top[0], top[1], top[2])]++;
            freq[nearest_selected(pal, all, palette_size,
                                  bottom[0], bottom[1], bottom[2])]++;
        }
    }
}

static int label_fill_cells(const LabelSampler *s, const PaletteEntry *pal,
                            const int *sel, int nsel, CoverArtGrid *out)
{
    short pair_fg[COVER_ART_MAX_CELLS];
    short pair_bg[COVER_ART_MAX_CELLS];
    int pair_count = 0;

    for (int y = 0; y < s->rows; y++) {
        for (int x = 0; x < s->cols; x++) {
            CoverArtCell *cell = &out->cells[y][x];
            if (!label_cell_inside(s, x, y)) {
                cell->glyph = COVER_GLYPH_SPACE;
                cell->fg = -1;
                cell->bg = -1;
                cell->pair = 0;
                continue;
            }

            unsigned char top[3];
            unsigned char bottom[3];
            label_sample_half(s, x, y, -0.25, top);
            label_sample_half(s, x, y, 0.25, bottom);
            short fg = (short)pal[sel[nearest_selected(
                pal, sel, nsel, top[0], top[1], top[2])]].index;
            short bg = (short)pal[sel[nearest_selected(
                pal, sel, nsel, bottom[0], bottom[1], bottom[2])]].index;

            cell->glyph = COVER_GLYPH_UPPER;
            cell->fg = fg;
            cell->bg = bg;
            cell->pair = 0;

            int found = 0;
            for (int k = 0; k < pair_count; k++) {
                if (pair_fg[k] == fg && pair_bg[k] == bg) {
                    found = 1;
                    break;
                }
            }
            if (!found && pair_count < COVER_ART_MAX_CELLS) {
                pair_fg[pair_count] = fg;
                pair_bg[pair_count] = bg;
                pair_count++;
            }
        }
    }

    out->colorized = 1;
    out->pair_count = pair_count;
    return pair_count;
}

int cover_art_cells_build_label(const unsigned char *src_rgb,
                                int src_w, int src_h,
                                int cols, int rows, double angle,
                                int tier, int pair_budget, int unicode,
                                CoverArtGrid *out)
{
    if (!src_rgb || !out ||
        src_w < 1 || src_h < 1 ||
        cols < 3 || cols > COVER_ART_MAX_COLS ||
        rows < 3 || rows > COVER_ART_MAX_ROWS) {
        return -1;
    }
    if (tier != 0 && tier != 8 && tier != 256) {
        return -1;
    }
    if (!isfinite(angle)) {
        return -1;
    }

    label_grid_clear(out, cols, rows);

    /* 标签不使用单色块：无颜色/ASCII/预算不足时交给调用方回退
     * （黑胶层画无标签的纯色碟心，整个彩色黑胶不可用时才退回字符动画）。 */
    if (tier == 0 || !unicode || pair_budget < 4) {
        return 0;
    }

    PaletteEntry pal[COVER_ART_PALETTE_MAX];
    int palette_size = build_palette(tier, pal);
    if (palette_size < 2) {
        return 0;
    }

    LabelSampler sampler;
    sampler.src = src_rgb;
    sampler.src_w = src_w;
    sampler.src_h = src_h;
    sampler.cols = cols;
    sampler.rows = rows;
    sampler.cx = (double)(cols - 1) / 2.0;
    sampler.cy = (double)(rows - 1) / 2.0;
    sampler.ca = 1.0;
    sampler.sa = 0.0;

    int freq[COVER_ART_PALETTE_MAX];
    label_collect_frequencies(&sampler, pal, palette_size, freq);

    int used[COVER_ART_PALETTE_MAX];
    int used_count = select_used_by_frequency(freq, palette_size, used);
    if (used_count == 0) {
        return 0;
    }

    sampler.ca = cos(angle);
    sampler.sa = sin(angle);

    int sel[COVER_ART_PALETTE_MAX];
    int selected_count = used_count;
    for (int i = 0; i < selected_count; i++) {
        sel[i] = used[i];
    }

    int pair_count = label_fill_cells(&sampler, pal, sel, selected_count, out);
    while (pair_count > pair_budget && selected_count > 2) {
        selected_count--;
        for (int i = 0; i < selected_count; i++) {
            sel[i] = used[i];
        }
        pair_count = label_fill_cells(&sampler, pal, sel,
                                      selected_count, out);
    }

    if (pair_count > pair_budget) {
        label_grid_clear(out, cols, rows);
    }
    return 0;
}
