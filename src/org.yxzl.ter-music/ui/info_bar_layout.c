/**
 * @file info_bar_layout.c
 * @brief 主界面信息窗格的自适应分区纯几何实现
 *
 * 只做整数运算，不依赖 ncurses。排布规则与硬性不变量见头文件：
 * 宽窗格横向（元数据 | 音频，右侧可选封面），窄而高窗格纵向
 * （封面在上，元数据、音频依次在下）。所有矩形都从内容区左上角
 * (1+INFO_BAR_PAD_LEFT, 1) 起算，永不触碰第 0 行标题、w-1/h-1 边框；
 * 封面额外与右边框保留空白。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/info_bar_layout.h"

#include <string.h>

static int min_int(int a, int b)
{
    return a < b ? a : b;
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

/* 横向：元数据 | 音频 [+ 封面]；cw/ch 已由调用方保证足够。 */
static void compute_horizontal(int x0, int y0, int cw, int ch,
                               int want_cover, InfoBarLayout *out)
{
    int cover_rows = 0;

    if (want_cover && ch >= INFO_BAR_COVER_MIN_ROWS) {
        int room = cw - INFO_BAR_MIN_TEXT_WITH_COVER -
                   INFO_BAR_COVER_GAP - INFO_BAR_COVER_RIGHT_PAD;
        if (room >= 2 * INFO_BAR_COVER_MIN_ROWS) {
            cover_rows = min_int(room / 2, ch);
            if (cover_rows > INFO_BAR_COVER_MAX_ROWS) {
                cover_rows = INFO_BAR_COVER_MAX_ROWS;
            }
            if (cover_rows < INFO_BAR_COVER_MIN_ROWS) {
                cover_rows = 0;
            }
        }
    }

    int text_width = cw;
    if (cover_rows > 0) {
        int cover_cols = cover_rows * 2;
        /* 封面末列固定为 w-4：右侧两列空白、一列与文字分隔。 */
        out->cover.x = x0 + cw - 2 - cover_cols;
        out->cover.y = y0;
        out->cover.w = cover_cols;
        out->cover.h = cover_rows;
        text_width = cw - cover_cols - INFO_BAR_COVER_GAP -
                     INFO_BAR_COVER_RIGHT_PAD;
    }

    int metadata_width = text_width / 2;
    int audio_width = text_width - metadata_width - 1;

    out->metadata.x = x0;
    out->metadata.y = y0;
    out->metadata.w = metadata_width;
    out->metadata.h = ch;
    out->metadata_rows = min_int(INFO_BAR_METADATA_LINES, ch);

    out->audio.x = x0 + metadata_width + 1;
    out->audio.y = y0;
    out->audio.w = audio_width;
    out->audio.h = ch;
    out->audio_rows = min_int(INFO_BAR_AUDIO_LINES, ch);

    out->stacked = 0;
}

/* 纵向：封面在上，元数据、音频依次在下，两段占满内容宽。 */
static void compute_stacked(int x0, int y0, int cw, int ch,
                            int want_cover, InfoBarLayout *out)
{
    int cover_rows = 0;

    if (want_cover && ch >= INFO_BAR_VERTICAL_COVER_MIN_ROWS && cw >= 12) {
        int max_by_width = (cw - 2) / 2;
        cover_rows = ch - INFO_BAR_VERTICAL_MIN_ROWS - INFO_BAR_COVER_GAP;
        cover_rows = min_int(cover_rows, INFO_BAR_COVER_MAX_ROWS);
        cover_rows = min_int(cover_rows, max_by_width);
        if (cover_rows < INFO_BAR_COVER_MIN_ROWS) {
            cover_rows = 0;
        }
    }

    int text_y = y0;
    int text_height = ch;
    if (cover_rows > 0) {
        int cover_cols = cover_rows * 2;
        out->cover.x = x0 + (cw - cover_cols) / 2;
        out->cover.y = y0;
        out->cover.w = cover_cols;
        out->cover.h = cover_rows;
        text_y = y0 + cover_rows + INFO_BAR_COVER_GAP;
        text_height = ch - cover_rows - INFO_BAR_COVER_GAP;
    }

    out->metadata.x = x0;
    out->metadata.y = text_y;
    out->metadata.w = cw;
    out->metadata.h = min_int(INFO_BAR_METADATA_LINES, text_height);
    out->metadata_rows = out->metadata.h;

    int audio_height = min_int(INFO_BAR_AUDIO_LINES,
                               text_height - out->metadata.h);
    out->audio.x = x0;
    out->audio.y = text_y + out->metadata.h;
    out->audio.w = cw;
    out->audio.h = audio_height;
    out->audio_rows = audio_height;

    out->stacked = 1;
}

void info_bar_layout_compute(int window_width, int window_height,
                             int want_cover, InfoBarLayout *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* 与渲染层一致：低于这个尺寸连内容行都没有，只画边框/标题。 */
    if (window_width < 6 || window_height < 3) {
        return;
    }

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
                           want_cover, out);
    } else {
        compute_stacked(x0, y0, content_width, content_height,
                        want_cover, out);
    }

    if (out->metadata.w <= 0 || out->metadata.h <= 0 ||
        out->audio.w <= 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    out->valid = 1;
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
    if (layout->metadata_rows < 1 ||
        layout->metadata_rows > INFO_BAR_METADATA_LINES) {
        return 0;
    }
    if (layout->audio_rows < 0 ||
        layout->audio_rows > INFO_BAR_AUDIO_LINES) {
        return 0;
    }

    if (!rect_inside_content(&layout->metadata, window_width, window_height)) {
        return 0;
    }
    if (!rect_inside_content(&layout->audio, window_width, window_height)) {
        return 0;
    }
    if (layout->metadata.w <= 0 || layout->metadata.h <= 0) {
        return 0;
    }
    if (layout->audio.w <= 0) {
        return 0;
    }
    if (layout->metadata_rows > layout->metadata.h) {
        return 0;
    }
    if (layout->audio_rows > layout->audio.h) {
        return 0;
    }
    if (rects_overlap(&layout->metadata, &layout->audio)) {
        return 0;
    }

    if (layout->stacked) {
        if (layout->audio.h > 0) {
            if (layout->audio.x != layout->metadata.x ||
                layout->audio.w != layout->metadata.w) {
                return 0;
            }
            if (layout->metadata.y + layout->metadata.h > layout->audio.y) {
                return 0;
            }
        }
    } else {
        if (layout->audio.h > 0) {
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
