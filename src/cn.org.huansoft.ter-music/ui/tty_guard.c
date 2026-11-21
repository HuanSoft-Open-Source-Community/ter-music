/**
 * @file tty_guard.c
 * @brief 终端输出守卫的实现（D1 记账 + F1 整屏强制重绘 + F2/F3 升级恢复）
 *
 * 只由玲珑容器内的 TUI 进程启用。设计边界见 ui/tty_guard.h 与
 * .tmp/uab-ui-fix/ROOTCAUSE.md §2：应用无法阻止 ll-box 重放字节，只能
 * 把“可见污染”的时间窗压到一次全屏重绘之内，并在出现 pty 背压证据时
 * 把终端解析器状态重新拉回已知态。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/tty_guard.h"

#include "cli/cli.h"
#include "logger/logger.h"
#include "ui/menu_anim.h"
#include "ui/rainbow.h"
#include "ui/tty_guard_policy.h"
#include "ui/ui.h"

#include <fcntl.h>
#include <ncursesw/ncurses.h>
#include <term.h>   /* keypad_xmit（smkx）：F2 之后恢复 ncurses keypad 依赖的键盘光标模式 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* F2 裸模式重置：顺序不可换（核心序列见 ui/tty_guard.h 的
 * ui_tty_guard_mode_reset_core()）：
 *   裸 ESC          取消任何悬挂的 ESC/CSI（VTE 收到裸 ESC 回 ground）
 *   ESC[0m          SGR 归零
 *   ESC[39;49m      默认前景/背景（清掉 38;5;N / 48;5;N 残留）
 *   ESC[r           DECSTBM 复位整屏（清掉 [a;br 残留滚动区；会把光标送回 home）
 *   ESC[?6l         DECOM 关（CUP 恢复绝对坐标）
 *   ESC[?7h         DECAWM 开（恢复自动换行）
 *   ESC(B           G0=ASCII（解掉 ACS/制表符映射残留）
 *   ESC[4l          关闭插入模式
 * 末尾**不发 DECCKM 关**：ncurses 的 keypad(TRUE) 只在终端支持 smkx 时开启
 * DECCKM 应用模式 + 键盘传输模式。旧版在这里无条件发 ESC[?1l(rmkx) 会让方向键
 * 退化成 ESC[A，被 getch 当成裸 ESC（t25 回归：设置页按方向键直接退回主界面）。
 * 现在发完核心序列后：有 terminfo smkx（keypad_xmit）则补发；无 smkx
 * （linux/vt220/ansi 等）则什么都不发，保持普通模式。 */

/* 修复带发出后，把“强制校准物理光标”的有效期向后延：一次高风险 burst 之后
 * 的若干帧里，每一帧开头都用绝对 CUP 把终端拉回 ncurses 的 belief，
 * 避免上一帧被重放改写的光标位置让本帧的相对定位继续连错。 */
#define UI_TTY_RESYNC_HOLD_MS 5000u

/* 统计摘要日志节拍（仅 --debug 日志可见）。 */
#define UI_TTY_STATS_LOG_MS 5000u

static int g_enabled;
static int g_io_fd = -1;

static unsigned long long g_burst_warn = UI_TTY_BURST_WARN_BYTES;
static unsigned long long g_repair_min_interval_ms = UI_TTY_REPAIR_MIN_INTERVAL_MS;
static unsigned long long g_mode_reset_cooldown_ms = UI_TTY_MODE_RESET_COOLDOWN_MS;
static unsigned long long g_hard_reset_cooldown_ms = UI_TTY_HARD_RESET_COOLDOWN_MS;

static int g_frame_open;
static unsigned long long g_frame_start_wchar;
static unsigned long long g_frame_start_ms;

static int g_risk_pending;
static int g_force_repair;
static int g_repair_active;
static int g_repair_due;
/* 本帧由 F1 修复 flush 自己写掉的字节：close_frame 必须扣除它再做风险分级，
 * 否则修复帧自身 ~31KB 会被当成“高风险 burst” → 重新置 risk → 又触发下一次
 * 修复（自我续命，t19 实测 F1 340 次/300s）。（t23 修复） */
