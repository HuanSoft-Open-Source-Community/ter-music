/**
 * @file info_bar_layout.c
 * @brief 主界面信息窗格的自适应分区纯几何实现（无 ncurses 依赖）
 *
 * 规则、常量与硬性不变量见 ui/info_bar_layout.h。实现只做整数运算：
 *
 *   - 横向（wide）：元数据/音频两列宽度按调用方给出的内容宽度动态分配，
 *     某块无行时把全部文字宽度让给另一块；封面在右、可缩到 3 行；
 *   - 纵向（narrow/tall）：封面在上，元数据/音频依次在下，全宽；
 *   - 文本块内部可以分成 1..3 个子列（column-major），宽而矮时把行铺开，
 *     只有在列数受宽度限制时才对低优先级行做裁剪；
 *   - cover 优先级 auto/text-first/cover-first 只影响“封面多大/是否隐藏”，
 *     不改变任何矩形合法性约束。
 *
 * 所有矩形从内容区左上角 (1 + INFO_BAR_PAD_LEFT, 1) 起算，永不触碰第 0 行
 * 标题与 w-1/h-1 边框；封面额外与右边框留白。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/info_bar_layout.h"

#include <string.h>

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static int clamp_rows(int value, int max_rows)
{
    if (value < 0) {
        return 0;
    }
    if (value > max_rows) {
        return max_rows;
    }
    return value;
}

static int rect_is_empty(const InfoBarRect *rect)
{
    return !rect || rect->w <= 0 || rect->h <= 0;
}

/* 矩形是否完整落在窗格内容区：列 1..w-2、行 1..h-2。 */
static int rect_inside_content(const InfoBarRect *rect, int window_width,
                               int window_height)
{
    if (!rect) {
        return 0;
    }
    if (rect->x < 1 || rect->y < 1 || rect->w < 0 || rect->h < 0) {
        return 0;
    }
    if (rect->x + rect->w > window_width - 1) {
        return 0;
    }
    if (rect->y + rect->h > window_height - 1) {
        return 0;
    }
    return 1;
}

