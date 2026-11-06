/**
 * @file menu_anim_math.c
 * @brief 菜单入场/消失动画的纯数学实现（见 ui/menu_anim_math.h）
 */

#include "ui/menu_anim_math.h"

#include <stddef.h>

/* 完整档：下拉 4 行；右键 3 行 + 5 列。削弱档统一 1 行/1 列。 */
#define MENU_ANIM_FULL_VERTICAL_ROWS    4
#define MENU_ANIM_FULL_DIAGONAL_ROWS    3
#define MENU_ANIM_FULL_DIAGONAL_COLS    5
#define MENU_ANIM_REDUCED_ROWS          1
#define MENU_ANIM_REDUCED_COLS          1

/* 时长：入场/消失分开，削弱档约为完整档的一半。 */
#define MENU_ANIM_FULL_VERTICAL_IN_MS   140u
#define MENU_ANIM_FULL_VERTICAL_OUT_MS  110u
#define MENU_ANIM_FULL_DIAGONAL_IN_MS   150u
#define MENU_ANIM_FULL_DIAGONAL_OUT_MS  115u
#define MENU_ANIM_REDUCED_IN_MS         70u
#define MENU_ANIM_REDUCED_OUT_MS        55u

#define MENU_ANIM_DEGRADE_MS            25u
#define MENU_ANIM_DISABLE_MS            55u

/* 窗口要整块留在屏幕内；末行留给底部提示条。 */
static int clamp_axis(int value, int min_value, int max_value)
{
    if (max_value < min_value) {
        return min_value;
    }
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void tier_offsets(UIMenuAnimTier tier, UIMenuAnimStyle style,
                         int *rows, int *cols)
{
    *rows = 0;
    *cols = 0;
    if (tier == UI_MENU_ANIM_TIER_OFF) {
        return;
    }
    if (style == UI_MENU_ANIM_VERTICAL) {
        *rows = (tier == UI_MENU_ANIM_TIER_FULL)
                    ? MENU_ANIM_FULL_VERTICAL_ROWS
                    : MENU_ANIM_REDUCED_ROWS;
        return;
    }
    if (tier == UI_MENU_ANIM_TIER_FULL) {
        *rows = MENU_ANIM_FULL_DIAGONAL_ROWS;
        *cols = MENU_ANIM_FULL_DIAGONAL_COLS;
    } else {
        *rows = MENU_ANIM_REDUCED_ROWS;
        *cols = MENU_ANIM_REDUCED_COLS;
    }
}

UIMenuAnimPlan ui_menu_anim_plan(UIMenuAnimStyle style, UIMenuAnimTier tier,
                                 UIRect final_rect, int screen_rows, int screen_cols)
{
    UIMenuAnimPlan plan;
    plan.from_top = final_rect.top;
    plan.from_left = final_rect.left;
    plan.to_top = final_rect.top;
    plan.to_left = final_rect.left;

    if (tier == UI_MENU_ANIM_TIER_OFF || final_rect.rows <= 0 ||
        final_rect.cols <= 0 || screen_rows <= 0 || screen_cols <= 0) {
        return plan;
    }

    int dy = 0;
    int dx = 0;
    tier_offsets(tier, style, &dy, &dx);

    /* 起点允许落在屏幕内、不压末行提示条；窗口本身可能比屏幕大，此时
     * max_top 取 0，仍然只移动可用范围内的那几行。 */
    const int max_top = (screen_rows - 1 - final_rect.rows > 0)
                            ? (screen_rows - 1 - final_rect.rows)
                            : 0;
    const int max_left = (screen_cols - final_rect.cols > 0)
                             ? (screen_cols - final_rect.cols)
                             : 0;

    int from_top = clamp_axis(final_rect.top - dy, 0, max_top);
    int from_left = (style == UI_MENU_ANIM_VERTICAL)
                        ? final_rect.left
                        : clamp_axis(final_rect.left - dx, 0, max_left);

    if (from_top == final_rect.top && from_left == final_rect.left) {
        /* 贴边回退放不下：换到反方向再试一次。 */
        from_top = clamp_axis(final_rect.top + dy, 0, max_top);
        from_left = (style == UI_MENU_ANIM_VERTICAL)
                        ? final_rect.left
                        : clamp_axis(final_rect.left + dx, 0, max_left);
    }

    if (from_top == final_rect.top && from_left == final_rect.left) {
        return plan;   /* 没有任何一格可移动：调用方按无动画处理 */
    }

    plan.from_top = from_top;
    plan.from_left = from_left;
    return plan;
}

unsigned ui_menu_anim_duration_ms(UIMenuAnimTier tier, UIMenuAnimStyle style,
                                  int exiting)
{
    if (tier == UI_MENU_ANIM_TIER_OFF) {
        return 0;
    }
    if (tier == UI_MENU_ANIM_TIER_REDUCED) {
        return exiting ? MENU_ANIM_REDUCED_OUT_MS : MENU_ANIM_REDUCED_IN_MS;
    }
    if (style == UI_MENU_ANIM_VERTICAL) {
        return exiting ? MENU_ANIM_FULL_VERTICAL_OUT_MS
                       : MENU_ANIM_FULL_VERTICAL_IN_MS;
    }
    return exiting ? MENU_ANIM_FULL_DIAGONAL_OUT_MS
                   : MENU_ANIM_FULL_DIAGONAL_IN_MS;
}

int ui_menu_anim_ease(int progress_permille, int exiting)
{
    if (progress_permille <= 0) {
        return 0;
    }
    if (progress_permille >= 1000) {
        return 1000;
    }

    const long long t = progress_permille;
    if (exiting) {
        /* ease-in cubic */
        return (int)((t * t * t) / 1000000LL);
    }
    /* ease-out cubic：1 - (1-t)^3 */
    const long long inv = 1000LL - t;
    return 1000 - (int)((inv * inv * inv) / 1000000LL);
}

static int lerp_round(int from, int to, int eased_permille)
{
    const long long delta = (long long)(to - from) * eased_permille;
    if (delta >= 0) {
        return from + (int)((delta + 500) / 1000);
    }
    return from - (int)((-delta + 500) / 1000);
}

void ui_menu_anim_position(const UIMenuAnimPlan *plan, unsigned elapsed_ms,
                           unsigned duration_ms, int exiting,
                           int *top, int *left)
{
    if (!plan || !top || !left) {
        return;
    }
    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        *top = plan->to_top;
        *left = plan->to_left;
        return;
    }

    int progress = (int)(((unsigned long long)elapsed_ms * 1000ULL) /
                         (unsigned long long)duration_ms);
    if (progress < 0) {
        progress = 0;
    }
    if (progress > 1000) {
        progress = 1000;
    }
    const int eased = ui_menu_anim_ease(progress, exiting);
    *top = lerp_round(plan->from_top, plan->to_top, eased);
    *left = lerp_round(plan->from_left, plan->to_left, eased);
}

UIMenuAnimTier ui_menu_anim_tier_after_frame(UIMenuAnimTier tier,
                                             unsigned avg_frame_cost_ms)
{
    if (tier == UI_MENU_ANIM_TIER_OFF) {
        return UI_MENU_ANIM_TIER_OFF;
    }
    if (avg_frame_cost_ms >= MENU_ANIM_DISABLE_MS) {
        return UI_MENU_ANIM_TIER_OFF;
    }
    if (tier == UI_MENU_ANIM_TIER_FULL &&
        avg_frame_cost_ms >= MENU_ANIM_DEGRADE_MS) {
        return UI_MENU_ANIM_TIER_REDUCED;
    }
    return tier;
}
