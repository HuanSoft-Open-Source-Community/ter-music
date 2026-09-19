/**
 * @file visualizer.h
 * @brief 可视化采样（FFT 频段）——后端侧接口
 *
 * 可视化的**采样与状态**由后端拥有（`audio/audio_visualizer.c`）：它是
 * “当前曲目信息”的一部分，daemon 与 TUI 都要用；**渲染**归前端
 * （`ui/visualizer.c`），经 `player_visualizer()` 取数。
 *
 * 这些声明原先放在 `ui/ui.h` 里，属界面头文件被后端引用；2026-09-15 的
 * 架构调整把它们移到这里，使 `audio/` 不必认识任何界面头。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef AUDIO_VISUALIZER_H
#define AUDIO_VISUALIZER_H

#include <stdint.h>

/* 清空频谱状态（换曲 / 停止时调用） */
void reset_visualizer_state(void);

/* 送入一帧 PCM 样本做 FFT（由播放线程在写出音频时调用） */
void push_visualizer_samples(const int32_t *samples, int frame_count, int channels);

/* 取一帧频谱快照：levels/peaks 各 max_levels 个（0-255），
 * last_update_ms 为本次采样时刻（单调时钟毫秒）。 */
void get_visualizer_snapshot(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);

#endif /* AUDIO_VISUALIZER_H */
