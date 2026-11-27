/**
 * @file test_info_bar_layout.c
 * @brief ui/info_bar_layout.c 的单元测试（自适应布局 t22）
 *
 * 覆盖：
 *   - 尺寸矩阵 w=8..200 × h=3..40 × cover on/off × 三种 cover 优先级 ×
 *     元数据 0..5 行 × 音频 0..4 行：矩形合法、互不重叠、validate 恒真；
 *   - 两列动态宽度：内容宽度不同时左/右列按需分配；某块无行时让出全部宽度；
 *   - 高度不足按 per-line priority 裁剪：keep_mask 只保留低优先级数值的行，
 *     显示顺序不变（bit i 仍对应第 i 行）；
 *   - 宽而矮时块内多列流动（1..3 列），行尽量全部可见；
 *   - 封面优先级 auto/text-first/cover-first 的差异与最小 3 行封面；
 *   - 代表性尺寸的精确几何 + 手工构造非法布局必须被 validate 拒绝。
 *
 * 运行：scripts/test/run-unit-tests.sh test_info_bar_layout
 */

#include "ui/info_bar_layout.h"

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

static int rect_empty(const InfoBarRect *r)
{
    return !r || r->w <= 0 || r->h <= 0;
}

static int rect_inside_content(const InfoBarRect *r, int w, int h)
{
    return r->x >= 1 && r->y >= 1 && r->w >= 0 && r->h >= 0 &&
           r->x + r->w <= w - 1 && r->y + r->h <= h - 1;
}

