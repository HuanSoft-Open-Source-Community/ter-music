/**
 * @file test_context_menu_layout.c
 * @brief ui/context_menu_layout.c 的单元测试：右键菜单位置限制与滚动
 *
 * 覆盖：
 *   - 根菜单在四角/边缘的上翻与左翻，结果完整落在
 *     [0, screen_cols) × [0, screen_rows-1)（末行提示条除外）；
 *   - 超宽/超高收缩与极小终端拒绝；
 *   - 子菜单右侧优先、右侧溢出放左侧、底部溢出上移、最终钳制；
 *   - 菜单框本地行 → 条目下标（边框/页脚/越界返回 -1）；
 *   - 滚动偏移保持选中项可见；
 *   - 屏幕矩形严格相交判定（边缘相切不算）。
 *
 * 运行：scripts/test/run-unit-tests.sh test_context_menu_layout
 * 依赖（见同名 .srcs）：ui/context_menu_layout.c + ui/mouse_routing.c
 */

#include "ui/context_menu_layout.h"

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

static int inside_screen(const UIRect *r, int rows, int cols)
{
    return r->top >= 0 && r->left >= 0 && r->rows > 0 && r->cols > 0 &&
           r->top + r->rows <= rows - 1 &&   /* 末行提示条不覆盖 */
           r->left + r->cols <= cols;
}

static void test_place_normal(void)
{
    UIRect r;
    CHECK(ui_ctx_menu_place_rect(5, 10, 20, 10, 30, 100, &r) == 1,
          "place: normal accepted");
    CHECK(r.top == 5 && r.left == 10 && r.rows == 10 && r.cols == 20,
          "place: normal keeps anchor");
    CHECK(inside_screen(&r, 30, 100), "place: normal inside screen");
}

static void test_place_edges(void)
{
    UIRect r;

    CHECK(ui_ctx_menu_place_rect(0, 0, 20, 10, 30, 100, &r) == 1 &&
          r.top == 0 && r.left == 0, "place: top-left anchor");
    CHECK(ui_ctx_menu_place_rect(-5, -5, 20, 10, 30, 100, &r) == 1 &&
          r.top == 0 && r.left == 0, "place: negative anchor clamps to 0,0");

    /* 右下角：必须上翻 + 左翻，且不压最后一行 */
    CHECK(ui_ctx_menu_place_rect(29, 99, 20, 10, 30, 100, &r) == 1,
          "place: bottom-right accepted");
    CHECK(r.top == 19 && r.left == 79, "place: bottom-right flips up and left");
    CHECK(inside_screen(&r, 30, 100), "place: bottom-right stays visible");

    /* 恰好贴底/贴右：不翻转，只钳制到边界 */
    CHECK(ui_ctx_menu_place_rect(20, 80, 20, 9, 30, 100, &r) == 1 &&
          r.top == 20 && r.left == 80, "place: exact fit keeps anchor");
}

static void test_place_oversize(void)
{
    UIRect r;
    CHECK(ui_ctx_menu_place_rect(5, 10, 500, 10, 30, 100, &r) == 1 &&
          r.cols == 100 && r.left == 0, "place: oversize width shrinks to screen");
    CHECK(ui_ctx_menu_place_rect(5, 10, 20, 500, 30, 100, &r) == 1 &&
          r.rows == 29, "place: oversize height leaves hint row");
    CHECK(inside_screen(&r, 30, 100), "place: oversize result inside screen");
}

static void test_place_too_small(void)
{
    UIRect r;
    CHECK(ui_ctx_menu_place_rect(0, 0, 20, 10, 3, 100, &r) == 0,
          "place: 3-row terminal refused");
    CHECK(ui_ctx_menu_place_rect(0, 0, 20, 10, 30, 8, &r) == 0,
          "place: 8-col terminal refused");
    CHECK(ui_ctx_menu_place_rect(0, 0, 0, 10, 30, 100, &r) == 0,
          "place: zero width refused");
}

