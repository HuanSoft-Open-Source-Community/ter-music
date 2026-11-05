/**
 * @file test_cover_art_cells.c
 * @brief ui/cover_art_cells.c 的单元测试
 *
 * 覆盖：色阶选择、三槽 pair 预算拆分（含黑胶固定色对/标签槽）、彩色 ▀
 * 双层规则、单色/ASCII 回退、配色压缩（唯一 pair 数不超过预算）、
 * 黑胶标签的圆形掩码与旋转采样，以及非法参数。
 *
 * 运行：scripts/test/run-unit-tests.sh test_cover_art_cells
 * 依赖（见同名 .srcs）：src/cn.org.huansoft.ter-music/ui/cover_art_cells.c
 * 链接（见同名 .env）：-lm（标签旋转采样使用 sin/cos/lround）
 */

#include "ui/cover_art_cells.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

/* 写一个 cols x 1 的单元：top/bottom 各一行像素。 */
static void put_cell(unsigned char *rgb, int cols, int x,
                     int tr, int tg, int tb, int br, int bg, int bb)
{
    rgb[(x * 3) + 0] = (unsigned char)tr;
    rgb[(x * 3) + 1] = (unsigned char)tg;
    rgb[(x * 3) + 2] = (unsigned char)tb;
    unsigned char *bottom = rgb + (size_t)cols * 3;
    bottom[(x * 3) + 0] = (unsigned char)br;
    bottom[(x * 3) + 1] = (unsigned char)bg;
    bottom[(x * 3) + 2] = (unsigned char)bb;
}

static void check_tier_and_plan(void)
{
    CHECK(cover_art_tier_from_caps(0, 256, 1) == 0,
          "monochrome terminal falls back to mono");
    CHECK(cover_art_tier_from_caps(1, 256, 0) == 0,
          "ASCII fallback forces mono");
    CHECK(cover_art_tier_from_caps(1, 7, 1) == 0,
          "fewer than 8 colors falls back to mono");
    CHECK(cover_art_tier_from_caps(1, 8, 1) == 8,
          "8-color terminal selects the 8-color tier");
    CHECK(cover_art_tier_from_caps(1, 16, 1) == 8,
          "16-color terminal uses the 8-color tier");
    CHECK(cover_art_tier_from_caps(1, 88, 1) == 8,
          "88-color terminal uses the 8-color tier");
    CHECK(cover_art_tier_from_caps(1, 256, 1) == 256,
          "256-color terminal selects the 256-color tier");

    CoverArtPairPlan plan;

    cover_art_pair_plan(65536, &plan);
    CHECK(plan.color_vinyl == 1,
          "large COLOR_PAIRS enables colored vinyl");
    CHECK(plan.info_base == 32 && plan.info_budget == 106,
          "large pair plan gives info 106 pairs at base 32");
    CHECK(plan.controls_base == 138 && plan.controls_budget == 53,
          "large pair plan gives controls 53 pairs at base 138");
    CHECK(plan.label_base == 191 && plan.label_budget == 53,
          "large pair plan gives the vinyl label 53 pairs at base 191");
    CHECK(plan.fixed_base == 244 && plan.fixed_budget == 12,
          "large pair plan reserves 12 fixed vinyl pairs at base 244");
    CHECK(plan.fixed_base + plan.fixed_budget <= 256,
          "fixed vinyl pairs stay inside the 8-bit pair field");

    cover_art_pair_plan(256, &plan);
    CHECK(plan.info_budget == 106 && plan.controls_budget == 53 &&
          plan.label_budget == 53 && plan.fixed_budget == 12,
          "256 COLOR_PAIRS uses the same capped 106/53/53+12 plan");

    cover_art_pair_plan(128, &plan);
    CHECK(plan.color_vinyl == 1 &&
          plan.info_budget == 42 && plan.controls_budget == 21 &&
          plan.label_budget == 21 && plan.fixed_budget == 12,
          "128 COLOR_PAIRS splits 42/21/21 plus 12 fixed pairs");

    /* 64 个可用色对恰好启用彩色黑胶；63 个则不启用，封面保持旧比例。 */
    cover_art_pair_plan(96, &plan);
    CHECK(plan.color_vinyl == 1 &&
          plan.info_budget == 26 && plan.controls_budget == 13 &&
          plan.label_budget == 13 && plan.fixed_budget == 12,
          "96 COLOR_PAIRS is the minimum colored-vinyl plan 26/13/13+12");
    cover_art_pair_plan(95, &plan);
    CHECK(plan.color_vinyl == 0 && plan.label_budget == 0 &&
          plan.fixed_budget == 0 && plan.info_budget == 42 &&
          plan.controls_budget == 21,
          "95 COLOR_PAIRS keeps the old 42/21 cover split");

    cover_art_pair_plan(64, &plan);
    CHECK(plan.color_vinyl == 0 && plan.info_budget == 21 &&
          plan.controls_budget == 11,
          "64 pairs keep the old 21/11 cover split");
    cover_art_pair_plan(32, &plan);
    CHECK(plan.color_vinyl == 0 && plan.info_budget == 0 &&
          plan.controls_budget == 0,
          "32 pairs leaves no cover pairs");
    cover_art_pair_plan(0, &plan);
    CHECK(plan.color_vinyl == 0 && plan.info_budget == 0 &&
          plan.controls_budget == 0,
          "no pairs leaves no cover pairs");
    cover_art_pair_plan(65536, NULL); /* 不得崩溃 */
}

