/**
 * @file test_layout_geometry.c
 * @brief ui/layout_geometry.c 的单元测试
 *
 * 重点：把所有预设映射到终端单元格后逐格栅格化，断言主区域每个单元格
 * 恰好被一个窗格覆盖（无空白、无重叠），包括极小终端。
 *
 * 运行：scripts/test/run-unit-tests.sh test_layout_geometry
 * 依赖（见同名 .srcs）：layout_spec.c + layout_geometry.c
 */

#include "ui/layout_geometry.h"
#include "ui/layout_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int load_text_file(const char *path, char **out, size_t *len)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size <= 0) {
        fclose(file);
        return -1;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[got] = '\0';
    *out = buffer;
    *len = got;
    return 0;
}

static int build_preset_catalog(LayoutCatalog *catalog)
{
    char *text = NULL;
    size_t len = 0;
    if (load_text_file("data/layouts/layouts.json", &text, &len) != 0 &&
        load_text_file("../data/layouts/layouts.json", &text, &len) != 0) {
        return -1;
    }
    char err[256] = "";
    int rc = layout_spec_parse(text, len, catalog, err, sizeof(err));
    free(text);
    return rc;
}

static int pane_covers(const LayoutPaneRect *rect, int x, int y)
{
    return rect->visible && rect->has_cells &&
           x >= rect->x && x < rect->x + rect->w &&
           y >= rect->y && y < rect->y + rect->h;
}

static void check_tiling(const LayoutSpec *spec, int rows, int cols,
                         const char *label)
{
    LayoutResolved resolved;
    if (layout_spec_resolve(spec, &resolved) != 0) {
        CHECK(0, "layout resolves");
        return;
    }

    LayoutGeometry geometry;
    layout_geometry_compute(&resolved, rows, cols, &geometry);

    if (rows < 2 || cols < 1) {
        CHECK(!geometry.valid, "empty main area is invalid");
        return;
    }
    CHECK(geometry.valid, "non-empty main area is valid");

    for (int y = 0; y < rows - 1; y++) {
        for (int x = 0; x < cols; x++) {
            int owners = 0;
            for (int k = 0; k < LAYOUT_PANE_COUNT; k++) {
                if (pane_covers(&geometry.panes[k], x, y)) {
                    owners++;
                }
            }
            g_checks++;
            if (owners != 1) {
                g_failures++;
                printf("  FAIL %s: cell (%d,%d) has %d owners (%dx%d)\n",
                       label, x, y, owners, cols, rows);
                return;
            }
        }
    }
}

static void test_main_area(void)
{
    int x, y, w, h;
    layout_geometry_main_area(30, 100, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 100 && h == 29,
          "main area excludes the bottom menu row");
    layout_geometry_main_area(1, 10, &x, &y, &w, &h);
    CHECK(h == 0, "one-row terminal has no main area");
    layout_geometry_main_area(2, 2, &x, &y, &w, &h);
    CHECK(w == 2 && h == 1, "two-row terminal keeps one main row");
}

static void test_classic_exact(void)
{
    LayoutSpec classic;
    CHECK(layout_spec_build_fallback(&classic) == 0, "fallback builds");

    LayoutResolved resolved;
    CHECK(layout_spec_resolve(&classic, &resolved) == 0,
          "classic resolves");
    LayoutGeometry geometry;
    layout_geometry_compute(&resolved, 30, 100, &geometry);
    CHECK(geometry.valid, "classic geometry valid at 100x30");

    const LayoutPaneRect *playlist = &geometry.panes[LAYOUT_PANE_PLAYLIST];
    const LayoutPaneRect *info = &geometry.panes[LAYOUT_PANE_INFO];
    const LayoutPaneRect *controls = &geometry.panes[LAYOUT_PANE_CONTROLS];
    const LayoutPaneRect *spectrum = &geometry.panes[LAYOUT_PANE_SPECTRUM];
    const LayoutPaneRect *lyrics = &geometry.panes[LAYOUT_PANE_LYRICS];

    CHECK(playlist->x == 0 && playlist->y == 0 &&
          playlist->w == 68 && playlist->h == 15, "classic playlist rect");
    CHECK(info->x == 0 && info->y == 15 && info->w == 68 && info->h == 7,
          "classic info rect");
    CHECK(controls->x == 0 && controls->y == 22 &&
          controls->w == 68 && controls->h == 7, "classic controls rect");
    CHECK(spectrum->x == 68 && spectrum->y == 0 &&
          spectrum->w == 32 && spectrum->h == 5, "classic spectrum rect");
    CHECK(lyrics->x == 68 && lyrics->y == 5 &&
          lyrics->w == 32 && lyrics->h == 24, "classic lyrics rect");

    check_tiling(&classic, 30, 100, "classic 100x30");
}