static int rects_overlap(const InfoBarRect *a, const InfoBarRect *b)
{
    if (rect_is_empty(a) || rect_is_empty(b)) {
        return 0;
    }
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int popcount_mask(unsigned mask)
{
    int count = 0;
    while (mask) {
        count += (int)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

/* 子列数上限：视觉行数不足 2 时不分列（1 行分列只会更难读）。 */
static int flow_cols_for(int lines, int avail_rows, int width)
{
    if (lines <= avail_rows || avail_rows < INFO_BAR_FLOW_MIN_ROWS) {
        return 1;
    }
    int needed = (lines + avail_rows - 1) / avail_rows;
    int max_fit = (width + INFO_BAR_TEXT_COL_GAP) /
                  (INFO_BAR_MIN_TEXT_COL_WIDTH + INFO_BAR_TEXT_COL_GAP);
    if (max_fit < 1) {
        max_fit = 1;
    }
    int cols = min_int(needed, min_int(max_fit, INFO_BAR_FLOW_MAX_COLS));
    if (cols < 1) {
        cols = 1;
    }
    if (cols > lines) {
        cols = lines;
    }
    return cols;
}

/* 按优先级数值选 keep 行；NULL 表示按行号（0 优先级最高）。
 * 返回 keep_mask（bit i = 原始第 i 行被保留），不改变原始显示顺序。 */
static unsigned select_keep_mask(int lines, const int *priority, int keep)
{
    if (lines <= 0 || keep <= 0) {
        return 0;
    }
    if (keep >= lines) {
        return (lines >= 32) ? 0xffffffffu : ((1u << lines) - 1u);
    }

    unsigned mask = 0;
    for (int k = 0; k < keep; k++) {
        int best = -1;
        int best_prio = 0;
        for (int i = 0; i < lines; i++) {
            if (mask & (1u << i)) {
                continue;
            }
            int prio = priority ? priority[i] : i;
            if (best < 0 || prio < best_prio) {
                best = i;
                best_prio = prio;
            }
        }
        if (best < 0) {
            break;
        }
        mask |= (1u << best);
    }
    return mask;
}

/* 横向文字宽度分配：两块都按内容需要，但各自不小于 INFO_BAR_MIN_COL_WIDTH；
 * 一块无行时另一块拿全部宽度。 */
static void split_text_width(int text_width, int metadata_lines,
                             int audio_lines, int metadata_desired,
                             int audio_desired, int *metadata_w, int *audio_w)
{
    *metadata_w = 0;
    *audio_w = 0;
    if (text_width <= 0) {
        return;
    }
    if (metadata_lines <= 0 && audio_lines <= 0) {
        return;
    }
    if (metadata_lines <= 0) {
        *audio_w = text_width;
        return;
    }
    if (audio_lines <= 0) {
        *metadata_w = text_width;
        return;
    }

    int min_width = INFO_BAR_MIN_COL_WIDTH;
    int available = text_width - INFO_BAR_TEXT_COL_GAP;
    if (available < 2 * min_width) {
        /* 正常路径不应到这里（cover 安排会保证文字最小宽）；退化时仍按
         * 最小宽度的比例分，validate 会拒绝非法结果，渲染层不会画。 */
        *metadata_w = max_int(1, available / 2);
        *audio_w = max_int(0, available - *metadata_w);
        return;
    }

    int want_m = metadata_desired > 0 ? metadata_desired : min_width;
    int want_a = audio_desired > 0 ? audio_desired : min_width;
    want_m = max_int(want_m, min_width);
    want_a = max_int(want_a, min_width);
    want_m = min_int(want_m, available - min_width);
    want_a = min_int(want_a, available - min_width);

    int extra = available - 2 * min_width;
    if (want_m + want_a <= available) {
        int rest = available - want_m - want_a;
        /* 余数给左列（与旧实现的 text_width/2 一致，测试/回归可预期）。 */
        *metadata_w = want_m + (rest + 1) / 2;
        *audio_w = want_a + rest - (rest + 1) / 2;
    } else {
        int weight_m = want_m - min_width;
        int weight_a = want_a - min_width;
        int weight_sum = weight_m + weight_a;
        if (weight_sum <= 0) {
            *metadata_w = min_width + extra / 2;
        } else {
            *metadata_w = min_width + extra * weight_m / weight_sum;
        }
        *audio_w = available - *metadata_w;
    }
    if (*metadata_w < min_width) {
        *metadata_w = min_width;
        *audio_w = available - *metadata_w;
    }
    if (*audio_w < min_width) {
        *audio_w = min_width;
        *metadata_w = available - *audio_w;
    }
}

/* 横向封面行数：0=隐藏。文字区最小宽 48、封面最小 3 行。 */
static int choose_cover_horizontal(int content_width, int content_height,
                                   int want_cover, int cover_priority,
                                   int has_text, int metadata_lines,
                                   int audio_lines, int metadata_desired,
                                   int audio_desired)
{
    if (!want_cover || content_height < INFO_BAR_COVER_MIN_ROWS) {
        return 0;
    }

    int text_min = has_text ? INFO_BAR_MIN_TEXT_WITH_COVER : 0;
    int reserve = text_min + (has_text ? INFO_BAR_COVER_GAP : 0) +
                  INFO_BAR_COVER_RIGHT_PAD;
    int max_by_width = (content_width - reserve) / 2;
    if (max_by_width < INFO_BAR_COVER_MIN_ROWS) {
        return 0;
    }
    int cap = min_int(content_height, INFO_BAR_COVER_MAX_ROWS);
    cap = min_int(cap, max_by_width);
    if (cap < INFO_BAR_COVER_MIN_ROWS) {
        return 0;
    }

    int desired_text = text_min;
    if (has_text) {
        int want_m = metadata_desired > 0 ? metadata_desired
                                          : INFO_BAR_MIN_COL_WIDTH;
        int want_a = audio_desired > 0 ? audio_desired
                                       : INFO_BAR_MIN_COL_WIDTH;
        int both = (metadata_lines > 0 && audio_lines > 0)
                       ? INFO_BAR_TEXT_COL_GAP : 0;
        desired_text = want_m + want_a + both;
        if (desired_text < text_min) {
            desired_text = text_min;
        }
    }

    int rows = cap;
    if (cover_priority == INFO_COVER_PRIORITY_TEXT_FIRST) {
        int leave = content_width - desired_text -
                    (has_text ? INFO_BAR_COVER_GAP : 0) -
                    INFO_BAR_COVER_RIGHT_PAD;
        int max_leave = leave / 2;
        if (max_leave < INFO_BAR_COVER_MIN_ROWS) {
            return 0;
        }
        rows = min_int(cap, max_leave);
    } else if (cover_priority == INFO_COVER_PRIORITY_AUTO) {
        /* auto：先缩小封面尽量保留文字理想宽度；缩到最小仍不够时，保留
         * 最小封面（文字仍有 text_min），而不是直接隐藏。 */
        int leave = content_width - desired_text -
                    (has_text ? INFO_BAR_COVER_GAP : 0) -
                    INFO_BAR_COVER_RIGHT_PAD;
        int max_leave = leave / 2;
        if (max_leave >= INFO_BAR_COVER_MIN_ROWS) {
            rows = min_int(cap, max_leave);
        } else {
            rows = INFO_BAR_COVER_MIN_ROWS;
        }
    }
    /* cover-first：rows = cap（宽度约束已保证文字 ≥ text_min）。 */
    if (rows < INFO_BAR_COVER_MIN_ROWS) {
        rows = INFO_BAR_COVER_MIN_ROWS;
    }
    if (rows > cap) {
        rows = cap;
    }
    return rows;
}

/* 纵向封面行数：0=隐藏。text-first 要求全部文字优先；auto 先缩封面；
 * cover-first 让封面尽量大（文字仍至少 1 行/最小宽度）。 */
static int choose_cover_stacked(int content_width, int content_height,
                                int want_cover, int cover_priority,
                                int metadata_lines, int audio_lines)
{
    if (!want_cover) {
        return 0;
    }
    int max_by_width = (content_width - INFO_BAR_COVER_RIGHT_PAD) / 2;
    if (max_by_width < INFO_BAR_COVER_MIN_ROWS) {
        return 0;
    }
    int max_h = min_int(max_by_width, INFO_BAR_COVER_MAX_ROWS);
    max_h = min_int(max_h, content_height);
    if (max_h < INFO_BAR_COVER_MIN_ROWS) {
        return 0;
    }

    int text_lines = metadata_lines + audio_lines;
    int gap = text_lines > 0 ? INFO_BAR_COVER_GAP : 0;

    if (cover_priority == INFO_COVER_PRIORITY_COVER_FIRST) {
        /* 封面优先，但有文字请求时至少给文字留 1 行（+ gap），否则
         * cover-first 会在极矮窗格里把文字全吃掉，用户看不到任何字段。 */
        int keep_text = (text_lines > 0) ? 1 : 0;
        int rows = content_height - keep_text - gap;
        rows = min_int(rows, max_h);
        return (rows >= INFO_BAR_COVER_MIN_ROWS) ? rows : 0;
    }

    if (content_height >= text_lines + gap + INFO_BAR_COVER_MIN_ROWS) {
        return min_int(max_h, content_height - text_lines - gap);
    }
    /* 纵向空间不足：auto/text-first 都隐藏封面保文字（auto 的“先缩小”
     * 体现在上面能放多少文字就缩到多少；缩到最小仍放不下则隐藏）。
     * cover-first 已在上面分支优先保留封面。 */
    return 0;
}

static void set_text_region(InfoBarRect *region, int x, int y, int w, int h)
{
    region->x = x;
    region->y = y;
    region->w = w;
    region->h = h;
}

/* 横向：元数据 | 音频 [+ 封面]。 */
static void compute_horizontal(int x0, int y0, int cw, int ch,
                               int want_cover, int cover_priority,
                               int metadata_lines, int audio_lines,
                               const int *metadata_priority,
                               const int *audio_priority,
                               int metadata_desired, int audio_desired,
                               InfoBarLayout *out)
{
    int has_text = (metadata_lines > 0 || audio_lines > 0);
    int cover_rows = choose_cover_horizontal(cw, ch, want_cover, cover_priority,
                                             has_text, metadata_lines,
                                             audio_lines, metadata_desired,
                                             audio_desired);
    int text_width = cw;
    if (cover_rows > 0) {
        int cover_cols = cover_rows * 2;
        out->cover.x = x0 + cw - INFO_BAR_COVER_RIGHT_PAD - cover_cols;
        out->cover.y = y0 + (ch - cover_rows) / 2;
        out->cover.w = cover_cols;
        out->cover.h = cover_rows;
        text_width = cw - cover_cols -
                     (has_text ? INFO_BAR_COVER_GAP : 0) -
                     INFO_BAR_COVER_RIGHT_PAD;
        if (text_width < 0) {
            text_width = 0;
        }
    }

    int metadata_w = 0, audio_w = 0;
    split_text_width(text_width, metadata_lines, audio_lines,
                     metadata_desired, audio_desired,
                     &metadata_w, &audio_w);

    int metadata_cols = 0, audio_cols = 0;
    int metadata_keep = 0, audio_keep = 0;
    if (metadata_lines > 0 && metadata_w > 0) {
        metadata_cols = flow_cols_for(metadata_lines, ch, metadata_w);
        int capacity = metadata_cols * ch;
        metadata_keep = min_int(metadata_lines, capacity);
    }
    if (audio_lines > 0 && audio_w > 0) {
        audio_cols = flow_cols_for(audio_lines, ch, audio_w);
        int capacity = audio_cols * ch;
        audio_keep = min_int(audio_lines, capacity);
    }

    int metadata_visual = metadata_keep > 0
                              ? (metadata_keep + metadata_cols - 1) / metadata_cols
                              : 0;
    int audio_visual = audio_keep > 0
                           ? (audio_keep + audio_cols - 1) / audio_cols
                           : 0;
    int visual_h = max_int(metadata_visual, audio_visual);

    if (metadata_keep > 0) {
        set_text_region(&out->metadata, x0,
                        y0 + (ch - visual_h) / 2, metadata_w, visual_h);
        out->metadata_rows = metadata_keep;
        out->metadata_cols = metadata_cols;
        out->metadata_keep_mask = select_keep_mask(metadata_lines,
                                                   metadata_priority,
                                                   metadata_keep);
    }
    if (audio_keep > 0) {
        int audio_x = x0;
        if (metadata_keep > 0) {
            audio_x += metadata_w + INFO_BAR_TEXT_COL_GAP;
        }
        set_text_region(&out->audio, audio_x,
                        y0 + (ch - visual_h) / 2, audio_w, visual_h);
        out->audio_rows = audio_keep;
        out->audio_cols = audio_cols;
        out->audio_keep_mask = select_keep_mask(audio_lines, audio_priority,
                                                audio_keep);
    }
    out->stacked = 0;
}

/* 纵向：封面在上，元数据、音频依次在下，两段占满内容宽。 */
static void compute_stacked(int x0, int y0, int cw, int ch,
                            int want_cover, int cover_priority,
                            int metadata_lines, int audio_lines,
                            const int *metadata_priority,
                            const int *audio_priority,
                            InfoBarLayout *out)
{
    int cover_rows = choose_cover_stacked(cw, ch, want_cover, cover_priority,
                                          metadata_lines, audio_lines);
    int gap = (cover_rows > 0 && (metadata_lines + audio_lines) > 0)
                  ? INFO_BAR_COVER_GAP : 0;
    int text_height = ch - cover_rows - gap;
    if (text_height < 0) {
        text_height = 0;
    }

    int metadata_keep = min_int(metadata_lines, text_height);
    int audio_keep = min_int(audio_lines, text_height - metadata_keep);

    int group = cover_rows + gap + metadata_keep + audio_keep;
    int top = y0 + (ch - group) / 2;
    if (top < y0) {
        top = y0;
    }

    if (cover_rows > 0) {
        int cover_cols = cover_rows * 2;
        out->cover.x = x0 + (cw - cover_cols) / 2;
        out->cover.y = top;
        out->cover.w = cover_cols;
        out->cover.h = cover_rows;
    }

    int text_y = top + cover_rows + gap;
    if (metadata_keep > 0) {
        set_text_region(&out->metadata, x0, text_y, cw, metadata_keep);
        out->metadata_rows = metadata_keep;
        out->metadata_cols = 1;
        out->metadata_keep_mask = select_keep_mask(metadata_lines,
                                                   metadata_priority,
                                                   metadata_keep);
    }
    if (audio_keep > 0) {
        set_text_region(&out->audio, x0, text_y + metadata_keep, cw, audio_keep);
        out->audio_rows = audio_keep;
        out->audio_cols = 1;
        out->audio_keep_mask = select_keep_mask(audio_lines, audio_priority,
                                                audio_keep);
    }
    out->stacked = 1;
}

void info_bar_layout_compute_rows(int window_width, int window_height,
                                  int want_cover, int metadata_rows,
                                  int audio_rows, InfoBarLayout *out)
{
    info_bar_layout_compute_adaptive(window_width, window_height, want_cover,
                                     INFO_COVER_PRIORITY_AUTO,
                                     metadata_rows, audio_rows,
                                     NULL, NULL, 0, 0, out);
}

void info_bar_layout_compute_adaptive(int window_width, int window_height,
                                      int want_cover, int cover_priority,
                                      int metadata_rows, int audio_rows,
                                      const int *metadata_priority,
                                      const int *audio_priority,
                                      int metadata_desired_w,
                                      int audio_desired_w,
                                      InfoBarLayout *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* 与渲染层一致：低于这个尺寸连内容行都没有，只画边框/标题。 */
    if (window_width < 6 || window_height < 3) {
        return;
    }
    if (cover_priority < 0 || cover_priority >= INFO_COVER_PRIORITY_COUNT) {
        cover_priority = INFO_COVER_PRIORITY_AUTO;
    }

    int metadata_lines = clamp_rows(metadata_rows, INFO_BAR_METADATA_LINES);
    int audio_lines = clamp_rows(audio_rows, INFO_BAR_AUDIO_LINES);

    int content_width = window_width - 2 - INFO_BAR_PAD_LEFT;
    int content_height = window_height - 2;
    int x0 = 1 + INFO_BAR_PAD_LEFT;
    int y0 = 1;

    /* 宽窗格横向；窄而高纵向；窄且矮（横向也放不下两列）纵向单列。 */
    int horizontal = (content_width >= INFO_BAR_VERTICAL_WIDTH) ||
                     (content_width >= INFO_BAR_COLUMNS_MIN_WIDTH &&
                      content_height < INFO_BAR_VERTICAL_MIN_ROWS);

    if (horizontal) {
        compute_horizontal(x0, y0, content_width, content_height,
                           want_cover, cover_priority,
                           metadata_lines, audio_lines,
                           metadata_priority, audio_priority,
                           metadata_desired_w, audio_desired_w, out);
    } else {
        compute_stacked(x0, y0, content_width, content_height,
                        want_cover, cover_priority,
                        metadata_lines, audio_lines,
                        metadata_priority, audio_priority, out);
    }

    out->valid = 1;
}

void info_bar_layout_compute(int window_width, int window_height,
                             int want_cover, InfoBarLayout *out)
{
    info_bar_layout_compute_rows(window_width, window_height, want_cover,
                                 INFO_BAR_METADATA_LINES, INFO_BAR_AUDIO_LINES,
                                 out);
}

int info_bar_layout_text_cell(const InfoBarRect *region, int cols,
                              int line, int lines, int *x, int *y, int *w)
{
    if (!region || !x || !y || !w) {
        return 0;
    }
    if (cols < 1 || line < 0 || lines <= 0 || line >= lines) {
        return 0;
    }
    if (region->w <= 0 || region->h <= 0) {
        return 0;
    }
    int visual_rows = (lines + cols - 1) / cols;
    if (visual_rows <= 0) {
        return 0;
    }
    int col = line / visual_rows;
    int row = line % visual_rows;
    if (col >= cols || row >= region->h) {
        return 0;
    }

    int gaps = (cols - 1) * INFO_BAR_TEXT_COL_GAP;
    int usable = region->w - gaps;
    if (usable < cols) {
        return 0;
    }
    int base = usable / cols;
    int rem = usable % cols;
    int cell_w = base + (col < rem ? 1 : 0);
    int offset = col * base + min_int(col, rem) +
                 col * INFO_BAR_TEXT_COL_GAP;
    *x = region->x + offset;
    *y = region->y + row;
    *w = cell_w;
    return 1;
}

int info_bar_layout_validate(int window_width, int window_height,
                             const InfoBarLayout *layout)
{
    if (!layout || !layout->valid) {
        return 0;
    }
    if (window_width < 6 || window_height < 3) {
        return 0;
    }
    if (layout->stacked != 0 && layout->stacked != 1) {
        return 0;
    }
    if (layout->metadata_rows < 0 ||
        layout->metadata_rows > INFO_BAR_METADATA_LINES) {
        return 0;
    }
    if (layout->audio_rows < 0 ||
        layout->audio_rows > INFO_BAR_AUDIO_LINES) {
        return 0;
    }
    if (layout->metadata_cols < 0 ||
        layout->metadata_cols > INFO_BAR_FLOW_MAX_COLS) {
        return 0;
    }
    if (layout->audio_cols < 0 ||
        layout->audio_cols > INFO_BAR_FLOW_MAX_COLS) {
        return 0;
    }
    if (popcount_mask(layout->metadata_keep_mask) != layout->metadata_rows) {
        return 0;
    }
    if (popcount_mask(layout->audio_keep_mask) != layout->audio_rows) {
        return 0;
    }
    if (layout->metadata_rows == 0 &&
        (layout->metadata_cols != 0 || layout->metadata_keep_mask != 0)) {
        return 0;
    }
    if (layout->audio_rows == 0 &&
        (layout->audio_cols != 0 || layout->audio_keep_mask != 0)) {
        return 0;
    }
    if (layout->metadata_rows > 0 && layout->metadata_cols == 0) {
        return 0;
    }
    if (layout->audio_rows > 0 && layout->audio_cols == 0) {
        return 0;
    }

    int metadata_empty = rect_is_empty(&layout->metadata);
    int audio_empty = rect_is_empty(&layout->audio);

    if (layout->metadata_rows == 0) {
        if (!metadata_empty) {
            return 0;
        }
    } else {
        if (metadata_empty) {
            return 0;
        }
        if (!rect_inside_content(&layout->metadata, window_width,
                                 window_height)) {
            return 0;
        }
        int visual = (layout->metadata_rows + layout->metadata_cols - 1) /
                     layout->metadata_cols;
        if (visual > layout->metadata.h) {
            return 0;
        }
        int min_width = layout->metadata_cols * INFO_BAR_MIN_TEXT_COL_WIDTH +
                        (layout->metadata_cols - 1) * INFO_BAR_TEXT_COL_GAP;
        int content_width = window_width - 2 - INFO_BAR_PAD_LEFT;
        if (min_width > content_width) {
            min_width = content_width;
        }
        if (layout->metadata.w < min_width) {
            return 0;
        }
    }

    if (layout->audio_rows == 0) {
        if (!audio_empty) {
            return 0;
        }
    } else {
        if (audio_empty) {
            return 0;
        }
        if (!rect_inside_content(&layout->audio, window_width,
                                 window_height)) {
            return 0;
        }
        int visual = (layout->audio_rows + layout->audio_cols - 1) /
                     layout->audio_cols;
        if (visual > layout->audio.h) {
            return 0;
        }
        int min_width = layout->audio_cols * INFO_BAR_MIN_TEXT_COL_WIDTH +
                        (layout->audio_cols - 1) * INFO_BAR_TEXT_COL_GAP;
        int content_width = window_width - 2 - INFO_BAR_PAD_LEFT;
        if (min_width > content_width) {
            min_width = content_width;
        }
        if (layout->audio.w < min_width) {
            return 0;
        }
    }

    if (!metadata_empty && !audio_empty) {
        if (rects_overlap(&layout->metadata, &layout->audio)) {
            return 0;
        }
        if (layout->stacked) {
            if (layout->audio.x != layout->metadata.x ||
                layout->audio.w != layout->metadata.w) {
                return 0;
            }
            if (layout->metadata.y + layout->metadata.h > layout->audio.y) {
                return 0;
            }
        } else {
            if (layout->audio.y != layout->metadata.y ||
                layout->audio.h != layout->metadata.h) {
                return 0;
            }
            /* 横向两列之间必须至少留一列空白。 */
            if (layout->metadata.x + layout->metadata.w >=
                layout->audio.x) {
                return 0;
            }
        }
    }

    int text_present = (layout->metadata_rows > 0 || layout->audio_rows > 0);
    int text_width = 0;
    if (layout->metadata_rows > 0) {
        text_width += layout->metadata.w;
    }
    if (layout->audio_rows > 0) {
        text_width += layout->audio.w;
        if (layout->metadata_rows > 0) {
            text_width += INFO_BAR_TEXT_COL_GAP;
        }
    }
    int content_width = window_width - 2 - INFO_BAR_PAD_LEFT;
    if (text_present && !layout->stacked &&
        (layout->cover.w > 0 || layout->cover.h > 0) &&
        content_width >= INFO_BAR_MIN_TEXT_WITH_COVER &&
        text_width < INFO_BAR_MIN_TEXT_WITH_COVER) {
        /* 横向、有封面且有文字时，文字总宽必须达到最小可用宽度（cover
         * 预留规则保证；某块无行时另一块就是全部文字宽度）。窄到内容区
         * 本身就低于该值时无法满足，交给列最小宽度约束。 */
        return 0;
    }

    if (layout->cover.w != 0 || layout->cover.h != 0) {
        if (layout->cover.w != 2 * layout->cover.h) {
            return 0;
        }
        if (layout->cover.h < INFO_BAR_COVER_MIN_ROWS ||
            layout->cover.h > INFO_BAR_COVER_MAX_ROWS) {
            return 0;
        }
        if (!rect_inside_content(&layout->cover, window_width,
                                 window_height)) {
            return 0;
        }
        /* 右侧至少空一列内容列，再才是右边框。 */
        if (layout->cover.x + layout->cover.w > window_width - 2) {
            return 0;
        }
        if (rects_overlap(&layout->cover, &layout->metadata) ||
            rects_overlap(&layout->cover, &layout->audio)) {
            return 0;
        }
    }

    return 1;
}
