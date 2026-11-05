/**
 * @file layout_spec.c
 * @brief TUI 主界面布局分栏树实现
 *
 * 布局以二叉分栏树为唯一权威模型：split 节点按 row/col 切开自己的矩形，
 * 叶子引用窗格种类。解析、编辑、运行时都从这棵树展开矩形，因此平铺是
 * 结构性保证，而不是事后校验。
 *
 * 旧版 v1 的自由矩形在解析层被转换成树；无法用 guillotine 切割表达的
 * 布局按阅读顺序重排。只有根 JSON/布局数组不可读或布局缺少合法 id 时
 * 才会跳过；几何、颜色、未知窗格等一律自动适配，不再拒绝整份布局。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/layout_spec.h"

#include "util/json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAYOUT_FILE_MAX   (256 * 1024)
#define LAYOUT_JSON_MAX   (256 * 1024)
#define LAYOUT_ERR_MAX    192

static void set_error(char *err, size_t err_size, const char *fmt, ...)
{
    if (!err || err_size == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int is_valid_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return 0;
    }
    size_t len = strlen(id);
    if (len >= LAYOUT_ID_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* ============================================================
 * 基本读写辅助
 * ============================================================ */

/* 读取必需字符串；返回 0 成功，-1 缺失/类型错误/超长。 */
static int read_required_string(JsonReader *reader, const char *key,
                                char *out, size_t out_size)
{
    out[0] = '\0';
    size_t written = json_get_string(reader, key, out, out_size);
    if (written == 0 || written >= out_size - 1) {
        return -1;
    }
    return 0;
}

/* 读取可选字符串；缺失返回 0，类型错误返回 -1。超长内容被安全截断。 */
static int read_optional_string(JsonReader *reader, const char *key,
                                char *out, size_t out_size)
{
    JsonValue value;
    out[0] = '\0';
    if (json_get_path(reader, key, &value) != 0) {
        return json_reader_ok(reader) ? 0 : -1;
    }
    if (value.type != JSON_VALUE_STRING) {
        return -1;
    }
    json_value_string(&value, out, out_size);
    return 0;
}

/* 读取可选整数；缺失用 def，类型错误用 def 并返回 -1。 */
static int read_optional_int(JsonReader *reader, const char *key, int def,
                             int *out)
{
    JsonValue value;
    *out = def;
    if (json_get_path(reader, key, &value) != 0) {
        return json_reader_ok(reader) ? 0 : -1;
    }
    if (value.type != JSON_VALUE_NUMBER) {
        return -1;
    }
    *out = (int)json_value_int(&value, def);
    return 0;
}

/* 颜色越界只夹取，不使布局失败。 */
static int read_color(JsonReader *reader, const char *key, int *out)
{
    int value = LAYOUT_COLOR_INHERIT;
    if (read_optional_int(reader, key, LAYOUT_COLOR_INHERIT, &value) != 0) {
        value = LAYOUT_COLOR_INHERIT;
    }
    *out = clamp_int(value, LAYOUT_COLOR_INHERIT, 255);
    return 0;
}

static int read_visible(JsonReader *reader, int *out)
{
    JsonValue value;
    *out = 1;
    if (json_get_path(reader, "visible", &value) != 0) {
        return json_reader_ok(reader) ? 0 : -1;
    }
    if (value.type == JSON_VALUE_BOOL) {
        *out = json_value_bool(&value, 1) != 0;
        return 0;
    }
    if (value.type == JSON_VALUE_NUMBER) {
        *out = json_value_int(&value, 1) != 0;
        return 0;
    }
    /* 无法解释的 visible：按可见处理，不拒绝布局。 */
    return 0;
}

const char *layout_pane_id_name(LayoutPaneKind kind)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST: return "playlist";
        case LAYOUT_PANE_INFO:     return "info";
        case LAYOUT_PANE_CONTROLS: return "controls";
        case LAYOUT_PANE_SPECTRUM: return "spectrum";
        case LAYOUT_PANE_LYRICS:   return "lyrics";
        case LAYOUT_PANE_VINYL:    return "vinyl";
        default:                   return "";
    }
}

int layout_pane_kind_from_name(const char *name, LayoutPaneKind *kind)
{
    if (!name || !kind) {
        return -1;
    }
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        if (strcmp(name, layout_pane_id_name((LayoutPaneKind)i)) == 0) {
            *kind = (LayoutPaneKind)i;
            return 0;
        }
    }
    return -1;
}

void layout_catalog_init(LayoutCatalog *catalog)
{
    if (catalog) {
        memset(catalog, 0, sizeof(*catalog));
    }
}

void layout_spec_init(LayoutSpec *spec)
{
    if (!spec) {
        return;
    }
    memset(spec, 0, sizeof(*spec));
    spec->root = -1;
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        spec->panes[i].fg = LAYOUT_COLOR_INHERIT;
        spec->panes[i].bg = LAYOUT_COLOR_INHERIT;
        spec->panes[i].border_fg = LAYOUT_COLOR_INHERIT;
        spec->panes[i].border_bg = LAYOUT_COLOR_INHERIT;
    }
}

/* ============================================================
 * 分栏树基础操作
 * ============================================================ */

static int node_alloc(LayoutSpec *spec, const LayoutNode *node)
{
    if (!spec || !node || spec->node_count >= LAYOUT_MAX_NODES) {
        return -1;
    }
    int index = spec->node_count++;
    spec->nodes[index] = *node;
    return index;
}

static int make_leaf(LayoutSpec *spec, LayoutPaneKind kind,
                     const LayoutPaneSpec *colors)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT) {
        return -1;
    }
    if (spec->panes[kind].enabled) {
        return -1;
    }
    if (colors) {
        spec->panes[kind] = *colors;
    }
    spec->panes[kind].enabled = 1;
    spec->pane_count++;
    LayoutNode node;
    memset(&node, 0, sizeof(node));
    node.is_leaf = 1;
    node.kind = kind;
    return node_alloc(spec, &node);
}

static int make_split(LayoutSpec *spec, LayoutSplitDir dir, int ratio,
                      int child0, int child1)
{
    if (!spec || child0 < 0 || child1 < 0) {
        return -1;
    }
    LayoutNode node;
    memset(&node, 0, sizeof(node));
    node.is_leaf = 0;
    node.dir = dir;
    node.ratio = clamp_int(ratio, 1, 999);
    node.child[0] = child0;
    node.child[1] = child1;
    return node_alloc(spec, &node);
}

int layout_spec_pane_count(const LayoutSpec *spec)
{
    if (!spec) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        if (spec->panes[i].enabled) {
            count++;
        }
    }
    return count;
}

static int collect_kinds(const LayoutSpec *spec, int index, LayoutPaneKind *out,
                         int capacity)
{
    if (!spec || index < 0 || index >= spec->node_count || capacity <= 0) {
        return -1;
    }
    const LayoutNode *node = &spec->nodes[index];
    if (node->is_leaf) {
        out[0] = node->kind;
        return 1;
    }
    int left = collect_kinds(spec, node->child[0], out, capacity);
    if (left < 0) {
        return -1;
    }
    int remain = capacity - left;
    if (remain <= 0) {
        return left;
    }
    int right = collect_kinds(spec, node->child[1], out + left, remain);
    if (right < 0) {
        return left;
    }
    return left + right;
}

int layout_spec_pane_at(const LayoutSpec *spec, int index)
{
    if (!spec || index < 0) {
        return -1;
    }
    LayoutPaneKind kinds[LAYOUT_MAX_PANES];
    int count = collect_kinds(spec, spec->root, kinds, LAYOUT_MAX_PANES);
    if (count < 0 || index >= count) {
        return -1;
    }
    return kinds[index];
}

int layout_spec_find_pane(const LayoutSpec *spec, LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT) {
        return -1;
    }
    for (int i = 0; i < spec->node_count; i++) {
        const LayoutNode *node = &spec->nodes[i];
        if (node->is_leaf && node->kind == kind) {
            return i;
        }
    }
    return -1;
}

const LayoutPaneSpec *layout_spec_pane_config(const LayoutSpec *spec,
                                              LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT ||
        !spec->panes[kind].enabled) {
        return NULL;
    }
    return &spec->panes[kind];
}

const LayoutPaneSpec *layout_spec_pane_colour(const LayoutSpec *spec,
                                              LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT) {
        return NULL;
    }
    return &spec->panes[kind];
}

