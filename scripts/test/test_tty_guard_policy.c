/**
 * @file test_tty_guard_policy.c
 * @brief ui/tty_guard_policy.c 的单元测试（纯决策逻辑，不依赖 ncurses）
 *
 * 覆盖：burst 分级、整屏重绘起始条件、F2/F3 升级冷却，以及 **F2 模式重置
 * 序列**（t25：末尾必须恢复 keypad/smkx，绝不能发 ESC[?1l 把 DECCKM 打回
 * 普通模式，否则方向键退化为 ESC[A → 被 getch 当成裸 ESC）。
 * 终端输出守卫的其余部分是 ncurses/容器相关的副作用代码，由 harness 真机
 * 验证覆盖，不在单测里伪造。
 *
 * 运行：scripts/test/run-unit-tests.sh test_tty_guard_policy
 * 依赖（见同名 .srcs）：ui/tty_guard_policy.c
 */

#include "ui/tty_guard.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                            \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_failures++;                                            \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

/* 每格 16 B + 每行 64 B 开销的保守估计：预算/估计 >= 行数。 */
static void test_classify_burst(void)
{
    CHECK(ui_tty_guard_classify_burst(0) == 0, "0 B normal");
    CHECK(ui_tty_guard_classify_burst(UI_TTY_BURST_WARN_BYTES - 1) == 0,
          "warn-1 normal");
    CHECK(ui_tty_guard_classify_burst(UI_TTY_BURST_WARN_BYTES) == 1,
          "warn risky");
    CHECK(ui_tty_guard_classify_burst(UI_TTY_BURST_SEVERE_BYTES - 1) == 1,
          "severe-1 risky");
    CHECK(ui_tty_guard_classify_burst(UI_TTY_BURST_SEVERE_BYTES) == 2,
          "severe");
    CHECK(ui_tty_guard_classify_burst(1ULL << 40) == 2, "huge severe");
}

/* 删除 F1 周期兜底后的语义锁（原兜底无证据也每 1.5s 整屏重绘一次）：
 * 稳定会话即使跑满 10 分钟、按主循环 40ms 步进，也不得因“时间到了”启动
 * 任何整屏重绘。这是**policy 层的防回归语义锁**：它在被删代码仍存在时同样
 * 通过（旧实现的兜底在有状态 tick 里，纯函数碰不到），因此**不能**用它证明
 * 删除生效；删除生效的反向验证见 t3 的有状态探针（旧 due=395/600s → 新 0）
 * 与 grep/strings 证据。 */
static void test_no_evidence_never_repaints(void)
{
    const unsigned long long step = 40; /* UI_INPUT_TIMEOUT_MS 空闲节拍 */
    const unsigned long long span = 10ULL * 60ULL * 1000ULL;
    unsigned long long last_done = 0;
    int passes = 0;

    for (unsigned long long now = 0; now <= span; now += step) {
        if (ui_tty_guard_should_start_pass(0, now, last_done, 0)) {
            passes++;
        }
    }
    CHECK(passes == 0, "no evidence -> 0 passes in 10 min");
}

/* 证据驱动方向：出现高风险 burst 证据后，最小间隔到达的第一拍就排期；
 * 未到最小间隔时仍受速率限制（速率限制不得因此放宽）。 */
static void test_evidence_triggers_repaint(void)
{
    const unsigned long long period = UI_TTY_REPAIR_PERIOD_MS;
    const unsigned long long now = 10ULL * 60ULL * 1000ULL;

    CHECK(ui_tty_guard_should_start_pass(0, now, 0, 0) == 0,
          "10 min with no evidence -> still no pass");
    CHECK(ui_tty_guard_should_start_pass(1, now, now - period, 0) == 1,
          "evidence + min interval elapsed -> pass");
    CHECK(ui_tty_guard_should_start_pass(1, now, now - 1, 0) == 0,
          "evidence but min interval not elapsed -> wait");
}

static void test_should_start_pass(void)
{
    const unsigned long long period = UI_TTY_REPAIR_PERIOD_MS;
    CHECK(ui_tty_guard_should_start_pass(0, 100000, 0, 0) == 0,
          "no risk -> no pass");
    CHECK(ui_tty_guard_should_start_pass(1, 0, 0, 1) == 0,
          "already repairing -> no reentry");
    CHECK(ui_tty_guard_should_start_pass(1, period - 1, 0, 0) == 0,
          "period not reached");
    CHECK(ui_tty_guard_should_start_pass(1, period, 0, 0) == 1,
          "period reached -> pass");
    CHECK(ui_tty_guard_should_start_pass(1, 50000, 50000 - period, 0) == 1,
          "last pass one period ago -> pass");
    CHECK(ui_tty_guard_should_start_pass(1, 100, 200, 0) == 0,
          "clock went backwards -> wait");
}