static void check_color_cells(void)
{
    CoverArtGrid grid;
    unsigned char rgb[6];

    /* 上红下蓝：彩色 ▀，fg=上半，bg=下半，二者不同。 */
    put_cell(rgb, 1, 0, 255, 0, 0, 0, 0, 255);
    CHECK(cover_art_cells_build(rgb, 1, 1, 8, 64, 1, &grid) == 0,
          "8-color build succeeds");
    CHECK(grid.colorized == 1, "red/blue is colorized");
    CHECK(grid.cells[0][0].glyph == COVER_GLYPH_UPPER,
          "color mode always uses the upper-half block");
    CHECK(grid.cells[0][0].fg >= 0 && grid.cells[0][0].fg < 8 &&
          grid.cells[0][0].bg >= 0 && grid.cells[0][0].bg < 8,
          "8-color indices stay in 0..7");
    CHECK(grid.cells[0][0].fg != grid.cells[0][0].bg,
          "red top and blue bottom get different colors");
    CHECK(grid.pair_count == 1, "red/blue uses one color pair");

    /* 上下同红：仍然 ▀，fg==bg，视觉上是全块。 */
    put_cell(rgb, 1, 0, 255, 0, 0, 255, 0, 0);
    CHECK(cover_art_cells_build(rgb, 1, 1, 8, 64, 1, &grid) == 0,
          "same-color build succeeds");
    CHECK(grid.colorized == 1 &&
          grid.cells[0][0].glyph == COVER_GLYPH_UPPER,
          "same-color cell still uses the upper-half block");
    CHECK(grid.cells[0][0].fg == grid.cells[0][0].bg,
          "same-color halves produce fg == bg");

    /* 256 色档：索引落在 16..255。 */
    put_cell(rgb, 1, 0, 255, 0, 0, 0, 0, 255);
    CHECK(cover_art_cells_build(rgb, 1, 1, 256, 64, 1, &grid) == 0,
          "256-color build succeeds");
    CHECK(grid.colorized == 1, "256-color image is colorized");
    CHECK(grid.cells[0][0].fg >= 16 && grid.cells[0][0].fg <= 255 &&
          grid.cells[0][0].bg >= 16 && grid.cells[0][0].bg <= 255,
          "256-color indices stay in 16..255");

    /* 预算不足：单色。 */
    put_cell(rgb, 1, 0, 255, 0, 0, 0, 0, 255);
    CHECK(cover_art_cells_build(rgb, 1, 1, 8, 3, 1, &grid) == 0,
          "tiny pair budget still succeeds");
    CHECK(grid.colorized == 0, "pair budget below 4 falls back to mono");
    CHECK(grid.cells[0][0].fg == -1 && grid.cells[0][0].bg == -1,
          "mono cells have no color indices");

    /* ASCII 回退：没有半块字形，只有 #/空格。 */
    put_cell(rgb, 1, 0, 255, 255, 255, 255, 255, 255);
    CHECK(cover_art_cells_build(rgb, 1, 1, 8, 64, 0, &grid) == 0,
          "ASCII build succeeds");
    CHECK(grid.colorized == 0, "ASCII fallback is mono");
    CHECK(grid.cells[0][0].glyph == COVER_GLYPH_ASCII,
          "bright ASCII cell uses '#'");
}

