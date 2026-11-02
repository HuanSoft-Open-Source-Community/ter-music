/**
 * @file test_layout_spec.c
 * @brief ui/layout_spec.c 的单元测试
 *
 * 覆盖 v2 分栏树解析、v1 自由矩形自动适配、强制平铺不变量、树编辑操作、
 * 序列化往返、目录合并与仓库预设。重点断言：几何/颜色/越界/空洞/重叠
 * 不再导致整份布局被拒绝。
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
          !catalog.layouts[0].panes[LAYOUT_PANE_LYRICS].enabled,
          "vinyl wins over lyrics");
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
    CHECK(layout_spec_pane_available(&spec, LAYOUT_PANE_LYRICS) == 0,
          "lyrics is unavailable while vinyl exists");
    CHECK(layout_spec_change_pane_kind(&spec, LAYOUT_PANE_VINYL,
                                       LAYOUT_PANE_LYRICS) == 0,
          "vinyl can be replaced back by lyrics");
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "kind replacement keeps force tiling");

    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST, 10) == 0,
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
                                 LAYOUT_SPLIT_COL, LAYOUT_PANE_VINYL) == -1,
          "lyrics/vinyl conflict blocks splitting");
    CHECK(layout_spec_pane_available(&spec, LAYOUT_PANE_VINYL) == 0,
          "vinyl unavailable while lyrics exists");

    /* 逐级放大到最小尺寸边界，失败后原布局必须仍然有效。 */
    for (int i = 0; i < 200; i++) {
        if (layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST, 10) != 0) {
            break;
        }
    }
    CHECK(layout_spec_validate(&spec, err, sizeof(err)) == 0,
          "failed ratio adjustments leave a valid layout");
    CHECK(layout_spec_adjust_ratio(&spec, LAYOUT_PANE_PLAYLIST, 500) == -1,
          "panes cannot be squeezed below the minimum");

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
    CHECK(layout_spec_adjust_ratio(&spec, (LayoutPaneKind)only, 10) == -1,
          "root pane has no parent split");
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

    if (g_failures == 0) {
        printf("PASS layout_spec (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_spec (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