static int rects_overlap(const InfoBarRect *a, const InfoBarRect *b)
{
    if (rect_empty(a) || rect_empty(b)) {
        return 0;
    }
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int popcount_u(unsigned v)
{
    int n = 0;
    while (v) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static int expect_horizontal(int w, int h)
{
    int cw = w - 2 - INFO_BAR_PAD_LEFT;
    int ch = h - 2;
    return cw >= INFO_BAR_VERTICAL_WIDTH ||
           (cw >= INFO_BAR_COLUMNS_MIN_WIDTH &&
            ch < INFO_BAR_VERTICAL_MIN_ROWS);
}

static int content_width(int w)
{
    return w - 2 - INFO_BAR_PAD_LEFT;
}

/* 一个组合的全部不变量。desired 宽度用 0（未知）或确定性值。 */
static void check_one(int w, int h, int want_cover, int cover_priority,
                      int metadata_rows, int audio_rows,
                      int metadata_desired, int audio_desired)
{
    int mp[INFO_BAR_METADATA_LINES];
    int ap[INFO_BAR_AUDIO_LINES];
    for (int i = 0; i < INFO_BAR_METADATA_LINES; i++) {
        mp[i] = i;
    }
    for (int i = 0; i < INFO_BAR_AUDIO_LINES; i++) {
        ap[i] = i;
    }

    InfoBarLayout layout;
    info_bar_layout_compute_adaptive(w, h, want_cover, cover_priority,
                                     metadata_rows, audio_rows, mp, ap,
                                     metadata_desired, audio_desired,
                                     &layout);

    int should_be_valid = (w >= 6 && h >= 3);
    CHECK(layout.valid == should_be_valid,
          "valid matches minimum pane size (w>=6 && h>=3)");
    if (!should_be_valid) {
        CHECK(!info_bar_layout_validate(w, h, &layout),
              "too-small pane never validates");
        return;
    }
    CHECK(info_bar_layout_validate(w, h, &layout),
          "computed layout always validates");
    CHECK(layout.valid, "pane at least 6x3 is valid");

    CHECK((layout.stacked == 0) == expect_horizontal(w, h),
          "orientation follows the documented rule");

    /* 矩形本身。 */
    if (!rect_empty(&layout.metadata)) {
        CHECK(rect_inside_content(&layout.metadata, w, h),
              "metadata inside content");
    }
    if (!rect_empty(&layout.audio)) {
        CHECK(rect_inside_content(&layout.audio, w, h),
              "audio inside content");
    }
    CHECK(!rects_overlap(&layout.metadata, &layout.audio),
          "text blocks never overlap");

    /* 行/列/keep_mask 语义。 */
    CHECK(layout.metadata_rows >= 0 &&
          layout.metadata_rows <= metadata_rows,
          "metadata rows are kept within request");
    CHECK(layout.audio_rows >= 0 && layout.audio_rows <= audio_rows,
          "audio rows are kept within request");
    CHECK(layout.metadata_rows <= INFO_BAR_METADATA_LINES &&
          layout.audio_rows <= INFO_BAR_AUDIO_LINES,
          "rows stay within block maxima");
    CHECK(popcount_u(layout.metadata_keep_mask) == layout.metadata_rows,
          "metadata keep_mask popcount == rows");
    CHECK(popcount_u(layout.audio_keep_mask) == layout.audio_rows,
          "audio keep_mask popcount == rows");

    if (metadata_rows > 0) {
        CHECK(layout.metadata_rows >= 1, "non-empty request keeps >=1 metadata row");
    }
    /* 纵向时元数据优先占用有限高度，音频可能整体让位（0 行）；只有横向
     * 两列并排时才要求非空请求至少保留一行。 */
    if (audio_rows > 0 && !layout.stacked) {
        CHECK(layout.audio_rows >= 1, "horizontal non-empty audio request keeps >=1 row");
    }

    if (layout.metadata_rows == 0) {
        CHECK(rect_empty(&layout.metadata) && layout.metadata_cols == 0 &&
              layout.metadata_keep_mask == 0,
              "zero metadata rows => empty rect/cols/mask");
    } else {
        CHECK(layout.metadata_cols >= 1 &&
              layout.metadata_cols <= INFO_BAR_FLOW_MAX_COLS,
              "metadata cols in 1..MAX");
        CHECK(!rect_empty(&layout.metadata), "metadata rows => rect present");
        int visual = (layout.metadata_rows + layout.metadata_cols - 1) /
                     layout.metadata_cols;
        CHECK(visual <= layout.metadata.h, "metadata visual rows fit region");
        int need_w = layout.metadata_cols * INFO_BAR_MIN_TEXT_COL_WIDTH +
                     (layout.metadata_cols - 1) * INFO_BAR_TEXT_COL_GAP;
        need_w = min_int(need_w, content_width(w));
        CHECK(layout.metadata.w >= need_w,
              "metadata block keeps minimum column width");
    }
    if (layout.audio_rows == 0) {
        CHECK(rect_empty(&layout.audio) && layout.audio_cols == 0 &&
              layout.audio_keep_mask == 0,
              "zero audio rows => empty rect/cols/mask");
    } else {
        CHECK(layout.audio_cols >= 1 &&
              layout.audio_cols <= INFO_BAR_FLOW_MAX_COLS,
              "audio cols in 1..MAX");
        CHECK(!rect_empty(&layout.audio), "audio rows => rect present");
        int visual = (layout.audio_rows + layout.audio_cols - 1) /
                     layout.audio_cols;
        CHECK(visual <= layout.audio.h, "audio visual rows fit region");
        int need_w = layout.audio_cols * INFO_BAR_MIN_TEXT_COL_WIDTH +
                     (layout.audio_cols - 1) * INFO_BAR_TEXT_COL_GAP;
        need_w = min_int(need_w, content_width(w));
        CHECK(layout.audio.w >= need_w,
              "audio block keeps minimum column width");
    }

    /* 某块无行时，另一块拿到全部文字宽度。 */
    if (metadata_rows == 0 && audio_rows > 0 && layout.audio_rows > 0 &&
        layout.cover.w == 0) {
        CHECK(layout.audio.w == content_width(w),
              "audio-only takes the full content width");
    }
    if (audio_rows == 0 && metadata_rows > 0 && layout.metadata_rows > 0 &&
        layout.cover.w == 0) {
        CHECK(layout.metadata.w == content_width(w),
              "metadata-only takes the full content width");
    }

    /* 封面。 */
    if (!want_cover) {
        CHECK(layout.cover.w == 0 && layout.cover.h == 0,
              "want_cover=0 never shows a cover");
    }
    if (layout.cover.w != 0 || layout.cover.h != 0) {
        CHECK(layout.cover.w == 2 * layout.cover.h, "cover is 2:1");
        CHECK(layout.cover.h >= INFO_BAR_COVER_MIN_ROWS &&
              layout.cover.h <= INFO_BAR_COVER_MAX_ROWS,
              "cover height in MIN..MAX");
        CHECK(rect_inside_content(&layout.cover, w, h),
              "cover inside content");
        CHECK(layout.cover.x + layout.cover.w <= w - 2,
              "cover keeps right blank column");
        CHECK(!rects_overlap(&layout.cover, &layout.metadata) &&
              !rects_overlap(&layout.cover, &layout.audio),
              "cover does not overlap text");
        if ((layout.metadata_rows > 0 || layout.audio_rows > 0) &&
            !layout.stacked && content_width(w) >= INFO_BAR_MIN_TEXT_WITH_COVER) {
            int text_w = 0;
            if (layout.metadata_rows > 0) {
                text_w += layout.metadata.w;
            }
            if (layout.audio_rows > 0) {
                if (layout.metadata_rows > 0) {
                    text_w += INFO_BAR_TEXT_COL_GAP;
                }
                text_w += layout.audio.w;
            }
            CHECK(text_w >= INFO_BAR_MIN_TEXT_WITH_COVER,
                  "cover leaves minimum text width");
        }
    }

    /* 默认（按行号）priority：保留的必是前 N 行。 */
    if (layout.metadata_rows < metadata_rows) {
        unsigned want_mask = (1u << layout.metadata_rows) - 1u;
        CHECK(layout.metadata_keep_mask == want_mask,
              "default metadata priority keeps a prefix");
    }
    if (layout.audio_rows < audio_rows) {
        unsigned want_mask = (1u << layout.audio_rows) - 1u;
        CHECK(layout.audio_keep_mask == want_mask,
              "default audio priority keeps a prefix");
    }
}

static void check_matrix(void)
{
    static const int priorities[] = {
        INFO_COVER_PRIORITY_AUTO,
        INFO_COVER_PRIORITY_TEXT_FIRST,
        INFO_COVER_PRIORITY_COVER_FIRST,
    };
    for (int w = 8; w <= 200; w++) {
        for (int h = 3; h <= 40; h++) {
            for (int cover = 0; cover <= 1; cover++) {
                for (size_t p = 0; p < sizeof(priorities) / sizeof(priorities[0]); p++) {
                    for (int mr = 0; mr <= INFO_BAR_METADATA_LINES; mr++) {
                        for (int ar = 0; ar <= INFO_BAR_AUDIO_LINES; ar++) {
                            int md = (w * 7 + mr * 13) % 90;
                            int ad = (h * 5 + ar * 11) % 70;
                            check_one(w, h, cover, priorities[p], mr, ar, md, ad);
                        }
                    }
                }
            }
        }
    }
}

/* 代表性尺寸：精确几何。 */
static void check_representative(void)
{
    InfoBarLayout layout;

    /* 100x17（stage 宽而矮）：封面 24x12 贴右，文字 35/34，全部 5+4 行。 */
    info_bar_layout_compute_adaptive(100, 17, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "100x17 uses columns");
    CHECK(layout.cover.w == 24 && layout.cover.h == 12,
          "100x17 cover 24x12");
    CHECK(layout.cover.x == 73 && layout.cover.y == 2,
          "100x17 cover at 73,2");
    CHECK(layout.metadata.w == 35 && layout.audio.w == 34,
          "100x17 text columns 35/34");
    CHECK(layout.audio.x == 38, "100x17 audio starts after gutter");
    CHECK(layout.metadata.y == 6 && layout.metadata.h == 5,
          "100x17 text block vertically centred");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "100x17 keeps all rows");
    CHECK(layout.metadata_keep_mask == 0x1fu &&
          layout.audio_keep_mask == 0x0fu,
          "100x17 keeps every line");

    /* 70x21：封面缩到 16x8，文字两列都不低于最小宽度。 */
    info_bar_layout_compute_adaptive(70, 21, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "70x21 uses columns");
    CHECK(layout.cover.w == 16 && layout.cover.h == 8,
          "70x21 cover shrinks to 16x8");
    CHECK(layout.cover.x + layout.cover.w == 67,
          "70x21 cover ends at w-3");
    CHECK(layout.metadata.w >= INFO_BAR_MIN_COL_WIDTH &&
          layout.audio.w >= INFO_BAR_MIN_COL_WIDTH,
          "70x21 keeps both text columns usable");

    /* 60x13 窄而高：纵向，auto 优先文字，封面隐藏，5+4 全显示。 */
    info_bar_layout_compute_adaptive(60, 13, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked, "60x13 stacks");
    CHECK(layout.cover.w == 0 && layout.cover.h == 0,
          "60x13 auto hides cover to keep all text");
    CHECK(layout.metadata.y == 2 && layout.metadata.w == 57,
          "60x13 text starts at row 2 and fills width");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "60x13 keeps all nine text lines");

    /* 50x8：横向，无封面，23/23。 */
    info_bar_layout_compute_adaptive(50, 8, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "50x8 uses columns");
    CHECK(layout.cover.w == 0, "50x8 has no cover");
    CHECK(layout.metadata.w == 23 && layout.audio.w == 23,
          "50x8 balanced 23/23 columns");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "50x8 keeps all rows");

    /* 45x17：纵向封面 10x5 居中，文字从第 7 行开始。 */
    info_bar_layout_compute_adaptive(45, 17, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked, "45x17 stacks");
    CHECK(layout.cover.w == 10 && layout.cover.h == 5 && layout.cover.x == 18 &&
          layout.cover.y == 1,
          "45x17 cover 10x5 centred at top of group");
    CHECK(layout.metadata.y == 7 && layout.metadata.w == 42,
          "45x17 text starts below cover");

    /* 30x40：最大封面 24x12，文字全宽。 */
    info_bar_layout_compute_adaptive(30, 40, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked, "30x40 stacks");
    CHECK(layout.cover.w == 24 && layout.cover.h == 12 && layout.cover.x == 3 &&
          layout.cover.y == 9,
          "30x40 max cover 24x12 at 3,9");
    CHECK(layout.metadata.y == 22, "30x40 text under cover");

    /* 14x40：新的最小封面 8x4 也允许显示（旧实现最小 5 行会隐藏）。 */
    info_bar_layout_compute_adaptive(14, 40, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked, "14x40 stacks");
    CHECK(layout.cover.w == 8 && layout.cover.h == 4,
          "14x40 shows 8x4 cover (new min 3 rows)");

    /* 120x3：极矮，横向各 1 行，宽度 58/58。 */
    info_bar_layout_compute_adaptive(120, 3, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "120x3 uses columns");
    CHECK(layout.cover.w == 0, "120x3 has no cover");
    CHECK(layout.metadata_rows == 1 && layout.audio_rows == 1,
          "120x3 keeps one line per column");
    CHECK(layout.metadata.w == 58 && layout.audio.w == 58,
          "120x3 balanced 58/58");

    /* 6x3：极窄，纵向单列，validate 仍必须通过。 */
    info_bar_layout_compute_adaptive(6, 3, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked, "6x3 degrades to stacked");
    CHECK(info_bar_layout_validate(6, 3, &layout), "6x3 still validates");
    CHECK(layout.metadata_rows == 1 && layout.audio_rows == 0,
          "6x3 draws one metadata line and no audio");
}

