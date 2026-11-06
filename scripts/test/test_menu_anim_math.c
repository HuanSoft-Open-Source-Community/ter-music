/**
 * @file test_menu_anim_math.c
 * @brief ui/menu_anim_math.c 的单元测试：菜单动画的起止几何、缓动与降级
 *
 * 覆盖：
 *   - 下拉（纵向）完整/削弱档的起点回退、贴顶回退换向、无可用位移；
 *   - 右键（对角）完整/削弱档的纵横向回退、贴左上角换向、窗口过大时不动；
 *   - 两种风格 × 两档 × 入场/消失的时长表；
 *   - 缓动的端点、单调性与 ease-in/ease-out 的中间点方向；
 *   - 按时间取位置：0、半程、超时、零时长；
 *   - 帧耗时降级：25ms/55ms 阈值、只降不升、OFF 恒 OFF。
 *
 * 运行：scripts/test/run-unit-tests.sh test_menu_anim_math
 * 依赖（见同名 .srcs）：ui/menu_anim_math.c
 */

#include "ui/menu_anim_math.h"

#include <stdio.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static UIRect R(int top, int left, int rows, int cols)
{
    UIRect r;
    r.top = top;
    r.left = left;
    r.rows = rows;
    r.cols = cols;
    return r;
}

static int plan_at_rest(const UIMenuAnimPlan *p)
{
    return p->from_top == p->to_top && p->from_left == p->to_left;
}

/* ── 纵向（下拉） ─────────────────────────────────────────────── */

static void test_plan_vertical(void)
{
    UIRect final = R(5, 10, 8, 20);
    UIMenuAnimPlan p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL,
                                         UI_MENU_ANIM_TIER_FULL, final, 30, 100);
    CHECK(p.from_top == 1 && p.from_left == 10, "vertical/full: 上退 4 行");
    CHECK(p.to_top == 5 && p.to_left == 10, "vertical/full: 终点保持锚点");

    p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL, UI_MENU_ANIM_TIER_REDUCED,
                          final, 30, 100);
    CHECK(p.from_top == 4 && p.from_left == 10, "vertical/reduced: 上退 1 行");

    p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL, UI_MENU_ANIM_TIER_OFF,
                          final, 30, 100);
    CHECK(plan_at_rest(&p), "vertical/off: 起终点相同");

    /* 贴顶：从 1 回退到 0，只剩 1 行动画。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL, UI_MENU_ANIM_TIER_FULL,
                          R(1, 10, 8, 20), 30, 100);
    CHECK(p.from_top == 0 && p.to_top == 1, "vertical/full: 贴顶钳到 0");

    /* 已在 0 行：向上放不下，换向下移。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL, UI_MENU_ANIM_TIER_FULL,
                          R(0, 10, 8, 20), 30, 100);
    CHECK(p.from_top == 4 && p.to_top == 0, "vertical/full: 贴顶换向向下");

    /* 窗口几乎占满屏幕：上下都放不下，视为无动画。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_VERTICAL, UI_MENU_ANIM_TIER_FULL,
                          R(0, 0, 29, 100), 30, 100);
    CHECK(plan_at_rest(&p), "vertical/full: 无可用位移时不动");
}

/* ── 对角（右键菜单） ─────────────────────────────────────────── */

static void test_plan_diagonal(void)
{
    UIRect final = R(5, 10, 8, 20);
    UIMenuAnimPlan p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL,
                                         UI_MENU_ANIM_TIER_FULL, final, 30, 100);
    CHECK(p.from_top == 2 && p.from_left == 5, "diagonal/full: 上 3 行 + 左 5 列");

    p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL, UI_MENU_ANIM_TIER_REDUCED,
                          final, 30, 100);
    CHECK(p.from_top == 4 && p.from_left == 9, "diagonal/reduced: 上 1 + 左 1");

    /* 靠左上：纵/横分别钳到 0，仍保持对角位移。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL, UI_MENU_ANIM_TIER_FULL,
                          R(2, 3, 8, 20), 30, 100);
    CHECK(p.from_top == 0 && p.from_left == 0 && p.to_top == 2 && p.to_left == 3,
          "diagonal/full: 贴左上分别钳制");

    /* 已在 (0,0)：回退方向放不下时整体换向，保证仍有对角运动。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL, UI_MENU_ANIM_TIER_FULL,
                          R(0, 0, 8, 20), 30, 100);
    CHECK(p.from_top == 3 && p.from_left == 5 && p.to_top == 0 && p.to_left == 0,
          "diagonal/full: 贴左上换向右下");

    /* 终端太小/窗口占满：任何方向都放不下。 */
    p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL, UI_MENU_ANIM_TIER_FULL,
                          R(0, 0, 29, 100), 30, 100);
    CHECK(plan_at_rest(&p), "diagonal/full: 无可用位移时不动");

    p = ui_menu_anim_plan(UI_MENU_ANIM_DIAGONAL, UI_MENU_ANIM_TIER_OFF,
                          final, 30, 100);
    CHECK(plan_at_rest(&p), "diagonal/off: 起终点相同");
}

/* ── 时长表 ───────────────────────────────────────────────────── */

