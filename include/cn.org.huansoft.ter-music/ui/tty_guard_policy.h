/**
 * @file tty_guard_policy.h
 * @brief 终端输出守卫的阈值常量与纯决策函数（不依赖 ncurses，可单测）
 *
 * 背景（.tmp/uab-ui-fix/ROOTCAUSE.md）：玲珑 ll-box 2.3.5 的冷启动转发在
 * 终端 pty 队列满时会“部分写已写出 k 字节却没有推进 tail”，下一轮从同一
 * tail 重放，把重复字节拼进终端流。应用侧**无法阻止**这次改写（发生在
 * stdout 之下的 ll-box 里），只能：
 *   1) 记账：/proc/self/io 的 `wchar` 增量 = 本帧真正写进终端的字节数；
 *   2) 分级：单帧 >= WARN 视为高风险 burst（pty 队列可能被灌满），
 *      >= SEVERE 视为严重 burst（ll-box 8 KiB ring 的 8 倍以上）；
 *   3) 恢复：整屏强制重绘（F1 = clearok 走 ClrUpdate；单轮写入实测 8.9–34.8 KiB，
 *      A6′ 上限 64 KiB）。分带绘制只用于 R3 的流光相位帧
 *      （见 ui/rainbow_band_policy.h），与本文件的恢复路径无关。
 *
 * 这些常量都可被环境变量覆盖（见 tty_guard.c 的 ui_tty_guard_init），
 * 默认值面向 237x57 的真实终端。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_TTY_GUARD_POLICY_H
#define UI_TTY_GUARD_POLICY_H

/* 单帧终端写入达到该值即视为高风险 burst（T2：ll-box 实测可用窗口 1.5–5.6 KiB）。
 * 默认 4096 字节；环境变量 TER_MUSIC_TTY_BURST_BYTES 可覆盖。 */
#define UI_TTY_BURST_WARN_BYTES 4096u

/* 单帧写入达到该值即视为严重 burst：远超 ll-box 8 KiB ring 的安全窗口。
 * 只用于日志分级与升级判据，不直接触发整屏重发。 */
#define UI_TTY_BURST_SEVERE_BYTES 65536u

/* 参考预算：A6′ 偏差日志用它判断“单轮整屏重绘是否超过原 4 KiB 分带目标”。
 * 原 A6（分带 <= 4 KiB）已由 Captain 裁决改为 A6′（单轮 <= 64 KiB）；此常量
 * 只作日志分级参考，不参与任何分带逻辑。 */
#define UI_TTY_BAND_BUDGET_BYTES 4096u

/* L1 整屏强制重绘周期：修复并进相位帧后单轮额外写入很小（实测 ~14KB），
 * 缩短周期可以把“重放注入 → 被抹掉”的驻留窗口压到 1 s 以内（T13 要求
 * 每段损坏 ≤3s 恢复；实测 800ms 周期下每段 1–2s 恢复）。 */
#define UI_TTY_REPAIR_PERIOD_MS 800u

/* F1 周期兜底（默认 1500ms）：距上次整屏重绘超过该值且没有真实高风险帧时也补
 * 一轮，避免“完全依赖 burst 证据”导致长期不修复。0 = 只在真实风险时修复
 * （t23 的 A/B 开关，见 IMPLEMENTATION.md §5.11）。 */
#define UI_TTY_REPAIR_BACKSTOP_MS 1500u

/* 修复期主循环输入超时：修复进行中把轮询缩短到 10 ms，让整屏重绘尽快完成。 */
#define UI_TTY_REPAIR_INPUT_TIMEOUT_MS 10

/* 帧耗时超过该值且写入 >= WARN：说明 write() 被 pty 队列阻塞，
 * 即 ll-box 部分写/重放的前置条件已经出现。 */
#define UI_TTY_STALL_FRAME_MS 120u

/* F2 裸模式重置的冷却：模式被毒化无法检测，只能按“出现背压证据”升级。 */
#define UI_TTY_MODE_RESET_COOLDOWN_MS 30000u

/* F3（endwin+refresh）冷却与前提：连续多次 F2 之后仍见背压停滞才启用。
 * 它会闪一次，只做最后兜底。 */
#define UI_TTY_HARD_RESET_COOLDOWN_MS 180000u
#define UI_TTY_HARD_RESET_AFTER_RESETS 3u

/* 升级决策返回值。 */
typedef enum {
    UI_TTY_ESCALATE_NONE = 0,
    UI_TTY_ESCALATE_MODES = 1, /* F2：裸模式重置（约 30 B） */
    UI_TTY_ESCALATE_HARD = 2   /* F3：endwin()+refresh() 兜底（会闪一次） */
} UiTtyEscalation;

/**
 * @brief 单帧写入量分级：0=正常，1=高风险（>= WARN），2=严重（>= SEVERE）。
 */
int ui_tty_guard_classify_burst(unsigned long long bytes);

/**
 * @brief 是否需要开始一轮新的整屏强制重绘。
 *
 * @param risk_pending      上一帧/本节拍出现过高风险 burst（或有强制请求）
 * @param now_ms            当前单调时钟（毫秒）
 * @param last_done_ms      上一轮整屏强制重绘完成时刻
 * @param repair_active     是否已有一轮在进行（进行中不重入）
 */
int ui_tty_guard_should_start_pass(int risk_pending, unsigned long long now_ms,
                                   unsigned long long last_done_ms,
                                   int repair_active);

/**
 * @brief 恢复等级升级决策。
 *
 * 应用侧拿不到“屏幕内容是否已坏”的读数（T5 §2.1），唯一可观测的机制信号
 * 是“write() 被 pty 队列阻塞”：帧耗时 >= UI_TTY_STALL_FRAME_MS 且写入量
 * >= WARN。只有出现这种证据才升级到 F2/F3，避免无证据时周期性重置模式。
 *
 * @param stalled_frames     自上次 F2 以来观测到的“背压停滞帧”数
 * @param now_ms             当前单调时钟（毫秒）
 * @param last_mode_reset_ms 上次 F2 时刻
 * @param last_hard_reset_ms 上次 F3 时刻
 * @param resets_since_hard  上次 F3 以来 F2 的次数
 */
UiTtyEscalation ui_tty_guard_escalate(unsigned long long stalled_frames,
                                      unsigned long long now_ms,
                                      unsigned long long last_mode_reset_ms,
                                      unsigned long long last_hard_reset_ms,
                                      unsigned long long resets_since_hard);

#endif /* UI_TTY_GUARD_POLICY_H */
