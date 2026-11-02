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

#endif /* UI_LAYOUT_EDITOR_H */
