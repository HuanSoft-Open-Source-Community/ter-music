/**
 * @file test_layout_spec.c
 * @brief ui/layout_spec.c 的单元测试
 *
 * 覆盖 JSON 解析、字段校验、严格平铺（无洞/无重叠/面积和）、
 * lyrics/vinyl 回退、目录合并，以及仓库内预设文件的整体合法性。
 *
 * 运行：scripts/test/run-unit-tests.sh test_layout_spec
 * 依赖（见同名 .srcs）：src/org.yxzl.ter-music/ui/layout_spec.c
 */

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
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return -1;
    }
    rewind(file);
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

static const char *find_preset_file(char *path, size_t path_size)
{
    static const char *const candidates[] = {
        "data/layouts/layouts.json",
        "../data/layouts/layouts.json",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *file = fopen(candidates[i], "rb");
        if (file) {
            fclose(file);
            snprintf(path, path_size, "%s", candidates[i]);
            return path;
        }
    }
    const char *source = getenv("TER_MUSIC_SOURCE_DIR");
    if (source && source[0]) {
        snprintf(path, path_size, "%s/data/layouts/layouts.json", source);
        FILE *file = fopen(path, "rb");
        if (file) {
            fclose(file);
            return path;
        }
    }
    return NULL;
}

static void test_embedded_catalogs(void)
{
    static const char *valid =
        "{"
        "\"version\":1,\"default\":\"split\","
        "\"layouts\":[{"
        "\"id\":\"split\",\"name\":\"Split\","
        "\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":500,\"h\":1000,"
        "\"fg\":7,\"bg\":-1,\"border_fg\":6},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";

    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(valid, strlen(valid), &catalog, err, sizeof(err)) == 0,
          "valid two-pane tiling parses");
    CHECK(catalog.count == 1, "one layout parsed");
    CHECK(strcmp(catalog.default_id, "split") == 0, "default id preserved");
    CHECK(catalog.layouts[0].panes[0].fg == 7, "explicit fg parsed");
    CHECK(catalog.layouts[0].panes[0].border_fg == 6, "explicit border fg parsed");
    CHECK(catalog.layouts[0].panes[1].fg == LAYOUT_COLOR_INHERIT,
          "missing color inherits");

    static const char *gap =
        "{\"layouts\":[{\"id\":\"gap\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":500,\"h\":1000},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":400,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(gap, strlen(gap), &catalog, err, sizeof(err)) != 0,
          "layout with a gap is rejected");

    static const char *overlap =
        "{\"layouts\":[{\"id\":\"overlap\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":600,\"h\":1000},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(overlap, strlen(overlap), &catalog, err, sizeof(err)) != 0,
          "overlapping layout is rejected");

    static const char *t_junction =
        "{\"layouts\":[{\"id\":\"tj\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":500,\"h\":1000},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":500,\"h\":400},"
        "{\"id\":\"lyrics\",\"x\":500,\"y\":400,\"w\":500,\"h\":600}"
        "]}]}";
    CHECK(layout_spec_parse(t_junction, strlen(t_junction), &catalog,
                            err, sizeof(err)) == 0,
          "T-junction tiling parses");

    static const char *out_of_bounds =
        "{\"layouts\":[{\"id\":\"oob\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":600,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(out_of_bounds, strlen(out_of_bounds), &catalog,
                            err, sizeof(err)) != 0,
          "out-of-bounds rect is rejected");

    static const char *hidden =
        "{\"layouts\":[{\"id\":\"hidden\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":1000,\"h\":1000,"
        "\"visible\":false},"
        "{\"id\":\"info\",\"x\":0,\"y\":0,\"w\":1000,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(hidden, strlen(hidden), &catalog, err, sizeof(err)) == 0,
          "hidden pane is excluded from tiling");
}

static void test_resolve_dual(void)
{
    LayoutSpec spec;
    memset(&spec, 0, sizeof(spec));
    snprintf(spec.id, sizeof(spec.id), "dual");
    spec.pane_count = 3;
    spec.panes[0] = (LayoutPaneSpec){LAYOUT_PANE_PLAYLIST, 1, 0, 0, 500, 1000,
                                     -2, -2, -2, -2};
    spec.panes[1] = (LayoutPaneSpec){LAYOUT_PANE_LYRICS, 1, 500, 0, 500, 1000,
                                     -2, -2, -2, -2};
    spec.panes[2] = (LayoutPaneSpec){LAYOUT_PANE_VINYL, 1, 500, 0, 500, 1000,
                                     -2, -2, -2, -2};

    char err[256] = "";
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "lyrics+vinyl overlap passes after fallback");

    LayoutSpec resolved;
    layout_spec_resolve(&spec, &resolved);
    CHECK(resolved.panes[1].visible == 0, "lyrics is hidden when vinyl exists");
    CHECK(resolved.panes[2].visible == 1, "vinyl stays visible");

    spec.panes[2].w = 400;
    spec.panes[2].x = 500;
    /* 500+400=900，隐藏 lyrics 后右侧只剩 400，面积不足 */
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) != 0,
          "vinyl not covering lyrics area is rejected");
}

