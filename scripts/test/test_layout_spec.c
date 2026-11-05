/**
 * @file test_layout_spec.c
 * @brief ui/layout_spec.c 的单元测试
 *
 * 覆盖 v2 分栏树解析、v1 自由矩形自动适配、强制平铺不变量、树编辑操作、
 * 序列化往返、目录合并与仓库预设。重点断言：几何/颜色/越界/空洞/重叠
 * 不再导致整份布局被拒绝。
 *
 * 运行：scripts/test/run-unit-tests.sh test_layout_spec
 * 依赖（见同名 .srcs）：src/cn.org.huansoft.ter-music/ui/layout_spec.c
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

static int parse_one(const char *json, LayoutCatalog *catalog)
{
    char err[256] = "";
    return layout_spec_parse(json, strlen(json), catalog, err, sizeof(err));
}

static const LayoutResolvedPane *find_resolved(const LayoutResolved *resolved,
                                               LayoutPaneKind kind)
{
    for (int i = 0; i < resolved->pane_count; i++) {
        if (resolved->panes[i].kind == kind) {
            return &resolved->panes[i];
        }
    }
    return NULL;
}

static long long resolved_area(const LayoutResolved *resolved)
{
    long long area = 0;
    for (int i = 0; i < resolved->pane_count; i++) {
        area += (long long)resolved->panes[i].w * resolved->panes[i].h;
    }
    return area;
}

static void test_tree_parse(void)
{
    static const char *tree =
        "{\"version\":2,\"layouts\":[{"
        "\"id\":\"tree\",\"name\":\"Tree\","
        "\"tree\":{\"split\":\"row\",\"ratio\":500,\"children\":["
        "{\"pane\":\"playlist\",\"fg\":7,\"bg\":-1},"
        "{\"pane\":\"info\",\"border_fg\":6}"
        "]}}]}";

    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(tree, strlen(tree), &catalog, err, sizeof(err)) == 0,
          "v2 tree parses");
    CHECK(catalog.count == 1, "one tree layout");
    CHECK(catalog.adjusted == 0, "clean tree is not auto-fitted");
    CHECK(catalog.skipped == 0, "clean tree is not skipped");
    CHECK(!catalog.layouts[0].normalized, "clean tree normalized=0");

    const LayoutSpec *layout = &catalog.layouts[0];
    const LayoutPaneSpec *playlist =
        layout_spec_pane_config(layout, LAYOUT_PANE_PLAYLIST);
    const LayoutPaneSpec *info =
        layout_spec_pane_config(layout, LAYOUT_PANE_INFO);
    CHECK(playlist && playlist->fg == 7 && playlist->bg == -1,
          "leaf colors parsed");
    CHECK(info && info->border_fg == 6, "border color parsed");
    CHECK(layout_spec_validate(layout, err, sizeof(err)) == 0,
          "tree validates");

    LayoutResolved resolved;
    CHECK(layout_spec_resolve(layout, &resolved) == 0, "tree resolves");
    CHECK(resolved.pane_count == 2, "tree resolves two panes");
    const LayoutResolvedPane *left =
        find_resolved(&resolved, LAYOUT_PANE_PLAYLIST);
    const LayoutResolvedPane *right =
        find_resolved(&resolved, LAYOUT_PANE_INFO);
    CHECK(left && left->x == 0 && left->w == 500 && left->h == 1000,
          "first child takes ratio share");
    CHECK(right && right->x == 500 && right->w == 500,
          "second child takes the rest");
    CHECK(resolved_area(&resolved) == LAYOUT_AREA_TOTAL,
          "resolved tree tiles the whole area");

    static const char *clamped =
        "{\"layouts\":[{"
        "\"id\":\"clamped\","
        "\"tree\":{\"split\":\"row\",\"ratio\":5000,\"children\":["
        "{\"pane\":\"playlist\"},{\"pane\":\"info\"}]}}]}";
    CHECK(layout_spec_parse(clamped, strlen(clamped), &catalog,
                            err, sizeof(err)) == 0,
          "out-of-range ratio does not reject the layout");
    CHECK(catalog.adjusted == 1, "ratio clamp counts as auto-fitted");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "clamped ratio still tiles");
}

static void test_legacy_conversion(void)
{
    static const char *classic =
        "{\"version\":1,\"default\":\"classic\",\"layouts\":[{"
        "\"id\":\"classic\",\"name\":\"Classic\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":680,\"h\":520},"
        "{\"id\":\"info\",\"x\":0,\"y\":520,\"w\":680,\"h\":260},"
        "{\"id\":\"controls\",\"x\":0,\"y\":780,\"w\":680,\"h\":220},"
        "{\"id\":\"spectrum\",\"x\":680,\"y\":0,\"w\":320,\"h\":200},"
        "{\"id\":\"lyrics\",\"x\":680,\"y\":200,\"w\":320,\"h\":800}"
        "]}]}";

    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(classic, strlen(classic), &catalog,
                            err, sizeof(err)) == 0,
          "v1 classic parses");
    CHECK(catalog.count == 1, "v1 classic layout count");
    CHECK(catalog.adjusted == 0, "exact v1 classic does not count as adjusted");
    const LayoutSpec *spec = &catalog.layouts[0];
    CHECK(layout_spec_validate(spec, err, sizeof(err)) == 0,
          "v1 classic converts to a valid tree");

    LayoutResolved resolved;
    CHECK(layout_spec_resolve(spec, &resolved) == 0, "v1 classic resolves");
    const LayoutResolvedPane *playlist =
        find_resolved(&resolved, LAYOUT_PANE_PLAYLIST);
    const LayoutResolvedPane *info =
        find_resolved(&resolved, LAYOUT_PANE_INFO);
    const LayoutResolvedPane *controls =
        find_resolved(&resolved, LAYOUT_PANE_CONTROLS);
    const LayoutResolvedPane *spectrum =
        find_resolved(&resolved, LAYOUT_PANE_SPECTRUM);
    const LayoutResolvedPane *lyrics =
        find_resolved(&resolved, LAYOUT_PANE_LYRICS);
    CHECK(playlist && playlist->x == 0 && playlist->y == 0 &&
          playlist->w == 680 && playlist->h == 520, "v1 playlist exact");
    CHECK(info && info->x == 0 && info->y == 520 &&
          info->w == 680 && info->h == 260, "v1 info exact");
    CHECK(controls && controls->x == 0 && controls->y == 780 &&
          controls->w == 680 && controls->h == 220, "v1 controls exact");
    CHECK(spectrum && spectrum->x == 680 && spectrum->y == 0 &&
          spectrum->w == 320 && spectrum->h == 200, "v1 spectrum exact");
    CHECK(lyrics && lyrics->x == 680 && lyrics->y == 200 &&
          lyrics->w == 320 && lyrics->h == 800, "v1 lyrics exact");

    static const char *gap =
        "{\"layouts\":[{\"id\":\"gap\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":500,\"h\":1000},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":400,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(gap, strlen(gap), &catalog, err, sizeof(err)) == 0,
          "gap layout is accepted");
    CHECK(catalog.count == 1 && catalog.adjusted == 1,
          "gap layout is auto-fitted");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "gap layout becomes force-tiled");
    CHECK(layout_spec_resolve(&catalog.layouts[0], &resolved) == 0 &&
          resolved_area(&resolved) == LAYOUT_AREA_TOTAL,
          "gap layout fills the main area");

    static const char *overlap =
        "{\"layouts\":[{\"id\":\"overlap\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":600,\"h\":1000},"
        "{\"id\":\"info\",\"x\":500,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(overlap, strlen(overlap), &catalog,
                            err, sizeof(err)) == 0,
          "overlap layout is accepted");
    CHECK(catalog.adjusted == 1 && catalog.layouts[0].normalized,
          "overlap layout is marked auto-fitted");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "overlap layout becomes force-tiled");

    static const char *out_of_bounds =
        "{\"layouts\":[{\"id\":\"oob\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":600,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(out_of_bounds, strlen(out_of_bounds), &catalog,
                            err, sizeof(err)) == 0,
          "out-of-bounds rect is accepted");
    CHECK(catalog.layouts[0].normalized, "out-of-bounds is auto-fitted");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "out-of-bounds becomes force-tiled");

    /* 风车形（中间补一块）没有贯穿切线，必须走阅读顺序重排。 */
    static const char *windmill =
        "{\"layouts\":[{\"id\":\"windmill\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":400,\"h\":600},"
        "{\"id\":\"info\",\"x\":400,\"y\":0,\"w\":600,\"h\":400},"
        "{\"id\":\"controls\",\"x\":0,\"y\":600,\"w\":600,\"h\":400},"
        "{\"id\":\"spectrum\",\"x\":600,\"y\":400,\"w\":400,\"h\":600},"
        "{\"id\":\"lyrics\",\"x\":400,\"y\":400,\"w\":200,\"h\":200}"
        "]}]}";
    CHECK(layout_spec_parse(windmill, strlen(windmill), &catalog,
                            err, sizeof(err)) == 0,
          "non-guillotine layout is accepted");
    CHECK(catalog.adjusted == 1 && catalog.layouts[0].normalized,
          "non-guillotine is marked auto-fitted");
    CHECK(layout_spec_pane_count(&catalog.layouts[0]) == 5,
          "non-guillotine keeps all panes");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "non-guillotine reflows to force tiling");

    static const char *hidden =
        "{\"layouts\":[{\"id\":\"hidden\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":1000,\"h\":1000,"
        "\"visible\":false},"
        "{\"id\":\"info\",\"x\":0,\"y\":0,\"w\":1000,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(hidden, strlen(hidden), &catalog,
                            err, sizeof(err)) == 0,
          "hidden pane is accepted");
    CHECK(layout_spec_pane_count(&catalog.layouts[0]) == 1 &&
          catalog.layouts[0].panes[LAYOUT_PANE_INFO].enabled,
          "hidden pane is excluded and the rest fills the area");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "hidden-pane result tiles");

    static const char *both =
        "{\"layouts\":[{\"id\":\"both\",\"panes\":["
        "{\"id\":\"playlist\",\"x\":0,\"y\":0,\"w\":500,\"h\":1000},"
        "{\"id\":\"lyrics\",\"x\":500,\"y\":0,\"w\":500,\"h\":1000},"
        "{\"id\":\"vinyl\",\"x\":500,\"y\":0,\"w\":500,\"h\":1000}"
        "]}]}";
    CHECK(layout_spec_parse(both, strlen(both), &catalog, err, sizeof(err)) == 0,
          "lyrics+vinyl legacy layout is accepted");
    CHECK(catalog.layouts[0].panes[LAYOUT_PANE_VINYL].enabled &&
          catalog.layouts[0].panes[LAYOUT_PANE_LYRICS].enabled,
          "legacy layout keeps both lyrics and vinyl");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "lyrics+vinyl result tiles");
}

