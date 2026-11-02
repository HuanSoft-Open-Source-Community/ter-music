/**
 * @file layout_spec.c
 * @brief TUI 主界面布局 JSON 规范实现
 *
 * 只负责解析/合并/严格平铺校验，不感知 ncurses。布局的可见性回退
 * （vinyl 可见时隐藏 lyrics）在这里完成，使几何层和运行时拿到的都是
 * “最终可见窗格”，从而可以要求它们严格铺满主区域。
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

#define LAYOUT_FILE_MAX (256 * 1024)

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

static int read_required_string(JsonReader *reader, const char *key,
                                char *out, size_t out_size,
                                char *err, size_t err_size)
{
    size_t written = json_get_string(reader, key, out, out_size);
    if (written == 0 || written >= out_size - 1) {
        set_error(err, err_size, "missing or overlong string '%s'", key);
        return -1;
    }
    return 0;
}

static int read_optional_string(JsonReader *reader, const char *key,
                                char *out, size_t out_size,
                                char *err, size_t err_size)
{
    JsonValue value;
    out[0] = '\0';
    if (json_get_path(reader, key, &value) != 0) {
        return 0;
    }
    if (value.type != JSON_VALUE_STRING) {
        set_error(err, err_size, "'%s' must be a string", key);
        return -1;
    }
    size_t written = json_value_string(&value, out, out_size);
    if (written >= out_size - 1) {
        set_error(err, err_size, "'%s' is too long", key);
        return -1;
    }
    return 0;
}

static int read_required_int(JsonReader *reader, const char *key, int *out,
                             char *err, size_t err_size)
{
    JsonValue value;
    if (json_get_path(reader, key, &value) != 0 ||
        value.type != JSON_VALUE_NUMBER) {
        set_error(err, err_size, "missing or non-numeric '%s'", key);
        return -1;
    }
    *out = (int)json_value_int(&value, 0);
    return 0;
}

static int read_optional_int(JsonReader *reader, const char *key, int def,
                             int *out, char *err, size_t err_size)
{
    JsonValue value;
    *out = def;
    if (json_get_path(reader, key, &value) != 0) {
        return 0;
    }
    if (value.type != JSON_VALUE_NUMBER) {
        set_error(err, err_size, "'%s' must be a number", key);
        return -1;
    }
    *out = (int)json_value_int(&value, def);
    return 0;
}

static int read_visible(JsonReader *reader, int *out,
                        char *err, size_t err_size)
{
    JsonValue value;
    *out = 1;
    if (json_get_path(reader, "visible", &value) != 0) {
        return 0;
    }
    if (value.type == JSON_VALUE_BOOL) {
        *out = json_value_bool(&value, 1);
        return 0;
    }
    if (value.type == JSON_VALUE_NUMBER) {
        *out = json_value_int(&value, 1) != 0;
        return 0;
    }
    set_error(err, err_size, "'visible' must be bool or 0/1");
    return -1;
}

static int read_color(JsonReader *reader, const char *key, int *out,
                      char *err, size_t err_size)
{
    if (read_optional_int(reader, key, LAYOUT_COLOR_INHERIT, out,
                          err, err_size) != 0) {
        return -1;
    }
    if (*out < LAYOUT_COLOR_INHERIT || *out > 255) {
        set_error(err, err_size, "'%s' must be -2,-1 or 0..255", key);
        return -1;
    }
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

static int parse_pane(JsonReader *reader, LayoutPaneSpec *pane,
                      char *err, size_t err_size)
{
    char id[LAYOUT_ID_MAX];
    if (read_required_string(reader, "id", id, sizeof(id),
                             err, err_size) != 0) {
        return -1;
    }

    LayoutPaneKind kind;
    if (layout_pane_kind_from_name(id, &kind) != 0) {
        /* 未知窗格跳过，而不是让整份布局失败；调用方会继续读取下一个。 */
        return 1;
    }

    memset(pane, 0, sizeof(*pane));
    pane->kind = kind;
    pane->fg = LAYOUT_COLOR_INHERIT;
    pane->bg = LAYOUT_COLOR_INHERIT;
    pane->border_fg = LAYOUT_COLOR_INHERIT;
    pane->border_bg = LAYOUT_COLOR_INHERIT;

    if (read_visible(reader, &pane->visible, err, err_size) != 0 ||
        read_required_int(reader, "x", &pane->x, err, err_size) != 0 ||
        read_required_int(reader, "y", &pane->y, err, err_size) != 0 ||
        read_required_int(reader, "w", &pane->w, err, err_size) != 0 ||
        read_required_int(reader, "h", &pane->h, err, err_size) != 0 ||
        read_color(reader, "fg", &pane->fg, err, err_size) != 0 ||
        read_color(reader, "bg", &pane->bg, err, err_size) != 0 ||
        read_color(reader, "border_fg", &pane->border_fg, err, err_size) != 0 ||
        read_color(reader, "border_bg", &pane->border_bg, err, err_size) != 0) {
        return -1;
    }

    if (pane->x < 0 || pane->y < 0 || pane->w <= 0 || pane->h <= 0 ||
        pane->x + pane->w > LAYOUT_AREA_UNITS ||
        pane->y + pane->h > LAYOUT_AREA_UNITS) {
        set_error(err, err_size,
                  "pane '%s' rect %d,%d,%d,%d is outside 0..1000",
                  id, pane->x, pane->y, pane->w, pane->h);
        return -1;
    }

    return 0;
}