static void test_preset_tilings(void)
{
    LayoutCatalog catalog;
    CHECK(build_preset_catalog(&catalog) == 0, "preset catalog parses");

    static const int sizes[][2] = {
        {1, 1}, {2, 2}, {3, 3}, {8, 20}, {10, 40}, {24, 80},
        {30, 100}, {48, 160}, {60, 200}, {80, 240},
    };

    for (int p = 0; p < catalog.count; p++) {
        char label[96];
        snprintf(label, sizeof(label), "%s", catalog.layouts[p].id);
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            check_tiling(&catalog.layouts[p], sizes[s][0], sizes[s][1],
                         label);
        }
    }
}

static void test_rescue(void)
{
    LayoutGeometry geometry;
    layout_geometry_rescue(30, 100, &geometry);
    CHECK(geometry.valid, "rescue geometry valid");
    CHECK(geometry.panes[LAYOUT_PANE_PLAYLIST].x == 0 &&
          geometry.panes[LAYOUT_PANE_PLAYLIST].y == 0 &&
          geometry.panes[LAYOUT_PANE_PLAYLIST].w == 100 &&
          geometry.panes[LAYOUT_PANE_PLAYLIST].h == 29,
          "rescue fills the whole main area");

    for (int y = 0; y < 29; y++) {
        for (int x = 0; x < 100; x++) {
            g_checks++;
            if (!pane_covers(&geometry.panes[LAYOUT_PANE_PLAYLIST], x, y)) {
                g_failures++;
                printf("  FAIL rescue leaves (%d,%d) blank\n", x, y);
                return;
            }
        }
    }
}


/* t23：编辑期最小可读尺寸（units ↔ cells 换算 + 吸附），与加载/校验无关。 */
static void test_editor_min_readable(void)
{
    const int main_cols = 80;
    const int main_rows = 23;   /* 80x24 终端去掉底部菜单行 */

    struct KindCase {
        LayoutPaneKind kind;
        int cols;
        int rows;
    } cases[] = {
        { LAYOUT_PANE_PLAYLIST, LAYOUT_EDITOR_MIN_PANE_COLS,
          LAYOUT_EDITOR_MIN_PANE_ROWS },
        { LAYOUT_PANE_INFO, LAYOUT_EDITOR_MIN_PANE_COLS,
          LAYOUT_EDITOR_MIN_PANE_ROWS },
        { LAYOUT_PANE_CONTROLS, LAYOUT_EDITOR_MIN_PANE_COLS,
          LAYOUT_EDITOR_MIN_PANE_ROWS },
        { LAYOUT_PANE_SPECTRUM, LAYOUT_EDITOR_MIN_SPECTRUM_COLS,
          LAYOUT_EDITOR_MIN_SPECTRUM_ROWS },
        { LAYOUT_PANE_LYRICS, LAYOUT_EDITOR_MIN_LYRICS_COLS,
          LAYOUT_EDITOR_MIN_LYRICS_ROWS },
        { LAYOUT_PANE_VINYL, LAYOUT_EDITOR_MIN_VINYL_COLS,
          LAYOUT_EDITOR_MIN_VINYL_ROWS },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int min_cols = 0;
        int min_rows = 0;
        layout_editor_min_cells_for_kind(cases[i].kind, &min_cols, &min_rows);
        CHECK(min_cols == cases[i].cols && min_rows == cases[i].rows,
              "per-kind minimum cells unchanged");

        int units_cols = layout_editor_units_for_cells(min_cols, main_cols);
        int units_rows = layout_editor_units_for_cells(min_rows, main_rows);
        CHECK(layout_editor_cells_for_units(units_cols, main_cols) == min_cols,
              "80x24: min columns round-trip to exactly min cells");
        CHECK(layout_editor_cells_for_units(units_cols - 1, main_cols) < min_cols,
              "80x24: one unit less is below min columns");
        CHECK(layout_editor_cells_for_units(units_rows, main_rows) == min_rows,
              "80x24: min rows round-trip to exactly min cells");
        CHECK(layout_editor_cells_for_units(units_rows - 1, main_rows) < min_rows,
              "80x24: one unit less is below min rows");
        CHECK(units_cols >= LAYOUT_MIN_PANE_UNITS &&
              units_rows >= LAYOUT_MIN_PANE_UNITS,
              "editor minimum never lowers the global 10-unit floor");
    }

    /* 80x24 的具体数值：列表 12 列 = 150 units，3 行 = 131 units；少一格被吸附。 */
    CHECK(layout_editor_units_for_cells(12, 80) == 150,
          "80x24: 12 cols needs 150 units");
    CHECK(layout_editor_cells_for_units(149, 80) == 11,
          "80x24: 149 units is only 11 cols");
    CHECK(layout_editor_units_for_cells(3, 23) == 131,
          "80x24: 3 rows needs 131 units");
    CHECK(layout_editor_cells_for_units(130, 23) == 2,
          "80x24: 130 units is only 2 rows");

    /* 分栏比例吸附：150/850 是合法边界；再缩一格吸附回边界。 */
    CHECK(layout_editor_snap_ratio(500, 150, 150) == 500,
          "snap: 50/50 stays");
    CHECK(layout_editor_snap_ratio(149, 150, 150) == 150,
          "snap: below left minimum absorbs to 150");
    CHECK(layout_editor_snap_ratio(860, 150, 150) == 850,
          "snap: above right minimum absorbs to 850");
    CHECK(layout_editor_snap_ratio(500, 600, 600) == 500,
          "snap: impossible interval leaves ratio unchanged");

    /* 终端异常/过小：纯换算不崩溃，返回“无法满足”的默认值。 */
    CHECK(layout_editor_units_for_cells(0, 80) == LAYOUT_AREA_UNITS &&
          layout_editor_units_for_cells(12, 0) == LAYOUT_AREA_UNITS,
          "degenerate extents return the whole axis");
    CHECK(layout_editor_cells_for_units(0, 80) == 0 &&
          layout_editor_cells_for_units(150, 0) == 0,
          "degenerate units/extent return zero cells");
}