/* 动态宽度 + 某块无行让宽。 */
static void check_dynamic_width(void)
{
    InfoBarLayout layout;

    info_bar_layout_compute_adaptive(120, 17, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 60, 10, &layout);
    CHECK(layout.valid && !layout.stacked, "dynamic width uses columns");
    CHECK(layout.metadata.w > layout.audio.w,
          "wider metadata content gets more columns");
    CHECK(layout.metadata.w + layout.audio.w + INFO_BAR_TEXT_COL_GAP ==
          content_width(120),
          "dynamic widths consume exactly the text width");
    CHECK(layout.metadata.w >= INFO_BAR_MIN_COL_WIDTH &&
          layout.audio.w >= INFO_BAR_MIN_COL_WIDTH,
          "dynamic widths respect both minima");

    info_bar_layout_compute_adaptive(120, 17, 0, INFO_COVER_PRIORITY_AUTO,
                                     0, 4, NULL, NULL, 0, 50, &layout);
    CHECK(layout.metadata.w == 0 && layout.metadata_rows == 0,
          "empty metadata block has no width");
    CHECK(layout.audio.w == content_width(120),
          "audio-only gets the whole text width");

    info_bar_layout_compute_adaptive(120, 17, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 0, NULL, NULL, 50, 0, &layout);
    CHECK(layout.audio.w == 0 && layout.audio_rows == 0,
          "empty audio block has no width");
    CHECK(layout.metadata.w == content_width(120),
          "metadata-only gets the whole text width");
}