int layout_spec_colour_effective(const LayoutSpec *spec, LayoutPaneKind kind,
                                 int field, int base, int *from_layout)
{
    const LayoutPaneSpec *pane;

    if (from_layout) {
        *from_layout = 0;
    }
    if (field != LAYOUT_COLOUR_FG && field != LAYOUT_COLOUR_BG) {
        return base;
    }
    pane = layout_spec_pane_colour(spec, kind);
    if (!pane) {
        return base;
    }
    int value = (field == LAYOUT_COLOUR_BG) ? pane->bg : pane->fg;
    if (value == LAYOUT_COLOR_INHERIT) {
        return base;
    }
    if (from_layout) {
        *from_layout = 1;
    }
    return value;
}

/* xterm 调色板 0-15 的 RGB；8 色终端只用到前 8 项。 */
static const int k_ansi_rgb[16][3] = {
    {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
    {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
    {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
};

/* 调色板下标 → RGB（16..231 是六级立方，232..255 是灰阶）。 */
void layout_color_rgb(int index, int *r, int *g, int *b)
{
    if (!r || !g || !b) {
        return;
    }
    if (index < 0) {
        index = 0;
    } else if (index > 255) {
        index = 255;
    }
    if (index < 16) {
        *r = k_ansi_rgb[index][0];
        *g = k_ansi_rgb[index][1];
        *b = k_ansi_rgb[index][2];
        return;
    }
    if (index < 232) {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        int i = index - 16;
        *r = levels[i / 36];
        *g = levels[(i / 6) % 6];
        *b = levels[i % 6];
        return;
    }
    int gray = 8 + 10 * (index - 232);
    *r = gray;
    *g = gray;
    *b = gray;
}

int layout_color_quantize(int value, int colors)
{
    if (value < 0) {
        return -1;
    }
    if (value > 255) {
        value = 255;
    }
    if (colors <= 0) {
        return -1;   /* 黑白终端：不启用颜色 */
    }
    if (colors >= 256) {
        return value;
    }

    int levels = (colors >= 16) ? 16 : 8;
    /* 0..15/0..7 是各终端一致的 ANSI 基本色，原样保留；其余按最近基色近似。
     * 不能拿 value < colors 判定：88 色终端的 45 号与 256 色终端的 45 号
     * 不是同一种颜色。 */
    if (value < levels) {
        return value;
    }
    int r = 0;
    int g = 0;
    int b = 0;
    layout_color_rgb(value, &r, &g, &b);
    int best = 0;
    long best_dist = -1;
    for (int i = 0; i < levels; i++) {
        long dr = r - k_ansi_rgb[i][0];
        long dg = g - k_ansi_rgb[i][1];
        long db = b - k_ansi_rgb[i][2];
        long dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

int layout_spec_lyrics_pane_suppressed(const LayoutSpec *spec,
                                      int lyrics_available)
{
    if (!spec || !spec->panes[LAYOUT_PANE_LYRICS].enabled ||
        !spec->panes[LAYOUT_PANE_VINYL].enabled) {
        return 0;
    }
    /* 两格同在、而当前曲目没有歌词：歌词格临时让位，余格补位。 */
    return lyrics_available ? 0 : 1;
}

int layout_spec_pane_available(const LayoutSpec *spec, LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT) {
        return 0;
    }
    /* 歌词与黑胶可以同时存在：运行时会按曲目有无歌词决定歌词格是否让位。 */
    return spec->panes[kind].enabled ? 0 : 1;
}

/* 查找 parent 与 slot；返回 1 找到，0 没有父节点，-1 参数错误。 */
static int find_parent(const LayoutSpec *spec, int target, int *parent_out,
                       int *slot_out)
{
    if (!spec || target < 0 || target >= spec->node_count) {
        return -1;
    }
    if (target == spec->root) {
        return 0;
    }
    for (int i = 0; i < spec->node_count; i++) {
        const LayoutNode *node = &spec->nodes[i];
        if (node->is_leaf) {
            continue;
        }
        for (int slot = 0; slot < 2; slot++) {
            if (node->child[slot] == target) {
                if (parent_out) *parent_out = i;
                if (slot_out) *slot_out = slot;
                return 1;
            }
        }
    }
    return -1;
}

/* 从 node 向上找第一个该方向的祖先分栏；slot 是该祖先里包含 node 的槽位。
 * 返回 1 找到，0 没有，-1 参数错误。 */
static int find_ancestor_split(const LayoutSpec *spec, int node,
                               LayoutSplitDir dir, int *parent_out,
                               int *slot_out)
{
    if (!spec || node < 0 || node >= spec->node_count ||
        (dir != LAYOUT_SPLIT_ROW && dir != LAYOUT_SPLIT_COL)) {
        return -1;
    }
    int current = node;
    for (int guard = 0; guard <= spec->node_count; guard++) {
        int parent = -1;
        int slot = 0;
        int found = find_parent(spec, current, &parent, &slot);
        if (found != 1 || parent < 0) {
            return 0;
        }
        if (spec->nodes[parent].dir == dir) {
            if (parent_out) *parent_out = parent;
            if (slot_out) *slot_out = slot;
            return 1;
        }
        current = parent;
    }
    return 0;
}

/* ============================================================
 * 解析 / 校验
 * ============================================================ */

static int resolve_node(const LayoutSpec *spec, int index,
                        int x0, int y0, int x1, int y1,
                        LayoutResolved *out)
{
    if (!spec || index < 0 || index >= spec->node_count ||
        !out || out->pane_count >= LAYOUT_MAX_PANES) {
        return -1;
    }
    const LayoutNode *node = &spec->nodes[index];
    if (node->is_leaf) {
        if (node->kind < 0 || node->kind >= LAYOUT_PANE_COUNT ||
            !spec->panes[node->kind].enabled) {
            return -1;
        }
        for (int i = 0; i < out->pane_count; i++) {
            if (out->panes[i].kind == node->kind) {
                return -1;   /* 重复窗格种类 */
            }
        }
        LayoutResolvedPane *pane = &out->panes[out->pane_count++];
        pane->kind = node->kind;
        pane->x = x0;
        pane->y = y0;
        pane->w = x1 - x0;
        pane->h = y1 - y0;
        return 0;
    }

    if (node->dir != LAYOUT_SPLIT_ROW && node->dir != LAYOUT_SPLIT_COL) {
        return -1;
    }
    if (node->ratio < 1 || node->ratio > 999) {
        return -1;
    }
    int span = (node->dir == LAYOUT_SPLIT_ROW) ? (x1 - x0) : (y1 - y0);
    if (span < 2) {
        return -1;
    }
    int cut = ((node->dir == LAYOUT_SPLIT_ROW) ? x0 : y0)
              + span * node->ratio / LAYOUT_AREA_UNITS;
    if (node->dir == LAYOUT_SPLIT_ROW) {
        cut = clamp_int(cut, x0 + 1, x1 - 1);
        if (resolve_node(spec, node->child[0], x0, y0, cut, y1, out) != 0 ||
            resolve_node(spec, node->child[1], cut, y0, x1, y1, out) != 0) {
            return -1;
        }
    } else {
        cut = clamp_int(cut, y0 + 1, y1 - 1);
        if (resolve_node(spec, node->child[0], x0, y0, x1, cut, out) != 0 ||
            resolve_node(spec, node->child[1], x0, cut, x1, y1, out) != 0) {
            return -1;
        }
    }
    return 0;
}

int layout_spec_resolve(const LayoutSpec *in, LayoutResolved *out)
{
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!in || in->root < 0 || in->root >= in->node_count) {
        return -1;
    }
    if (resolve_node(in, in->root, 0, 0,
                     LAYOUT_AREA_UNITS, LAYOUT_AREA_UNITS, out) != 0) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return out->pane_count > 0 ? 0 : -1;
}

int layout_spec_validate(const LayoutSpec *spec, char *err, size_t err_size)
{
    if (!spec) {
        set_error(err, err_size, "layout is null");
        return -1;
    }
    if (spec->root < 0 || spec->root >= spec->node_count) {
        set_error(err, err_size, "layout has no root node");
        return -1;
    }
    LayoutResolved resolved;
    if (layout_spec_resolve(spec, &resolved) != 0) {
        set_error(err, err_size, "layout tree cannot be resolved");
        return -1;
    }
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        if (spec->panes[i].enabled &&
            layout_spec_find_pane(spec, (LayoutPaneKind)i) < 0) {
            set_error(err, err_size,
                      "pane '%s' is enabled but not present in the tree",
                      layout_pane_id_name((LayoutPaneKind)i));
            return -1;
        }
    }

    LayoutPaneKind leaves[LAYOUT_MAX_PANES];
    int leaf_count = collect_kinds(spec, spec->root, leaves,
                                   LAYOUT_MAX_PANES);
    if (leaf_count <= 0 || leaf_count > LAYOUT_MAX_PANES) {
        set_error(err, err_size, "layout tree has an invalid leaf count");
        return -1;
    }
    for (int i = 0; i < leaf_count; i++) {
        if (!spec->panes[leaves[i]].enabled) {
            set_error(err, err_size,
                      "pane '%s' is in the tree but not enabled",
                      layout_pane_id_name(leaves[i]));
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (leaves[j] == leaves[i]) {
                set_error(err, err_size,
                          "pane '%s' appears more than once in the tree",
                          layout_pane_id_name(leaves[i]));
                return -1;
            }
        }
    }

    long long area_sum = 0;
    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutResolvedPane *pane = &resolved.panes[i];
        if (pane->x < 0 || pane->y < 0 || pane->w <= 0 || pane->h <= 0 ||
            pane->x + pane->w > LAYOUT_AREA_UNITS ||
            pane->y + pane->h > LAYOUT_AREA_UNITS) {
            set_error(err, err_size, "pane '%s' is outside the main area",
                      layout_pane_id_name(pane->kind));
            return -1;
        }
        area_sum += (long long)pane->w * pane->h;
    }

    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutResolvedPane *a = &resolved.panes[i];
        for (int j = i + 1; j < resolved.pane_count; j++) {
            const LayoutResolvedPane *b = &resolved.panes[j];
            int overlap = a->x < b->x + b->w && b->x < a->x + a->w &&
                          a->y < b->y + b->h && b->y < a->y + a->h;
            if (overlap) {
                set_error(err, err_size, "panes '%s' and '%s' overlap",
                          layout_pane_id_name(a->kind),
                          layout_pane_id_name(b->kind));
                return -1;
            }
        }
    }

    if (area_sum != LAYOUT_AREA_TOTAL) {
        set_error(err, err_size,
                  "layout does not tile the main area (area=%lld, expected=%d)",
                  area_sum, LAYOUT_AREA_TOTAL);
        return -1;
    }
    return 0;
}