/* 旧布局（20 units = 1 列，远小于编辑期 12 列，但 ≥ 全局 10 units）必须
 * 仍可解析/valid；新限制只在编辑器交互里生效。 */
static void test_old_layout_still_loads(void)
{
    static const char *old_small =
        "{\"layouts\":[{\"id\":\"old_small\",\"tree\":{"
        "\"split\":\"row\",\"ratio\":20,\"children\":["
        "{\"pane\":\"playlist\"},{\"pane\":\"info\"}]}}]}";
    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(old_small, strlen(old_small), &catalog,
                            err, sizeof(err)) == 0,
          "old 20-unit layout still parses");
    CHECK(catalog.count == 1, "old layout count");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "old 20-unit layout still validates (not rejected by editor minimum)");

    LayoutResolved resolved;
    CHECK(layout_spec_resolve(&catalog.layouts[0], &resolved) == 0,
          "old layout resolves");
    const LayoutResolvedPane *playlist = NULL;
    for (int i = 0; i < resolved.pane_count; i++) {
        if (resolved.panes[i].kind == LAYOUT_PANE_PLAYLIST) {
            playlist = &resolved.panes[i];
            break;
        }
    }
    CHECK(playlist && playlist->w == 20 && playlist->h == 1000,
          "old layout keeps its 20-unit geometry");
    CHECK(layout_editor_cells_for_units(playlist ? playlist->w : 0, 80) == 1,
          "old layout maps to 1 column (below editor 12)");
    CHECK(layout_spec_resolve(&catalog.layouts[0], &resolved) == 0 &&
          layout_editor_snap_ratio(playlist ? playlist->w : 0, 150, 150) == 150,
          "editor would absorb the old 20-unit split to 150 only during edit");

    LayoutGeometry geometry;
    layout_geometry_compute(&resolved, 1, 1, &geometry);
    CHECK(!geometry.valid, "tiny terminal has no main area and does not crash");
}

int main(void)
{
    test_main_area();
    test_classic_exact();
    test_preset_tilings();
    test_rescue();
    test_editor_min_readable();
    test_old_layout_still_loads();

    if (g_failures == 0) {
        printf("PASS layout_geometry (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_geometry (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
