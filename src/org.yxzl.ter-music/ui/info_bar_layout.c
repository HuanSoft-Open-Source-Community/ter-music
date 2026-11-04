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
 * 行数由调用方给出：主页信息窗格按 info_fields 中实际启用的字段
 * 传入元数据/音频行数；固定 5+4 的旧调用方走 info_bar_layout_compute()
 * 兼容包装。
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

/* 横向：元数据 | 音频 [+ 封面]；cw/ch 已由调用方保证足够。 */
static void compute_horizontal(int x0, int y0, int cw, int ch,
                               int want_cover, int metadata_req, int audio_req,
                               InfoBarLayout *out)
{
    int metadata_rows = clamp_rows(metadata_req, INFO_BAR_METADATA_LINES);
    int audio_rows = clamp_rows(audio_req, INFO_BAR_AUDIO_LINES);
    int cover_rows = 0;
    int has_text = (metadata_rows > 0 || audio_rows > 0);

    if (want_cover && ch >= INFO_BAR_COVER_MIN_ROWS) {
        /* 有文字时给文字留最小宽度；无文字时封面可以独享空间。 */
        int reserve = has_text
                          ? INFO_BAR_MIN_TEXT_WITH_COVER +
                                INFO_BAR_COVER_GAP + INFO_BAR_COVER_RIGHT_PAD
                          : 0;
        int room = cw - reserve;
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
        /* 封面末列固定为 w-4：右侧两列空白、一列与文字分隔；纵向居中。 */
        out->cover.x = x0 + cw - 2 - cover_cols;
        out->cover.y = y0 + (ch - cover_rows) / 2;
        out->cover.w = cover_cols;
        out->cover.h = cover_rows;
        text_width = cw - cover_cols - INFO_BAR_COVER_GAP -
                     INFO_BAR_COVER_RIGHT_PAD;
        if (text_width < 0) {
            text_width = 0;
        }
    }

    metadata_rows = min_int(metadata_rows, ch);
    audio_rows = min_int(audio_rows, ch);

    int rows = max_int(metadata_rows, audio_rows);
    int text_y = y0 + (ch - rows) / 2;

    if (metadata_rows > 0 && audio_rows > 0) {
        int metadata_width = text_width / 2;
        int audio_width = text_width - metadata_width - 1;
        if (metadata_width < 0) {
            metadata_width = 0;
        }
        if (audio_width < 0) {
            audio_width = 0;
        }
        out->metadata.x = x0;
        out->metadata.y = text_y;
        out->metadata.w = metadata_width;
        out->metadata.h = rows;
        out->metadata_rows = metadata_rows;

        out->audio.x = x0 + metadata_width + 1;
        out->audio.y = text_y;
        out->audio.w = audio_width;
        out->audio.h = rows;
        out->audio_rows = audio_rows;
    } else if (metadata_rows > 0) {
        out->metadata.x = x0;
        out->metadata.y = text_y;
        out->metadata.w = text_width;
        out->metadata.h = rows;
        out->metadata_rows = metadata_rows;
    } else if (audio_rows > 0) {
        out->audio.x = x0;
        out->audio.y = text_y;
        out->audio.w = text_width;
        out->audio.h = rows;
        out->audio_rows = audio_rows;
    }

    out->stacked = 0;
}

/* 纵向：封面在上，元数据、音频依次在下，两段占满内容宽。 */
static void compute_stacked(int x0, int y0, int cw, int ch,
                            int want_cover, int metadata_req, int audio_req,
                            InfoBarLayout *out)
{
    int metadata_req_clamped = clamp_rows(metadata_req, INFO_BAR_METADATA_LINES);
    int audio_req_clamped = clamp_rows(audio_req, INFO_BAR_AUDIO_LINES);
    int text_req = metadata_req_clamped + audio_req_clamped;
    int cover_rows = 0;

    if (want_cover && cw >= 12) {
        int gap = (text_req > 0) ? INFO_BAR_COVER_GAP : 0;
        int min_total = text_req + gap + INFO_BAR_COVER_MIN_ROWS;
        if (ch >= min_total) {
            int max_by_width = (cw - 2) / 2;
            cover_rows = ch - text_req - gap;
            if (cover_rows > INFO_BAR_COVER_MAX_ROWS) {
                cover_rows = INFO_BAR_COVER_MAX_ROWS;
            }
            if (cover_rows > max_by_width) {
                cover_rows = max_by_width;
            }
            if (cover_rows < INFO_BAR_COVER_MIN_ROWS) {
                cover_rows = 0;
            }
        }
    }

    int gap = (cover_rows > 0) ? INFO_BAR_COVER_GAP : 0;
    int text_height = ch - cover_rows - gap;
    if (text_height < 0) {
        text_height = 0;
    }
    int metadata_rows = min_int(metadata_req_clamped, text_height);
    int audio_rows = min_int(audio_req_clamped, text_height - metadata_rows);

    /* 封面与文字作为一整块在内容区垂直居中，块内保持"封面在上、文字在下"。 */
    int group = cover_rows + gap + metadata_rows + audio_rows;
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
    if (metadata_rows > 0) {
        out->metadata.x = x0;
        out->metadata.y = text_y;
        out->metadata.w = cw;
        out->metadata.h = metadata_rows;
        out->metadata_rows = metadata_rows;
    }

    if (audio_rows > 0) {
        out->audio.x = x0;
        out->audio.y = text_y + metadata_rows;
        out->audio.w = cw;
        out->audio.h = audio_rows;
        out->audio_rows = audio_rows;
    }

    out->stacked = 1;
}

void info_bar_layout_compute_rows(int window_width, int window_height,
                                  int want_cover, int metadata_rows,
                                  int audio_rows, InfoBarLayout *out)
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
                           want_cover, metadata_rows, audio_rows, out);
    } else {
        compute_stacked(x0, y0, content_width, content_height,
                        want_cover, metadata_rows, audio_rows, out);
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
        if (layout->metadata_rows > layout->metadata.h) {
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
        if (layout->audio_rows > layout->audio.h) {
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