static int parse_layout_object(const char *json, size_t len, LayoutSpec *layout,
                               char *err, size_t err_size)
{
    JsonReader reader;
    json_reader_init(&reader, json, len);

    memset(layout, 0, sizeof(*layout));
    if (read_required_string(&reader, "id", layout->id, sizeof(layout->id),
                             err, err_size) != 0) {
        return -1;
    }
    if (!is_valid_id(layout->id)) {
        set_error(err, err_size, "invalid layout id '%s'", layout->id);
        return -1;
    }
    if (read_optional_string(&reader, "name", layout->name,
                             sizeof(layout->name), err, err_size) != 0 ||
        read_optional_string(&reader, "name_key", layout->name_key,
                             sizeof(layout->name_key), err, err_size) != 0) {
        return -1;
    }

    JsonValue panes_value;
    if (json_get_path(&reader, "panes", &panes_value) != 0 ||
        panes_value.type != JSON_VALUE_ARRAY) {
        set_error(err, err_size, "layout '%s' has no 'panes' array",
                  layout->id);
        return -1;
    }

    JsonReader panes;
    json_reader_init(&panes, panes_value.start, panes_value.length);
    if (json_reader_enter(&panes, &panes_value) != 0) {
        set_error(err, err_size, "layout '%s': malformed 'panes'",
                  layout->id);
        return -1;
    }

    int seen[LAYOUT_PANE_COUNT] = {0};
    JsonValue pane_value;
    int rc;
    while ((rc = json_array_next(&panes, &pane_value)) == 1) {
        if (pane_value.type != JSON_VALUE_OBJECT) {
            set_error(err, err_size, "layout '%s': pane must be an object",
                      layout->id);
            return -1;
        }

        JsonReader pane_reader;
        json_reader_init(&pane_reader, pane_value.start, pane_value.length);
        LayoutPaneSpec pane;
        int prc = parse_pane(&pane_reader, &pane, err, err_size);
        if (prc < 0) {
            return -1;
        }
        if (prc > 0) {
            continue;   /* 未知窗格：跳过 */
        }
        if (seen[pane.kind]) {
            continue;   /* 重复窗格：跳过 */
        }
        if (layout->pane_count >= LAYOUT_MAX_PANES) {
            set_error(err, err_size, "layout '%s' has too many panes",
                      layout->id);
            return -1;
        }
        seen[pane.kind] = 1;
        layout->panes[layout->pane_count++] = pane;
    }
    if (rc < 0 || !json_reader_ok(&panes)) {
        set_error(err, err_size, "layout '%s': malformed panes array",
                  layout->id);
        return -1;
    }
    if (layout->pane_count == 0) {
        set_error(err, err_size, "layout '%s' has no known panes",
                  layout->id);
        return -1;
    }
    return 0;
}