static unsigned long long g_frame_repair_bytes;
static int g_stall_repair;   /* 停滞帧 → 下一帧定向修复（t18） */

/* F2（裸序列重置）与 F3（endwin+refresh 兜底）开关。
 * F3 默认**关闭**：t18 实测它在触发后改变损伤体制（见 IMPLEMENTATION.md §5.8），
 * 保留 TER_MUSIC_TTY_HARD_RESET=1 以备将来定位“模式被永久毒化”的场景。 */
static int g_modes_enabled = 1;
static int g_hard_reset_enabled = 0;
static unsigned long long g_repair_wchar_start;
static unsigned long long g_last_pass_done_ms;
static unsigned long long g_resync_until_ms;
static unsigned long long g_stats_log_at_ms;

static unsigned long long g_last_mode_reset_ms;
static unsigned long long g_last_hard_reset_ms;
static unsigned long long g_resets_since_hard;
static unsigned long long g_stalled_since_reset;

static UiTtyGuardStats g_stats;

static unsigned long long tty_guard_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

/* /proc/self/io 的 wchar：本进程写入字符设备/管道/pty 的字节数（内核累计）。
 * 日志走文件不计入 wchar，因此它恰好等于“真正发到终端的字节量”。 */
static int tty_guard_read_wchar(unsigned long long *out)
{
    if (g_io_fd < 0 || !out) {
        return -1;
    }
    char buf[256];
    ssize_t n = pread(g_io_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    const char *key = strstr(buf, "wchar:");
    if (!key) {
        return -1;
    }
    unsigned long long value = 0;
    if (sscanf(key + 6, "%llu", &value) != 1) {
        return -1;
    }
    *out = value;
    return 0;
}

static int tty_guard_env_ull(const char *name, unsigned long long fallback)
{
    const char *raw = getenv(name);
    if (!raw || raw[0] == '\0') {
        return (int)fallback;
    }
    char *end = NULL;
    unsigned long long value = strtoull(raw, &end, 10);
    if (end == raw || value == 0) {
        return (int)fallback;
    }
    return (int)value;
}

/* 把物理光标（以及 ncurses 的 belief）对齐到 (y, x)：mvcur 传 -1 起点时
 * 走绝对 CUP，并更新 SP->_cursrow/_curscol，之后 ncurses 的定位才是可信的。 */
static void tty_guard_sync_cursor_to(int y, int x)
{
    if (y < 0 || x < 0 || y >= LINES || x >= COLS) {
        return;
    }
    (void)mvcur(-1, -1, y, x);
}

void ui_tty_guard_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_enabled = 0;
    g_io_fd = -1;

    const char *guard_env = getenv("TER_MUSIC_TTY_GUARD");
    if (guard_env && guard_env[0] != '\0' && strcmp(guard_env, "0") == 0) {
        log_info("tty_guard", "disabled by TER_MUSIC_TTY_GUARD=0");
        return;
    }

    if (!cli_in_sandbox()) {
        /* 直跑路径（AppImage/deb）不产生任何额外输出，也不改变任何行为。 */
        return;
    }
    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) {
        return;
    }

    g_burst_warn = (unsigned long long)tty_guard_env_ull(
        "TER_MUSIC_TTY_BURST_BYTES", UI_TTY_BURST_WARN_BYTES);
    if (g_burst_warn < 256) {
        g_burst_warn = 256;
    }
    g_repair_min_interval_ms = (unsigned long long)tty_guard_env_ull(
        "TER_MUSIC_TTY_REPAIR_MS", UI_TTY_REPAIR_MIN_INTERVAL_MS);
    if (g_repair_min_interval_ms < 200) {
        g_repair_min_interval_ms = 200;
    }
    const char *env_modes = getenv("TER_MUSIC_TTY_MODES");
    if (env_modes && env_modes[0] != '\0') {
        g_modes_enabled = (strcmp(env_modes, "0") != 0);
    }
    const char *env_hard = getenv("TER_MUSIC_TTY_HARD_RESET");
    if (env_hard && env_hard[0] != '\0') {
        g_hard_reset_enabled = (strcmp(env_hard, "0") != 0);
    }

    g_io_fd = open("/proc/self/io", O_RDONLY | O_CLOEXEC);
    if (g_io_fd < 0) {
        log_warn("tty_guard", "disabled: /proc/self/io unavailable");
        return;
    }

    unsigned long long wchar_now = 0;
    if (tty_guard_read_wchar(&wchar_now) != 0) {
        log_warn("tty_guard", "disabled: cannot parse wchar from /proc/self/io");
        close(g_io_fd);
        g_io_fd = -1;
        return;
    }

    g_enabled = 1;
    unsigned long long now = tty_guard_now_ms();
    g_last_pass_done_ms = now - g_repair_min_interval_ms; /* 首次风险即可开修复 */
    g_last_mode_reset_ms = now - g_mode_reset_cooldown_ms;
    g_last_hard_reset_ms = now;
    g_stats_log_at_ms = now + UI_TTY_STATS_LOG_MS;
    g_resets_since_hard = 0; /* F3 的前提是“已经用过 >=3 次 F2”，从 0 起算 */

    log_info("tty_guard",
             "enabled: wchar=%llu burst_warn=%llu repair_min_interval=%llums "
             "band_budget=%uB",
             (unsigned long long)wchar_now, (unsigned long long)g_burst_warn,
             (unsigned long long)g_repair_min_interval_ms,
             (unsigned)UI_TTY_BAND_BUDGET_BYTES);
}