static void test_durations(void)
{
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_OFF,
                                   UI_MENU_ANIM_VERTICAL, 0) == 0,
          "duration/off: 0");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_OFF,
                                   UI_MENU_ANIM_DIAGONAL, 1) == 0,
          "duration/off exit: 0");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_FULL,
                                   UI_MENU_ANIM_VERTICAL, 0) == 140,
          "duration/full vertical in");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_FULL,
                                   UI_MENU_ANIM_VERTICAL, 1) == 110,
          "duration/full vertical out");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_FULL,
                                   UI_MENU_ANIM_DIAGONAL, 0) == 150,
          "duration/full diagonal in");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_FULL,
                                   UI_MENU_ANIM_DIAGONAL, 1) == 115,
          "duration/full diagonal out");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_REDUCED,
                                   UI_MENU_ANIM_VERTICAL, 0) == 70,
          "duration/reduced in");
    CHECK(ui_menu_anim_duration_ms(UI_MENU_ANIM_TIER_REDUCED,
                                   UI_MENU_ANIM_DIAGONAL, 1) == 55,
          "duration/reduced out");
}

/* ── 缓动 ─────────────────────────────────────────────────────── */

static void test_ease(void)
{
    CHECK(ui_menu_anim_ease(0, 0) == 0, "ease: 入场 0");
    CHECK(ui_menu_anim_ease(1000, 0) == 1000, "ease: 入场 1000");
    CHECK(ui_menu_anim_ease(0, 1) == 0, "ease: 消失 0");
    CHECK(ui_menu_anim_ease(1000, 1) == 1000, "ease: 消失 1000");
    CHECK(ui_menu_anim_ease(500, 0) > 500, "ease: 入场中段快于线性");
    CHECK(ui_menu_anim_ease(500, 1) < 500, "ease: 消失中段慢于线性");

    int prev_in = 0;
    int prev_out = 0;
    for (int p = 0; p <= 1000; p += 25) {
        int in = ui_menu_anim_ease(p, 0);
        int out = ui_menu_anim_ease(p, 1);
        CHECK(in >= prev_in, "ease: 入场单调");
        CHECK(out >= prev_out, "ease: 消失单调");
        CHECK(in >= 0 && in <= 1000, "ease: 入场范围");
        CHECK(out >= 0 && out <= 1000, "ease: 消失范围");
        prev_in = in;
        prev_out = out;
    }
}

/* ── 按时间取位置 ─────────────────────────────────────────────── */

static void test_position(void)
{
    UIMenuAnimPlan p;
    p.from_top = 10;
    p.from_left = 20;
    p.to_top = 14;
    p.to_left = 25;

    int top = -1;
    int left = -1;
    ui_menu_anim_position(&p, 0, 100, 0, &top, &left);
    CHECK(top == 10 && left == 20, "position: 0ms 在起点");

    ui_menu_anim_position(&p, 100, 100, 0, &top, &left);
    CHECK(top == 14 && left == 25, "position: 到时长在终点");

    ui_menu_anim_position(&p, 250, 100, 0, &top, &left);
    CHECK(top == 14 && left == 25, "position: 超时仍在终点");

    ui_menu_anim_position(&p, 50, 100, 0, &top, &left);
    CHECK(top >= 10 && top <= 14 && left >= 20 && left <= 25,
          "position: 半程在起终点之间");

    ui_menu_anim_position(&p, 50, 0, 0, &top, &left);
    CHECK(top == 14 && left == 25, "position: 零时长直接到终点");

    /* 终点在起点上方/左侧时也要能倒退。 */
    UIMenuAnimPlan back;
    back.from_top = 5;
    back.from_left = 5;
    back.to_top = 0;
    back.to_left = 0;
    ui_menu_anim_position(&back, 100, 100, 1, &top, &left);
    CHECK(top == 0 && left == 0, "position: 零时长/超时落到上左终点");
    ui_menu_anim_position(&back, 0, 100, 1, &top, &left);
    CHECK(top == 5 && left == 5, "position: 消失起点在原地");
}

/* ── 降级 ─────────────────────────────────────────────────────── */

static void test_degrade(void)
{
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_FULL, 0) ==
              UI_MENU_ANIM_TIER_FULL, "degrade: full/0 不动");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_FULL, 24) ==
              UI_MENU_ANIM_TIER_FULL, "degrade: full/24 不降");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_FULL, 25) ==
              UI_MENU_ANIM_TIER_REDUCED, "degrade: full/25 → reduced");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_FULL, 54) ==
              UI_MENU_ANIM_TIER_REDUCED, "degrade: full/54 → reduced");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_FULL, 55) ==
              UI_MENU_ANIM_TIER_OFF, "degrade: full/55 → off");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_REDUCED, 24) ==
              UI_MENU_ANIM_TIER_REDUCED, "degrade: reduced 不回升");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_REDUCED, 55) ==
              UI_MENU_ANIM_TIER_OFF, "degrade: reduced/55 → off");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_OFF, 0) ==
              UI_MENU_ANIM_TIER_OFF, "degrade: off 恒 off");
    CHECK(ui_menu_anim_tier_after_frame(UI_MENU_ANIM_TIER_OFF, 999) ==
              UI_MENU_ANIM_TIER_OFF, "degrade: off 不被抬升");
}

int main(void)
{
    test_plan_vertical();
    test_plan_diagonal();
    test_durations();
    test_ease();
    test_position();
    test_degrade();

    printf("test_menu_anim_math: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
