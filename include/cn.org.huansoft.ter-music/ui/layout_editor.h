/**
 * @file layout_editor.h
 * @brief TUI 布局编辑器（设置 → 外观 → 编辑/新建布局）
 *
 * 全屏编辑一份布局的工作副本：分栏、删除、比例、均分、换类型、交换与
 * 逐窗格颜色。所有几何操作都走分栏树的事务式接口，因此工作副本始终是
 * 强制平铺的；右侧实时预览与运行时使用同一套 resolve 结果。
 * 保存写入用户布局文件（~/.config/ter-music/layouts.json，同 id 覆盖
 * 内置预设）。编辑器激活期间接管全部按键；Esc/q 返回，S 保存。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_EDITOR_H
#define UI_LAYOUT_EDITOR_H

#include "ui/layout_spec.h"
#include "ui/mouse.h"

/** @brief 编辑器是否处于激活状态（设置输入分发据此拦截按键）。 */
int layout_editor_active(void);

/**
 * @brief 打开编辑器（以 base 的工作副本开始，立即全屏绘制）。
 * @param base   起始布局（深拷贝；通常为当前布局）
 * @param is_new 1 = 新建模式（自动生成 user_N 唯一 id 与默认名）
 */
void layout_editor_open(const LayoutSpec *base, int is_new);

/**
 * @brief 处理一个按键。仅在 layout_editor_active() 时有意义。
 * @return 1 = 按键已消费（含关闭编辑器的那一次）。
 */
int layout_editor_handle_key(int ch);

/** @brief 全屏重绘（打开与每次按键后由模块内部调用，外部一般无需直接调）。 */
void layout_editor_render(void);

/**
 * @brief 模态菜单（动作/换类型/颜色/帮助）是否展开。
 *
 * 全局鼠标分发用它置"弹层展开"标志：展开期间滚轮与点击都归菜单，
 * 不会落到菜单底下的设置页（竞争态由 ui/mouse_routing.c 统一裁决）。
 */
int layout_editor_menu_open(void);

/** @brief 取模态菜单框的屏幕矩形；未展开时返回 0。 */
int layout_editor_menu_rect(UIRect *out);

/** @brief 当前模态菜单窗口（void * 即 WINDOW *）；未展开时为 NULL。
 * 菜单进入/消失动画期间由 ui/menu_anim 负责移动；合成层据此把实时窗口
 * 压到 stdscr 之上。 */
void *layout_editor_menu_window(void);

/** @brief 丢弃当前模态菜单窗口（不播动画），供编辑器关闭/清理使用。 */
void layout_editor_close_menu(void);

/**
 * @brief 右键入口：在指针位置打开"窗格操作"菜单（贴边翻转/钳制）。
 *
 * 与 Enter 打开的菜单同一份条目与几何，只是把 anchor 设为指针坐标；
 * 终端小于编辑器最小尺寸时返回 0（不打开）。
 */
int layout_editor_open_actions_menu(int anchor_y, int anchor_x);

/**
 * @brief 处理一个鼠标事件。仅在 layout_editor_active() 时有意义。
 * @param ev 归一化后的事件（坐标为屏幕坐标）
 * @param d  归属判定结果：EDITOR_MENU 时 d->row/d->col 是菜单框本地坐标，
 *           框外点击为 -1；EDITOR_PANE 时 d->row/d->col 原样是屏幕坐标。
 * @return 1 = 已消费（编辑器全屏独占，事件永不穿透）
 */
int layout_editor_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);

#endif /* UI_LAYOUT_EDITOR_H */