/* 编辑器最小尺寸：每个窗格在两个轴上都至少 LAYOUT_MIN_PANE_UNITS。 */
static int all_panes_usable(const LayoutSpec *spec)
{
    LayoutResolved resolved;
    if (layout_spec_resolve(spec, &resolved) != 0) {
        return 0;
    }
    for (int i = 0; i < resolved.pane_count; i++) {
        if (resolved.panes[i].w < LAYOUT_MIN_PANE_UNITS ||
            resolved.panes[i].h < LAYOUT_MIN_PANE_UNITS) {
            return 0;
        }
    }
    return 1;
}

static int commit_if_valid(LayoutSpec *target, const LayoutSpec *trial,
                           int require_min_size)
{
    if (!target || !trial) {
        return -1;
    }
    char err[LAYOUT_ERR_MAX] = "";
    if (layout_spec_validate(trial, err, sizeof(err)) != 0) {
        return -1;
    }
    if (require_min_size && !all_panes_usable(trial)) {
        return -1;
    }
    *target = *trial;
    return 0;
}

/* ============================================================
 * 树编辑操作（事务式）
 * ============================================================ */

/* 从树中删除一个叶子并让兄弟接管；不校验、不检查最小尺寸。 */
static int remove_pane_inplace(LayoutSpec *spec, LayoutPaneKind kind);

int layout_spec_split_pane(LayoutSpec *spec, LayoutPaneKind target,
                           LayoutSplitDir dir, LayoutPaneKind new_kind)
{
    if (!spec || target < 0 || target >= LAYOUT_PANE_COUNT ||
        new_kind < 0 || new_kind >= LAYOUT_PANE_COUNT ||
        target == new_kind || dir > LAYOUT_SPLIT_COL ||
        !spec->panes[target].enabled ||
        !layout_spec_pane_available(spec, new_kind)) {
        return -1;
    }
    if (layout_spec_pane_count(spec) >= LAYOUT_MAX_PANES ||
        spec->node_count + 2 > LAYOUT_MAX_NODES) {
        return -1;
    }

    LayoutSpec trial = *spec;
    int index = layout_spec_find_pane(&trial, target);
    if (index < 0 || !trial.nodes[index].is_leaf) {
        return -1;
    }

    LayoutNode old_leaf = trial.nodes[index];
    LayoutNode new_leaf;
    memset(&new_leaf, 0, sizeof(new_leaf));
    new_leaf.is_leaf = 1;
    new_leaf.kind = new_kind;

    int old_index = node_alloc(&trial, &old_leaf);
    int new_index = node_alloc(&trial, &new_leaf);
    if (old_index < 0 || new_index < 0) {
        return -1;
    }

    LayoutNode *split = &trial.nodes[index];
    memset(split, 0, sizeof(*split));
    split->is_leaf = 0;
    split->dir = dir;
    split->ratio = 500;
    split->child[0] = old_index;
    split->child[1] = new_index;

    trial.panes[new_kind].enabled = 1;
    trial.pane_count++;
    trial.normalized = 0;
    return commit_if_valid(spec, &trial, 1);
}

int layout_spec_remove_pane(LayoutSpec *spec, LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT) {
        return -1;
    }
    LayoutSpec trial = *spec;
    if (remove_pane_inplace(&trial, kind) != 0) {
        return -1;
    }
    trial.normalized = 0;
    return commit_if_valid(spec, &trial, 1);
}

int layout_spec_change_pane_kind(LayoutSpec *spec, LayoutPaneKind old_kind,
                                 LayoutPaneKind new_kind)
{
    if (!spec || old_kind < 0 || old_kind >= LAYOUT_PANE_COUNT ||
        new_kind < 0 || new_kind >= LAYOUT_PANE_COUNT ||
        old_kind == new_kind || !spec->panes[old_kind].enabled) {
        return -1;
    }
    if (!layout_spec_pane_available(spec, new_kind)) {
        return -1;
    }
    LayoutSpec trial = *spec;
    int index = layout_spec_find_pane(&trial, old_kind);
    if (index < 0) {
        return -1;
    }
    trial.nodes[index].kind = new_kind;
    /* 换类型保留原窗格颜色，避免一次 t 把用户调好的配色清零。 */
    trial.panes[new_kind] = trial.panes[old_kind];
    trial.panes[new_kind].enabled = 1;
    trial.panes[old_kind].enabled = 0;
    trial.normalized = 0;
    return commit_if_valid(spec, &trial, 1);
}

int layout_spec_adjust_ratio(LayoutSpec *spec, LayoutPaneKind kind,
                             LayoutSplitDir dir, int delta)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT || delta == 0 ||
        !spec->panes[kind].enabled ||
        (dir != LAYOUT_SPLIT_ROW && dir != LAYOUT_SPLIT_COL)) {
        return -1;
    }
    int index = layout_spec_find_pane(spec, kind);
    if (index < 0) {
        return -1;
    }
    int parent = -1;
    int slot = 0;
    int found = find_ancestor_split(spec, index, dir, &parent, &slot);
    if (found < 0) {
        return -1;
    }
    if (found == 0) {
        return -2;   /* 这个方向没有祖先分栏可调 */
    }

    /* 整步会撞上"某格小于 1%"时按可用步长饱和：先试整步，不行再折半，
     * 直到 1 个单位；连 1 个单位都放不下才算到了极限。 */
    int base_ratio = spec->nodes[parent].ratio;
    int step = delta;
    while (step != 0) {
        int offset = (slot == 0) ? step : -step;
        int next = clamp_int(base_ratio + offset, 1, 999);
        if (next != base_ratio) {
            LayoutSpec trial = *spec;
            trial.nodes[parent].ratio = next;
            trial.normalized = 0;
            if (commit_if_valid(spec, &trial, 1) == 0) {
                return 0;
            }
        }
        step /= 2;
    }
    return -3;   /* 这个方向已经动不了了 */
}

