/**
 * @file test_tty_guard_policy.c
 * @brief ui/tty_guard_policy.c 的单元测试（纯决策逻辑，不依赖 ncurses）
 *
 * 覆盖：分带行数预算（A6：每带 <= 4 KiB）、burst 分级、L1 起始条件、
 * F2/F3 升级冷却。终端输出守卫的其余部分是 ncurses/容器相关的副作用代码，
 * 由 harness 真机验证覆盖，不在单测里伪造。
 *
 * 运行：scripts/test/run-unit-tests.sh test_tty_guard_policy
 * 依赖（见同名 .srcs）：ui/tty_guard_policy.c
 */

#include "ui/tty_guard_policy.h"

#include <stdio.h>

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

int main(void)
{
    test_classify_burst();
    test_should_start_pass();
    test_no_evidence_never_repaints();
    test_evidence_triggers_repaint();
    test_escalate();

    if (g_failures == 0) {
        printf("tty_guard_policy: %d checks passed\n", g_checks);
        return 0;
    }
    printf("tty_guard_policy: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