static void test_fallback_and_skip(void)
{
    static const char *empty_tree =
        "{\"layouts\":[{\"id\":\"empty\",\"name\":\"Empty\","
        "\"tree\":{\"split\":\"row\",\"ratio\":500,\"children\":[]}}]}";
    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(empty_tree, strlen(empty_tree), &catalog,
                            err, sizeof(err)) == 0,
          "empty tree falls back instead of rejecting");
    CHECK(catalog.count == 1, "fallback layout is present");
    CHECK(strcmp(catalog.layouts[0].id, "empty") == 0,
          "fallback preserves id");
    CHECK(strcmp(catalog.layouts[0].name, "Empty") == 0,
          "fallback preserves name");
    CHECK(catalog.layouts[0].normalized && catalog.adjusted == 1,
          "fallback is marked auto-fitted");
    CHECK(layout_spec_pane_count(&catalog.layouts[0]) == 5,
          "fallback has the classic panes");
    CHECK(layout_spec_validate(&catalog.layouts[0], err, sizeof(err)) == 0,
          "fallback is force-tiled");

    static const char *one_bad_one_good =
        "{\"layouts\":["
        "{\"panes\":[{\"id\":\"playlist\",\"x\":0,\"y\":0,"
        "\"w\":1000,\"h\":1000}]},"
        "{\"id\":\"good\",\"tree\":{\"pane\":\"playlist\"}}"
        "]}";
    CHECK(layout_spec_parse(one_bad_one_good, strlen(one_bad_one_good),
                            &catalog, err, sizeof(err)) == 0,
          "missing-id layout is skipped, valid layout remains");
    CHECK(catalog.count == 1 && catalog.skipped == 1,
          "skip count records the structurally unreadable layout");
    CHECK(strcmp(catalog.layouts[0].id, "good") == 0,
          "valid layout survives");
}

