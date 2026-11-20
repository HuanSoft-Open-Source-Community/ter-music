/**
 * @file rainbow_band_policy.c
 * @brief R3 分带流光的纯决策实现（无 ncurses，便于单测）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/rainbow_band_policy.h"

UiRainbowBandPlan ui_rainbow_band_plan(int rows, int band_rows, int index)
{
    UiRainbowBandPlan plan = { 0, 0, 0, 0 };
    if (rows <= 0) {
        return plan;
    }
    if (band_rows <= 0) {
        band_rows = 1;
    }
    if (band_rows > rows) {
        band_rows = rows;
    }

    int bands = (rows + band_rows - 1) / band_rows;
    if (bands < 1) {
        bands = 1;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= bands) {
        index %= bands;
    }

    int y0 = index * band_rows;
    int y1 = y0 + band_rows;
    if (y0 > rows) {
        y0 = rows;
    }
    if (y1 > rows) {
        y1 = rows;
    }

    plan.y0 = y0;
    plan.y1 = y1;
    if (index + 1 >= bands) {
        plan.next = 0;
        plan.wrapped = 1;
    } else {
        plan.next = index + 1;
        plan.wrapped = 0;
    }
    return plan;
}

int ui_rainbow_band_phase(unsigned long long now_ms, unsigned long long step_ms,
                          int steps)
{
    if (steps <= 0) {
        return 0;
    }
    if (step_ms == 0) {
        step_ms = 1;
    }
    /* 落后就丢弃：相位只由墙钟决定，绝不因为“扫描没跟上”而快进或补帧。 */
    unsigned long long tick = now_ms / step_ms;
    return (int)(tick % (unsigned long long)steps);
}

int ui_rainbow_band_rows_for_write(int current_rows, unsigned long long wrote,
                                   unsigned long long budget, int max_rows)
{
    if (max_rows < 1) {
        max_rows = 1;
    }
    if (current_rows < 1) {
        current_rows = 1;
    }
    if (current_rows > max_rows) {
        current_rows = max_rows;
    }
    if (budget == 0 || wrote == 0) {
        /* 没有测量值时不猜：保持当前带高。 */
        return current_rows;
    }
    if (wrote > budget) {
        return (current_rows > 1) ? current_rows - 1 : 1;
    }
    /* 还有余量才加宽：预计“多一行”仍不超过预算的 60% 时才加。 */
    unsigned long long projected =
        (unsigned long long)current_rows ? (wrote * (unsigned long long)(current_rows + 1) /
                                            (unsigned long long)current_rows)
                                         : wrote;
    if (current_rows < max_rows && projected * 10ULL <= budget * 6ULL) {
        return current_rows + 1;
    }
    return current_rows;
}
