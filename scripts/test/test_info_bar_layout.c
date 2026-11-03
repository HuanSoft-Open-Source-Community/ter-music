/**
 * @file test_info_bar_layout.c
 * @brief ui/info_bar_layout.c 的单元测试
 *
 * 覆盖：横向/纵向模式选择、封面隐藏与显示、奇数宽度、极窄/极矮窗格，
 * 以及“所有矩形落在内容区内、互不重叠、封面永不贴右边框”的硬性不变量。
 * 另含手工构造的非法布局，确保 info_bar_layout_validate() 能拒绝它们。
 *
 * 运行：scripts/test/run-unit-tests.sh test_info_bar_layout
 * 依赖（见同名 .srcs）：src/org.yxzl.ter-music/ui/info_bar_layout.c
 */

#include "ui/info_bar_layout.h"

#include <stdio.h>

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

static int rect_inside_content(const InfoBarRect *r, int w, int h)
{
    return r->x >= 1 && r->y >= 1 && r->w >= 0 && r->h >= 0 &&
           r->x + r->w <= w - 1 && r->y + r->h <= h - 1;
}

static int rects_overlap(const InfoBarRect *a, const InfoBarRect *b)
{
    if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0) {
        return 0;
    }
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int expect_horizontal(int w, int h)
{
    int cw = w - 2 - INFO_BAR_PAD_LEFT;
    int ch = h - 2;
    return cw >= INFO_BAR_VERTICAL_WIDTH ||
           (cw >= INFO_BAR_COLUMNS_MIN_WIDTH &&
            ch < INFO_BAR_VERTICAL_MIN_ROWS);
}

static void check_one(int w, int h, int want_cover)
{
    InfoBarLayout layout;
    info_bar_layout_compute(w, h, want_cover, &layout);

    if (!layout.valid) {
        CHECK(w < 6 || h < 3, "only panes below 6x3 are invalid");
        CHECK(!info_bar_layout_validate(w, h, &layout),
              "an invalid layout must not validate");
        return;
    }

    CHECK(w >= 6 && h >= 3, "valid layouts need at least 6x3 cells");
    CHECK(info_bar_layout_validate(w, h, &layout),
          "computed layout must validate");

    CHECK(rect_inside_content(&layout.metadata, w, h),
          "metadata stays inside the content rectangle");
    CHECK(rect_inside_content(&layout.audio, w, h),
          "audio stays inside the content rectangle");
    CHECK(layout.metadata.w > 0 && layout.metadata.h > 0,
          "metadata region is non-empty");
    CHECK(layout.audio.w > 0, "audio region has a width");

    CHECK(layout.metadata_rows >= 1 &&
          layout.metadata_rows <= INFO_BAR_METADATA_LINES,
          "metadata row count is 1..5");
    CHECK(layout.metadata_rows <= layout.metadata.h,
          "metadata rows fit the region");
    CHECK(layout.audio_rows >= 0 &&
          layout.audio_rows <= INFO_BAR_AUDIO_LINES,
          "audio row count is 0..4");
    CHECK(layout.audio_rows <= layout.audio.h,
          "audio rows fit the region");

    CHECK(!rects_overlap(&layout.metadata, &layout.audio),
          "metadata and audio never overlap");

    if (layout.stacked) {
        CHECK(layout.audio.x == layout.metadata.x &&
              layout.audio.w == layout.metadata.w,
              "stacked blocks share the content width");
        CHECK(layout.metadata.y + layout.metadata.h <= layout.audio.y,
              "stacked audio starts below metadata");
    } else {
        CHECK(layout.audio.y == layout.metadata.y &&
              layout.audio.h == layout.metadata.h,
              "columns share the same rows");
        CHECK(layout.metadata.x + layout.metadata.w < layout.audio.x,
              "columns keep at least one blank column between them");
    }

    CHECK((layout.stacked == 0) == expect_horizontal(w, h),
          "horizontal/stacked choice follows the documented rule");

    if (layout.cover.w == 0 && layout.cover.h == 0) {
        /* 隐藏封面没有矩形；want_cover=1 时隐藏本身是合法的降级。 */
    } else {
        CHECK(layout.cover.w == 2 * layout.cover.h,
              "cover is two columns per row");
        CHECK(layout.cover.h >= INFO_BAR_COVER_MIN_ROWS &&
              layout.cover.h <= INFO_BAR_COVER_MAX_ROWS,
              "cover height is 5..12 rows");
        CHECK(rect_inside_content(&layout.cover, w, h),
              "cover stays inside the content rectangle");
        CHECK(layout.cover.x + layout.cover.w <= w - 2,
              "cover keeps a blank content column before the right border");
        CHECK(!rects_overlap(&layout.cover, &layout.metadata) &&
              !rects_overlap(&layout.cover, &layout.audio),
              "cover does not overlap text blocks");
        CHECK(layout.cover.w > 0 && layout.cover.h > 0,
              "a visible cover has both dimensions");
    }
}

