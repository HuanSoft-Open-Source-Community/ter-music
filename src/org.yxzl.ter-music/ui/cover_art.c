/**
 * @file cover_art.c
 * @brief TUI 字符封面：图片缩放、配色网格缓存、颜色对绑定与绘制
 *
 * 彩色单元格由纯模块 ui/cover_art_cells.c 生成；本文件负责：
 *   - 探测终端颜色能力并拆分信息/控制区/黑胶标签/固定色对的 pair 预算；
 *   - 把源图缩放到 cols × rows*2 的 RGB 缓冲，或缩放到黑胶标签的方形源图；
 *   - 每个槽位独立缓存网格，并把 (fg,bg) 绑定到本槽 pair 区间；
 *   - 按角度重建黑胶中心标签的旋转圆形网格（按角度桶缓存）；
 *   - 逐单元设置属性和输出字形。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/cover_art.h"

#include <math.h>
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "ui/braille/braille_art.h"
#include "util/utf8.h"

typedef struct {
    int valid;
    char path[MAX_PATH_LEN];
    int cols;
    int rows;
    CoverArtGrid grid;
} CoverArtSlotCache;

static CoverArtSlotCache g_slots[COVER_ART_SLOT_COUNT];

static int g_configured;
static int g_ascii;
static int g_slot_tier[COVER_ART_SLOT_COUNT];
static int g_slot_budget[COVER_ART_SLOT_COUNT];
static int g_slot_base[COVER_ART_SLOT_COUNT];

static CoverArtPairPlan g_pair_plan;
static int g_label_tier;
static int g_vinyl_tier;

/* 黑胶中心标签：源图按方形缓存，网格按角度桶缓存。 */
typedef struct {
    int valid;
    int failed;
    char path[MAX_PATH_LEN];
    int cols;
    int rows;
    unsigned char *src;
    int src_size;
    int angle_bucket;
    CoverArtGrid grid;
} CoverArtLabelCache;

static CoverArtLabelCache g_label;

#define COVER_ART_LABEL_ANGLE_STEPS 64
#define COVER_ART_LABEL_SRC_MIN 16
#define COVER_ART_LABEL_SRC_MAX 96

/* 首次绘制前探测能力；终端颜色能力在进程内固定，不再重算。 */
static void cover_art_configure(void)
{
    if (g_configured) {
        return;
    }
    g_configured = 1;

    g_ascii = use_ascii_fallback_ui() ? 1 : 0;

    int has = has_colors() ? 1 : 0;
    int colors = has ? (int)COLORS : 0;
    int pairs = has ? (int)COLOR_PAIRS : 0;

    int tier = cover_art_tier_from_caps(has, colors, g_ascii ? 0 : 1);
    cover_art_pair_plan(pairs, &g_pair_plan);

    g_slot_base[COVER_ART_SLOT_INFO] = g_pair_plan.info_base;
    g_slot_base[COVER_ART_SLOT_CONTROLS] = g_pair_plan.controls_base;
    g_slot_budget[COVER_ART_SLOT_INFO] = g_pair_plan.info_budget;
    g_slot_budget[COVER_ART_SLOT_CONTROLS] = g_pair_plan.controls_budget;
    g_slot_tier[COVER_ART_SLOT_INFO] =
        g_pair_plan.info_budget >= 4 ? tier : 0;
    g_slot_tier[COVER_ART_SLOT_CONTROLS] =
        g_pair_plan.controls_budget >= 4 ? tier : 0;

    g_label_tier = (g_pair_plan.color_vinyl && g_pair_plan.label_budget >= 4)
                       ? tier : 0;
    g_vinyl_tier = (g_pair_plan.color_vinyl &&
                    g_pair_plan.fixed_budget >= COVER_ART_VINYL_FIXED_PAIRS)
                       ? tier : 0;
}

/* 把源图 RGBA 缩放到 dst_w × dst_h 的 RGB；alpha 忽略。 */
static int load_scaled_rgb(const char *image_path, int dst_w, int dst_h,
                           unsigned char **out)
{
    if (!image_path || !out || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }
    *out = NULL;

    unsigned char *rgba = NULL;
    int src_w = 0;
    int src_h = 0;
    if (load_image(image_path, &rgba, &src_w, &src_h) != 0) {
        return -1;
    }
    if (src_w <= 0 || src_h <= 0) {
        free(rgba);
        return -1;
    }

    size_t src_pixels = (size_t)src_w * (size_t)src_h;
    size_t dst_pixels = (size_t)dst_w * (size_t)dst_h;

    unsigned char *rgb = malloc(dst_pixels * 3);
    unsigned char *channel_src = malloc(src_pixels);
    unsigned char *channel_dst = malloc(dst_pixels);
    if (!rgb || !channel_src || !channel_dst) {
        free(rgba);
        free(rgb);
        free(channel_src);
        free(channel_dst);
        return -1;
    }

    for (int channel = 0; channel < 3; channel++) {
        for (size_t i = 0; i < src_pixels; i++) {
            channel_src[i] = rgba[i * 4 + (size_t)channel];
        }
        if (resize_gray(channel_src, src_w, src_h,
                        channel_dst, dst_w, dst_h) != 0) {
            free(rgba);
            free(rgb);
            free(channel_src);
            free(channel_dst);
            return -1;
        }
        for (size_t i = 0; i < dst_pixels; i++) {
            rgb[i * 3 + (size_t)channel] = channel_dst[i];
        }
    }

    free(rgba);
    free(channel_src);
    free(channel_dst);
    *out = rgb;
    return 0;
}