static void test_serialize_roundtrip(void)
{
    LayoutSpec src;
    CHECK(layout_spec_build_fallback(&src) == 0, "fallback builds");
    src.panes[LAYOUT_PANE_PLAYLIST].fg = 51;
    src.panes[LAYOUT_PANE_PLAYLIST].bg = -1;
    src.panes[LAYOUT_PANE_INFO].border_fg = 39;

    LayoutCatalog catalog;
    layout_catalog_init(&catalog);
    catalog.layouts[0] = src;
    catalog.count = 1;
    snprintf(catalog.default_id, sizeof(catalog.default_id), "%s", src.id);

    char *json = layout_spec_serialize(&catalog);
    CHECK(json != NULL, "serialize returns text");
    if (!json) {
        return;
    }
    CHECK(strstr(json, "\"version\":2") != NULL, "version 2 emitted");
    CHECK(strstr(json, "\"tree\"") != NULL, "tree emitted");
    CHECK(strstr(json, "\"panes\"") != NULL, "legacy mirror emitted");

    LayoutCatalog back;
    char err[256] = "";
    CHECK(layout_spec_parse(json, strlen(json), &back, err, sizeof(err)) == 0,
          "serialized text parses back");
    free(json);
    if (back.count != 1) {
        CHECK(back.count == 1, "roundtrip layout count");
        return;
    }
    const LayoutSpec *out = &back.layouts[0];
    CHECK(strcmp(out->id, src.id) == 0, "roundtrip id");
    CHECK(strcmp(out->name, src.name) == 0, "roundtrip name");
    CHECK(strcmp(back.default_id, src.id) == 0, "roundtrip default id");
    CHECK(layout_spec_pane_count(out) == layout_spec_pane_count(&src),
          "roundtrip pane count");
    const LayoutPaneSpec *a =
        layout_spec_pane_config(&src, LAYOUT_PANE_PLAYLIST);
    const LayoutPaneSpec *b =
        layout_spec_pane_config(out, LAYOUT_PANE_PLAYLIST);
    CHECK(a && b && a->fg == b->fg && a->bg == b->bg,
          "roundtrip leaf colors");
    const LayoutPaneSpec *c =
        layout_spec_pane_config(out, LAYOUT_PANE_INFO);
    CHECK(c && c->border_fg == 39, "roundtrip border color");
    CHECK(layout_spec_validate(out, err, sizeof(err)) == 0,
          "roundtrip still validates");

    LayoutResolved ra;
    LayoutResolved rb;
    CHECK(layout_spec_resolve(&src, &ra) == 0 &&
          layout_spec_resolve(out, &rb) == 0,
          "roundtrip resolves");
    CHECK(ra.pane_count == rb.pane_count, "roundtrip resolved count");
    for (int i = 0; i < ra.pane_count; i++) {
        const LayoutResolvedPane *pa = find_resolved(&ra, ra.panes[i].kind);
        const LayoutResolvedPane *pb = find_resolved(&rb, ra.panes[i].kind);
        CHECK(pa && pb && pa->x == pb->x && pa->y == pb->y &&
              pa->w == pb->w && pa->h == pb->h,
              "roundtrip resolved geometry");
    }
}

static void test_tree_operations(void)
{
    LayoutSpec spec;
    CHECK(layout_spec_build_fallback(&spec) == 0, "ops: fallback builds");
    char err[256] = "";

    CHECK(layout_spec_remove_pane(&spec, LAYOUT_PANE_INFO) == 0,
          "remove info");
    CHECK(layout_spec_pane_count(&spec) == 4 &&
          layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "remove keeps force tiling");

    CHECK(layout_spec_split_pane(&spec, LAYOUT_PANE_PLAYLIST,
                                 LAYOUT_SPLIT_ROW, LAYOUT_PANE_INFO) == 0,
          "split playlist and re-add info");
    CHECK(layout_spec_pane_count(&spec) == 5 &&
          layout_spec_find_pane(&spec, LAYOUT_PANE_INFO) >= 0,
          "split adds the new pane");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "split keeps force tiling");

    spec.panes[LAYOUT_PANE_LYRICS].fg = 123;
    CHECK(layout_spec_change_pane_kind(&spec, LAYOUT_PANE_LYRICS,
                                       LAYOUT_PANE_VINYL) == 0,
          "lyrics can be replaced by vinyl");
    CHECK(spec.panes[LAYOUT_PANE_VINYL].enabled &&
          !spec.panes[LAYOUT_PANE_LYRICS].enabled,
          "vinyl replaces lyrics");
    CHECK(spec.panes[LAYOUT_PANE_VINYL].fg == 123,
          "kind change preserves pane colours");
    CHECK(layout_spec_pane_available(&spec, LAYOUT_PANE_LYRICS) == 1,
          "lyrics stays available next to vinyl (they may coexist)");
    CHECK(layout_spec_change_pane_kind(&spec, LAYOUT_PANE_VINYL,
                                       LAYOUT_PANE_LYRICS) == 0,
          "vinyl can be replaced back by lyrics");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "kind replacement keeps force tiling");

    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST,
                                   LAYOUT_SPLIT_ROW, 10) == 0,
          "ratio can be adjusted");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "ratio adjustment keeps force tiling");
    CHECK(layout_spec_equalize_pane(&spec, LAYOUT_PANE_PLAYLIST) == 0,
          "ratio can be equalized");
    CHECK(layout_spec_swap_panes(&spec, LAYOUT_PANE_PLAYLIST,
                                 LAYOUT_PANE_INFO) == 0,
          "panes can be swapped");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "swap keeps force tiling");

    CHECK(layout_spec_split_pane(&spec, LAYOUT_PANE_PLAYLIST,
                                 LAYOUT_SPLIT_COL, LAYOUT_PANE_VINYL) == 0,
          "vinyl can be split in while lyrics exists");
    CHECK(spec.panes[LAYOUT_PANE_LYRICS].enabled &&
          spec.panes[LAYOUT_PANE_VINYL].enabled &&
          layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "lyrics and vinyl coexist in one layout");
    CHECK(layout_spec_pane_available(&spec, LAYOUT_PANE_VINYL) == 0,
          "vinyl is unavailable once it is in the layout");
    CHECK(layout_spec_remove_pane(&spec, LAYOUT_PANE_VINYL) == 0,
          "the extra vinyl pane can be removed again");

    /* 逐级放大到最小尺寸边界，失败后原布局必须仍然有效。 */
    for (int i = 0; i < 200; i++) {
        if (layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST,
                                     LAYOUT_SPLIT_ROW, 10) != 0) {
            break;
        }
    }
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "failed ratio adjustments leave a valid layout");
    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST,
                                   LAYOUT_SPLIT_ROW, 500) == -3,
          "at the limit the ratio reports -3 (no step fits)");

    /* 删除到只剩一个窗格后，删除与比例调整都必须失败。 */
    while (layout_spec_pane_count(&spec) > 1) {
        int kind = layout_spec_pane_at(&spec, 0);
        CHECK(kind >= 0, "pane at index");
        if (kind < 0 || layout_spec_remove_pane(&spec,
                                                (LayoutPaneKind)kind) != 0) {
            break;
        }
    }
    CHECK(layout_spec_pane_count(&spec) == 1, "reduce to a single pane");
    int only = layout_spec_pane_at(&spec, 0);
    CHECK(only >= 0 && layout_spec_remove_pane(&spec,
                                               (LayoutPaneKind)only) == -1,
          "cannot remove the last pane");
    CHECK(layout_spec_adjust_ratio(&spec, (LayoutPaneKind)only,
                                   LAYOUT_SPLIT_ROW, 10) == -2,
          "root pane has no split to adjust");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "single-pane layout is force-tiled");
}

static void test_catalog_merge(void)
{
    LayoutCatalog base;
    layout_catalog_init(&base);
    LayoutSpec fallback;
    CHECK(layout_spec_build_fallback(&fallback) == 0, "merge: fallback");
    base.layouts[0] = fallback;
    base.count = 1;
    snprintf(base.default_id, sizeof(base.default_id), "classic");

    LayoutCatalog user;
    layout_catalog_init(&user);
    user.count = 2;
    user.layouts[0] = fallback;
    snprintf(user.layouts[0].name, sizeof(user.layouts[0].name), "Mine");
    CHECK(layout_spec_build_fallback(&user.layouts[1]) == 0,
          "merge: second fallback");
    snprintf(user.layouts[1].id, sizeof(user.layouts[1].id), "extra");
    snprintf(user.layouts[1].name, sizeof(user.layouts[1].name), "Extra");

    CHECK(layout_catalog_merge(&base, &user) == 2, "merge count");
    CHECK(strcmp(base.layouts[0].name, "Mine") == 0,
          "same id is overridden");
    CHECK(layout_catalog_find(&base, "extra") != NULL, "new id appended");
}