int ui_tty_guard_repair_active(void)
{
    return g_repair_active;
}

void ui_tty_guard_request_repair(void)
{
    if (!g_enabled) {
        return;
    }
    /* 允许下一拍就开新一轮：把“上轮完成时刻”回退一个最小间隔。 */
    unsigned long long now = tty_guard_now_ms();
    if (now >= g_repair_min_interval_ms) {
        g_last_pass_done_ms = now - g_repair_min_interval_ms;
    }
    g_force_repair = 1;
    g_risk_pending = 1;
}

void ui_tty_guard_log_stats(void)
{
    if (!g_enabled) {
        return;
    }
    log_info("tty_guard",
             "stats: frames=%llu bytes=%llu max_burst=%llu risky=%llu severe=%llu "
             "stalled=%llu passes=%llu repair_bytes=%llu "
             "max_repair_write=%llu F2=%llu F3=%llu",
             g_stats.frames, g_stats.total_bytes, g_stats.max_burst,
             g_stats.risky_frames, g_stats.severe_frames,
             g_stats.stalled_frames, g_stats.repair_passes,
             g_stats.repair_bytes,
             g_stats.repair_max_band, g_stats.mode_resets, g_stats.hard_resets);
}

/* ── F1：整屏强制重绘（并进下一次帧合成） ───────────────────────── */

/*
 * 为什么是“整屏一次”而不是“分带多次”（实现记录，勿回退）：
 *
 * ncurses 的差量刷新只写 newscr 与 curscr（物理屏的内存影像）不同的格子；
 * 终端被 ll-box 重放污染后 curscr 仍“看起来对”，必须让物理屏 belief 作废
 * 才能重画。公开 API 里能作废 belief 的只有两种：
 *   1) clearok(curscr) → doupdate 走 ClrUpdate：逐行 ClrToEOL+PutRange 重画，
 *      不经过滚动优化器，也不会产生任何“定位技巧”；
 *   2) wredrawln()/redrawwin() → 把 curscr 整行清零 + 重算行哈希。
 * 实测 (2) 会触发滚动优化器 _nc_scroll_optimize 的行移动误判：清零行与
 * newscr 里的空白行哈希相同，优化器会发出 ins/del/滚屏，把整屏整体错位
 * （guard 开启后 wrap=51、物理滚屏=44、边框/歌词错位）；用“只改 curscr
 * 第 0 列哨兵格”绕过哈希也不行（同样产生 wrap=51/滚屏=44）。因此 F1 只用
 * clearok 的 ClrUpdate 路径，整屏一次完成。
 *
 * 自激控制：
 *   - 只在“出现高风险 burst 证据”时启动（D1），最小间隔 UI_TTY_REPAIR_MIN_INTERVAL_MS；
 *   - 修复不单独 doupdate，而是并进下一次相位帧（take_repair_due），
 *     一次 flush 同时完成“相位重绘 + 整屏强制重绘”，不产生第二个大 burst；
 *   - 菜单动画帧由动画模块整帧合成，不插队。
 */