static void check_fixed_cases(void)
{
    InfoBarLayout layout;

    /* 200x50 终端下 stage 布局的信息窗格（100x17，宽而矮）。 */
    info_bar_layout_compute(100, 17, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "stage info pane uses columns");
    CHECK(layout.cover.w == 24 && layout.cover.h == 12,
          "stage cover is 24x12");
    CHECK(layout.cover.x == 73, "stage cover starts at x=73");
    CHECK(layout.cover.x + layout.cover.w == 97,
          "stage cover leaves two columns before the border");
    CHECK(layout.metadata.w == 35 && layout.audio.w == 34,
          "stage text columns are 35/34");
    CHECK(layout.audio.x == 38, "stage audio starts after one gutter column");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "stage draws all five metadata and four audio lines");

    /* 140x60 终端下 stage 布局的信息窗格（70x21）。 */
    info_bar_layout_compute(70, 21, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "70x21 info pane uses columns");
    CHECK(layout.cover.w == 16 && layout.cover.h == 8,
          "70x21 cover shrinks to 16x8");
    CHECK(layout.cover.x + layout.cover.w == 67,
          "70x21 cover ends at w-4");
    CHECK(layout.metadata.w >= INFO_BAR_MIN_COL_WIDTH &&
          layout.audio.w >= INFO_BAR_MIN_COL_WIDTH,
          "70x21 keeps both text columns usable");
    CHECK(layout.cover.y == 6, "70x21 cover is vertically centred");
    CHECK(layout.metadata.y == 8 && layout.audio.y == layout.metadata.y,
          "70x21 text block is vertically centred");

    /* 120x40 终端下 stage 布局的信息窗格（60x13）：窄而高，纵向。 */
    info_bar_layout_compute(60, 13, 1, &layout);
    CHECK(layout.valid && layout.stacked, "60x13 info pane stacks");
    CHECK(layout.cover.w == 0 && layout.cover.h == 0,
          "60x13 pane is too short for a cover");
    CHECK(layout.metadata.y == 2 && layout.metadata.w == 57,
          "stacked metadata is centred and uses the full content width");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "stacked mode keeps all nine text lines");

    /* 100x24 终端下 stage 布局的信息窗格（50x8）：窄且矮，横向无封面。 */
    info_bar_layout_compute(50, 8, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "50x8 info pane uses columns");
    CHECK(layout.cover.w == 0 && layout.cover.h == 0,
          "50x8 pane has no room for a cover");
    CHECK(layout.metadata.w == 23 && layout.audio.w == 23,
          "50x8 splits into 23/23 columns");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "50x8 still draws all text lines");

    /* 90x50 终端下 stage 布局的信息窗格（45x17）：窄而高，封面居上。 */
    info_bar_layout_compute(45, 17, 1, &layout);
    CHECK(layout.valid && layout.stacked, "45x17 info pane stacks");
    CHECK(layout.cover.w == 10 && layout.cover.h == 5,
          "45x17 stacks a 10x5 cover on top");
    CHECK(layout.cover.x == 18, "45x17 cover is centred");
    CHECK(layout.metadata.y == 7,
          "45x17 text starts below the cover and one blank row");
    CHECK(layout.metadata.w == 42 && layout.audio.w == 42,
          "45x17 stacked text fills the content width");

    /* 自定义高窄侧栏：30x40，封面 24x12 居中，文字全宽。 */
    info_bar_layout_compute(30, 40, 1, &layout);
    CHECK(layout.valid && layout.stacked, "30x40 side pane stacks");
    CHECK(layout.cover.w == 24 && layout.cover.h == 12,
          "30x40 shows the maximum cover");
    CHECK(layout.cover.x == 3, "30x40 cover is centred");
    CHECK(layout.cover.x + layout.cover.w == 27,
          "30x40 cover leaves one column before the border");
    CHECK(layout.cover.y == 9, "30x40 cover sits at the centred block top");
    CHECK(layout.metadata.y == 22,
          "30x40 text starts right below the centred cover");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "30x40 keeps all text lines");

    /* 高而宽的窗格：文字块在内容区里垂直居中（无封面）。 */
    info_bar_layout_compute(70, 25, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "70x25 info pane uses columns");
    CHECK(layout.metadata.y == 10 && layout.audio.y == layout.metadata.y,
          "70x25 text block is vertically centred");
    CHECK(layout.metadata.h == 5 && layout.audio.h == 5,
          "70x25 text blocks shrink to the drawn rows");

    /* 更窄：16 宽仍可放 10x5 封面；14 宽则不放。 */
    info_bar_layout_compute(16, 40, 1, &layout);
    CHECK(layout.valid && layout.stacked, "16x40 side pane stacks");
    CHECK(layout.cover.w == 10 && layout.cover.h == 5,
          "16x40 fits a 10x5 cover with margins");
    CHECK(layout.cover.x == 3 &&
          layout.cover.x + layout.cover.w == 13,
          "16x40 cover keeps one blank column on the right");
    info_bar_layout_compute(14, 40, 1, &layout);
    CHECK(layout.valid && layout.cover.w == 0,
          "14x40 is too narrow for a cover with margins");

    /* 极矮窗格：横向两列，行数按内容高度截断。 */
    info_bar_layout_compute(120, 3, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "120x3 uses columns");
    CHECK(layout.metadata_rows == 1 && layout.audio_rows == 1,
          "120x3 draws one line per column");
    CHECK(layout.cover.w == 0, "120x3 has no cover");

    info_bar_layout_compute(100, 4, 1, &layout);
    CHECK(layout.metadata_rows == 2 && layout.audio_rows == 2,
          "100x4 draws two lines per column");

    /* 无封面请求：文字列应占满内容宽度，且不预留封面空间。 */
    info_bar_layout_compute(100, 17, 0, &layout);
    CHECK(layout.valid && layout.cover.w == 0,
          "want_cover=0 never shows a cover");
    CHECK(layout.metadata.w == 48 && layout.audio.w == 48,
          "want_cover=0 gives the text the full width");

    /* 65 宽内容：横向，封面 14x7。 */
    info_bar_layout_compute(68, 12, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "68x12 uses columns");
    CHECK(layout.cover.w == 14 && layout.cover.h == 7,
          "68x12 shows a 14x7 cover");
    CHECK(layout.metadata.w >= INFO_BAR_MIN_COL_WIDTH,
          "68x12 keeps the metadata column usable");

    /* 62 宽内容、矮：横向；49 宽文字区仍能放 10x5 封面。 */
    info_bar_layout_compute(64, 10, 1, &layout);
    CHECK(layout.valid && !layout.stacked, "64x10 uses columns");
    CHECK(layout.cover.w == 10 && layout.cover.h == 5,
          "64x10 shows a 10x5 cover");

    /* 极窄/极矮：无效或退化到单列，绝不越界。 */
    info_bar_layout_compute(6, 3, 1, &layout);
    CHECK(layout.valid && layout.stacked, "6x3 degrades to one column");
    CHECK(layout.metadata_rows == 1 && layout.audio_rows == 0,
          "6x3 draws one metadata line and no audio");
    CHECK(layout.cover.w == 0, "6x3 has no cover");

    info_bar_layout_compute(6, 2, 1, &layout);
    CHECK(!layout.valid, "6x2 is too short");
    info_bar_layout_compute(5, 10, 1, &layout);
    CHECK(!layout.valid, "5x10 is too narrow");
}