/* 把源图缩放到 cols × rows*2 的 RGB（半块字符的上下两个半像素）。 */
static int load_target_rgb(const char *image_path, int cols, int rows,
                           unsigned char **out)
{
    return load_scaled_rgb(image_path, cols, rows * 2, out);
}

/* 把网格里的 (fg,bg) 映射到给定 pair 区间；掩码空格不占色对。 */
static int bind_grid_pairs(int base, int budget, CoverArtGrid *grid)
{
    if (!grid || !grid->colorized) {
        return 0;
    }
    if (budget <= 0) {
        return -1;
    }

    short pair_fg[COVER_ART_PAIR_CAP];
    short pair_bg[COVER_ART_PAIR_CAP];
    int map_count = 0;

    for (int row = 0; row < grid->rows; row++) {
        for (int col = 0; col < grid->cols; col++) {
            CoverArtCell *cell = &grid->cells[row][col];
            if (cell->glyph == COVER_GLYPH_SPACE ||
                cell->fg < 0 || cell->bg < 0) {
                cell->pair = 0;
                continue;
            }
            int found = -1;
            for (int i = 0; i < map_count; i++) {
                if (pair_fg[i] == cell->fg && pair_bg[i] == cell->bg) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                if (map_count >= budget) {
                    return -1;
                }
                int pair = base + map_count;
                /* 兼容 8 位 pair 字段；同时不得越过 COLOR_PAIRS。 */
                if (pair > 255 || pair >= (int)COLOR_PAIRS) {
                    return -1;
                }
                if (init_pair((short)pair, cell->fg, cell->bg) != OK) {
                    return -1;
                }
                pair_fg[map_count] = cell->fg;
                pair_bg[map_count] = cell->bg;
                found = map_count;
                map_count++;
            }
            cell->pair = base + found;
        }
    }
    return 0;
}

static int bind_pairs(int slot, CoverArtGrid *grid)
{
    return bind_grid_pairs(g_slot_base[slot], g_slot_budget[slot], grid);
}

const CoverArtGrid *cover_art_get(int slot, const char *image_path,
                                  int cols, int rows)
{
    if (slot < 0 || slot >= COVER_ART_SLOT_COUNT ||
        !image_path || image_path[0] == '\0' ||
        cols < 1 || cols > COVER_ART_MAX_COLS ||
        rows < 1 || rows > COVER_ART_MAX_ROWS) {
        return NULL;
    }

    cover_art_configure();

    CoverArtSlotCache *cache = &g_slots[slot];
    if (cache->valid && cache->cols == cols && cache->rows == rows &&
        strcmp(cache->path, image_path) == 0) {
        return &cache->grid;
    }
    cache->valid = 0;

    unsigned char *rgb = NULL;
    if (load_target_rgb(image_path, cols, rows, &rgb) != 0) {
        return NULL;
    }

    CoverArtGrid grid;
    int unicode = g_ascii ? 0 : 1;
    if (cover_art_cells_build(rgb, cols, rows,
                              g_slot_tier[slot], g_slot_budget[slot],
                              unicode, &grid) != 0) {
        free(rgb);
        return NULL;
    }

    if (grid.colorized && bind_pairs(slot, &grid) != 0) {
        /* 颜色对绑定失败：本槽回退单色，绝不画错色。 */
        if (cover_art_cells_build(rgb, cols, rows, 0,
                                  g_slot_budget[slot], unicode, &grid) != 0) {
            free(rgb);
            return NULL;
        }
    }
    free(rgb);

    memset(cache, 0, sizeof(*cache));
    cache->valid = 1;
    snprintf(cache->path, sizeof(cache->path), "%s", image_path);
    cache->cols = cols;
    cache->rows = rows;
    cache->grid = grid;
    return &cache->grid;
}

/* 黑胶标签按 2π/64 的角度桶缓存，避免 100ms 节拍每帧重新量化。 */
static int label_angle_bucket(double angle)
{
    if (!isfinite(angle)) {
        return -1;
    }
    const double step = (2.0 * M_PI) / (double)COVER_ART_LABEL_ANGLE_STEPS;
    double wrapped = fmod(angle, 2.0 * M_PI);
    if (wrapped < 0.0) {
        wrapped += 2.0 * M_PI;
    }
    int bucket = (int)(wrapped / step + 0.5);
    return bucket % COVER_ART_LABEL_ANGLE_STEPS;
}

/* 方形源图分辨率：按标签像素尺寸的 2 倍过采样，限制在 16..96。 */
static int label_src_size(int cols, int rows)
{
    int size = 2 * (cols > rows * 2 ? cols : rows * 2);
    if (size < COVER_ART_LABEL_SRC_MIN) {
        size = COVER_ART_LABEL_SRC_MIN;
    }
    if (size > COVER_ART_LABEL_SRC_MAX) {
        size = COVER_ART_LABEL_SRC_MAX;
    }
    return size;
}