static void test_user_catalog_ops(void)
{
    LayoutSpec spec;
    CHECK(layout_spec_build_fallback(&spec) == 0, "user ops: fallback");
    snprintf(spec.id, sizeof(spec.id), "mine");
    snprintf(spec.name, sizeof(spec.name), "Mine");

    LayoutCatalog user;
    layout_catalog_init(&user);
    CHECK(layout_user_catalog_upsert(&user, &spec) == 1, "upsert appends");
    snprintf(spec.name, sizeof(spec.name), "Mine2");
    CHECK(layout_user_catalog_upsert(&user, &spec) == 1,
          "upsert same id replaces");
    CHECK(strcmp(user.layouts[0].name, "Mine2") == 0, "upsert overwrites");
    CHECK(layout_user_catalog_remove(&user, "nope") == 1,
          "remove unknown id is a no-op");
    CHECK(layout_user_catalog_remove(&user, "mine") == 0,
          "remove drops the entry");

    LayoutCatalog full;
    layout_catalog_init(&full);
    LayoutSpec dummy;
    CHECK(layout_spec_build_fallback(&dummy) == 0, "user ops: dummy");
    int filled = 1;
    for (int i = 0; i < LAYOUT_MAX_CATALOG; i++) {
        snprintf(dummy.id, sizeof(dummy.id), "cap%02d", i);
        if (layout_user_catalog_upsert(&full, &dummy) < 0) {
            filled = 0;
            break;
        }
    }
    CHECK(filled == 1, "catalog fills to cap");
    snprintf(dummy.id, sizeof(dummy.id), "overflow");
    CHECK(layout_user_catalog_upsert(&full, &dummy) < 0,
          "beyond-cap upsert is rejected");
    CHECK(layout_user_catalog_remove(&full, "cap00") ==
          LAYOUT_MAX_CATALOG - 1, "remove frees a catalog slot");
    snprintf(dummy.id, sizeof(dummy.id), "overflow");
    CHECK(layout_user_catalog_upsert(&full, &dummy) == LAYOUT_MAX_CATALOG,
          "upsert after remove succeeds");
}


static void test_validation_invariants(void)
{
    LayoutSpec spec;
    char err[256] = "";
    CHECK(layout_spec_build_fallback(&spec) == 0, "validate: fallback builds");

    spec.panes[LAYOUT_PANE_INFO].enabled = 0;
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) != 0,
          "validate refuses a leaf that is not enabled");

    CHECK(layout_spec_build_fallback(&spec) == 0, "validate: rebuild fallback");
    int info = layout_spec_find_pane(&spec, LAYOUT_PANE_INFO);
    CHECK(info >= 0, "validate: info leaf exists");
    spec.nodes[info].kind = LAYOUT_PANE_PLAYLIST;
    spec.panes[LAYOUT_PANE_INFO].enabled = 0;
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) != 0,
          "validate refuses duplicate pane kinds");
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
    CHECK(load_text_file(found, &text, &len) == 0,
          "shipped layouts.json is readable");
    if (!text) {
        return;
    }
    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse(text, len, &catalog, err, sizeof(err)) == 0,
          "shipped layouts.json parses");
    free(text);

    CHECK(catalog.count == 7, "seven presets");
    CHECK(catalog.adjusted == 0, "presets need no auto-fitting");
    CHECK(catalog.skipped == 0, "presets are not skipped");
    CHECK(layout_catalog_find(&catalog, "classic") != NULL, "classic exists");
    CHECK(layout_catalog_find(&catalog, "vinyl") != NULL, "vinyl exists");
    CHECK(layout_catalog_find(&catalog, "dual") == NULL,
          "dual demo preset is removed");
    CHECK(layout_catalog_find(&catalog, catalog.default_id) != NULL,
          "default id resolves");
    for (int i = 0; i < catalog.count; i++) {
        char why[256] = "";
        CHECK(layout_spec_validate(&catalog.layouts[i], why, sizeof(why)) == 0,
              "every preset tiles strictly");
    }

    const LayoutSpec *classic = layout_catalog_find(&catalog, "classic");
    LayoutResolved resolved;
    CHECK(classic && layout_spec_resolve(classic, &resolved) == 0,
          "classic preset resolves");
    const LayoutResolvedPane *playlist =
        find_resolved(&resolved, LAYOUT_PANE_PLAYLIST);
    const LayoutResolvedPane *controls =
        find_resolved(&resolved, LAYOUT_PANE_CONTROLS);
    CHECK(playlist && playlist->w == 680 && playlist->h == 520,
          "classic preset playlist geometry");
    CHECK(controls && controls->y == 780 && controls->h == 220,
          "classic preset controls geometry");
}