static void check_mono_cells(void)
{
    CoverArtGrid grid;
    unsigned char rgb[6];

    put_cell(rgb, 1, 0, 255, 255, 255, 0, 0, 0);
    CHECK(cover_art_cells_build(rgb, 1, 1, 0, 64, 1, &grid) == 0,
          "mono build succeeds");
    CHECK(grid.colorized == 0 &&
          grid.cells[0][0].glyph == COVER_GLYPH_UPPER,
          "white top / black bottom uses the upper half block");
    CHECK(grid.cells[0][0].fg == -1 && grid.cells[0][0].bg == -1,
          "mono upper block has no colors");

    put_cell(rgb, 1, 0, 255, 255, 255, 255, 255, 255);
    CHECK(cover_art_cells_build(rgb, 1, 1, 0, 64, 1, &grid) == 0,
          "mono full-bright build succeeds");
    CHECK(grid.cells[0][0].glyph == COVER_GLYPH_FULL,
          "both halves bright use the full block in mono");

    put_cell(rgb, 1, 0, 0, 0, 0, 0, 0, 0);
    CHECK(cover_art_cells_build(rgb, 1, 1, 0, 64, 1, &grid) == 0,
          "mono all-dark build succeeds");
    CHECK(grid.cells[0][0].glyph == COVER_GLYPH_SPACE,
          "both halves dark use a space in mono");
}

static void check_palette_reduction(void)
{
    CoverArtGrid grid;
    enum { COLS = 8 };
    unsigned char rgb[COLS * 2 * 3];

    /* 8 个不同的上下色组合，制造较多唯一 pair。 */
    for (int x = 0; x < COLS; x++) {
        int tr = (x * 31) % 256;
        int tg = (x * 67) % 256;
        int tb = (x * 97) % 256;
        int br = (x * 131) % 256;
        int bg = (x * 173) % 256;
        int bb = (x * 211) % 256;
        put_cell(rgb, COLS, x, tr, tg, tb, br, bg, bb);
    }

    CHECK(cover_art_cells_build(rgb, COLS, 1, 256, 80, 1, &grid) == 0,
          "large palette build succeeds");
    CHECK(grid.pair_count <= 80, "pair count respects an 80-pair budget");

    CHECK(cover_art_cells_build(rgb, COLS, 1, 256, 6, 1, &grid) == 0,
          "tight palette build succeeds");
    CHECK(grid.pair_count <= 6, "pair count respects a 6-pair budget");
    if (grid.colorized) {
        for (int x = 0; x < COLS; x++) {
            CHECK(grid.cells[0][x].glyph == COVER_GLYPH_UPPER,
                  "colorized cells all use the upper half block");
        }
    }

    CHECK(cover_art_cells_build(rgb, COLS, 1, 256, 4, 1, &grid) == 0,
          "minimum palette build succeeds");
    CHECK(grid.pair_count <= 4, "pair count respects the minimum 4-pair budget");
}

static void fill_split_source(unsigned char *rgb, int size)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            unsigned char *p =
                rgb + ((size_t)y * (size_t)size + (size_t)x) * 3;
            if (x < size / 2) {
                p[0] = 255;
                p[1] = 0;
                p[2] = 0;
            } else {
                p[0] = 0;
                p[1] = 0;
                p[2] = 255;
            }
        }
    }
}

static void check_label_cells(void)
{
    enum { SRC = 16, COLS = 5, ROWS = 5 };
    unsigned char src[SRC * SRC * 3];
    CoverArtGrid upright;
    CoverArtGrid flipped;

    fill_split_source(src, SRC);

    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, 0.0,
                                      8, 64, 1, &upright) == 0,
          "label build succeeds");
    CHECK(upright.colorized == 1, "label is colorized");
    CHECK(upright.cells[0][0].glyph == COVER_GLYPH_SPACE,
          "label corner is masked");
    CHECK(upright.cells[0][0].fg == -1 && upright.cells[0][0].bg == -1,
          "masked label cell has no colors");
    CHECK(upright.cells[2][2].glyph == COVER_GLYPH_UPPER,
          "label center uses the upper-half block");
    CHECK(upright.cells[2][2].fg == upright.cells[2][2].bg,
          "flat source halves share one color");
    CHECK(upright.cells[2][0].fg == 1,
          "label left edge samples the red half");
    CHECK(upright.cells[2][0].bg == 1,
          "label left edge bottom also samples red");
    CHECK(upright.pair_count <= 64, "label respects the pair budget");

    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, M_PI,
                                      8, 64, 1, &flipped) == 0,
          "rotated label build succeeds");
    CHECK(flipped.cells[2][0].fg == 4,
          "half-turn rotation swaps the label's left edge to blue");
    CHECK(flipped.cells[2][0].fg != upright.cells[2][0].fg,
          "rotation changes sampled label colors");

    /* 颜色不可用：标签不用单色块，明确交给调用方回退字符。 */
    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, 0.0,
                                      0, 64, 1, &upright) == 0,
          "mono label build returns success");
    CHECK(upright.colorized == 0, "mono label is not colorized");
    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, 0.0,
                                      8, 64, 0, &upright) == 0,
          "ASCII label build returns success");
    CHECK(upright.colorized == 0, "ASCII label is not colorized");
    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, 0.0,
                                      8, 3, 1, &upright) == 0,
          "tight label budget returns success");
    CHECK(upright.colorized == 0,
          "label budget below 4 falls back to characters");

    /* 极小预算：2 色最多 4 个 pair，仍应装得下。 */
    CHECK(cover_art_cells_build_label(src, SRC, SRC, COLS, ROWS, 0.0,
                                      8, 4, 1, &upright) == 0,
          "minimum label budget builds");
    CHECK(upright.colorized == 1 && upright.pair_count <= 4,
          "label respects the minimum 4-pair budget");
}

