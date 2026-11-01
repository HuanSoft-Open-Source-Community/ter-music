/**
 * @file layout_geometry.c
 * @brief 主界面布局的像素几何实现
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/layout_geometry.h"

#include <string.h>

void layout_geometry_main_area(int rows, int cols,
                               int *x, int *y, int *w, int *h)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = cols;
    if (h) *h = rows > 0 ? rows - 1 : 0;   /* 最后一行是菜单提示条 */
}

/* 累计边界取整：相邻窗格共享边界时得到同一个像素坐标。 */
static int edge(int origin, int units, int extent)
{
    long long scaled = (long long)units * (long long)extent;
    return origin + (int)(scaled / LAYOUT_AREA_UNITS);
}

void layout_geometry_compute(const LayoutSpec *resolved, int rows, int cols,
                             LayoutGeometry *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    int area_x, area_y, area_w, area_h;
    layout_geometry_main_area(rows, cols, &area_x, &area_y, &area_w, &area_h);
    out->x = area_x;
    out->y = area_y;
    out->w = area_w;
    out->h = area_h;

    if (!resolved || area_w <= 0 || area_h <= 0) {
        return;
    }
    out->valid = 1;
    out->pane_count = LAYOUT_PANE_COUNT;

    for (int i = 0; i < resolved->pane_count && i < LAYOUT_MAX_PANES; i++) {
        const LayoutPaneSpec *pane = &resolved->panes[i];
        LayoutPaneRect *rect = &out->panes[pane->kind];
        rect->visible = pane->visible ? 1 : 0;
        if (!rect->visible) {
            continue;
        }

        int sx = edge(area_x, pane->x, area_w);
        int ex = edge(area_x, pane->x + pane->w, area_w);
        int sy = edge(area_y, pane->y, area_h);
        int ey = edge(area_y, pane->y + pane->h, area_h);

        rect->x = sx;
        rect->y = sy;
        rect->w = ex - sx;
        rect->h = ey - sy;
        rect->has_cells = (rect->w > 0 && rect->h > 0) ? 1 : 0;
    }
}

void layout_geometry_rescue(int rows, int cols, LayoutGeometry *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    int area_x, area_y, area_w, area_h;
    layout_geometry_main_area(rows, cols, &area_x, &area_y, &area_w, &area_h);
    out->x = area_x;
    out->y = area_y;
    out->w = area_w;
    out->h = area_h;
    if (area_w <= 0 || area_h <= 0) {
        return;
    }

    out->valid = 1;
    out->pane_count = LAYOUT_PANE_COUNT;
    out->panes[LAYOUT_PANE_PLAYLIST].visible = 1;
    out->panes[LAYOUT_PANE_PLAYLIST].has_cells = 1;
    out->panes[LAYOUT_PANE_PLAYLIST].x = area_x;
    out->panes[LAYOUT_PANE_PLAYLIST].y = area_y;
    out->panes[LAYOUT_PANE_PLAYLIST].w = area_w;
    out->panes[LAYOUT_PANE_PLAYLIST].h = area_h;
}