/* 窗格颜色：设置页需要的"按种类读写 + 生效色解析"纯逻辑。 */
static void test_pane_colours(void)
{
    LayoutSpec spec;
    CHECK(layout_spec_build_fallback(&spec) == 0, "colour: fallback built");

    /* classic 里没有 vinyl：pane_config 按 enabled 过滤，pane_colour 不过滤。 */
    CHECK(layout_spec_pane_config(&spec, LAYOUT_PANE_VINYL) == NULL,
          "disabled pane has no tree config");
    const LayoutPaneSpec *vinyl =
        layout_spec_pane_colour(&spec, LAYOUT_PANE_VINYL);
    CHECK(vinyl != NULL, "disabled pane still exposes colours");
    CHECK(vinyl && vinyl->enabled == 0 &&
          vinyl->fg == LAYOUT_COLOR_INHERIT && vinyl->bg == LAYOUT_COLOR_INHERIT,
          "colours start inherited");
    CHECK(layout_spec_pane_colour(&spec, LAYOUT_PANE_COUNT) == NULL,
          "colour accessor rejects out-of-range kind");

    int from_layout = 1;
    CHECK(layout_spec_colour_effective(&spec, LAYOUT_PANE_INFO,
                                       LAYOUT_COLOUR_FG, 7, &from_layout) == 7,
          "inherited colour falls back to base");
    CHECK(from_layout == 0, "inherit reports from_layout=0");

    spec.panes[LAYOUT_PANE_INFO].fg = 45;
    CHECK(layout_spec_colour_effective(&spec, LAYOUT_PANE_INFO,
                                       LAYOUT_COLOUR_FG, 7, &from_layout) == 45,
          "explicit colour wins over base");
    CHECK(from_layout == 1, "override reports from_layout=1");

    /* -1（终端默认）是显式写下的值，和 -2（继承）语义不同。 */
    spec.panes[LAYOUT_PANE_INFO].bg = -1;
    CHECK(layout_spec_colour_effective(&spec, LAYOUT_PANE_INFO,
                                       LAYOUT_COLOUR_BG, 8, &from_layout) == -1,
          "terminal default counts as an override");
    CHECK(from_layout == 1, "terminal default reports from_layout=1");

    CHECK(layout_spec_colour_effective(&spec, LAYOUT_PANE_INFO, 7, 9, NULL) == 9,
          "bad field number returns base");
    CHECK(layout_spec_colour_effective(&spec, LAYOUT_PANE_COUNT,
                                       LAYOUT_COLOUR_FG, 3, NULL) == 3,
          "bad kind returns base");
    CHECK(layout_spec_colour_effective(NULL, LAYOUT_PANE_INFO,
                                        LAYOUT_COLOUR_FG, 4, NULL) == 4,
          "null spec returns base");

    /* 序列化：显式色往返一致，继承字段不写出去、读回来仍是继承。 */
    LayoutSpec src;
    CHECK(layout_spec_build_fallback(&src) == 0, "colour roundtrip: fallback");
    src.panes[LAYOUT_PANE_PLAYLIST].fg = 51;
    src.panes[LAYOUT_PANE_SPECTRUM].bg = 17;
    src.panes[LAYOUT_PANE_LYRICS].fg = -1;
    src.panes[LAYOUT_PANE_VINYL].fg = 201;   /* 未启用，不该被写出 */

    LayoutCatalog catalog;
    layout_catalog_init(&catalog);
    catalog.layouts[0] = src;
    catalog.count = 1;
    snprintf(catalog.default_id, sizeof(catalog.default_id), "%s", src.id);

    char *json = layout_spec_serialize(&catalog);
    CHECK(json != NULL, "colour roundtrip serializes");
    if (!json) {
        return;
    }
    LayoutCatalog back;
    char err[256] = "";
    CHECK(layout_spec_parse(json, strlen(json), &back, err, sizeof(err)) == 0,
          "colour roundtrip parses back");
    free(json);
    if (back.count != 1) {
        CHECK(back.count == 1, "colour roundtrip layout count");
        return;
    }
    const LayoutSpec *out = &back.layouts[0];
    CHECK(layout_spec_colour_effective(out, LAYOUT_PANE_PLAYLIST,
                                       LAYOUT_COLOUR_FG, 7, NULL) == 51,
          "roundtrip keeps explicit fg");
    CHECK(layout_spec_colour_effective(out, LAYOUT_PANE_SPECTRUM,
                                       LAYOUT_COLOUR_BG, 8, NULL) == 17,
          "roundtrip keeps explicit bg");
    CHECK(layout_spec_colour_effective(out, LAYOUT_PANE_LYRICS,
                                       LAYOUT_COLOUR_FG, 8, NULL) == -1,
          "roundtrip keeps terminal default");
    const LayoutPaneSpec *controls =
        layout_spec_pane_colour(out, LAYOUT_PANE_CONTROLS);
    CHECK(controls && controls->fg == LAYOUT_COLOR_INHERIT &&
          controls->bg == LAYOUT_COLOR_INHERIT,
          "inherited colours come back as inherit");
    const LayoutPaneSpec *back_vinyl =
        layout_spec_pane_colour(out, LAYOUT_PANE_VINYL);
    CHECK(back_vinyl && back_vinyl->fg == LAYOUT_COLOR_INHERIT,
          "disabled pane colours are not persisted");
}