int layout_spec_equalize_pane(LayoutSpec *spec, LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT ||
        !spec->panes[kind].enabled) {
        return -1;
    }
    int index = layout_spec_find_pane(spec, kind);
    if (index < 0) {
        return -1;
    }
    int parent = -1;
    int slot = 0;
    if (find_parent(spec, index, &parent, &slot) != 1 || parent < 0) {
        return -1;
    }
    LayoutSpec trial = *spec;
    trial.nodes[parent].ratio = 500;
    trial.normalized = 0;
    return commit_if_valid(spec, &trial, 1);
}

int layout_spec_swap_panes(LayoutSpec *spec, LayoutPaneKind a,
                           LayoutPaneKind b)
{
    if (!spec || a < 0 || a >= LAYOUT_PANE_COUNT ||
        b < 0 || b >= LAYOUT_PANE_COUNT || a == b ||
        !spec->panes[a].enabled || !spec->panes[b].enabled) {
        return -1;
    }
    LayoutSpec trial = *spec;
    int ai = layout_spec_find_pane(&trial, a);
    int bi = layout_spec_find_pane(&trial, b);
    if (ai < 0 || bi < 0) {
        return -1;
    }
    trial.nodes[ai].kind = b;
    trial.nodes[bi].kind = a;
    trial.normalized = 0;
    return commit_if_valid(spec, &trial, 1);
}

/* 这里实现 remove_pane_inplace 的递归重建。 */
static int rebuild_without(const LayoutSpec *spec, int index,
                           LayoutPaneKind drop, LayoutNode *out, int *count)
{
    if (!spec || index < 0 || index >= spec->node_count || !out || !count) {
        return -1;
    }
    const LayoutNode *node = &spec->nodes[index];
    if (node->is_leaf) {
        if (node->kind == drop) {
            return -1;
        }
        if (*count >= LAYOUT_MAX_NODES) {
            return -1;
        }
        out[*count] = *node;
        return (*count)++;
    }

    int left = rebuild_without(spec, node->child[0], drop, out, count);
    int right = rebuild_without(spec, node->child[1], drop, out, count);
    if (left < 0 && right < 0) {
        return -1;
    }
    if (left < 0) {
        return right;
    }
    if (right < 0) {
        return left;
    }
    if (*count >= LAYOUT_MAX_NODES) {
        return -1;
    }
    LayoutNode parent = *node;
    parent.child[0] = left;
    parent.child[1] = right;
    out[*count] = parent;
    return (*count)++;
}

static int remove_pane_inplace(LayoutSpec *spec, LayoutPaneKind kind)
{
    if (!spec || kind < 0 || kind >= LAYOUT_PANE_COUNT ||
        !spec->panes[kind].enabled || layout_spec_pane_count(spec) <= 1) {
        return -1;
    }
    LayoutNode rebuilt[LAYOUT_MAX_NODES];
    int count = 0;
    int root = rebuild_without(spec, spec->root, kind, rebuilt, &count);
    if (root < 0 || count <= 0) {
        return -1;
    }
    memcpy(spec->nodes, rebuilt, sizeof(rebuilt));
    if (count < LAYOUT_MAX_NODES) {
        memset(&spec->nodes[count], 0,
               (size_t)(LAYOUT_MAX_NODES - count) * sizeof(spec->nodes[0]));
    }
    spec->node_count = count;
    spec->root = root;
    spec->panes[kind].enabled = 0;
    spec->pane_count--;
    return 0;
}

int layout_spec_build_fallback(LayoutSpec *out)
{
    if (!out) {
        return -1;
    }
    layout_spec_init(out);
    snprintf(out->id, sizeof(out->id), "%s", "classic");
    snprintf(out->name, sizeof(out->name), "%s", "Classic");
    snprintf(out->name_key, sizeof(out->name_key), "%s",
             "layout.preset.classic");

    int playlist = make_leaf(out, LAYOUT_PANE_PLAYLIST, NULL);
    int info = make_leaf(out, LAYOUT_PANE_INFO, NULL);
    int controls = make_leaf(out, LAYOUT_PANE_CONTROLS, NULL);
    int spectrum = make_leaf(out, LAYOUT_PANE_SPECTRUM, NULL);
    int lyrics = make_leaf(out, LAYOUT_PANE_LYRICS, NULL);
    if (playlist < 0 || info < 0 || controls < 0 || spectrum < 0 ||
        lyrics < 0) {
        return -1;
    }
    int info_controls = make_split(out, LAYOUT_SPLIT_COL, 542, info, controls);
    int left = make_split(out, LAYOUT_SPLIT_COL, 520, playlist, info_controls);
    int right = make_split(out, LAYOUT_SPLIT_COL, 200, spectrum, lyrics);
    int root = make_split(out, LAYOUT_SPLIT_ROW, 680, left, right);
    if (info_controls < 0 || left < 0 || right < 0 || root < 0) {
        return -1;
    }
    out->root = root;
    out->normalized = 0;
    return 0;
}

/* ============================================================
 * v1 自由矩形 -> 分栏树导入
 * ============================================================ */

typedef struct {
    LayoutPaneKind kind;
    int x, y, w, h;
    int fg, bg, border_fg, border_bg;
    int clamped;
} LegacyPane;

static int ratio_for_cut(int start, int end, int cut)
{
    int span = end - start;
    if (span <= 0) {
        return 500;
    }
    int offset = cut - start;
    if (offset < 1) offset = 1;
    if (offset >= span) offset = span - 1;
    int ratio = (offset * LAYOUT_AREA_UNITS + span - 1) / span;
    return clamp_int(ratio, 1, 999);
}

static int build_guillotine(LayoutSpec *spec, const LegacyPane *panes,
                            const int *indices, int count,
                            int x0, int y0, int x1, int y1, int *normalized)
{
    if (!spec || !panes || !indices || count <= 0) {
        return -1;
    }
    if (count == 1) {
        const LegacyPane *pane = &panes[indices[0]];
        if (pane->x != x0 || pane->y != y0 ||
            pane->x + pane->w != x1 || pane->y + pane->h != y1) {
            *normalized = 1;
        }
        LayoutPaneSpec colors;
        memset(&colors, 0, sizeof(colors));
        colors.fg = pane->fg;
        colors.bg = pane->bg;
        colors.border_fg = pane->border_fg;
        colors.border_bg = pane->border_bg;
        return make_leaf(spec, pane->kind, &colors);
    }

    long long best_score = -1;
    int best_cut = -1;
    int best_vertical = 0;

    /* 垂直切线：候选只可能落在某个窗格的左右边上。 */
    for (int i = 0; i < count; i++) {
        const LegacyPane *pane = &panes[indices[i]];
        int candidates[2] = { pane->x, pane->x + pane->w };
        for (int c = 0; c < 2; c++) {
            int cut = candidates[c];
            if (cut <= x0 || cut >= x1) {
                continue;
            }
            int left_count = 0;
            int right_count = 0;
            int ok = 1;
            for (int j = 0; j < count; j++) {
                const LegacyPane *q = &panes[indices[j]];
                if (q->x + q->w <= cut) {
                    left_count++;
                } else if (q->x >= cut) {
                    right_count++;
                } else {
                    ok = 0;
                    break;
                }
            }
            if (!ok || left_count == 0 || right_count == 0) {
                continue;
            }
            long long area_left = (long long)(cut - x0) * (y1 - y0);
            long long area_right = (long long)(x1 - cut) * (y1 - y0);
            long long score = area_left < area_right ? area_left : area_right;
            if (score > best_score) {
                best_score = score;
                best_cut = cut;
                best_vertical = 1;
            }
        }
    }

    /* 水平切线。 */
    for (int i = 0; i < count; i++) {
        const LegacyPane *pane = &panes[indices[i]];
        int candidates[2] = { pane->y, pane->y + pane->h };
        for (int c = 0; c < 2; c++) {
            int cut = candidates[c];
            if (cut <= y0 || cut >= y1) {
                continue;
            }
            int top_count = 0;
            int bottom_count = 0;
            int ok = 1;
            for (int j = 0; j < count; j++) {
                const LegacyPane *q = &panes[indices[j]];
                if (q->y + q->h <= cut) {
                    top_count++;
                } else if (q->y >= cut) {
                    bottom_count++;
                } else {
                    ok = 0;
                    break;
                }
            }
            if (!ok || top_count == 0 || bottom_count == 0) {
                continue;
            }
            long long area_top = (long long)(cut - y0) * (x1 - x0);
            long long area_bottom = (long long)(y1 - cut) * (x1 - x0);
            long long score = area_top < area_bottom ? area_top : area_bottom;
            if (score > best_score) {
                best_score = score;
                best_cut = cut;
                best_vertical = 0;
            }
        }
    }

    if (best_cut < 0) {
        return -1;
    }

    int first[LAYOUT_MAX_PANES];
    int second[LAYOUT_MAX_PANES];
    int first_count = 0;
    int second_count = 0;
    for (int i = 0; i < count; i++) {
        const LegacyPane *pane = &panes[indices[i]];
        if (best_vertical) {
            if (pane->x + pane->w <= best_cut) {
                first[first_count++] = indices[i];
            } else {
                second[second_count++] = indices[i];
            }
        } else {
            if (pane->y + pane->h <= best_cut) {
                first[first_count++] = indices[i];
            } else {
                second[second_count++] = indices[i];
            }
        }
    }

    int child0;
    int child1;
    int ratio;
    if (best_vertical) {
        ratio = ratio_for_cut(x0, x1, best_cut);
        child0 = build_guillotine(spec, panes, first, first_count,
                                  x0, y0, best_cut, y1, normalized);
        child1 = build_guillotine(spec, panes, second, second_count,
                                  best_cut, y0, x1, y1, normalized);
    } else {
        ratio = ratio_for_cut(y0, y1, best_cut);
        child0 = build_guillotine(spec, panes, first, first_count,
                                  x0, y0, x1, best_cut, normalized);
        child1 = build_guillotine(spec, panes, second, second_count,
                                  x0, best_cut, x1, y1, normalized);
    }
    if (child0 < 0 || child1 < 0) {
        return -1;
    }
    return make_split(spec, best_vertical ? LAYOUT_SPLIT_ROW : LAYOUT_SPLIT_COL,
                      ratio, child0, child1);
}