/* 高度不足：按 priority 选行，显示顺序/bit 位置不变。 */
static void check_priority_cropping(void)
{
    InfoBarLayout layout;
    int mp[5] = {4, 0, 2, 1, 3};   /* 第 1 行优先级最高，第 0 行最低 */
    int ap[4] = {2, 0, 3, 1};      /* 第 1 行最高，第 2 行最低 */

    info_bar_layout_compute_adaptive(44, 3, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, mp, ap, 0, 0, &layout);
    CHECK(layout.valid && !layout.stacked, "44x3 uses columns");
    CHECK(layout.metadata_rows == 1 && layout.metadata_keep_mask == (1u << 1),
          "metadata keeps the highest-priority line (index 1)");
    CHECK(layout.audio_rows == 1 && layout.audio_keep_mask == (1u << 1),
          "audio keeps the highest-priority line (index 1)");

    info_bar_layout_compute_adaptive(44, 5, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, mp, ap, 0, 0, &layout);
    CHECK(layout.metadata_rows == 3 && layout.metadata_keep_mask == 0x0eu,
          "metadata keeps indices 1,3,2 for three rows");
    CHECK(layout.audio_rows == 3 && layout.audio_keep_mask == 0x0bu,
          "audio keeps indices 1,3,0 for three rows");

    /* 保留行仍按原始显示顺序紧凑绘制：bit 1 在 bit 3 前。 */
    CHECK(layout.metadata_keep_mask & (1u << 1) &&
          layout.metadata_keep_mask & (1u << 2) &&
          !(layout.metadata_keep_mask & (1u << 0)),
          "low-priority first line is the one dropped");
}