/* 派生副本 id：只在同 id 覆盖写不回去时用来自增，绝不产生重复条目。 */
static void test_copy_id(void)
{
    LayoutCatalog catalog;
    layout_catalog_init(&catalog);
    LayoutSpec spec;
    CHECK(layout_spec_build_fallback(&spec) == 0, "copy id: fallback");
    catalog.layouts[0] = spec;
    catalog.count = 1;

    char id[LAYOUT_ID_MAX];
    CHECK(layout_catalog_next_copy_id(&catalog, "classic", id, sizeof(id)) == 0,
          "first copy gets -2");
    CHECK(strcmp(id, "classic-2") == 0, "copy id is classic-2");

    snprintf(spec.id, sizeof(spec.id), "%s", id);
    catalog.layouts[1] = spec;
    catalog.count = 2;
    CHECK(layout_catalog_next_copy_id(&catalog, "classic", id, sizeof(id)) == 0,
          "second copy gets -3");
    CHECK(strcmp(id, "classic-3") == 0, "copy id skips taken slots");

    char long_base[LAYOUT_ID_MAX];
    memset(long_base, 'a', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = '\0';
    CHECK(layout_catalog_next_copy_id(&catalog, long_base, id, sizeof(id)) == -1,
          "overlong base id is rejected");
    CHECK(layout_catalog_next_copy_id(&catalog, "", id, sizeof(id)) == -1,
          "empty base id is rejected");
    CHECK(layout_catalog_next_copy_id(NULL, "classic", id, sizeof(id)) == -1,
          "null catalog is rejected");
}


/* 双向比例：同一窗格水平（ROW）与垂直（COL）都能调，`]` 恒为放大。 */
static void test_ratio_axis(void)
{
    LayoutSpec spec;
    char err[256] = "";
    CHECK(layout_spec_build_fallback(&spec) == 0, "ratio: fallback built");
    int root = spec.root;
    int root_ratio = spec.nodes[root].ratio;

    /* info 的直接父级是上下分栏，水平方向要往上找到根行分栏。 */
    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_INFO,
                                   LAYOUT_SPLIT_ROW, 10) == 0,
          "ratio: horizontal adjust reaches the row ancestor");
    CHECK(spec.nodes[root].ratio == root_ratio + 10,
          "ratio: row ancestor ratio grew by delta");

    LayoutResolved before;
    CHECK(layout_spec_resolve(&spec, &before) == 0, "ratio: resolve before height");

    int col_split = -1;
    for (int i = 0; i < spec.node_count; i++) {
        if (spec.nodes[i].is_leaf || spec.nodes[i].dir != LAYOUT_SPLIT_COL) {
            continue;
        }
        int first = spec.nodes[i].child[0];
        if (first >= 0 && spec.nodes[first].is_leaf &&
            spec.nodes[first].kind == LAYOUT_PANE_INFO) {
            col_split = i;
            break;
        }
    }
    CHECK(col_split >= 0, "ratio: vertical split above info found");
    int col_ratio = spec.nodes[col_split].ratio;
    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_INFO,
                                   LAYOUT_SPLIT_COL, 10) == 0,
          "ratio: vertical adjust uses the col ancestor");
    CHECK(spec.nodes[col_split].ratio == col_ratio + 10,
          "ratio: info sits in child[0], so the split ratio grows");

    LayoutResolved after;
    CHECK(layout_spec_resolve(&spec, &after) == 0, "ratio: resolve after height");
    const LayoutResolvedPane *b = find_resolved(&before, LAYOUT_PANE_INFO);
    const LayoutResolvedPane *a = find_resolved(&after, LAYOUT_PANE_INFO);
    CHECK(b && a && a->h > b->h,
          "ratio: info grew because child[0] ratio grew");

    /* 同一方向上，处在 child[1] 的窗格用同样的 ] 会变大。 */
    int controls_col = -1;
    for (int i = 0; i < spec.node_count; i++) {
        if (spec.nodes[i].is_leaf || spec.nodes[i].dir != LAYOUT_SPLIT_COL) {
            continue;
        }
        int second = spec.nodes[i].child[1];
        if (second >= 0 && spec.nodes[second].is_leaf &&
            spec.nodes[second].kind == LAYOUT_PANE_CONTROLS) {
            controls_col = i;
            break;
        }
    }
    CHECK(controls_col >= 0, "ratio: vertical split above controls found");
    if (controls_col >= 0) {
        LayoutResolved before_c;
        CHECK(layout_spec_resolve(&spec, &before_c) == 0,
              "ratio: resolve before controls adjust");
        int c_ratio = spec.nodes[controls_col].ratio;
        CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_CONTROLS,
                                       LAYOUT_SPLIT_COL, 10) == 0,
              "ratio: controls can grow vertically");
        CHECK(spec.nodes[controls_col].ratio == c_ratio - 10,
              "ratio: child[1] grows when the split ratio shrinks");
        LayoutResolved after_c;
        CHECK(layout_spec_resolve(&spec, &after_c) == 0,
              "ratio: resolve after controls adjust");
        const LayoutResolvedPane *cb =
            find_resolved(&before_c, LAYOUT_PANE_CONTROLS);
        const LayoutResolvedPane *ca =
            find_resolved(&after_c, LAYOUT_PANE_CONTROLS);
        CHECK(cb && ca && ca->h > cb->h, "ratio: controls became taller");
    }

    /* 祖先全是同一方向时，另一个方向没有可分栏。 */
    LayoutSpec flat;
    CHECK(layout_spec_build_fallback(&flat) == 0, "ratio: flat fallback");
    CHECK(layout_spec_remove_pane(&flat, LAYOUT_PANE_LYRICS) == 0,
          "ratio: drop lyrics");
    CHECK(layout_spec_remove_pane(&flat, LAYOUT_PANE_CONTROLS) == 0,
          "ratio: drop controls");
    CHECK(layout_spec_remove_pane(&flat, LAYOUT_PANE_SPECTRUM) == 0,
          "ratio: drop spectrum");
    CHECK(layout_spec_adjust_ratio(&flat, LAYOUT_PANE_INFO,
                                   LAYOUT_SPLIT_ROW, 10) == -2,
          "ratio: no row ancestor reports -2");
    CHECK(layout_spec_adjust_ratio(&flat, LAYOUT_PANE_INFO,
                                   LAYOUT_SPLIT_COL, 10) == 0,
          "ratio: col ancestor still adjustable");

    /* 整步会撞上 1% 下限时按可用步长饱和，而不是整步拒绝。 */
    LayoutSpec sat;
    CHECK(layout_spec_build_fallback(&sat) == 0, "ratio: saturating fallback");
    int sat_before = sat.nodes[sat.root].ratio;
    CHECK(layout_spec_adjust_ratio(&sat, LAYOUT_PANE_PLAYLIST,
                                   LAYOUT_SPLIT_ROW, 400) == 0,
          "ratio: an oversize step still applies");
    CHECK(sat.nodes[sat.root].ratio > sat_before &&
          sat.nodes[sat.root].ratio < 999,
          "ratio: the applied step saturated below the clamp");
    CHECK(layout_spec_validate(&sat, err, sizeof(err)) == 0,
          "ratio: saturated layout still tiles");

    /* 巨大反向 delta 也只走到"还能合法落位"的步长，绝不压破 1%。 */
    int before_big = sat.nodes[sat.root].ratio;
    CHECK(layout_spec_adjust_ratio(&sat, LAYOUT_PANE_PLAYLIST,
                                   LAYOUT_SPLIT_ROW, -999) == 0,
          "ratio: huge negative delta still applies a smaller step");
    CHECK(sat.nodes[sat.root].ratio < before_big &&
          sat.nodes[sat.root].ratio > 1,
          "ratio: it stopped at a legal ratio, not at the clamp");
    CHECK(layout_spec_validate(&sat, err, sizeof(err)) == 0,
          "ratio: the saturated step keeps the layout valid");
    CHECK(layout_spec_adjust_ratio(&sat, LAYOUT_PANE_PLAYLIST,
                                   LAYOUT_SPLIT_ROW, 0) == -1,
          "ratio: zero delta is refused");
}

/* 歌词与黑胶可以共存；摘掉歌词格后剩下的窗格仍铺满主区（运行时让位）。 */
static void test_coexistence(void)
{
    LayoutSpec spec;
    CHECK(layout_spec_build_fallback(&spec) == 0, "coexist: fallback built");
    CHECK(layout_spec_split_pane(&spec, LAYOUT_PANE_LYRICS, LAYOUT_SPLIT_COL,
                                 LAYOUT_PANE_VINYL) == 0,
          "coexist: vinyl can be added next to lyrics");
    CHECK(spec.panes[LAYOUT_PANE_LYRICS].enabled &&
          spec.panes[LAYOUT_PANE_VINYL].enabled,
          "coexist: both panes enabled");

    char err[256] = "";
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "coexist: a layout with both panes validates");
    LayoutResolved resolved;
    CHECK(layout_spec_resolve(&spec, &resolved) == 0 &&
          resolved.pane_count == 6,
          "coexist: all six panes resolve");

    LayoutSpec without = spec;
    CHECK(layout_spec_remove_pane(&without, LAYOUT_PANE_LYRICS) == 0,
          "coexist: lyrics can be removed for a track without lyrics");
    CHECK(layout_spec_validate(&without, err, sizeof(err)) == 0,
          "coexist: layout without lyrics still validates");
    LayoutResolved filled;
    CHECK(layout_spec_resolve(&without, &filled) == 0 &&
          filled.pane_count == 5,
          "coexist: five panes remain");
    long long area = 0;
    for (int i = 0; i < filled.pane_count; i++) {
        area += (long long)filled.panes[i].w * filled.panes[i].h;
    }
    CHECK(area == LAYOUT_AREA_TOTAL,
          "coexist: remaining panes fill the whole main area");

    /* 双格布局存档再读回，两格与颜色都要原样保留。 */
    LayoutCatalog dual;
    layout_catalog_init(&dual);
    dual.layouts[0] = spec;
    dual.count = 1;
    snprintf(dual.default_id, sizeof(dual.default_id), "%s", spec.id);
    char *text = layout_spec_serialize(&dual);
    CHECK(text != NULL, "coexist: dual layout serializes");
    if (text) {
        LayoutCatalog back;
        char serr[256] = "";
        CHECK(layout_spec_parse(text, strlen(text), &back, serr,
                                sizeof(serr)) == 0,
              "coexist: dual layout parses back");
        if (back.count == 1) {
            CHECK(back.layouts[0].panes[LAYOUT_PANE_LYRICS].enabled &&
                  back.layouts[0].panes[LAYOUT_PANE_VINYL].enabled,
                  "coexist: round-trip keeps both panes");
        }
        free(text);
    }

    const char *json = "{\"version\":2,\"default\":\"dual\",\"layouts\":["
                       "{\"id\":\"dual\",\"name\":\"Dual\",\"tree\":{\"split\":"
                       "\"row\",\"ratio\":500,\"children\":[{\"pane\":\"lyrics\"},"
                       "{\"pane\":\"vinyl\"}]}}]}";
    LayoutCatalog catalog;
    char perr[256] = "";
    CHECK(layout_spec_parse(json, strlen(json), &catalog, perr,
                            sizeof(perr)) == 0,
          "coexist: JSON with both panes parses");
    if (catalog.count == 1) {
        CHECK(catalog.layouts[0].panes[LAYOUT_PANE_LYRICS].enabled &&
              catalog.layouts[0].panes[LAYOUT_PANE_VINYL].enabled,
              "coexist: parsing keeps both panes");
        CHECK(catalog.layouts[0].normalized == 0,
              "coexist: keeping both is not counted as auto-fitting");
    }
}