static void tty_guard_note_repair_write(unsigned long long written)
{
    g_repair_active = 0;
    g_risk_pending = 0;
    g_force_repair = 0;
    g_last_pass_done_ms = tty_guard_now_ms();
    g_stats.repair_passes++;
    g_stats.repair_bytes += written;
    if (written > g_stats.repair_max_band) {
        g_stats.repair_max_band = written;
    }
    log_debug("tty_guard",
              "L1 full repaint #%llu done: wrote=%lluB (exempted from risk) "
              "frames=%llu bytes=%llu max_burst=%llu",
              g_stats.repair_passes, written, g_stats.frames,
              g_stats.total_bytes, g_stats.max_burst);
    /* A6 可观测性：分带方案在 ncurses 6.5 上不可用（见上），单轮写入必然
     * 超过 4 KiB 分带预算。这里把偏差写进日志，便于验收核对。 */
    if (written > (unsigned long long)UI_TTY_BAND_BUDGET_BYTES) {
        log_debug("tty_guard",
                  "A6 deviation: single full repaint wrote %lluB > %uB band budget "
                  "(banded forcing is unsafe on ncurses 6.5)",
                  written, (unsigned)UI_TTY_BAND_BUDGET_BYTES);
    }
}

int ui_tty_guard_wchar_now(unsigned long long *out)
{
    if (!g_enabled || g_io_fd < 0 || !out) {
        return -1;
    }
    return tty_guard_read_wchar(out);
}

int ui_tty_guard_take_repair_due(void)
{
    if (!g_enabled || !g_repair_due) {
        return 0;
    }
    g_repair_due = 0;
    g_repair_active = 1;
    g_frame_repair_bytes = 0;
    unsigned long long before = 0;
    (void)tty_guard_read_wchar(&before);
    g_repair_wchar_start = before;
    return 1;
}

void ui_tty_guard_repair_flushed(void)
{
    if (!g_repair_active) {
        return;
    }
    unsigned long long after = 0;
    (void)tty_guard_read_wchar(&after);
    unsigned long long written =
        (after >= g_repair_wchar_start) ? (after - g_repair_wchar_start) : 0;
    g_frame_repair_bytes += written;
    tty_guard_note_repair_write(written);
}

/* ── F2/F3 ────────────────────────────────────────────────────────── */

void ui_tty_guard_reset_terminal_modes(void)
{
    if (!g_enabled || !g_modes_enabled) {
        return;
    }
    unsigned long long now = tty_guard_now_ms();
    if (now >= g_last_mode_reset_ms &&
        (now - g_last_mode_reset_ms) < g_mode_reset_cooldown_ms) {
        return;
    }

    int cursor_y = 0;
    int cursor_x = 0;
    getyx(curscr, cursor_y, cursor_x);

    /* 先把物理光标挪到行首：裸 ESC 不能落在“待换行（pending wrap）”的最后一格上
     * （终端会把它当控制序列起始，但部分回放器/终端实现会按可打印字符处理，
     * 产生一次多余换行→底行滚屏）。行首位置不在任何 pending-wrap 状态。 */
    tty_guard_sync_cursor_to(cursor_y, 0);
    (void)putp(ui_tty_guard_mode_reset_core());
    /* 恢复终端自己开启过的键盘传输/光标模式（terminfo smkx）。
     * 无 keypad_xmit 的终端（linux/vt220 等）本来就没开过，这里什么都不发；
     * 硬编码 xterm 的 ESC[?1h 会在那类终端制造新的方向键回归（t31）。 */
    int keypad_absent = !(keypad_xmit && keypad_xmit[0] != '\0');
    const char *keypad_restore = ui_tty_guard_keypad_restore_seq(keypad_xmit);
    if (keypad_restore[0] != '\0') {
        (void)putp(keypad_restore);
    }
    /* 裸序列之后立刻用绝对 CUP 把物理光标/属性 belief 校准回 ncurses；
     * 否则 ESC[r 把光标送回 home，而 curses 仍按旧位置做相对定位。 */
    tty_guard_sync_cursor_to(cursor_y, cursor_x);

    g_last_mode_reset_ms = now;
    g_stats.mode_resets++;
    g_resets_since_hard++;
    g_stalled_since_reset = 0;
    log_info("tty_guard",
             "F2 mode reset #%llu: ESC/SGR/bg/DECSTBM/DECOM/DECAWM/G0/insert "
             "+ keypad restore: %s",
             g_stats.mode_resets,
             keypad_absent ? "skipped (no terminfo smkx)" : "terminfo keypad_xmit");
}