int layout_spec_validate(const LayoutSpec *spec, char *err, size_t err_size)
{
    if (!spec) {
        set_error(err, err_size, "layout is null");
        return -1;
    }

    LayoutSpec resolved;
    layout_spec_resolve(spec, &resolved);

    int visible_count = 0;
    long long area_sum = 0;
    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutPaneSpec *pane = &resolved.panes[i];
        if (!pane->visible) {
            continue;
        }
        if (pane->x < 0 || pane->y < 0 || pane->w <= 0 || pane->h <= 0 ||
            pane->x + pane->w > LAYOUT_AREA_UNITS ||
            pane->y + pane->h > LAYOUT_AREA_UNITS) {
            set_error(err, err_size, "pane '%s' is outside the main area",
                      layout_pane_id_name(pane->kind));
            return -1;
        }
        visible_count++;
        area_sum += (long long)pane->w * pane->h;
    }
    if (visible_count == 0) {
        set_error(err, err_size, "layout has no visible panes");
        return -1;
    }

    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutPaneSpec *a = &resolved.panes[i];
        if (!a->visible) {
            continue;
        }
        for (int j = i + 1; j < resolved.pane_count; j++) {
            const LayoutPaneSpec *b = &resolved.panes[j];
            if (!b->visible) {
                continue;
            }
            int overlap = a->x < b->x + b->w && b->x < a->x + a->w &&
                          a->y < b->y + b->h && b->y < a->y + a->h;
            if (overlap) {
                set_error(err, err_size,
                          "panes '%s' and '%s' overlap",
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

    int version = (int)json_get_int(&reader, "version", 1);
    if (version < 1) {
        version = 1;
    }
    (void)version;

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
            out->rejected++;
            continue;
        }
        if (item.type != JSON_VALUE_OBJECT) {
            out->rejected++;
            continue;
        }

        LayoutSpec layout;
        char layout_err[192] = "";
        if (parse_layout_object(item.start, item.length, &layout,
                                layout_err, sizeof(layout_err)) != 0 ||
            layout_spec_validate(&layout, layout_err, sizeof(layout_err)) != 0) {
            out->rejected++;
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
            out->rejected++;
            continue;
        }
        out->layouts[out->count++] = layout;
    }

    if (rc < 0 || !json_reader_ok(&layouts)) {
        set_error(err, err_size, "malformed layouts array");
        return -1;
    }

    if (out->count == 0) {
        set_error(err, err_size, "no valid layouts");
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

void layout_spec_resolve(const LayoutSpec *in, LayoutSpec *out)
{
    if (!out) {
        return;
    }
    if (!in) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = *in;

    int has_vinyl = 0;
    for (int i = 0; i < out->pane_count; i++) {
        if (out->panes[i].kind == LAYOUT_PANE_VINYL && out->panes[i].visible) {
            has_vinyl = 1;
            break;
        }
    }
    if (!has_vinyl) {
        return;
    }
    for (int i = 0; i < out->pane_count; i++) {
        if (out->panes[i].kind == LAYOUT_PANE_LYRICS) {
            out->panes[i].visible = 0;
        }
    }
}

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

    if (src->default_id[0] != '\0' && layout_catalog_find(dst, src->default_id)) {
        snprintf(dst->default_id, sizeof(dst->default_id), "%s",
                 src->default_id);
    }
    return dst->count;
}

int layout_spec_build_fallback(LayoutSpec *out)
{
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", "classic");
    snprintf(out->name, sizeof(out->name), "%s", "Classic");
    snprintf(out->name_key, sizeof(out->name_key), "%s", "layout.preset.classic");

    static const LayoutPaneSpec panes[] = {
        { LAYOUT_PANE_PLAYLIST, 1,   0,   0, 680, 520, -2, -2, -2, -2 },
        { LAYOUT_PANE_INFO,     1,   0, 520, 680, 260, -2, -2, -2, -2 },
        { LAYOUT_PANE_CONTROLS, 1,   0, 780, 680, 220, -2, -2, -2, -2 },
        { LAYOUT_PANE_SPECTRUM, 1, 680,   0, 320, 200, -2, -2, -2, -2 },
        { LAYOUT_PANE_LYRICS,   1, 680, 200, 320, 800, -2, -2, -2, -2 },
    };
    out->pane_count = (int)(sizeof(panes) / sizeof(panes[0]));
    memcpy(out->panes, panes, sizeof(panes));
    return 0;
}

const LayoutSpec *layout_catalog_find(const LayoutCatalog *catalog, const char *id)
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

/* ============================================================
 * 序列化（布局编辑器保存）；与解析层互逆，风格对齐内置预设文件
 * ============================================================ */

#define LAYOUT_JSON_MAX (256 * 1024)

static size_t append_pane_json(char *out, size_t out_size, size_t pos,
                               const LayoutPaneSpec *pane)
{
    pos = json_append_raw(out, out_size, pos, "{\"id\":\"");
    pos = json_append_raw(out, out_size, pos,
                          layout_pane_id_name(pane->kind));
    pos = json_append_raw(out, out_size, pos, "\"");
    /* 写出器不自动补成员分隔逗号，每个键前必须显式带 "," */
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
    if (!pane->visible) {
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "visible");
        pos = json_append_bool(out, out_size, pos, 0);
    }
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
    pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, "{\"version\":1");
    if (catalog->default_id[0] != '\0') {
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"default\":");
        pos = json_append_escaped(out, LAYOUT_JSON_MAX, pos,
                                  catalog->default_id);
    }
    pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"layouts\":[");

    for (int i = 0; i < catalog->count; i++) {
        const LayoutSpec *layout = &catalog->layouts[i];
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
        pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",\"panes\":[");
        for (int j = 0; j < layout->pane_count; j++) {
            if (j > 0) {
                pos = json_append_raw(out, LAYOUT_JSON_MAX, pos, ",");
            }
            pos = append_pane_json(out, LAYOUT_JSON_MAX, pos,
                                   &layout->panes[j]);
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

    /* 先写临时文件再原子改名：中途崩溃不会留下半截 JSON */
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
    int rc = 0;
    if (written != len || fflush(file) != 0 || fclose(file) != 0) {
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
        return user->count;   /* 不存在：无动作 */
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