/* 让位判定：两格同在时只看曲目有没有歌词，两个方向都要对。 */
static void test_lyrics_suppression_decision(void)
{
    LayoutSpec dual;
    CHECK(layout_spec_build_fallback(&dual) == 0, "suppress: fallback");
    CHECK(layout_spec_split_pane(&dual, LAYOUT_PANE_LYRICS, LAYOUT_SPLIT_COL,
                                 LAYOUT_PANE_VINYL) == 0,
          "suppress: dual layout built");
    CHECK(layout_spec_lyrics_pane_suppressed(&dual, 0) == 1,
          "suppress: no lyrics -> lyrics pane steps aside");
    CHECK(layout_spec_lyrics_pane_suppressed(&dual, 1) == 0,
          "suppress: with lyrics -> both panes stay");

    LayoutSpec lyrics_only;
    CHECK(layout_spec_build_fallback(&lyrics_only) == 0,
          "suppress: lyrics-only fallback");
    CHECK(layout_spec_remove_pane(&lyrics_only, LAYOUT_PANE_VINYL) == 0 ||
          1, "suppress: vinyl already absent");
    CHECK(layout_spec_remove_pane(&lyrics_only, LAYOUT_PANE_INFO) == 0 &&
          layout_spec_remove_pane(&lyrics_only, LAYOUT_PANE_CONTROLS) == 0 &&
          layout_spec_remove_pane(&lyrics_only, LAYOUT_PANE_SPECTRUM) == 0,
          "suppress: reduce to a lyrics-only layout");
    CHECK(layout_spec_pane_count(&lyrics_only) == 2,
          "suppress: playlist + lyrics remain");
    CHECK(layout_spec_lyrics_pane_suppressed(&lyrics_only, 0) == 0,
          "suppress: lyrics-only layout never evicts (in-pane fallback)");
    CHECK(layout_spec_lyrics_pane_suppressed(&lyrics_only, 1) == 0,
          "suppress: lyrics-only layout with lyrics stays");

    /* 布局里根本没有 lyrics 格（playlist + vinyl）：让位判定必须恒 0，
     * 不能因为"没有歌词"就去摘一个不存在的格子（会走 remove 失败兜底）。 */
    LayoutSpec no_lyrics = dual;
    CHECK(layout_spec_remove_pane(&no_lyrics, LAYOUT_PANE_LYRICS) == 0,
          "suppress: remove lyrics from dual");
    CHECK(layout_spec_remove_pane(&no_lyrics, LAYOUT_PANE_INFO) == 0 &&
          layout_spec_remove_pane(&no_lyrics, LAYOUT_PANE_CONTROLS) == 0 &&
          layout_spec_remove_pane(&no_lyrics, LAYOUT_PANE_SPECTRUM) == 0,
          "suppress: reduce to playlist + vinyl");
    CHECK(layout_spec_pane_count(&no_lyrics) == 2,
          "suppress: playlist + vinyl remain");
    CHECK(layout_spec_lyrics_pane_suppressed(&no_lyrics, 0) == 0,
          "suppress: layout without lyrics pane never evicts");
    CHECK(layout_spec_lyrics_pane_suppressed(&no_lyrics, 1) == 0,
          "suppress: layout without lyrics pane stable with lyrics");

    CHECK(layout_spec_lyrics_pane_suppressed(NULL, 0) == 0,
          "suppress: null spec is safe");

    /* 有歌词时解析回 6 格、无歌词时摘掉歌词格解析回 5 格且铺满。 */
    LayoutResolved both;
    CHECK(layout_spec_resolve(&dual, &both) == 0 && both.pane_count == 6,
          "suppress: with lyrics all six panes resolve");
    LayoutSpec evicted = dual;
    CHECK(layout_spec_remove_pane(&evicted, LAYOUT_PANE_LYRICS) == 0,
          "suppress: eviction applies");
    LayoutResolved after;
    CHECK(layout_spec_resolve(&evicted, &after) == 0 && after.pane_count == 5,
          "suppress: evicted layout resolves five panes");
    long long area = 0;
    for (int i = 0; i < after.pane_count; i++) {
        area += (long long)after.panes[i].w * after.panes[i].h;
    }
    CHECK(area == LAYOUT_AREA_TOTAL, "suppress: evicted layout fills the area");
}

/* 256 → 16 → 8 → 黑白 的量化：预设配色与主题色都走这一条。 */
static void test_colour_quantize(void)
{
    CHECK(layout_color_quantize(51, 256) == 51, "quantise: 256 colours keep value");
    CHECK(layout_color_quantize(51, 16) == 14, "quantise: 51 is bright cyan on 16");
    CHECK(layout_color_quantize(51, 8) == 6, "quantise: 51 is cyan on 8");
    CHECK(layout_color_quantize(252, 8) == 7, "quantise: light grey becomes white");
    CHECK(layout_color_quantize(203, 8) == 1, "quantise: salmon becomes red");
    CHECK(layout_color_quantize(207, 8) == 5, "quantise: 207 becomes magenta");
    CHECK(layout_color_quantize(7, 8) == 7, "quantise: in-range value kept");
    CHECK(layout_color_quantize(-1, 8) == -1, "quantise: transparent stays");
    CHECK(layout_color_quantize(-2, 8) == -1, "quantise: inherit stays unset");
    CHECK(layout_color_quantize(300, 256) == 255, "quantise: above 255 is clamped");
    CHECK(layout_color_quantize(3, 0) == -1, "quantise: mono terminals drop colour");
    /* 88 色终端：256 色下标不能原样透传，按 16 基色近似。 */
    CHECK(layout_color_quantize(208, 88) == 3, "quantise: 208 becomes yellow on 88");
    CHECK(layout_color_quantize(9, 88) == 9, "quantise: basic ANSI colours pass through");
}