/* cover 优先级：同一内容宽度下三种策略的差异。 */
static void check_cover_priority(void)
{
    InfoBarLayout auto_l, text_l, cover_l;
    info_bar_layout_compute_adaptive(80, 17, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 60, 10, &auto_l);
    info_bar_layout_compute_adaptive(80, 17, 1, INFO_COVER_PRIORITY_TEXT_FIRST,
                                     5, 4, NULL, NULL, 60, 10, &text_l);
    info_bar_layout_compute_adaptive(80, 17, 1, INFO_COVER_PRIORITY_COVER_FIRST,
                                     5, 4, NULL, NULL, 60, 10, &cover_l);

    CHECK(info_bar_layout_validate(80, 17, &auto_l) &&
          info_bar_layout_validate(80, 17, &text_l) &&
          info_bar_layout_validate(80, 17, &cover_l),
          "all three cover priorities validate");
    CHECK(auto_l.cover.w > 0 && auto_l.cover.h >= INFO_BAR_COVER_MIN_ROWS,
          "auto keeps a minimum cover when text is narrow");
    CHECK(text_l.cover.w == 0,
          "text-first hides cover to keep the desired text width");
    CHECK(cover_l.cover.w > auto_l.cover.w,
          "cover-first makes the cover larger than auto");
    CHECK(auto_l.metadata.w >= INFO_BAR_MIN_COL_WIDTH &&
          text_l.metadata.w > auto_l.metadata.w,
          "text-first gives text more width than auto");
    CHECK(cover_l.metadata.w >= INFO_BAR_MIN_COL_WIDTH &&
          cover_l.metadata.w < auto_l.metadata.w,
          "cover-first shrinks text to its minimum");

    /* 无内容宽度时 auto 保留最大可用封面（与旧行为一致）。 */
    info_bar_layout_compute_adaptive(100, 17, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &auto_l);
    CHECK(auto_l.cover.w == 24 && auto_l.cover.h == 12,
          "auto without metrics keeps the maximum cover");
}

