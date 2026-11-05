/**
 * @file context_menu_layout.c
 * @brief 右键上下文菜单的纯几何实现（见 ui/context_menu_layout.h）
 */

#include "ui/context_menu_layout.h"

#include <stddef.h>

static int clamp_size(int *w, int *h, int screen_rows, int screen_cols)
{
    const int max_h = screen_rows - 1;   /* 末行留给底部提示条 */
    if (screen_rows <= 0 || screen_cols <= 0) {
        return 0;
    }
    if (*w > screen_cols) {
        *w = screen_cols;
    }
    if (*h > max_h) {
        *h = max_h;
    }
    if (*w < UI_CTX_MENU_MIN_W || *h < UI_CTX_MENU_MIN_H) {
        return 0;
    }
    return 1;
}

int ui_popup_place_rect(int anchor_y, int anchor_x, int w, int h,
                        int min_top, int max_bottom,
                        int min_left, int max_right, UIRect *out)
{
    if (!out || w <= 0 || h <= 0) {
        return 0;
    }
    if (min_top > max_bottom || min_left > max_right) {
        return 0;
    }

    const int avail_w = max_right - min_left + 1;
    const int avail_h = max_bottom - min_top + 1;
    if (w > avail_w) {
        w = avail_w;
    }
    if (h > avail_h) {
        h = avail_h;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    int top = anchor_y;
    if (top < min_top) {
        top = min_top;
    }
    if (top > max_bottom - h + 1) {
        top = anchor_y - h;                 /* 下溢：整体移到锚点上方 */
    }
    if (top < min_top) {
        top = min_top;
    }
    if (top > max_bottom - h + 1) {
        top = max_bottom - h + 1;           /* 仍放不下：钳到区域底部 */
    }

    int left = anchor_x;
    if (left < min_left) {
        left = min_left;
    }
    if (left > max_right - w + 1) {
        left = anchor_x - w;                /* 右溢：整体移到锚点左侧 */
    }
    if (left < min_left) {
        left = min_left;
    }
    if (left > max_right - w + 1) {
        left = max_right - w + 1;           /* 仍放不下：钳到区域右侧 */
    }

    out->top = top;
    out->left = left;
    out->rows = h;
    out->cols = w;
    return 1;
}

int ui_ctx_menu_place_rect(int anchor_y, int anchor_x, int w, int h,
                           int screen_rows, int screen_cols, UIRect *out)
{
    if (!out || w <= 0 || h <= 0) {
        return 0;
    }
    if (!clamp_size(&w, &h, screen_rows, screen_cols)) {
        return 0;
    }

    /* 右键菜单的既有边界：末行留给提示条，菜单不覆盖。 */
    return ui_popup_place_rect(anchor_y, anchor_x, w, h,
                               0, screen_rows - 2,
                               0, screen_cols - 1, out);
}

int ui_ctx_menu_place_submenu(const UIRect *parent, int item_screen_row,
                              int w, int h, int screen_rows, int screen_cols,
                              UIRect *out)
{
    if (!out || !parent || parent->cols <= 0 || w <= 0 || h <= 0) {
        return 0;
    }
    if (!clamp_size(&w, &h, screen_rows, screen_cols)) {
        return 0;
    }

    const int max_bottom = screen_rows - 1;

    int left = parent->left + parent->cols;
    if (left + w > screen_cols) {
        left = parent->left - w;
    }
    if (left < 0) {
        left = 0;
    }
    if (left + w > screen_cols) {
        left = screen_cols - w;
    }

    int top = item_screen_row;
    if (top + h > max_bottom) {
        top = item_screen_row - h + 1;
    }
    if (top < 0) {
        top = 0;
    }
    if (top + h > max_bottom) {
        top = max_bottom - h;
    }
    if (top < 0) {
        top = 0;
    }

    out->top = top;
    out->left = left;
    out->rows = h;
    out->cols = w;
    return 1;
}

int ui_ctx_menu_item_index(int local_row, int visible, int scroll, int count)
{
    if (visible <= 0 || local_row < 1 || local_row > visible) {
        return -1;
    }
    int idx = scroll + local_row - 1;
    if (idx < 0 || idx >= count) {
        return -1;
    }
    return idx;
}

int ui_ctx_menu_scroll_for(int selected, int count, int visible, int current)
{
    if (count <= 0 || visible <= 0) {
        return 0;
    }
    const int max_scroll = (count > visible) ? (count - visible) : 0;

    if (current < 0) {
        current = 0;
    } else if (current > max_scroll) {
        current = max_scroll;
    }

    if (selected < current) {
        current = selected;
    } else if (selected >= current + visible) {
        current = selected - visible + 1;
    }

    if (current < 0) {
        current = 0;
    }
    if (current > max_scroll) {
        current = max_scroll;
    }
    return current;
}

int ui_rect_intersects(const UIRect *a, const UIRect *b)
{
    if (!a || !b || a->rows <= 0 || a->cols <= 0 ||
        b->rows <= 0 || b->cols <= 0) {
        return 0;
    }
    return a->left < b->left + b->cols &&
           b->left < a->left + a->cols &&
           a->top < b->top + b->rows &&
           b->top < a->top + a->rows;
}
