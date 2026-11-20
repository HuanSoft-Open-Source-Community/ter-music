/**
 * @file tty_guard.h
 * @brief 终端输出守卫：高风险 burst 记账 + 整屏强制重绘 + 模式重置（容器内）
 *
 * 针对 ll-box 冷启动转发“部分写未消费 → 旧窗口重放”把重复字节拼进终端流
 * 的问题（.tmp/uab-ui-fix/ROOTCAUSE.md），应用侧只能做三件事：
 *   D1 检测：/proc/self/io 的 `wchar` 差值 = 本帧真正写进终端的字节数，
 *            按 UI_TTY_BURST_WARN_BYTES 分级记账（不使用已证伪的
 *            nc_set_term_driver）。
 *   F1 恢复：整屏强制重绘。`clearok(curscr)` 让 doupdate 走 ClrUpdate，
 *            强制重画字形（不只是 SGR）；并与下一次相位帧合并成一次 flush。
 *   F2/F3：裸模式重置 / endwin+refresh 兜底，只在出现 pty 背压证据时升级。
 *
 * 全部逻辑只在 `cli_in_sandbox()` 为真（玲珑容器内）时启用；直跑
 * （AppImage/deb）路径不产生任何额外输出。
 *
 * 环境变量（均可选，默认即生产值）：
 *   TER_MUSIC_TTY_GUARD=0          关闭整个守卫（默认开启，仅容器内生效）
 *   TER_MUSIC_TTY_BURST_BYTES=N    高风险 burst 阈值（默认 4096）
 *   TER_MUSIC_TTY_REPAIR_MS=N      整屏强制重绘周期（默认 800）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_TTY_GUARD_H
#define UI_TTY_GUARD_H

#include "ui/tty_guard_policy.h"

/* 运行期统计（日志与验收证据用；字段均为“本次进程累计”）。 */
typedef struct {
    unsigned long long frames;              /* 记账帧数 */
    unsigned long long risky_frames;        /* 写入 >= WARN 的帧数 */
    unsigned long long severe_frames;       /* 写入 >= SEVERE 的帧数 */
    unsigned long long stalled_frames;      /* 写入 >= WARN 且帧耗时 >= STALL 的帧数 */
    unsigned long long total_bytes;         /* 全部帧的 wchar 增量之和 */
    unsigned long long max_burst;           /* 单帧最大写入 */
    unsigned long long repair_passes;       /* 完成的整屏强制重绘轮数 */
    unsigned long long repair_max_band;     /* 单带最大写入（A6 应 <= 4 KiB） */
    unsigned long long repair_bytes;        /* 修复带累计写入 */
    unsigned long long mode_resets;         /* F2 次数 */
    unsigned long long hard_resets;         /* F3 次数 */
} UiTtyGuardStats;

/** @brief 初始化（必须在 initscr 之后调用一次）。非容器内自动禁用。 */
void ui_tty_guard_init(void);

/**
 * @brief 每轮主循环开头调用：结算上一帧的 wchar 增量，并开启新一帧。
 *
 * 帧边界用“连续两次调用之间”定义，所以主循环里任何 `continue` 都不会
 * 漏记账；代价是最后一帧在退出时不被统计（无所谓）。
 */
void ui_tty_guard_frame_begin(void);

/** @brief 每轮主循环末尾（呈现之后）调用：必要时安排一轮修复。 */
void ui_tty_guard_tick(void);

/**
 * @brief 取出“本轮需要整屏强制重绘”的请求（取走即清除）。
 *
 * 修复不单独 doupdate，而是**并进下一次帧合成**：调用方在 compose 之后、
 * flush 之前对 curscr 置 clearok，一次 doupdate 同时完成“相位重绘 + 全屏强制
 * 重绘”，避免同一拍出现两个大 burst（自激）。
 *
 * @return 1 = 本次 flush 需要走 ClrUpdate；0 = 普通帧
 */
int ui_tty_guard_take_repair_due(void);

/** @brief 与 take_repair_due 配对：在带 clearok 的那次 flush 之后调用，结算本轮修复。 */
void ui_tty_guard_repair_flushed(void);

/**
 * @brief 读取当前 /proc/self/io 的 wchar 累计值（字节）。
 *
 * R3 分带绘制用它度量“这一帧到底往终端写了多少”，从而自适应带高、
 * 保证单帧写入不超过预算。未启用守卫时返回失败。
 *
 * @param out 输出 wchar 累计值
 * @return 0 成功，-1 不可用
 */
int ui_tty_guard_wchar_now(unsigned long long *out);

/** @brief 是否有一轮整屏强制重绘正在进行（主循环据此缩短输入超时）。 */
int ui_tty_guard_repair_active(void);

/** @brief 请求尽快开始一轮整屏强制重绘（resize 重建后调用）。 */
void ui_tty_guard_request_repair(void);

/**
 * @brief F2：裸模式重置（约 30 B），把终端解析器拉回 ncurses 的默认 belief。
 *
 * 调用受冷却限制（UI_TTY_MODE_RESET_COOLDOWN_MS，内部提前返回）。
 * 必须保证 ncurses 的输出已排空（调用点在帧边界，doupdate 已 flush）。
 */
void ui_tty_guard_reset_terminal_modes(void);

/** @brief 把统计写进调试日志（退出前和周期性摘要使用）。 */
void ui_tty_guard_log_stats(void);

#endif /* UI_TTY_GUARD_H */
