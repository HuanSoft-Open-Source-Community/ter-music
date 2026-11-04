/**
 * @file context_menu_layout.h
 * @brief 右键上下文菜单的**纯几何**计算（不依赖 ncurses）
 *
 * 菜单的屏幕摆放、子菜单摆放、滚动与行命中全部集中在这里，渲染与鼠标分发
 * 共用同一份推导，单测可在 scripts/test/test_context_menu_layout.c 里对
 * 「锚点 × 屏幕尺寸 × 菜单尺寸」做穷举边界断言。
 *
 * 约束（与 docs 的右键章节一致）：
 *   - 菜单完整落在 [0, screen_cols) × [0, screen_rows-1)：最后一行留给底部
 *     提示条，菜单绝不覆盖；
 *   - 锚点在右下边缘时先上翻/左翻，仍放不下才钳制；
 *   - 子菜单优先放父菜单右侧、与当前条目对齐，右侧放不下放左侧；
 *   - 终端太小（宽 < UI_CTX_MENU_MIN_W 或可用高 < UI_CTX_MENU_MIN_H）
 *     返回失败，调用方不打开菜单。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_CONTEXT_MENU_LAYOUT_H
#define UI_CONTEXT_MENU_LAYOUT_H

#include "ui/mouse.h"

#define UI_CTX_MENU_MIN_W 12
#define UI_CTX_MENU_MIN_H 4

/**
 * @brief 把锚点 + 期望尺寸钳制成完整落在屏幕内的菜单矩形。
 *
 * @param anchor_y    右击的屏幕行（0 基，可越界）
 * @param anchor_x    右击的屏幕列（0 基，可越界）
 * @param w           期望宽度（列）；超屏时收缩
 * @param h           期望高度（行）；超屏时收缩
 * @param screen_rows 终端行数
 * @param screen_cols 终端列数
 * @param out         结果矩形
 * @return 1 = 可放置；0 = 终端太小/参数非法（不打开菜单）
 */
int ui_ctx_menu_place_rect(int anchor_y, int anchor_x, int w, int h,
                           int screen_rows, int screen_cols, UIRect *out);

/**
 * @brief 子菜单矩形：优先父菜单右侧，越界放左侧；纵向与条目行对齐并钳制。
 *
 * @param parent          父菜单矩形
 * @param item_screen_row 当前选中条目在屏幕上的行（0 基）
 */
int ui_ctx_menu_place_submenu(const UIRect *parent, int item_screen_row,
                              int w, int h, int screen_rows, int screen_cols,
                              UIRect *out);

/**
 * @brief 菜单框本地行 → 条目下标。
 *
 * 布局约定：第 0 行是上边框/标题，第 1..visible 行是条目，随后一行是页脚，
 * 最后一行是下边框。边框/页脚/越界返回 -1。
 */
int ui_ctx_menu_item_index(int local_row, int visible, int scroll, int count);

/** @brief 保持选中项可见的滚动偏移（0..count-visible）。 */
int ui_ctx_menu_scroll_for(int selected, int count, int visible, int current);

/**
 * @brief 两个屏幕矩形是否严格重叠（只接触边缘不算）。
 *
 * 供菜单合成刷新判断"这个窗口会不会盖住菜单"；空/非法矩形返回 0。
 */
int ui_rect_intersects(const UIRect *a, const UIRect *b);

#endif /* UI_CONTEXT_MENU_LAYOUT_H */
