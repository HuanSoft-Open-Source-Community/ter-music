/**
 * @file tty_guard_policy.c
 * @brief 终端输出守卫的纯决策函数（无 ncurses / 无全局状态，便于单测）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/tty_guard_policy.h"

int ui_tty_guard_classify_burst(unsigned long long bytes)
{
    if (bytes >= (unsigned long long)UI_TTY_BURST_SEVERE_BYTES) {
        return 2;
    }
    if (bytes >= (unsigned long long)UI_TTY_BURST_WARN_BYTES) {
        return 1;
    }
    return 0;
}

int ui_tty_guard_should_start_pass(int risk_pending, unsigned long long now_ms,
                                   unsigned long long last_done_ms,
                                   int repair_active)
{
    if (!risk_pending || repair_active) {
        return 0;
    }
    /* 单调时钟回绕/初始化保护：now < last_done 时按“刚完成”处理 */
    if (now_ms < last_done_ms) {
        return 0;
    }
    return (now_ms - last_done_ms) >= (unsigned long long)UI_TTY_REPAIR_MIN_INTERVAL_MS;
}

UiTtyEscalation ui_tty_guard_escalate(unsigned long long stalled_frames,
                                      unsigned long long now_ms,
                                      unsigned long long last_mode_reset_ms,
                                      unsigned long long last_hard_reset_ms,
                                      unsigned long long resets_since_hard)
{
    if (stalled_frames == 0) {
        return UI_TTY_ESCALATE_NONE;
    }

    /* F3：至少 3 次“背压停滞帧”，且已经用过 >=3 次 F2、距上次 F3 超过冷却，
     * 才认作“模式可能被毒化且 F2 没能解决”。 */
    if (stalled_frames >= 3 &&
        resets_since_hard >= UI_TTY_HARD_RESET_AFTER_RESETS &&
        now_ms >= last_hard_reset_ms &&
        (now_ms - last_hard_reset_ms) >=
            (unsigned long long)UI_TTY_HARD_RESET_COOLDOWN_MS) {
        return UI_TTY_ESCALATE_HARD;
    }

    if (now_ms >= last_mode_reset_ms &&
        (now_ms - last_mode_reset_ms) >=
            (unsigned long long)UI_TTY_MODE_RESET_COOLDOWN_MS) {
        return UI_TTY_ESCALATE_MODES;
    }

    return UI_TTY_ESCALATE_NONE;
}
