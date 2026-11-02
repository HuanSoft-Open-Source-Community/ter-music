/**
 * @file layout_geometry.h
 * @brief 主界面布局的像素几何（纯算术，无 ncurses 依赖）
 *
 * 输入是分栏树展开后的千分比矩形，输出每个窗格在终端主区域内的
 * 单元格矩形。主区域 = 终端去掉底部菜单栏后的全部区域。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_GEOMETRY_H
#define UI_LAYOUT_GEOMETRY_H

#include "ui/layout_spec.h"

typedef struct {
    int visible;        /* 解析后的可见性 */
    int has_cells;      /* w > 0 && h > 0；0 表示小于一个像素，无窗口 */
    int x, y, w, h;     /* 终端单元格坐标（0 基） */
} LayoutPaneRect;

typedef struct {
    int valid;          /* 1 = 主区域非空 */
    int x, y, w, h;     /* 主区域 */
    int pane_count;
    LayoutPaneRect panes[LAYOUT_PANE_COUNT];
} LayoutGeometry;

/**
 * @brief 计算主区域：终端去掉底部菜单栏（最后一行）。
 */
void layout_geometry_main_area(int rows, int cols,
                               int *x, int *y, int *w, int *h);

/**
 * @brief 把分栏树展开结果映射到单元格。
 *
 * 共享边界使用同一 floor 公式，强制平铺的布局在像素级也不会产生
 * 缝隙或重叠。宽或高为 0 的窗格 has_cells=0。
 */
void layout_geometry_compute(const LayoutResolved *resolved, int rows, int cols,
                             LayoutGeometry *out);

/**
 * @brief 保底几何：单窗格全屏 playlist。
 *
 * 仅在选中布局的某个正面积窗格 newwin 失败时使用，保证主区域仍有
 * 一个窗口填满，不留下空白。主区域为空时 valid=0。
 */
void layout_geometry_rescue(int rows, int cols, LayoutGeometry *out);

#endif /* UI_LAYOUT_GEOMETRY_H */
