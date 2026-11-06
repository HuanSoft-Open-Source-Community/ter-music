/**
 * @file menu_anim.h
 * @brief 菜单入场/消失动画的状态机、ghost 与合成层
 *
 * 设计约束：
 *   - ncurses 无关的起止几何、缓动、降级判断在 ui/menu_anim_math.h；
 *   - 进入动画只借用菜单自己的窗口（mvwin），不复制；
 *   - 消失动画用 dupwin() 复制一份 ghost 后由本模块持有，调用方可以立即
 *     销毁原窗口；ghost 到期/取消时由本模块 delwin；
 *   - 每帧按 z 序重新压入所有可见层，保证移动中的菜单不会留下旧像素；
 *     流光模式直接复用 ui_rainbow_paint()，同一帧只做一次 doupdate；
 *   - 能力不足或实测单帧合成过慢时只降不升：FULL → REDUCED → OFF。
 *
 * 所有状态只在 UI 单线程内读写。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_MENU_ANIM_H
#define UI_MENU_ANIM_H

#include <ncursesw/ncurses.h>

#include "ui/menu_anim_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化档位探测；幂等，通常在 init_ncurses() 末尾调用。 */
void ui_menu_anim_init(void);

/** @brief 释放 ghost；不得在 endwin() 之后调用。不主动合成。 */
void ui_menu_anim_shutdown(void);

/** @brief 重新按终端能力探测档位（流光开关时用），并清空帧耗时样本。 */
void ui_menu_anim_reset_tier(void);

/**
 * @brief 启动进入动画：把 win 移到推导入场起点；结束时停在 final。
 *
 * @param win        已创建的菜单窗口（仍是最终尺寸）
 * @param style      纵向 / 对角
 * @param final_top  最终屏幕行
 * @param final_left 最终屏幕列
 * @return 1 = 已注册；0 = 无动画（调用方保持窗口在最终位置）
 */
int ui_menu_anim_begin_enter(WINDOW *win, UIMenuAnimStyle style,
                             int final_top, int final_left);

/**
 * @brief 从 win 当前内容复制 ghost 并启动消失动画。
 *
 * **调用方仍然拥有 win，必须随后 delwin(win)。** ghost 由本模块持有。
 *
 * @param layer z 序提示（上下文菜单层号），仅用于日志与排序排查
 * @return 1 = 已启动 ghost；0 = 未启动（调用方直接瞬时关闭）
 */
int ui_menu_anim_begin_exit(WINDOW *win, UIMenuAnimStyle style, int layer);

/** @brief 移除 win 的 ENTER/SETTLED 记录（窗口将被销毁前调用）。 */
void ui_menu_anim_cancel_window(WINDOW *win);

/**
 * @brief 清掉所有动画状态：ghost 删除、活动窗口 snap 到终点、SETTLED 丢弃。
 *
 * @param repaint 1 = 删除 ghost 后补一帧底层合成（新 overlay/模态入口用）；
 *                视图切换/布局重建/退出传 0，由后续整屏重绘覆盖。
 */
void ui_menu_anim_cancel_all(int repaint);

/**
 * @brief 立即收尾所有消失动画（输入到达或新 overlay 打开时）。
 *
 * @param repaint 1 = 删除 ghost 后补一帧底层合成
 */
void ui_menu_anim_finish_exits(int repaint);

/** @brief 是否有进行中的进入/消失动画（SETTLED 不算）。 */
int ui_menu_anim_active(void);

/** @brief 是否有进行中的消失动画（ghost）。 */
int ui_menu_anim_exit_active(void);

/**
 * @brief 推进一帧并合成（仅在位置变化/ghost 删除时真正 doupdate）。
 *
 * @return 1 = 本轮由本模块处理（调用方不要再跑普通流光合成）；
 *         0 = 没有活动动画（调用方照常 ui_rainbow_paint()）。
 */
int ui_menu_anim_frame(void);

/** @brief 当前 ghost 数（消失动画的快照层）。 */
int ui_menu_anim_ghost_count(void);

/** @brief 第 index 个 ghost 窗口；越界返回 NULL。 */
WINDOW *ui_menu_anim_ghost_window(int index);

/** @brief ghost 是否与给定屏幕矩形相交（只接触边缘不算）。 */
int ui_menu_anim_ghost_intersects_rect(int top, int left, int rows, int cols);

/** @brief 把所有 ghost 重新压回虚拟屏（不 doupdate）；返回压回层数。 */
int ui_menu_anim_reassert_ghosts(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_ANIM_H */
