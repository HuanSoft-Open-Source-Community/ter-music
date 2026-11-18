/**
 * @file rainbow_band_policy.h
 * @brief R3 分带流光的纯决策函数（无 ncurses / 无全局状态，便于单测）
 *
 * t18 裁决（Captain）：R3 采用「墙钟相位 + 每帧最多染一带 + 绝不追帧/补帧」。
 * 本模块只做三件事，全部是可单测的纯函数：
 *   1) 给定屏幕行数/带高/当前带序号，算出本帧要重染的行区间（每帧 ≤1 带）；
 *   2) 给定单调时钟，算出当前应使用的色阶（落后就丢弃，不快进）；
 *   3) 给定上一帧的实测写入字节，决定下一帧的带高（预算内尽量宽）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef TER_MUSIC_UI_RAINBOW_BAND_POLICY_H
#define TER_MUSIC_UI_RAINBOW_BAND_POLICY_H

/** 一帧的分带绘制计划。 */
typedef struct {
    int y0;      /**< 本帧重染的首行（含），无带时等于 y1 */
    int y1;      /**< 本帧重染的末行（不含） */
    int next;    /**< 下一帧的带序号 */
    int wrapped; /**< 非 0 表示本帧扫完了最后一带（下一帧回到 0） */
} UiRainbowBandPlan;

/**
 * @brief 计算一帧的带区间（每帧最多一带，索引越界自动回绕）。
 *
 * @param rows      屏幕行数（<=0 时返回空带）
 * @param band_rows 带高（<=0 时按 1 处理）
 * @param index     当前带序号（<0 或 >= 带数时回绕）
 * @return 本帧的行区间与下一带序号
 */
UiRainbowBandPlan ui_rainbow_band_plan(int rows, int band_rows, int index);

/**
 * @brief 墙钟相位：当前应使用的色阶（落后丢弃、不快进/不补帧）。
 *
 * @param now_ms  单调时钟毫秒
 * @param step_ms 每色阶毫秒（<=0 时按 1 处理）
 * @param steps   色阶总数（<=0 时返回 0）
 * @return 0..steps-1 的色阶
 */
int ui_rainbow_band_phase(unsigned long long now_ms, unsigned long long step_ms,
                          int steps);

/**
 * @brief 依据上一帧实测写入决定下一帧带高（预算内尽量宽，至少 1 行）。
 *
 * @param current_rows 当前带高
 * @param wrote        上一帧实测写入字节
 * @param budget       单帧预算字节
 * @param max_rows     带高上限
 * @return 下一帧带高（1..max_rows）
 */
int ui_rainbow_band_rows_for_write(int current_rows, unsigned long long wrote,
                                   unsigned long long budget, int max_rows);

#endif /* TER_MUSIC_UI_RAINBOW_BAND_POLICY_H */
