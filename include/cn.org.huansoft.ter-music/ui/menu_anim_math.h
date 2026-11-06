/**
 * @file menu_anim_math.h
 * @brief 菜单入场/消失动画的纯数学层（不依赖 ncurses）
 *
 * 只负责"从哪个屏幕格滑到哪个屏幕格、每毫秒滑到哪、终端能力不够时
 * 退到哪一档"，窗口移动/合成/ghost 生命周期归 ui/menu_anim.c。
 * 因为没有 ncurses 依赖，这份推导由 scripts/test/test_menu_anim_math.c
 * 做穷举断言。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_MENU_ANIM_MATH_H
#define UI_MENU_ANIM_MATH_H

#include "ui/mouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 菜单运动风格。 */
typedef enum {
    UI_MENU_ANIM_VERTICAL = 0,   /**< 下拉式选择菜单：只沿纵向滑动。 */
    UI_MENU_ANIM_DIAGONAL = 1    /**< 右键菜单：纵向 + 横向同时滑动。 */
} UIMenuAnimStyle;

/**
 * @brief 动画档位（只会逐级下降，不会回升）。
 *
 * OFF     = 完全不做位移动画；
 * REDUCED = 削弱档：只滑 1 行/1 列、时长更短；
 * FULL    = 完整档。
 */
typedef enum {
    UI_MENU_ANIM_TIER_OFF = 0,
    UI_MENU_ANIM_TIER_REDUCED = 1,
    UI_MENU_ANIM_TIER_FULL = 2
} UIMenuAnimTier;

/** @brief 一段直线位移的起止屏幕格（窗口左上角）。 */
typedef struct {
    int from_top;
    int from_left;
    int to_top;
    int to_left;
} UIMenuAnimPlan;

/**
 * @brief 按风格/档位推导入场起点；终点就是 final_rect 的左上角。
 *
 * 起点 = 终点沿运动方向回退偏移量，并把整窗钳制在屏幕内（末行提示条不压）。
 * 回退方向贴边时自动换向；如果两个方向都放不下（起终点相同），
 * from_* == to_*，调用方应视为"无动画"。
 */
UIMenuAnimPlan ui_menu_anim_plan(UIMenuAnimStyle style, UIMenuAnimTier tier,
                                 UIRect final_rect, int screen_rows, int screen_cols);

/** @brief 入场/消失时长（毫秒）；OFF 返回 0。 */
unsigned ui_menu_anim_duration_ms(UIMenuAnimTier tier, UIMenuAnimStyle style,
                                  int exiting);

/**
 * @brief 把 0..1000 的线性进度缓动成 0..1000。
 *
 * 入场 ease-out（先快后慢），消失 ease-in（先慢后快）；端点精确。
 */
int ui_menu_anim_ease(int progress_permille, int exiting);

/**
 * @brief 按经过毫秒数算当前窗口左上角。
 *
 * 时间一到直接落到终点；慢帧只会跳格，不会把总时长拉长。
 */
void ui_menu_anim_position(const UIMenuAnimPlan *plan, unsigned elapsed_ms,
                           unsigned duration_ms, int exiting,
                           int *top, int *left);

/**
 * @brief 根据最近若干帧的平均合成耗时决定是否降级。
 *
 * 只降不升：avg >= 55ms → OFF；avg >= 25ms 且 FULL → REDUCED。
 */
UIMenuAnimTier ui_menu_anim_tier_after_frame(UIMenuAnimTier tier,
                                             unsigned avg_frame_cost_ms);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_ANIM_MATH_H */