static void sort_reading_order(const LegacyPane *panes, int *indices, int count)
{
    for (int i = 1; i < count; i++) {
        int key = indices[i];
        int j = i - 1;
        while (j >= 0) {
            const LegacyPane *a = &panes[indices[j]];
            const LegacyPane *b = &panes[key];
            int after = (a->y > b->y) || (a->y == b->y && a->x > b->x);
            if (!after) {
                break;
            }
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }
}

static int build_balanced(LayoutSpec *spec, const LegacyPane *panes,
                          const int *indices, int count,
                          int x0, int y0, int x1, int y1, int *normalized)
{
    if (!spec || !panes || !indices || count <= 0) {
        return -1;
    }
    if (count == 1) {
        const LegacyPane *pane = &panes[indices[0]];
        *normalized = 1;
        LayoutPaneSpec colors;
        memset(&colors, 0, sizeof(colors));
        colors.fg = pane->fg;
        colors.bg = pane->bg;
        colors.border_fg = pane->border_fg;
        colors.border_bg = pane->border_bg;
        return make_leaf(spec, pane->kind, &colors);
    }

    long long total_width = 0;
    long long total_height = 0;
    for (int i = 0; i < count; i++) {
        total_width += panes[indices[i]].w;
        total_height += panes[indices[i]].h;
    }
    int vertical = total_width >= total_height;
    int mid = count / 2;
    long long first_measure = 0;
    for (int i = 0; i < mid; i++) {
        first_measure += vertical ? panes[indices[i]].w : panes[indices[i]].h;
    }
    long long total_measure = vertical ? total_width : total_height;
    int ratio = total_measure > 0
        ? (int)((first_measure * LAYOUT_AREA_UNITS + total_measure - 1) /
                total_measure)
        : 500;
    ratio = clamp_int(ratio, 1, 999);

    int child0;
    int child1;
    if (vertical) {
        int cut = x0 + (x1 - x0) * ratio / LAYOUT_AREA_UNITS;
        cut = clamp_int(cut, x0 + 1, x1 - 1);
        child0 = build_balanced(spec, panes, indices, mid,
                                x0, y0, cut, y1, normalized);
        child1 = build_balanced(spec, panes, indices + mid, count - mid,
                                cut, y0, x1, y1, normalized);
    } else {
        int cut = y0 + (y1 - y0) * ratio / LAYOUT_AREA_UNITS;
        cut = clamp_int(cut, y0 + 1, y1 - 1);
        child0 = build_balanced(spec, panes, indices, mid,
                                x0, y0, x1, cut, normalized);
        child1 = build_balanced(spec, panes, indices + mid, count - mid,
                                x0, cut, x1, y1, normalized);
    }
    if (child0 < 0 || child1 < 0) {
        return -1;
    }
    *normalized = 1;
    return make_split(spec, vertical ? LAYOUT_SPLIT_ROW : LAYOUT_SPLIT_COL,
                      ratio, child0, child1);
}

static int legacy_rect_matches(const LegacyPane *pane,
                               const LayoutResolved *resolved)
{
    for (int i = 0; i < resolved->pane_count; i++) {
        if (resolved->panes[i].kind == pane->kind) {
            return resolved->panes[i].x == pane->x &&
                   resolved->panes[i].y == pane->y &&
                   resolved->panes[i].w == pane->w &&
                   resolved->panes[i].h == pane->h;
        }
    }
    return 0;
}

static int import_legacy_panes(LayoutSpec *spec, const LegacyPane *panes,
                               int count, int force_normalized)
{
    if (!spec || !panes || count <= 0) {
        return -1;
    }

    LayoutSpec trial;
    layout_spec_init(&trial);
    snprintf(trial.id, sizeof(trial.id), "%s", spec->id);
    snprintf(trial.name, sizeof(trial.name), "%s", spec->name);
    snprintf(trial.name_key, sizeof(trial.name_key), "%s", spec->name_key);

    int indices[LAYOUT_MAX_PANES];
    for (int i = 0; i < count; i++) {
        indices[i] = i;
    }

    int normalized = force_normalized;
    int root = build_guillotine(&trial, panes, indices, count,
                                0, 0, LAYOUT_AREA_UNITS, LAYOUT_AREA_UNITS,
                                &normalized);
    if (root < 0) {
        layout_spec_init(&trial);
        snprintf(trial.id, sizeof(trial.id), "%s", spec->id);
        snprintf(trial.name, sizeof(trial.name), "%s", spec->name);
        snprintf(trial.name_key, sizeof(trial.name_key), "%s", spec->name_key);
        sort_reading_order(panes, indices, count);
        normalized = 1;
        root = build_balanced(&trial, panes, indices, count,
                              0, 0, LAYOUT_AREA_UNITS, LAYOUT_AREA_UNITS,
                              &normalized);
    }
    if (root < 0) {
        return -1;
    }
    trial.root = root;

    if (!normalized) {
        LayoutResolved resolved;
        if (layout_spec_resolve(&trial, &resolved) != 0) {
            return -1;
        }
        for (int i = 0; i < count; i++) {
            if (!legacy_rect_matches(&panes[i], &resolved)) {
                normalized = 1;
                break;
            }
        }
    }
    trial.normalized = normalized;

    char err[LAYOUT_ERR_MAX] = "";
    if (layout_spec_validate(&trial, err, sizeof(err)) != 0) {
        return -1;
    }
    *spec = trial;
    return 0;
}

/* ============================================================
 * JSON 解析：v2 分栏树 + v1 矩形
 * ============================================================ */

static int parse_tree_node(const JsonValue *value, LayoutSpec *spec, int depth);

static int parse_leaf(const JsonValue *value, LayoutSpec *spec, int depth)
{
    (void)depth;
    if (!value || value->type != JSON_VALUE_OBJECT || !spec) {
        return -1;
    }
    JsonReader reader;
    json_reader_init(&reader, value->start, value->length);

    char id[LAYOUT_ID_MAX];
    if (read_required_string(&reader, "pane", id, sizeof(id)) != 0) {
        return -1;
    }
    LayoutPaneKind kind;
    if (layout_pane_kind_from_name(id, &kind) != 0) {
        spec->normalized = 1;
        return -1;
    }
    if (spec->panes[kind].enabled) {
        spec->normalized = 1;
        return -1;
    }
    LayoutPaneSpec colors;
    memset(&colors, 0, sizeof(colors));
    colors.fg = LAYOUT_COLOR_INHERIT;
    colors.bg = LAYOUT_COLOR_INHERIT;
    colors.border_fg = LAYOUT_COLOR_INHERIT;
    colors.border_bg = LAYOUT_COLOR_INHERIT;
    read_color(&reader, "fg", &colors.fg);
    read_color(&reader, "bg", &colors.bg);
    read_color(&reader, "border_fg", &colors.border_fg);
    read_color(&reader, "border_bg", &colors.border_bg);
    if (!json_reader_ok(&reader)) {
        spec->normalized = 1;
    }
    return make_leaf(spec, kind, &colors);
}

static int parse_children(const JsonValue *children, LayoutSpec *spec,
                          int depth, int *child0, int *child1)
{
    if (!children || children->type != JSON_VALUE_ARRAY || !spec ||
        !child0 || !child1) {
        return -1;
    }
    JsonReader reader;
    json_reader_init(&reader, children->start, children->length);
    if (json_reader_enter(&reader, children) != 0) {
        return -1;
    }
    JsonValue child;
    int count = 0;
    int rc;
    while ((rc = json_array_next(&reader, &child)) == 1) {
        if (count >= 2) {
            spec->normalized = 1;   /* 多余子节点忽略 */
            continue;
        }
        int parsed = parse_tree_node(&child, spec, depth + 1);
        if (count == 0) {
            *child0 = parsed;
        } else {
            *child1 = parsed;
        }
        count++;
    }
    if (rc < 0 || !json_reader_ok(&reader)) {
        return -1;
    }
    if (count >= 2) {
        return 0;
    }
    if (count == 1) {
        spec->normalized = 1;
        *child1 = -1;
        return 0;
    }
    return -1;
}

static int parse_tree_node(const JsonValue *value, LayoutSpec *spec, int depth)
{
    if (!value || value->type != JSON_VALUE_OBJECT || !spec ||
        depth > LAYOUT_MAX_NODES) {
        return -1;
    }
    JsonReader reader;
    json_reader_init(&reader, value->start, value->length);

    JsonValue pane_value;
    if (json_get_path(&reader, "pane", &pane_value) == 0) {
        return parse_leaf(value, spec, depth);
    }

    JsonValue split_value;
    char dir_name[16];
    if (json_get_path(&reader, "split", &split_value) != 0 ||
        split_value.type != JSON_VALUE_STRING) {
        spec->normalized = 1;
        return -1;
    }
    json_value_string(&split_value, dir_name, sizeof(dir_name));
    LayoutSplitDir dir;
    if (strcmp(dir_name, "row") == 0) {
        dir = LAYOUT_SPLIT_ROW;
    } else if (strcmp(dir_name, "col") == 0) {
        dir = LAYOUT_SPLIT_COL;
    } else {
        dir = LAYOUT_SPLIT_ROW;
        spec->normalized = 1;
    }

    int ratio = 500;
    if (read_optional_int(&reader, "ratio", 500, &ratio) != 0) {
        ratio = 500;
        spec->normalized = 1;
    }
    if (ratio < 1 || ratio > 999) {
        spec->normalized = 1;
    }
    ratio = clamp_int(ratio, 1, 999);

    JsonValue children;
    if (json_get_path(&reader, "children", &children) != 0 ||
        children.type != JSON_VALUE_ARRAY) {
        spec->normalized = 1;
        return -1;
    }
    int child0 = -1;
    int child1 = -1;
    if (parse_children(&children, spec, depth, &child0, &child1) != 0) {
        spec->normalized = 1;
        return -1;
    }
    if (child0 < 0 && child1 < 0) {
        return -1;
    }
    if (child0 < 0) {
        return child1;
    }
    if (child1 < 0) {
        return child0;
    }
    return make_split(spec, dir, ratio, child0, child1);
}

static int parse_legacy_panes(JsonReader *reader, LayoutSpec *layout)
{
    JsonValue panes_value;
    if (json_get_path(reader, "panes", &panes_value) != 0 ||
        panes_value.type != JSON_VALUE_ARRAY) {
        return -1;
    }
    JsonReader panes;
    json_reader_init(&panes, panes_value.start, panes_value.length);
    if (json_reader_enter(&panes, &panes_value) != 0) {
        return -1;
    }

    LegacyPane parsed[LAYOUT_MAX_PANES];
    int count = 0;
    int force_normalized = 0;
    JsonValue pane_value;
    int rc;
    while ((rc = json_array_next(&panes, &pane_value)) == 1) {
        if (pane_value.type != JSON_VALUE_OBJECT) {
            force_normalized = 1;
            continue;
        }
        JsonReader pr;
        json_reader_init(&pr, pane_value.start, pane_value.length);

        char id[LAYOUT_ID_MAX];
        if (read_required_string(&pr, "id", id, sizeof(id)) != 0) {
            force_normalized = 1;
            continue;
        }
        LayoutPaneKind kind;
        if (layout_pane_kind_from_name(id, &kind) != 0) {
            force_normalized = 1;
            continue;
        }
        int visible = 1;
        if (read_visible(&pr, &visible) != 0 || !visible) {
            force_normalized = 1;
            continue;
        }

        int x = 0, y = 0, w = 0, h = 0;
        if (read_optional_int(&pr, "x", 0, &x) != 0 ||
            read_optional_int(&pr, "y", 0, &y) != 0 ||
            read_optional_int(&pr, "w", 0, &w) != 0 ||
            read_optional_int(&pr, "h", 0, &h) != 0 ||
            w <= 0 || h <= 0) {
            force_normalized = 1;
            continue;
        }

        LegacyPane pane;
        memset(&pane, 0, sizeof(pane));
        pane.kind = kind;
        pane.fg = LAYOUT_COLOR_INHERIT;
        pane.bg = LAYOUT_COLOR_INHERIT;
        pane.border_fg = LAYOUT_COLOR_INHERIT;
        pane.border_bg = LAYOUT_COLOR_INHERIT;
        read_color(&pr, "fg", &pane.fg);
        read_color(&pr, "bg", &pane.bg);
        read_color(&pr, "border_fg", &pane.border_fg);
        read_color(&pr, "border_bg", &pane.border_bg);

        int ox = x;
        int oy = y;
        int ow = w;
        int oh = h;
        x = clamp_int(x, 0, LAYOUT_AREA_UNITS - 1);
        y = clamp_int(y, 0, LAYOUT_AREA_UNITS - 1);
        w = clamp_int(w, 1, LAYOUT_AREA_UNITS - x);
        h = clamp_int(h, 1, LAYOUT_AREA_UNITS - y);
        if (x != ox || y != oy || w != ow || h != oh) {
            force_normalized = 1;
        }
        pane.x = x;
        pane.y = y;
        pane.w = w;
        pane.h = h;

        int skip = 0;
        for (int i = 0; i < count; i++) {
            if (parsed[i].kind == kind) {
                skip = 1;
                break;
            }
        }
        if (skip) {
            force_normalized = 1;
            continue;
        }
        if (count >= LAYOUT_MAX_PANES) {
            force_normalized = 1;
            break;
        }
        parsed[count++] = pane;
    }
    if (rc < 0 || !json_reader_ok(&panes)) {
        return -1;
    }
    if (count <= 0) {
        return -1;
    }
    return import_legacy_panes(layout, parsed, count, force_normalized);
}

static void apply_classic_identity(LayoutSpec *layout)
{
    char id[LAYOUT_ID_MAX];
    char name[LAYOUT_NAME_MAX];
    char name_key[LAYOUT_KEY_MAX];
    snprintf(id, sizeof(id), "%s", layout->id);
    snprintf(name, sizeof(name), "%s", layout->name);
    snprintf(name_key, sizeof(name_key), "%s", layout->name_key);
    layout_spec_build_fallback(layout);
    if (id[0] != '\0') {
        snprintf(layout->id, sizeof(layout->id), "%s", id);
    }
    if (name[0] != '\0') {
        snprintf(layout->name, sizeof(layout->name), "%s", name);
    }
    if (name_key[0] != '\0') {
        snprintf(layout->name_key, sizeof(layout->name_key), "%s", name_key);
    }
    layout->normalized = 1;
}

/* 以树为准同步 enabled 标记；旧的 panes 镜像里可能多出树中没有的窗格。 */
static void sync_enabled_panes(LayoutSpec *layout)
{
    int present[LAYOUT_PANE_COUNT] = {0};
    for (int i = 0; i < layout->node_count; i++) {
        const LayoutNode *node = &layout->nodes[i];
        if (node->is_leaf && node->kind >= 0 &&
            node->kind < LAYOUT_PANE_COUNT) {
            present[node->kind] = 1;
        }
    }
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        layout->panes[i].enabled = present[i];
    }
}

