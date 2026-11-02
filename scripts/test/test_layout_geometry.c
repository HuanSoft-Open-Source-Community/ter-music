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

int main(void)
{
    test_main_area();
    test_classic_exact();
    test_preset_tilings();
    test_rescue();

    if (g_failures == 0) {
        printf("PASS layout_geometry (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_geometry (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