/* 预设配色不变量：逐格显式、底色透明、同局两两不同、降到 8 色仍不同。 */
static void test_preset_colours(const LayoutCatalog *catalog)
{
    for (int i = 0; i < catalog->count; i++) {
        const LayoutSpec *layout = &catalog->layouts[i];
        int explicit_fg = 1;
        int transparent = 1;
        int border_inherit = 1;
        int distinct = 1;
        int distinct8 = 1;
        int distinct16 = 1;
        int seen8[8] = {0};
        int seen16[16] = {0};

        for (int k = 0; k < LAYOUT_PANE_COUNT; k++) {
            if (!layout->panes[k].enabled) {
                continue;
            }
            if (layout->panes[k].fg == LAYOUT_COLOR_INHERIT) {
                explicit_fg = 0;
            }
            if (layout->panes[k].bg != -1) {
                transparent = 0;
            }
            if (layout->panes[k].border_fg != LAYOUT_COLOR_INHERIT ||
                layout->panes[k].border_bg != LAYOUT_COLOR_INHERIT) {
                border_inherit = 0;
            }
            int q = layout_color_quantize(layout->panes[k].fg, 8);
            if (q >= 0 && q < 8) {
                if (seen8[q]) {
                    distinct8 = 0;
                }
                seen8[q] = 1;
            }
            int q16 = layout_color_quantize(layout->panes[k].fg, 16);
            if (q16 >= 0 && q16 < 16) {
                if (seen16[q16]) {
                    distinct16 = 0;
                }
                seen16[q16] = 1;
            }
        }
        for (int a = 0; a < LAYOUT_PANE_COUNT && distinct; a++) {
            if (!layout->panes[a].enabled) {
                continue;
            }
            for (int b = a + 1; b < LAYOUT_PANE_COUNT; b++) {
                if (layout->panes[b].enabled &&
                    layout->panes[a].fg == layout->panes[b].fg) {
                    distinct = 0;
                }
            }
        }
        char what[160];
        snprintf(what, sizeof(what), "%s: every pane has an explicit colour",
                 layout->id);
        CHECK(explicit_fg, what);
        snprintf(what, sizeof(what), "%s: every pane background is transparent",
                 layout->id);
        CHECK(transparent, what);
        snprintf(what, sizeof(what), "%s: borders follow the pane colour",
                 layout->id);
        CHECK(border_inherit, what);
        snprintf(what, sizeof(what), "%s: no two panes share a colour",
                 layout->id);
        CHECK(distinct, what);
        snprintf(what, sizeof(what),
                 "%s: colours stay distinct on 8-colour terminals", layout->id);
        CHECK(distinct8, what);
        snprintf(what, sizeof(what),
                 "%s: colours stay distinct on 16-colour terminals", layout->id);
        CHECK(distinct16, what);
    }
}


/* 旧版编辑器把预设的 name_key 清掉了：同名覆盖在合并前要能补回来。 */
static void test_preset_name_relink(void)
{
    LayoutCatalog presets;
    layout_catalog_init(&presets);
    LayoutSpec preset;
    CHECK(layout_spec_build_fallback(&preset) == 0, "relink: fallback");
    snprintf(preset.id, sizeof(preset.id), "%s", "vinyl");
    snprintf(preset.name, sizeof(preset.name), "%s", "Vinyl");
    snprintf(preset.name_key, sizeof(preset.name_key), "%s",
             "layout.preset.vinyl");
    presets.layouts[0] = preset;
    presets.count = 1;

    LayoutCatalog user;
    layout_catalog_init(&user);
    LayoutSpec broken = preset;
    broken.name_key[0] = '\0';          /* 旧版编辑器留下的样子 */
    user.layouts[0] = broken;
    user.count = 1;
    layout_catalog_adopt_preset_names(&user, &presets);
    CHECK(strcmp(user.layouts[0].name_key, "layout.preset.vinyl") == 0,
          "relink: the preset name key is restored");

    /* 用户改过名的覆盖不动。 */
    LayoutCatalog renamed;
    layout_catalog_init(&renamed);
    LayoutSpec custom = broken;
    snprintf(custom.name, sizeof(custom.name), "%s", "我的黑胶");
    renamed.layouts[0] = custom;
    renamed.count = 1;
    layout_catalog_adopt_preset_names(&renamed, &presets);
    CHECK(renamed.layouts[0].name_key[0] == '\0' &&
          strcmp(renamed.layouts[0].name, "我的黑胶") == 0,
          "relink: a renamed override keeps its own name");

    /* 自带 key 的、以及内置里没有的 id，都不改。 */
    LayoutCatalog own;
    layout_catalog_init(&own);
    LayoutSpec with_key = preset;
    snprintf(with_key.id, sizeof(with_key.id), "%s", "vinyl");
    own.layouts[0] = with_key;
    own.count = 1;
    layout_catalog_adopt_preset_names(&own, &presets);
    CHECK(strcmp(own.layouts[0].name_key, "layout.preset.vinyl") == 0,
          "relink: an existing name key is left alone");

    LayoutCatalog alien;
    layout_catalog_init(&alien);
    LayoutSpec user_made = preset;
    snprintf(user_made.id, sizeof(user_made.id), "%s", "user_1");
    snprintf(user_made.name, sizeof(user_made.name), "%s", "自定义布局 1");
    user_made.name_key[0] = '\0';
    alien.layouts[0] = user_made;
    alien.count = 1;
    layout_catalog_adopt_preset_names(&alien, &presets);
    CHECK(alien.layouts[0].name_key[0] == '\0',
          "relink: layouts without a preset id are untouched");

    /* 名字为空时连名字一起补。 */
    LayoutCatalog blank;
    layout_catalog_init(&blank);
    LayoutSpec unnamed = broken;
    unnamed.name[0] = '\0';
    blank.layouts[0] = unnamed;
    blank.count = 1;
    layout_catalog_adopt_preset_names(&blank, &presets);
    CHECK(strcmp(blank.layouts[0].name, "Vinyl") == 0 &&
          blank.layouts[0].name_key[0] != '\0',
          "relink: an empty name is filled from the preset");

    layout_catalog_adopt_preset_names(NULL, &presets);
    layout_catalog_adopt_preset_names(&user, NULL);
    CHECK(1, "relink: null arguments are safe");
}

int main(void)
{
    test_tree_parse();
    test_legacy_conversion();
    test_fallback_and_skip();
    test_serialize_roundtrip();
    test_tree_operations();
    test_catalog_merge();
    test_user_catalog_ops();
    test_validation_invariants();
    test_shipped_presets();
    test_pane_colours();
    test_copy_id();
    test_ratio_axis();
    test_coexistence();
    test_lyrics_suppression_decision();
    test_colour_quantize();
    test_preset_name_relink();
    {
        char path[512];
        const char *found = find_preset_file(path, sizeof(path));
        LayoutCatalog presets;
        layout_catalog_init(&presets);
        if (found) {
            char *text = NULL;
            size_t len = 0;
            char err[256] = "";
            if (load_text_file(found, &text, &len) == 0 && text &&
                layout_spec_parse(text, len, &presets, err, sizeof(err)) == 0) {
                test_preset_colours(&presets);
            }
            free(text);
        }
    }

    if (g_failures == 0) {
        printf("PASS layout_spec (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_spec (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
