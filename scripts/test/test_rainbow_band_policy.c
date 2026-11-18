/**
 * @file test_rainbow_band_policy.c
 * @brief R3 分带纯决策单测：每帧 ≤1 带、落后不追帧、墙钟相位、rows 边界
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/rainbow_band_policy.h"

#include <stdio.h>
#include <stdlib.h>

static int g_checks;
static int g_failures;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("FAIL %s\n", what);
    }
}

static void test_plan_basic(void)
{
    /* 57 行、带高 1：一帧一行，索引递增，末尾回绕 */
    UiRainbowBandPlan p = ui_rainbow_band_plan(57, 1, 0);
    check(p.y0 == 0 && p.y1 == 1, "rows=57 band=1 index=0 -> [0,1)");
    check(p.next == 1 && p.wrapped == 0, "index=0 next=1 not wrapped");

    p = ui_rainbow_band_plan(57, 1, 56);
    check(p.y0 == 56 && p.y1 == 57, "last band -> [56,57)");
    check(p.next == 0 && p.wrapped == 1, "last band wraps to 0");

    p = ui_rainbow_band_plan(57, 1, 57); /* 越界回绕 */
    check(p.y0 == 0 && p.y1 == 1, "index=bands -> wrapped to first band");

    p = ui_rainbow_band_plan(57, 1, -3);
    check(p.y0 == 0 && p.y1 == 1, "negative index clamped to first band");
}

static void test_plan_never_more_than_one_band(void)
{
    /* 任意 rows/band/index：本帧行数必须 <= band_rows（每帧最多一带） */
    for (int rows = 1; rows <= 80; rows++) {
        for (int band = 1; band <= 10; band++) {
            for (int idx = 0; idx <= (rows / band) + 2; idx++) {
                UiRainbowBandPlan p = ui_rainbow_band_plan(rows, band, idx);
                int height = p.y1 - p.y0;
                check(height >= 0, "band height non-negative");
                if (band <= rows) {
                    check(height <= band, "band height <= band_rows");
                }
                check(p.y1 <= rows, "band end within screen");
                check(p.next >= 0, "next index non-negative");
            }
        }
    }
}

static void test_plan_boundaries(void)
{
    UiRainbowBandPlan p = ui_rainbow_band_plan(0, 4, 0);
    check(p.y0 == 0 && p.y1 == 0 && p.wrapped == 0, "rows=0 -> empty band");

    p = ui_rainbow_band_plan(-5, 4, 2);
    check(p.y0 == 0 && p.y1 == 0, "negative rows -> empty band");

    p = ui_rainbow_band_plan(5, 0, 0); /* 带高 0 按 1 处理 */
    check(p.y0 == 0 && p.y1 == 1, "band_rows=0 -> 1 row");

    p = ui_rainbow_band_plan(3, 10, 0); /* 带高大于屏高 -> 夹到屏高 */
    check(p.y0 == 0 && p.y1 == 3 && p.wrapped == 1, "band_rows>rows -> whole screen");
    check(p.next == 0, "single band wraps immediately");
}

static void test_phase_wallclock(void)
{
    const unsigned long long step = 180;
    check(ui_rainbow_band_phase(0, step, 128) == 0, "t=0 -> phase 0");
    check(ui_rainbow_band_phase(179, step, 128) == 0, "t=179 -> phase 0");
    check(ui_rainbow_band_phase(180, step, 128) == 1, "t=180 -> phase 1");
    check(ui_rainbow_band_phase(180 * 5 + 17, step, 128) == 5, "t=917 -> phase 5");
    check(ui_rainbow_band_phase(180 * 128, step, 128) == 0, "one cycle wraps to 0");
    check(ui_rainbow_band_phase(180 * 129 + 7, step, 128) == 1, "phase keeps counting");

    /* 落后丢弃：即使中间错过很久，相位只按墙钟取值，不快进也不补帧 */
    check(ui_rainbow_band_phase(180ULL * 100000ULL, step, 96) ==
              (int)((180ULL * 100000ULL / 180ULL) % 96ULL),
          "huge lag -> phase from wall clock only");

    check(ui_rainbow_band_phase(1234, 0, 16) == (int)(1234ULL % 16ULL),
          "step_ms=0 treated as 1ms");
    check(ui_rainbow_band_phase(1234, step, 0) == 0, "steps=0 -> 0");
    check(ui_rainbow_band_phase(1234, step, -4) == 0, "negative steps -> 0");
}

static void test_rows_for_write(void)
{
    const unsigned long long budget = 4096;
    check(ui_rainbow_band_rows_for_write(1, 0, budget, 3) == 1,
          "zero write -> keep (projection gate)");
    check(ui_rainbow_band_rows_for_write(2, budget + 1, budget, 3) == 1,
          "over budget -> shrink");
    check(ui_rainbow_band_rows_for_write(1, budget + 1, budget, 3) == 1,
          "over budget at 1 -> stay 1");
    check(ui_rainbow_band_rows_for_write(1, 1000, budget, 3) == 2,
          "cheap frame -> grow");
    check(ui_rainbow_band_rows_for_write(3, 1000, budget, 3) == 3,
          "at max -> stay max");
    check(ui_rainbow_band_rows_for_write(0, 1000, budget, 3) == 2,
          "0 rows treated as 1 then grow");
    check(ui_rainbow_band_rows_for_write(5, 1000, budget, 3) == 3,
          "current above max -> clamp then keep");
    check(ui_rainbow_band_rows_for_write(1, 1000, 0, 3) == 1,
          "budget=0 -> keep");
    check(ui_rainbow_band_rows_for_write(1, 3000, budget, 3) == 1,
          "no headroom after projection -> stay");
    check(ui_rainbow_band_rows_for_write(1, 1000, budget, 0) == 1,
          "max_rows=0 -> 1");
}

int main(void)
{
    test_plan_basic();
    test_plan_never_more_than_one_band();
    test_plan_boundaries();
    test_phase_wallclock();
    test_rows_for_write();

    printf("rainbow_band_policy: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("FAIL test_rainbow_band_policy\n");
        return 1;
    }
    printf("PASS test_rainbow_band_policy\n");
    return 0;
}