/* 手工构造非法布局，确认校验器能逐条拒绝。 */
static void check_invalid_layouts(void)
{
    InfoBarLayout layout;

    info_bar_layout_compute(100, 17, 1, &layout);
    CHECK(info_bar_layout_validate(100, 17, &layout),
          "base layout validates");

    InfoBarLayout bad = layout;

    bad.cover.h = bad.cover.w; /* 不再是 2:1 */
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "non-2:1 cover is rejected");

    bad = layout;
    bad.cover.x = 100 - 2; /* 贴到最后一个内容列 */
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "cover touching the last content column is rejected");

    bad = layout;
    bad.metadata.w = 200;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "metadata outside the pane is rejected");

    bad = layout;
    bad.metadata_rows = INFO_BAR_METADATA_LINES + 1;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "too many metadata rows is rejected");

    bad = layout;
    bad.audio_rows = INFO_BAR_AUDIO_LINES + 1;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "too many audio rows is rejected");

    bad = layout;
    bad.audio.x = bad.metadata.x; /* 两列重叠 */
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "overlapping columns are rejected");

    info_bar_layout_compute(60, 13, 1, &layout);
    CHECK(info_bar_layout_validate(60, 13, &layout),
          "stacked base layout validates");

    bad = layout;
    bad.audio.y = bad.metadata.y; /* 纵向顺序错误 */
    CHECK(!info_bar_layout_validate(60, 13, &bad),
          "stacked audio above metadata is rejected");

    info_bar_layout_compute(100, 17, 0, &layout);
    bad = layout;
    bad.cover.w = 2; /* 隐藏封面却只有一个维度 */
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "half-specified hidden cover is rejected");
}

int main(void)
{
    check_fixed_cases();

    for (int width = 1; width <= 320; width++) {
        for (int height = 1; height <= 120; height++) {
            check_one(width, height, 0);
            check_one(width, height, 1);
        }
    }

    check_invalid_layouts();

    if (g_failures == 0) {
        printf("PASS info_bar_layout (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL info_bar_layout (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