static int finalize_layout(LayoutSpec *layout)
{
    sync_enabled_panes(layout);
    for (int guard = 0; guard < LAYOUT_MAX_PANES; guard++) {
        char err[LAYOUT_ERR_MAX] = "";
        if (layout_spec_validate(layout, err, sizeof(err)) == 0) {
            return 0;
        }
        if (layout_spec_pane_count(layout) <= 1) {
            break;
        }
        LayoutResolved resolved;
        if (layout_spec_resolve(layout, &resolved) != 0) {
            break;
        }
        int smallest = 0;
        long long smallest_area = (long long)resolved.panes[0].w *
                                  resolved.panes[0].h;
        for (int i = 1; i < resolved.pane_count; i++) {
            long long area = (long long)resolved.panes[i].w *
                             resolved.panes[i].h;
            if (area < smallest_area) {
                smallest_area = area;
                smallest = i;
            }
        }
        if (remove_pane_inplace(layout, resolved.panes[smallest].kind) != 0) {
            break;
        }
        layout->normalized = 1;
    }
    char err[LAYOUT_ERR_MAX] = "";
    return layout_spec_validate(layout, err, sizeof(err)) == 0 ? 0 : -1;
}

static int parse_layout_object(const char *json, size_t len, LayoutSpec *layout,
                               char *err, size_t err_size)
{
    if (!json || !layout) {
        return -1;
    }
    layout_spec_init(layout);

    JsonReader reader;
    json_reader_init(&reader, json, len);

    if (read_required_string(&reader, "id", layout->id,
                             sizeof(layout->id)) != 0 ||
        !is_valid_id(layout->id)) {
        set_error(err, err_size, "missing or invalid layout id");
        return -1;
    }
    if (read_optional_string(&reader, "name", layout->name,
                             sizeof(layout->name)) != 0) {
        layout->name[0] = '\0';
        layout->normalized = 1;
    }
    if (read_optional_string(&reader, "name_key", layout->name_key,
                             sizeof(layout->name_key)) != 0) {
        layout->name_key[0] = '\0';
        layout->normalized = 1;
    }
    if (!json_reader_ok(&reader)) {
        set_error(err, err_size, "malformed layout object");
        return -1;
    }

    JsonValue tree;
    int have_tree = 0;
    if (json_get_path(&reader, "tree", &tree) == 0 &&
        tree.type == JSON_VALUE_OBJECT) {
        have_tree = 1;
        int root = parse_tree_node(&tree, layout, 0);
        if (root >= 0) {
            layout->root = root;
        }
    }

    if (layout->root < 0) {
        if (have_tree) {
            layout->normalized = 1;
        }
        /* 旧版 v1 或 v2 的矩形镜像。 */
        LayoutSpec legacy;
        layout_spec_init(&legacy);
        snprintf(legacy.id, sizeof(legacy.id), "%s", layout->id);
        snprintf(legacy.name, sizeof(legacy.name), "%s", layout->name);
        snprintf(legacy.name_key, sizeof(legacy.name_key), "%s",
                 layout->name_key);
        legacy.normalized = layout->normalized;
        if (parse_legacy_panes(&reader, &legacy) == 0) {
            legacy.normalized |= layout->normalized;
            *layout = legacy;
        }
    }

    if (layout->root < 0) {
        apply_classic_identity(layout);
        return 0;
    }

    sync_enabled_panes(layout);
    if (finalize_layout(layout) != 0) {
        apply_classic_identity(layout);
    }
    return 0;
}