static void check_invalid_arguments(void)
{
    CoverArtGrid grid;
    unsigned char rgb[6] = {0};
    unsigned char src[16 * 16 * 3] = {0};

    CHECK(cover_art_cells_build(NULL, 1, 1, 8, 64, 1, &grid) == -1,
          "NULL rgb is rejected");
    CHECK(cover_art_cells_build(rgb, 1, 1, 8, 64, 1, NULL) == -1,
          "NULL output is rejected");
    CHECK(cover_art_cells_build(rgb, 0, 1, 8, 64, 1, &grid) == -1,
          "zero columns rejected");
    CHECK(cover_art_cells_build(rgb, 1, 0, 8, 64, 1, &grid) == -1,
          "zero rows rejected");
    CHECK(cover_art_cells_build(rgb, COVER_ART_MAX_COLS + 1, 1, 8, 64, 1,
                                &grid) == -1,
          "too many columns rejected");
    CHECK(cover_art_cells_build(rgb, 1, COVER_ART_MAX_ROWS + 1, 8, 64, 1,
                                &grid) == -1,
          "too many rows rejected");
    CHECK(cover_art_cells_build(rgb, 1, 1, 42, 64, 1, &grid) == -1,
          "unknown color tier rejected");

    CHECK(cover_art_cells_build_label(NULL, 16, 16, 5, 5, 0.0,
                                      8, 64, 1, &grid) == -1,
          "NULL label source is rejected");
    CHECK(cover_art_cells_build_label(src, 16, 16, 5, 5, 0.0,
                                      8, 64, 1, NULL) == -1,
          "NULL label output is rejected");
    CHECK(cover_art_cells_build_label(src, 0, 16, 5, 5, 0.0,
                                      8, 64, 1, &grid) == -1,
          "zero-width label source is rejected");
    CHECK(cover_art_cells_build_label(src, 16, 16, 2, 5, 0.0,
                                      8, 64, 1, &grid) == -1,
          "label needs at least three columns");
    CHECK(cover_art_cells_build_label(src, 16, 16, 5, 2, 0.0,
                                      8, 64, 1, &grid) == -1,
          "label needs at least three rows");
    CHECK(cover_art_cells_build_label(src, 16, 16, COVER_ART_MAX_COLS + 1, 5,
                                      0.0, 8, 64, 1, &grid) == -1,
          "too many label columns rejected");
    CHECK(cover_art_cells_build_label(src, 16, 16, 5, COVER_ART_MAX_ROWS + 1,
                                      0.0, 8, 64, 1, &grid) == -1,
          "too many label rows rejected");
    CHECK(cover_art_cells_build_label(src, 16, 16, 5, 5, 0.0,
                                      42, 64, 1, &grid) == -1,
          "unknown label color tier rejected");
    CHECK(cover_art_cells_build_label(src, 16, 16, 5, 5, NAN,
                                      8, 64, 1, &grid) == -1,
          "non-finite label rotation rejected");
}

int main(void)
{
    check_tier_and_plan();
    check_color_cells();
    check_mono_cells();
    check_palette_reduction();
    check_label_cells();
    check_invalid_arguments();

    if (g_failures == 0) {
        printf("PASS cover_art_cells (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL cover_art_cells (%d/%d checks failed)\n",
           g_failures, g_checks);
    return 1;
}