static void tty_guard_hard_reset(void)
{
    unsigned long long now = tty_guard_now_ms();
    if (now >= g_last_hard_reset_ms &&
        (now - g_last_hard_reset_ms) < g_hard_reset_cooldown_ms) {
        return;
    }

    log_info("tty_guard",
             "F3 fallback: endwin()+refresh() (forced mode re-emit; may flash once)");
    endwin();
    (void)refresh();

    g_stats.hard_resets++;
    g_last_hard_reset_ms = now;
    g_resets_since_hard = 0;
    g_stalled_since_reset = 0;
    /* refresh() 自己整屏重绘；把下一轮重绘排到一个最小间隔之后，避免同一拍叠加。 */
    g_repair_active = 0;
    g_repair_due = 0;
    g_risk_pending = 0;
    g_force_repair = 0;
    g_last_pass_done_ms = now;
}

/* ── D1：帧记账 ───────────────────────────────────────────────────── */

static void tty_guard_close_frame(unsigned long long now)
{
    unsigned long long wchar_now = 0;
    if (tty_guard_read_wchar(&wchar_now) != 0) {
        return;
    }

    unsigned long long delta = 0;
    if (wchar_now >= g_frame_start_wchar) {
        delta = wchar_now - g_frame_start_wchar;
    }
    /* 扣掉本帧 F1 修复 flush 自己写的字节，只对“应用自身输出”做风险分级，
     * 否则修复帧会把 risk 重新点亮，形成每 800ms 一次的自我续命修复。 */
    unsigned long long repair_bytes = g_frame_repair_bytes;
    g_frame_repair_bytes = 0;
    unsigned long long net = (delta >= repair_bytes) ? (delta - repair_bytes) : 0;
    if (repair_bytes > 0) {
        log_debug("tty_guard",
                  "repair frame exempted: wrote=%lluB repair=%lluB net=%lluB",
                  delta, repair_bytes, net);
    }
    unsigned long long duration = (now >= g_frame_start_ms) ? (now - g_frame_start_ms) : 0;

    g_stats.frames++;
    g_stats.total_bytes += delta;
    if (delta > g_stats.max_burst) {
        g_stats.max_burst = delta;
    }

    int level = ui_tty_guard_classify_burst(net);
    if (net >= g_burst_warn) {
        g_stats.risky_frames++;
        g_risk_pending = 1;
        g_resync_until_ms = now + UI_TTY_RESYNC_HOLD_MS;
    }
    if (level >= 2) {
        g_stats.severe_frames++;
    }

    /* 背压证据：写入不小、但这一帧耗时远超预算 ⇒ write() 被 pty 队列阻塞，
     * 正是 ll-box 可能发生部分写/重放的前置条件。 */
    if (net >= g_burst_warn && duration >= (unsigned long long)UI_TTY_STALL_FRAME_MS) {
        g_stats.stalled_frames++;
        g_stalled_since_reset++;
        /* 背压停滞是“可能已发生部分写/重放”的最强证据：立刻安排一次定向修复，
         * 而不是等最小间隔到点（最多 UI_TTY_REPAIR_MIN_INTERVAL_MS = 800ms）。 */
        g_stall_repair = 1;
        g_risk_pending = 1;
        g_resync_until_ms = now + UI_TTY_RESYNC_HOLD_MS;
        log_debug("tty_guard",
                  "backpressure frame: %llu B (net) in %llums (>= %ums)",
                  net, duration, (unsigned)UI_TTY_STALL_FRAME_MS);

        UiTtyEscalation esc = ui_tty_guard_escalate(
            g_stalled_since_reset, now, g_last_mode_reset_ms,
            g_last_hard_reset_ms, g_resets_since_hard);
        if (esc == UI_TTY_ESCALATE_HARD && g_hard_reset_enabled) {
            tty_guard_hard_reset();
        } else if (esc == UI_TTY_ESCALATE_HARD || esc == UI_TTY_ESCALATE_MODES) {
            /* F3 关闭时降级为 F2（裸序列重置），不执行 endwin+refresh。 */
            ui_tty_guard_reset_terminal_modes();
        }
    }

    if (now >= g_stats_log_at_ms) {
        g_stats_log_at_ms = now + UI_TTY_STATS_LOG_MS;
        log_debug("tty_guard",
                  "burst summary: frames=%llu bytes=%llu max=%llu risky=%llu "
                  "severe=%llu stalled=%llu passes=%llu max_band=%lluB",
                  g_stats.frames, g_stats.total_bytes, g_stats.max_burst,
                  g_stats.risky_frames, g_stats.severe_frames,
                  g_stats.stalled_frames, g_stats.repair_passes,
                  g_stats.repair_max_band);
    }
}