/* ============================================================
 * 目录解析
 * ============================================================ */

int layout_spec_parse(const char *json, size_t len, LayoutCatalog *out,
                      char *err, size_t err_size)
{
    if (!json || !out || len == 0) {
        set_error(err, err_size, "empty layout data");
        return -1;
    }
    layout_catalog_init(out);

    JsonReader reader;
    json_reader_init(&reader, json, len);
    json_get_string(&reader, "default", out->default_id,
                    sizeof(out->default_id));

    JsonValue layouts_value;
    if (json_get_path(&reader, "layouts", &layouts_value) != 0 ||
        layouts_value.type != JSON_VALUE_ARRAY) {
        set_error(err, err_size, "missing 'layouts' array");
        return -1;
    }

    JsonReader layouts;
    json_reader_init(&layouts, layouts_value.start, layouts_value.length);
    if (json_reader_enter(&layouts, &layouts_value) != 0) {
        set_error(err, err_size, "malformed 'layouts' array");
        return -1;
    }

    JsonValue item;
    int rc;
    while ((rc = json_array_next(&layouts, &item)) == 1) {
        if (out->count >= LAYOUT_MAX_CATALOG) {
            out->skipped++;
            continue;
        }
        if (item.type != JSON_VALUE_OBJECT) {
            out->skipped++;
            continue;
        }
        LayoutSpec layout;
        char layout_err[LAYOUT_ERR_MAX] = "";
        if (parse_layout_object(item.start, item.length, &layout,
                                layout_err, sizeof(layout_err)) != 0) {
            out->skipped++;
            if (err && err_size && err[0] == '\0') {
                set_error(err, err_size, "%s", layout_err);
            }
            continue;
        }
        int duplicate = 0;
        for (int i = 0; i < out->count; i++) {
            if (strcmp(out->layouts[i].id, layout.id) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            out->skipped++;
            continue;
        }
        if (layout.normalized) {
            out->adjusted++;
        }
        out->layouts[out->count++] = layout;
    }

    if (rc < 0 || !json_reader_ok(&layouts)) {
        set_error(err, err_size, "malformed layouts array");
        return -1;
    }
    if (out->count == 0) {
        set_error(err, err_size, "no readable layouts");
        return -1;
    }
    if (out->default_id[0] == '\0' ||
        !layout_catalog_find(out, out->default_id)) {
        snprintf(out->default_id, sizeof(out->default_id), "%s",
                 out->layouts[0].id);
    }
    return 0;
}

int layout_spec_parse_file(const char *path, LayoutCatalog *out,
                           char *err, size_t err_size)
{
    if (!path || !out) {
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_error(err, err_size, "cannot open '%s'", path);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_error(err, err_size, "cannot seek '%s'", path);
        return -1;
    }
    long size = ftell(file);
    if (size <= 0 || size > LAYOUT_FILE_MAX) {
        fclose(file);
        set_error(err, err_size, "invalid layout file size: %s", path);
        return -1;
    }
    rewind(file);
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        set_error(err, err_size, "out of memory reading '%s'", path);
        return -1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[got] = '\0';
    if (got != (size_t)size) {
        free(buffer);
        set_error(err, err_size, "short read: %s", path);
        return -1;
    }
    int result = layout_spec_parse(buffer, got, out, err, err_size);
    free(buffer);
    return result;
}

/* ============================================================
 * 合并 / 序列化 / 文件写入
 * ============================================================ */

int layout_catalog_merge(LayoutCatalog *dst, const LayoutCatalog *src)
{
    if (!dst || !src) {
        return dst ? dst->count : 0;
    }
    for (int i = 0; i < src->count; i++) {
        const LayoutSpec *layout = &src->layouts[i];
        int found = -1;
        for (int j = 0; j < dst->count; j++) {
            if (strcmp(dst->layouts[j].id, layout->id) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            dst->layouts[found] = *layout;
        } else if (dst->count < LAYOUT_MAX_CATALOG) {
            dst->layouts[dst->count++] = *layout;
        }
    }
    if (src->default_id[0] != '\0' &&
        layout_catalog_find(dst, src->default_id)) {
        snprintf(dst->default_id, sizeof(dst->default_id), "%s",
                 src->default_id);
    }
    return dst->count;
}

const LayoutSpec *layout_catalog_find(const LayoutCatalog *catalog,
                                      const char *id)
{
    if (!catalog || !id) {
        return NULL;
    }
    for (int i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->layouts[i].id, id) == 0) {
            return &catalog->layouts[i];
        }
    }
    return NULL;
}

void layout_catalog_adopt_preset_names(LayoutCatalog *incoming,
                                       const LayoutCatalog *existing)
{
    if (!incoming || !existing) {
        return;
    }
    for (int i = 0; i < incoming->count; i++) {
        LayoutSpec *layout = &incoming->layouts[i];
        if (layout->name_key[0]) {
            continue;
        }
        const LayoutSpec *preset = layout_catalog_find(existing, layout->id);
        if (!preset || !preset->name_key[0]) {
            continue;
        }
        if (layout->name[0] && strcmp(layout->name, preset->name) != 0) {
            continue;   /* 用户改过名：保留自定义名，不挂预设键 */
        }
        snprintf(layout->name_key, sizeof(layout->name_key), "%s",
                 preset->name_key);
        if (!layout->name[0]) {
            snprintf(layout->name, sizeof(layout->name), "%s", preset->name);
        }
    }
}