static void test_catalog_merge(void)
{
    LayoutCatalog base;
    layout_catalog_init(&base);
    LayoutSpec fallback;
    CHECK(layout_spec_build_fallback(&fallback) == 0, "fallback builds");
    base.layouts[0] = fallback;
    base.count = 1;
    snprintf(base.default_id, sizeof(base.default_id), "classic");

    LayoutCatalog user;
    layout_catalog_init(&user);
    user.count = 2;
    snprintf(user.layouts[0].id, sizeof(user.layouts[0].id), "classic");
    snprintf(user.layouts[0].name, sizeof(user.layouts[0].name), "Mine");
    user.layouts[0].pane_count = 1;
    user.layouts[0].panes[0] = (LayoutPaneSpec){LAYOUT_PANE_PLAYLIST, 1,
                                                0, 0, 1000, 1000,
                                                -2, -2, -2, -2};
    snprintf(user.layouts[1].id, sizeof(user.layouts[1].id), "extra");
    snprintf(user.layouts[1].name, sizeof(user.layouts[1].name), "Extra");
    user.layouts[1].pane_count = 1;
    user.layouts[1].panes[0] = (LayoutPaneSpec){LAYOUT_PANE_PLAYLIST, 1,
                                                0, 0, 1000, 1000,
                                                -2, -2, -2, -2};

    CHECK(layout_catalog_merge(&base, &user) == 2, "merge count");
    CHECK(strcmp(base.layouts[0].name, "Mine") == 0, "same id is overridden");
    CHECK(layout_catalog_find(&base, "extra") != NULL, "new id appended");
}

static void test_shipped_presets(void)
{
    char path[512];
    const char *found = find_preset_file(path, sizeof(path));
    CHECK(found != NULL, "shipped layouts.json is found");
    if (!found) {
        return;
    }

    char *text = NULL;
    size_t len = 0;
    CHECK(load_text_file(found, &text, &len) == 0, "shipped layouts.json is readable");
    if (!text) {
        return;
    }

    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(text, len, &catalog, err, sizeof(err)) == 0,
          "shipped layouts.json parses");
    free(text);

    CHECK(catalog.count >= 6, "at least six presets");
    CHECK(layout_catalog_find(&catalog, "classic") != NULL, "classic exists");
    CHECK(layout_catalog_find(&catalog, "vinyl") != NULL, "vinyl exists");
    CHECK(layout_catalog_find(&catalog, "dual") != NULL, "dual exists");
    CHECK(layout_catalog_find(&catalog, catalog.default_id) != NULL,
          "default id resolves");

    for (int i = 0; i < catalog.count; i++) {
        char why[256] = "";
        CHECK(layout_spec_validate(&catalog.layouts[i], why, sizeof(why)) == 0,
              "every preset tiles strictly");
    }

    const LayoutSpec *dual = layout_catalog_find(&catalog, "dual");
    CHECK(dual != NULL && dual->pane_count == 6, "dual defines six panes");
    if (dual) {
        LayoutSpec resolved;
        layout_spec_resolve(dual, &resolved);
        int lyrics_visible = 0;
        int vinyl_visible = 0;
        for (int i = 0; i < resolved.pane_count; i++) {
            if (resolved.panes[i].kind == LAYOUT_PANE_LYRICS) {
                lyrics_visible = resolved.panes[i].visible;
            }
            if (resolved.panes[i].kind == LAYOUT_PANE_VINYL) {
                vinyl_visible = resolved.panes[i].visible;
            }
        }
        CHECK(lyrics_visible == 0, "dual hides lyrics");
        CHECK(vinyl_visible == 1, "dual keeps vinyl");
        CHECK(layout_spec_validate(dual, err, sizeof(err)) == 0,
              "dual remains a strict tiling");
    }
}

int main(void)
{
    test_embedded_catalogs();
    test_resolve_dual();
    test_catalog_merge();
    test_shipped_presets();

    if (g_failures == 0) {
        printf("PASS layout_spec (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_spec (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