/* 宽而矮：块内多列流动；高够时回到单列。 */
static void check_flow(void)
{
    InfoBarLayout layout;

    info_bar_layout_compute_adaptive(200, 5, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.valid && layout.stacked == 0, "200x5 uses columns");
    CHECK(layout.metadata_cols == 2 && layout.audio_cols == 2,
          "200x5 flows metadata/audio into two sub-columns");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "200x5 keeps all 5+4 lines");
    CHECK(layout.metadata.h == 3 && layout.audio.h == 3,
          "200x5 shares 3 visual rows; audio 4 lines flow into 2 columns");

    info_bar_layout_compute_adaptive(200, 8, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.metadata_cols == 1 && layout.audio_cols == 1,
          "200x8 is tall enough for single columns");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "200x8 keeps all rows");

    info_bar_layout_compute_adaptive(200, 4, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.metadata_cols == 3 && layout.audio_cols == 2,
          "200x4 flows into 3/2 sub-columns");
    CHECK(layout.metadata_rows == 5 && layout.audio_rows == 4,
          "200x4 keeps all rows through flow");

    info_bar_layout_compute_adaptive(120, 3, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(layout.metadata_cols == 1 && layout.audio_cols == 1 &&
          layout.metadata_rows == 1 && layout.audio_rows == 1,
          "one visual row does not flow; lower priorities are cropped");

    /* 文本单元几何：2 列、4 行 -> 2 行/列，column-major。 */
    InfoBarRect region = { .x = 2, .y = 1, .w = 61, .h = 3 };
    int x = 0, y = 0, w = 0;
    CHECK(info_bar_layout_text_cell(&region, 2, 0, 4, &x, &y, &w) &&
          x == 2 && y == 1 && w == 30,
          "text cell line 0 at left column");
    CHECK(info_bar_layout_text_cell(&region, 2, 1, 4, &x, &y, &w) &&
          x == 2 && y == 2 && w == 30,
          "text cell line 1 at left column second row");
    CHECK(info_bar_layout_text_cell(&region, 2, 2, 4, &x, &y, &w) &&
          x == 33 && y == 1 && w == 30,
          "text cell line 2 starts right column");
    CHECK(!info_bar_layout_text_cell(&region, 2, 4, 4, &x, &y, &w),
          "text cell rejects line >= lines");
    CHECK(!info_bar_layout_text_cell(&region, 0, 0, 1, &x, &y, &w),
          "text cell rejects cols=0");
}

/* 手工构造非法布局，确认校验器逐条拒绝。 */
static void check_invalid_layouts(void)
{
    InfoBarLayout layout;
    info_bar_layout_compute_adaptive(100, 17, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(info_bar_layout_validate(100, 17, &layout), "base layout validates");

    InfoBarLayout bad = layout;
    bad.cover.h = bad.cover.w;
    CHECK(!info_bar_layout_validate(100, 17, &bad), "non-2:1 cover rejected");

    bad = layout;
    bad.cover.h = 2;
    bad.cover.w = 4;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "cover below new minimum 3 rows rejected");

    bad = layout;
    bad.cover.x = 100 - 2;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "cover touching last content column rejected");

    bad = layout;
    bad.metadata.w = 200;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "metadata outside pane rejected");

    bad = layout;
    bad.metadata_rows = INFO_BAR_METADATA_LINES + 1;
    bad.metadata_keep_mask |= (1u << INFO_BAR_METADATA_LINES);
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "too many metadata rows rejected");

    bad = layout;
    bad.audio_rows = INFO_BAR_AUDIO_LINES + 1;
    bad.audio_keep_mask |= (1u << INFO_BAR_AUDIO_LINES);
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "too many audio rows rejected");

    bad = layout;
    bad.metadata_cols = INFO_BAR_FLOW_MAX_COLS + 1;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "too many sub-columns rejected");

    bad = layout;
    bad.metadata_keep_mask = 0;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "keep_mask popcount mismatch rejected");

    bad = layout;
    bad.audio.x = bad.metadata.x;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "overlapping columns rejected");

    info_bar_layout_compute_adaptive(60, 13, 1, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    CHECK(info_bar_layout_validate(60, 13, &layout),
          "stacked base layout validates");
    bad = layout;
    bad.audio.y = bad.metadata.y;
    CHECK(!info_bar_layout_validate(60, 13, &bad),
          "stacked audio above metadata rejected");

    info_bar_layout_compute_adaptive(100, 17, 0, INFO_COVER_PRIORITY_AUTO,
                                     5, 4, NULL, NULL, 0, 0, &layout);
    bad = layout;
    bad.cover.w = 2;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "half-specified hidden cover rejected");

    info_bar_layout_compute_adaptive(100, 17, 0, INFO_COVER_PRIORITY_AUTO,
                                     0, 0, NULL, NULL, 0, 0, &layout);
    bad = layout;
    bad.metadata.w = 1;
    bad.metadata.h = 1;
    CHECK(!info_bar_layout_validate(100, 17, &bad),
          "rows=0 with non-empty metadata rect rejected");
}

int main(void)
{
    check_representative();
    check_matrix();
    check_dynamic_width();
    check_priority_cropping();
    check_cover_priority();
    check_flow();
    check_invalid_layouts();

    if (g_failures == 0) {
        printf("PASS info_bar_layout (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL info_bar_layout (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