int layout_catalog_next_copy_id(const LayoutCatalog *catalog, const char *base_id,
                                char *out, size_t out_size)
{
    if (!catalog || !base_id || !base_id[0] || !out || out_size == 0) {
        return -1;
    }
    for (int n = 2; n <= 99; n++) {
        char candidate[LAYOUT_ID_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s-%d", base_id, n);
        if (written <= 0 || (size_t)written >= sizeof(candidate)) {
            /* 基础 id 已经快到上限，序号只会更长：直接判失败，不截断。 */
            return -1;
        }
        if (!layout_catalog_find(catalog, candidate) && is_valid_id(candidate)) {
            snprintf(out, out_size, "%s", candidate);
            return 0;
        }
    }
    return -1;
}

static size_t append_color_fields(char *out, size_t out_size, size_t pos,
                                  const LayoutPaneSpec *pane)
{
    if (pane->fg != LAYOUT_COLOR_INHERIT) {
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "fg");
        pos = json_append_int(out, out_size, pos, pane->fg);
    }
    if (pane->bg != LAYOUT_COLOR_INHERIT) {
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "bg");
        pos = json_append_int(out, out_size, pos, pane->bg);
    }
    if (pane->border_fg != LAYOUT_COLOR_INHERIT) {
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "border_fg");
        pos = json_append_int(out, out_size, pos, pane->border_fg);
    }
    if (pane->border_bg != LAYOUT_COLOR_INHERIT) {
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "border_bg");
        pos = json_append_int(out, out_size, pos, pane->border_bg);
    }
    return pos;
}

static size_t append_tree_json(char *out, size_t out_size, size_t pos,
                               const LayoutSpec *layout, int index)
{
    const LayoutNode *node = &layout->nodes[index];
    pos = json_append_raw(out, out_size, pos, "{");
    if (node->is_leaf) {
        pos = json_append_key(out, out_size, pos, "pane");
        pos = json_append_escaped(out, out_size, pos,
                                  layout_pane_id_name(node->kind));
        pos = append_color_fields(out, out_size, pos,
                                  &layout->panes[node->kind]);
        return json_append_raw(out, out_size, pos, "}");
    }
    pos = json_append_key(out, out_size, pos, "split");
    pos = json_append_escaped(out, out_size, pos,
                              node->dir == LAYOUT_SPLIT_ROW ? "row" : "col");
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "ratio");
    pos = json_append_int(out, out_size, pos, node->ratio);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "children");
    pos = json_append_raw(out, out_size, pos, "[");
    pos = append_tree_json(out, out_size, pos, layout, node->child[0]);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = append_tree_json(out, out_size, pos, layout, node->child[1]);
    pos = json_append_raw(out, out_size, pos, "]}");
    return pos;
}

static size_t append_mirror_pane(char *out, size_t out_size, size_t pos,
                                 const LayoutSpec *layout,
                                 const LayoutResolvedPane *pane)
{
    pos = json_append_raw(out, out_size, pos, "{\"id\":");
    pos = json_append_escaped(out, out_size, pos,
                              layout_pane_id_name(pane->kind));
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "x");
    pos = json_append_int(out, out_size, pos, pane->x);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "y");
    pos = json_append_int(out, out_size, pos, pane->y);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "w");
    pos = json_append_int(out, out_size, pos, pane->w);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "h");
    pos = json_append_int(out, out_size, pos, pane->h);
    pos = append_color_fields(out, out_size, pos, &layout->panes[pane->kind]);
    return json_append_raw(out, out_size, pos, "}");
}

char *layout_spec_serialize(const LayoutCatalog *catalog)
{
    if (!catalog || catalog->count <= 0) {
        return NULL;
    }
    char *out = malloc(LAYOUT_JSON_MAX);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, "{\"version\":2");
    if (catalog->default_id[0] != '\0') {
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"default\":");
        pos = json_append_escaped(out, LAYOUT_JSON_MAX, pos,
                                  catalog->default_id);
    }
    pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"layouts\":[");

    for (int i = 0; i < catalog->count; i++) {
        const LayoutSpec *layout = &catalog->layouts[i];
        LayoutResolved resolved;
        if (layout->root < 0 ||
            layout_spec_resolve(layout, &resolved) != 0) {
            free(out);
            return NULL;
        }
        if (i > 0) {
            pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",");
        }
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, "{\"id\":");
        pos = json_append_escaped(out, LAYOUT_JSON_MAX, pos, layout->id);
        if (layout->name[0] != '\0') {
            pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"name\":");
            pos = json_append_escaped(out, LAYOUT_JSON_MAX, pos, layout->name);
        }
        if (layout->name_key[0] != '\0') {
            pos = json_append_raw(out, LAYOUT_JSON_MAX, pos,
                                  ",\"name_key\":");
            pos = json_append_escaped(out, LAYOUT_JSON_MAX, pos,
                                      layout->name_key);
        }
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"tree\":");
        pos = append_tree_json(out, LAYOUT_JSON_MAX, pos, layout, layout->root);
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"panes\":[");
        for (int j = 0; j < resolved.pane_count; j++) {
            if (j > 0) {
                pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",");
            }
            pos = append_mirror_pane(out, LAYOUT_JSON_MAX, pos, layout,
                                     &resolved.panes[j]);
        }
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, "]}");
    }
    pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, "]}");

    if (json_writer_overflowed(LAYOUT_JSON_MAX, pos)) {
        free(out);
        return NULL;
    }
    return out;
}

int layout_spec_write_file(const char *path, const LayoutCatalog *catalog,
                           char *err, size_t err_size)
{
    if (!path || path[0] == '\0' || !catalog) {
        set_error(err, err_size, "invalid write target");
        return -1;
    }
    char *json = layout_spec_serialize(catalog);
    if (!json) {
        set_error(err, err_size,
                  "cannot serialize layouts (empty or too large)");
        return -1;
    }

    size_t path_len = strlen(path);
    char *tmp = malloc(path_len + sizeof(".tmp"));
    if (!tmp) {
        set_error(err, err_size, "out of memory");
        free(json);
        return -1;
    }
    snprintf(tmp, path_len + sizeof(".tmp"), "%s.tmp", path);
    FILE *file = fopen(tmp, "wb");
    if (!file) {
        set_error(err, err_size, "cannot open '%s' for writing", tmp);
        free(tmp);
        free(json);
        return -1;
    }
    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, file);
    int write_ok = (written == len && fflush(file) == 0);
    if (fclose(file) != 0) {
        write_ok = 0;
    }
    int rc = 0;
    if (!write_ok) {
        set_error(err, err_size, "short write on '%s'", tmp);
        remove(tmp);
        rc = -1;
    }
    free(json);
    if (rc != 0) {
        free(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        set_error(err, err_size, "cannot rename '%s' to '%s'", tmp, path);
        remove(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int layout_user_catalog_upsert(LayoutCatalog *user, const LayoutSpec *layout)
{
    if (!user || !layout || !is_valid_id(layout->id)) {
        return -1;
    }
    for (int i = 0; i < user->count; i++) {
        if (strcmp(user->layouts[i].id, layout->id) == 0) {
            user->layouts[i] = *layout;
            return user->count;
        }
    }
    if (user->count >= LAYOUT_MAX_CATALOG) {
        return -1;
    }
    user->layouts[user->count++] = *layout;
    return user->count;
}

int layout_user_catalog_remove(LayoutCatalog *user, const char *id)
{
    if (!user || !id) {
        return -1;
    }
    int found = -1;
    for (int i = 0; i < user->count; i++) {
        if (strcmp(user->layouts[i].id, id) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return user->count;
    }
    for (int i = found; i < user->count - 1; i++) {
        user->layouts[i] = user->layouts[i + 1];
    }
    user->count--;
    memset(&user->layouts[user->count], 0, sizeof(LayoutSpec));
    if (user->default_id[0] != '\0' && strcmp(user->default_id, id) == 0) {
        user->default_id[0] = '\0';
    }
    return user->count;
}