static void label_cache_reset(void)
{
    free(g_label.src);
    memset(&g_label, 0, sizeof(g_label));
}

const CoverArtGrid *cover_art_get_label(const char *image_path,
                                        int cols, int rows, double angle)
{
    if (!image_path || image_path[0] == '\0' ||
        cols < 3 || cols > COVER_ART_MAX_COLS ||
        rows < 3 || rows > COVER_ART_MAX_ROWS) {
        return NULL;
    }

    cover_art_configure();
    if (g_label_tier == 0 || g_pair_plan.label_budget < 4) {
        return NULL;
    }

    int bucket = label_angle_bucket(angle);
    if (bucket < 0) {
        return NULL;
    }

    int same_key = g_label.path[0] != '\0' &&
                   strcmp(g_label.path, image_path) == 0 &&
                   g_label.cols == cols && g_label.rows == rows;
    if (same_key && g_label.valid && g_label.angle_bucket == bucket) {
        return &g_label.grid;
    }
    /* 同一角度桶内的失败不重试；换一个角度桶再试，避免封面暂时读不到
     * 时整个进程都失去中心标签。 */
    if (same_key && g_label.failed && g_label.angle_bucket == bucket) {
        return NULL;
    }

    if (!same_key || !g_label.src) {
        label_cache_reset();
        int src_size = label_src_size(cols, rows);
        unsigned char *src = NULL;
        if (load_scaled_rgb(image_path, src_size, src_size, &src) != 0) {
            snprintf(g_label.path, sizeof(g_label.path), "%s", image_path);
            g_label.cols = cols;
            g_label.rows = rows;
            g_label.failed = 1;
            g_label.angle_bucket = bucket;
            return NULL;
        }
        g_label.src = src;
        g_label.src_size = src_size;
        snprintf(g_label.path, sizeof(g_label.path), "%s", image_path);
        g_label.cols = cols;
        g_label.rows = rows;
    }

    CoverArtGrid grid;
    if (cover_art_cells_build_label(g_label.src, g_label.src_size,
                                    g_label.src_size, cols, rows, angle,
                                    g_label_tier, g_pair_plan.label_budget,
                                    g_ascii ? 0 : 1, &grid) != 0 ||
        !grid.colorized ||
        bind_grid_pairs(g_pair_plan.label_base, g_pair_plan.label_budget,
                        &grid) != 0) {
        g_label.valid = 0;
        g_label.failed = 1;
        g_label.angle_bucket = bucket;
        return NULL;
    }

    g_label.grid = grid;
    g_label.angle_bucket = bucket;
    g_label.valid = 1;
    g_label.failed = 0;
    return &g_label.grid;
}

int cover_art_vinyl_colors(int *tier, int *pair_base, int *pair_budget)
{
    if (tier) {
        *tier = 0;
    }
    if (pair_base) {
        *pair_base = 0;
    }
    if (pair_budget) {
        *pair_budget = 0;
    }

    cover_art_configure();
    if (g_vinyl_tier == 0 ||
        g_pair_plan.fixed_budget < COVER_ART_VINYL_FIXED_PAIRS) {
        return 0;
    }
    if (tier) {
        *tier = g_vinyl_tier;
    }
    if (pair_base) {
        *pair_base = g_pair_plan.fixed_base;
    }
    if (pair_budget) {
        *pair_budget = g_pair_plan.fixed_budget;
    }
    return 1;
}

static const char *glyph_text(unsigned char glyph)
{
    switch (glyph) {
        case COVER_GLYPH_UPPER: return "\xe2\x96\x80";  /* ▀ */
        case COVER_GLYPH_LOWER: return "\xe2\x96\x84";  /* ▄ */
        case COVER_GLYPH_FULL:  return "\xe2\x96\x88";  /* █ */
        case COVER_GLYPH_ASCII: return "#";
        case COVER_GLYPH_SPACE:
        default:                return " ";
    }
}

void cover_art_draw(WINDOW *win, int x, int y,
                    const CoverArtGrid *grid, int body_attr)
{
    if (!win || !grid) {
        return;
    }

    int h, w;
    getmaxyx(win, h, w);
    if (h < 3 || w < 3) {
        return;
    }

    for (int row = 0; row < grid->rows; row++) {
        for (int col = 0; col < grid->cols; col++) {
            int screen_row = y + row;
            int screen_col = x + col;
            if (screen_row < 1 || screen_row >= h - 1 ||
                screen_col < 1 || screen_col >= w - 1) {
                continue;
            }

            const CoverArtCell *cell = &grid->cells[row][col];
            int attr = body_attr;
            if (grid->colorized && cell->pair > 0) {
                attr = COLOR_PAIR(cell->pair);
            }
            wattrset(win, attr);
            mvwaddstr(win, screen_row, screen_col, glyph_text(cell->glyph));
        }
    }
    wattrset(win, body_attr);
}