static void test_submenu(void)
{
    UIRect parent = {5, 10, 10, 20};   /* 行 5..14，列 10..29 */
    UIRect r;

    CHECK(ui_ctx_menu_place_submenu(&parent, 7, 15, 8, 30, 100, &r) == 1,
          "submenu: right side accepted");
    CHECK(r.left == 30 && r.top == 7, "submenu: prefers right side, aligned");
    CHECK(inside_screen(&r, 30, 100), "submenu: right side inside screen");

    /* 右侧溢出：放父菜单左侧 */
    UIRect right_parent = {5, 70, 10, 20};
    CHECK(ui_ctx_menu_place_submenu(&right_parent, 7, 15, 8, 30, 100, &r) == 1,
          "submenu: right overflow accepted");
    CHECK(r.left == 55, "submenu: right overflow flips to left side");

    /* 底部溢出：上移，仍完整可见 */
    CHECK(ui_ctx_menu_place_submenu(&parent, 25, 15, 8, 30, 100, &r) == 1,
          "submenu: bottom overflow accepted");
    CHECK(r.top == 18 && inside_screen(&r, 30, 100),
          "submenu: bottom overflow moves up inside screen");

    /* 极小屏拒绝 */
    CHECK(ui_ctx_menu_place_submenu(&parent, 7, 15, 8, 3, 100, &r) == 0,
          "submenu: tiny screen refused");
}

static void test_item_index(void)
{
    CHECK(ui_ctx_menu_item_index(0, 5, 0, 10) == -1, "item: border row misses");
    CHECK(ui_ctx_menu_item_index(1, 5, 0, 10) == 0, "item: first row");
    CHECK(ui_ctx_menu_item_index(5, 5, 0, 10) == 4, "item: last visible row");
    CHECK(ui_ctx_menu_item_index(6, 5, 0, 10) == -1, "item: footer misses");
    CHECK(ui_ctx_menu_item_index(1, 5, 2, 10) == 2, "item: scroll offset applied");
    CHECK(ui_ctx_menu_item_index(4, 5, 2, 10) == 5, "item: scroll last row");
    CHECK(ui_ctx_menu_item_index(5, 5, 2, 6) == -1, "item: beyond count misses");
    CHECK(ui_ctx_menu_item_index(1, 0, 0, 10) == -1, "item: no visible rows");
    CHECK(ui_ctx_menu_item_index(-1, 5, 0, 10) == -1, "item: negative row misses");
}

static void test_scroll(void)
{
    CHECK(ui_ctx_menu_scroll_for(0, 10, 5, 0) == 0, "scroll: first stays");
    CHECK(ui_ctx_menu_scroll_for(9, 10, 5, 0) == 5, "scroll: last moves window");
    CHECK(ui_ctx_menu_scroll_for(2, 10, 5, 5) == 2, "scroll: moving up shifts");
    CHECK(ui_ctx_menu_scroll_for(3, 10, 5, 2) == 2, "scroll: inside window stays");
    CHECK(ui_ctx_menu_scroll_for(0, 3, 5, 0) == 0, "scroll: fewer items than rows");
    CHECK(ui_ctx_menu_scroll_for(5, 10, 5, 99) == 5, "scroll: current clamped");
}

static void test_rect_intersects(void)
{
    UIRect a = { .top = 2, .left = 5, .rows = 19, .cols = 37 };
    UIRect disjoint = { .top = 0, .left = 60, .rows = 10, .cols = 20 };
    UIRect touching = { .top = 2, .left = 42, .rows = 19, .cols = 10 };
    UIRect overlap = { .top = 10, .left = 30, .rows = 8, .cols = 20 };
    UIRect inside = { .top = 5, .left = 10, .rows = 3, .cols = 3 };
    UIRect empty = { .top = 0, .left = 0, .rows = 0, .cols = 0 };

    CHECK(ui_rect_intersects(&a, &a) == 1, "rect: self intersects");
    CHECK(ui_rect_intersects(&a, &disjoint) == 0, "rect: disjoint");
    CHECK(ui_rect_intersects(&a, &touching) == 0, "rect: edge touch is not overlap");
    CHECK(ui_rect_intersects(&a, &overlap) == 1, "rect: partial overlap");
    CHECK(ui_rect_intersects(&a, &inside) == 1, "rect: containment");
    CHECK(ui_rect_intersects(&a, &empty) == 0, "rect: empty rejected");
    CHECK(ui_rect_intersects(NULL, &a) == 0, "rect: null rejected");
}

int main(void)
{
    printf("test_context_menu_layout\n");
    test_place_normal();
    test_place_edges();
    test_place_oversize();
    test_place_too_small();
    test_submenu();
    test_item_index();
    test_scroll();
    test_rect_intersects();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