static void test_escalate(void)
{
    const unsigned long long now = 1000000ULL;
    const unsigned long long cooldown = UI_TTY_MODE_RESET_COOLDOWN_MS;
    const unsigned long long hard_cd = UI_TTY_HARD_RESET_COOLDOWN_MS;

    CHECK(ui_tty_guard_escalate(0, now, 0, 0, 0) == UI_TTY_ESCALATE_NONE,
          "no stall -> none");
    CHECK(ui_tty_guard_escalate(1, now, now - cooldown, now - hard_cd, 0) ==
              UI_TTY_ESCALATE_MODES,
          "1 stall, cooldown elapsed -> F2");
    CHECK(ui_tty_guard_escalate(1, now, now - 10, now - hard_cd, 0) ==
              UI_TTY_ESCALATE_NONE,
          "1 stall, F2 cooling down -> none");
    CHECK(ui_tty_guard_escalate(2, now, now - cooldown, now - 10, 1) ==
              UI_TTY_ESCALATE_MODES,
          "stalls but F3 not armed -> F2");
    /* F3 前提：>=3 次 F2、>=3 个停滞帧、距上次 F3 超过冷却。 */
    CHECK(ui_tty_guard_escalate(3, now, now - cooldown, now - hard_cd, 3) ==
              UI_TTY_ESCALATE_HARD,
          "3 stalls + 3 resets + cooldown -> F3");
    /* F3 冷却中时退回到更便宜的 F2（不能连发 endwin/refresh）。 */
    CHECK(ui_tty_guard_escalate(3, now, now - cooldown, now - 10, 3) ==
              UI_TTY_ESCALATE_MODES,
          "F3 cooling down -> fall back to F2");
    CHECK(ui_tty_guard_escalate(3, now, now - cooldown, now - hard_cd, 2) ==
              UI_TTY_ESCALATE_MODES,
          "only 2 F2 -> still F2, not F3");
}

/* t25：F2 序列必须恢复 keypad(smkx)，不得把 DECCKM 打回普通模式。 */
static void test_f2_mode_reset_sequence(void)
{
    const char *core = ui_tty_guard_mode_reset_core();
    CHECK(core != NULL && core[0] == '\033', "core starts with bare ESC");
    CHECK(strstr(core, "\033[?1l") == NULL,
          "core must NOT turn DECCKM off (rmkx)");
    CHECK(strstr(core, "\033[?1h") == NULL,
          "core does not carry keypad restore (caller appends smkx)");
    /* 其余语义保持：SGR0 / 默认色 / DECSTBM / DECOM / DECAWM / G0 / insert */
    CHECK(strstr(core, "\033[0m") != NULL, "core has SGR reset");
    CHECK(strstr(core, "\033[39;49m") != NULL, "core has default fg/bg");
    CHECK(strstr(core, "\033[r") != NULL, "core resets DECSTBM");
    CHECK(strstr(core, "\033[?6l") != NULL, "core disables DECOM");
    CHECK(strstr(core, "\033[?7h") != NULL, "core enables DECAWM");
    CHECK(strstr(core, "\033(B") != NULL, "core resets G0=ASCII");
    CHECK(strstr(core, "\033[4l") != NULL, "core disables insert mode");
    CHECK(strstr(core, "\033[0m") < strstr(core, "\033[4l"),
          "core keeps SGR before insert-mode reset");
}

static void test_f2_keypad_restore(void)
{
    /* 无 terminfo smkx 的终端（linux/vt220/ansi/sun）：不发明 xterm smkx，
     * 返回空串 = 调用方什么都不发（这些终端本来就没开过键盘传输/DECCKM）。 */
    const char *absent = ui_tty_guard_keypad_restore_seq(NULL);
    CHECK(absent != NULL, "NULL keypad_xmit -> non-NULL (empty) string");
    CHECK(absent[0] == '\0', "NULL keypad_xmit -> empty (nothing to restore)");
    CHECK(ui_tty_guard_keypad_restore_seq("")[0] == '\0',
          "empty keypad_xmit -> empty (nothing to restore)");
    /* 终端自带 keypad_xmit 逐字节使用（xterm/screen 等） */
    static const char own[] = "\033[?1h\033=";
    CHECK(ui_tty_guard_keypad_restore_seq(own) == own,
          "terminfo keypad_xmit used verbatim");
    static const char other[] = "\033[?1h\033=Z";
    CHECK(strcmp(ui_tty_guard_keypad_restore_seq(other), other) == 0,
          "non-empty custom keypad_xmit returned as-is");

    /* 组合（有 smkx）：核心 + 恢复，DECCKM 最终是“应用模式”，全程不出现 ?1l */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s%s", ui_tty_guard_mode_reset_core(), own);
    CHECK(strstr(buf, "?1l") == NULL, "F2 full sequence never sends rmkx");
    CHECK(strstr(buf, "?1h") != NULL, "F2 with smkx enables DECCKM (app mode)");
    size_t len = strlen(buf);
    CHECK(len >= 3 && buf[len - 1] == '=', "F2 with smkx ends with ESC=");

    /* 组合（无 smkx）：不得凭空补 ?1h，也不得发 ?1l —— 与 PRE-FIX 的安全行为一致 */
    snprintf(buf, sizeof(buf), "%s%s", ui_tty_guard_mode_reset_core(),
             ui_tty_guard_keypad_restore_seq(NULL));
    CHECK(strstr(buf, "?1h") == NULL, "F2 without smkx must NOT invent DECCKM on");
    CHECK(strstr(buf, "?1l") == NULL, "F2 without smkx must NOT send rmkx either");
    CHECK(strlen(buf) == strlen(ui_tty_guard_mode_reset_core()),
          "F2 without smkx appends nothing");
}

int main(void)
{
    test_classify_burst();
    test_should_start_pass();
    test_no_evidence_never_repaints();
    test_evidence_triggers_repaint();
    test_escalate();
    test_f2_mode_reset_sequence();
    test_f2_keypad_restore();

    if (g_failures == 0) {
        printf("tty_guard_policy: %d checks passed\n", g_checks);
        return 0;
    }
    printf("tty_guard_policy: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