void ui_tty_guard_frame_begin(void)
{
    if (!g_enabled) {
        return;
    }
    unsigned long long now = tty_guard_now_ms();

    if (g_frame_open) {
        tty_guard_close_frame(now);
    }

    /* 谐振期（上一次高风险 burst 之后 5 s）内，每帧开头都把物理光标钉回
     * ncurses 的 belief：被重放污染过的帧不会再让下一帧的相对定位连错。 */
    if (now < g_resync_until_ms) {
        int cursor_y = 0;
        int cursor_x = 0;
        getyx(curscr, cursor_y, cursor_x);
        tty_guard_sync_cursor_to(cursor_y, cursor_x);
    }

    g_frame_start_ms = tty_guard_now_ms();
    g_frame_open = tty_guard_read_wchar(&g_frame_start_wchar) == 0;
}

void ui_tty_guard_tick(void)
{
    if (!g_enabled) {
        return;
    }
    unsigned long long now = tty_guard_now_ms();

    if (g_repair_active || g_repair_due) {
        return;
    }
    /* 流光激活时**也**要做 F1：R1 让“只有空格的行”不再随相位重绘，
     * 一旦这类行被 ll-box 重放污染，模型没有变化 ⇒ ncurses 永远不重写它，
     * 污染会一直留在屏上（实测：R1 之后 180s relay 会话终屏 53 处框线违规）。
     * F1 的整屏强制重绘只补这一块，单轮写入见日志（远小于相位帧）。 */
    /* 菜单动画帧由动画模块整帧合成，不插队。 */
    if (ui_menu_anim_active()) {
        return;
    }
    if (g_stall_repair) {
        g_stall_repair = 0;
        g_repair_due = 1;
        log_debug("tty_guard", "L1 full repaint queued (post-stall, urgent)");
        return;
    }
    /* 只认证据：帧记账发现的高风险 burst（risk）或显式请求（resize 重建）。
     * 没有任何计时器兜底——稳定会话不会因“时间到了”而整屏重绘。 */
    int request = g_risk_pending || g_force_repair;
    if (ui_tty_guard_should_start_pass(request, now, g_last_pass_done_ms,
                                       g_repair_active)) {
        /* 只排期：真正的 clearok+flush 并进下一帧（由 ui_rainbow_paint 在
         * 帧合成里消费），避免同一拍出现两个大 burst。 */
        g_repair_due = 1;
        log_debug("tty_guard", "L1 full repaint queued (risk=%d force=%d)",
                  g_risk_pending, g_force_repair);
    }
}
