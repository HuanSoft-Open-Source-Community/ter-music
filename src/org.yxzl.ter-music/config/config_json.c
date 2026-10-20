/**
 * @file config_json.c
 * @brief AppConfig 与 JSON 的双向编解码（D-Bus Config 接口用）
 *
 * 键名与 XML 元素名逐字相同（见 config/schema.h），因此
 * config.xml、Config.GetAll/Set 与 ConfigChanged 三处的字段名一致，
 * 前端不需要维护第二套命名。
 *
 * 两条约定：
 *  - 渲染（config_render_json）输出全部字段，分区与 config.xml 相同：
 *    paths / theme / preferences / equalizer。
 *  - 应用（config_apply_json）只接受已知键与已知分区，任何未知键即整体
 *    失败，避免“改了一半”的配置落盘。应用是原子的：先改副本，全部成功
 *    后再提交。
 *
 * 远程音乐源（服务器列表与密码）不属于核心配置：自 config v6 起由前端
 * 自己保存（见 remote/ 与 README 的“前端远程”一节），因此补丁里出现
 * remote_connections 会被整体拒绝。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "config/config_json.h"

#include "config/config.h"

#include "config/config_fields.h"
#include "config/schema.h"
#include "keymap/actions.h"
#include "logger/logger.h"
#include "util/json.h"
#include "util/keyname.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *const k_sections[] = {
    "paths", "theme", "preferences", "equalizer", "keybindings", NULL
};

static int compare_override_ptr(const void *lhs, const void *rhs)
{
    const KeyBindingOverride *a = *(const KeyBindingOverride *const *)lhs;
    const KeyBindingOverride *b = *(const KeyBindingOverride *const *)rhs;
    return strcmp(a->action, b->action);
}

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t len = src ? strlen(src) : 0;
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    if (len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static int field_clamp(const ConfigFieldDef *def, int value)
{
    if (def->max >= def->min && def->max > 0) {
        if (value < def->min) return def->min;
        if (value > def->max) return def->max;
    }
    return value;
}

/* ── 渲染 ───────────────────────────────────────────────────────── */

int config_render_json(const AppConfig *cfg, char *out, size_t out_size)
{
    if (!cfg || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "version");
    pos = json_append_int(out, out_size, pos, cfg->config_version);

    for (int s = 0; k_sections[s] != NULL; s++) {
        const char *section = k_sections[s];
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, section);

        if (strcmp(section, "equalizer") == 0) {
            pos = json_append_char(out, out_size, pos, '{');
            pos = json_append_key(out, out_size, pos, "enabled");
            pos = json_append_int(out, out_size, pos, cfg->eq_enabled);
            pos = json_append_raw(out, out_size, pos, ",");
            pos = json_append_key(out, out_size, pos, "preamp");
            pos = json_append_int(out, out_size, pos, cfg->eq_preamp);
            pos = json_append_raw(out, out_size, pos, ",");
            pos = json_append_key(out, out_size, pos, "bands");
            pos = json_append_char(out, out_size, pos, '[');
            for (int i = 0; i < EQ_BAND_COUNT; i++) {
                if (i > 0) pos = json_append_char(out, out_size, pos, ',');
                pos = json_append_int(out, out_size, pos, cfg->eq_band_gains[i]);
            }
            pos = json_append_char(out, out_size, pos, ']');
            pos = json_append_char(out, out_size, pos, '}');
            continue;
        }

        if (strcmp(section, "keybindings") == 0) {
            pos = json_append_char(out, out_size, pos, '{');

            int stored = cfg->keybinding_override_count;
            if (stored < 0) stored = 0;
            if (stored > MAX_KEYBINDING_OVERRIDES) stored = MAX_KEYBINDING_OVERRIDES;

            const KeyBindingOverride *list[MAX_KEYBINDING_OVERRIDES];
            int count = 0;
            for (int i = 0; i < stored; i++) {
                const KeyBindingOverride *entry = &cfg->keybinding_overrides[i];
                if (entry->action[0] == '\0' || entry->key[0] == '\0') {
                    continue;
                }
                int duplicate = 0;
                for (int j = 0; j < count; j++) {
                    if (strcmp(list[j]->action, entry->action) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    list[count++] = entry;
                }
            }
            qsort(list, (size_t)count, sizeof(list[0]), compare_override_ptr);

            for (int i = 0; i < count; i++) {
                if (i > 0) pos = json_append_char(out, out_size, pos, ',');
                pos = json_append_key(out, out_size, pos, list[i]->action);
                pos = json_append_escaped(out, out_size, pos, list[i]->key);
            }
            pos = json_append_char(out, out_size, pos, '}');
            continue;
        }

        pos = json_append_char(out, out_size, pos, '{');
        int written = 0;
        for (int i = 0; i < FIELD_COUNT; i++) {
            const ConfigFieldEntry *entry = &k_fields[i];
            if (strcmp(entry->section, section) != 0) {
                continue;
            }
            if (written > 0) {
                pos = json_append_raw(out, out_size, pos, ",");
            }
            pos = json_append_key(out, out_size, pos, entry->field.key);

            const char *base = (const char *)cfg;
            switch (entry->field.type) {
                case CFG_INT:
                    pos = json_append_int(out, out_size, pos,
                                          *(const int *)(base + entry->field.offset));
                    break;
                case CFG_FLOAT:
                    pos = json_append_double(out, out_size, pos,
                                             *(const float *)(base + entry->field.offset), 2);
                    break;
                case CFG_STRING:
                default:
                    pos = json_append_escaped(out, out_size, pos,
                                              base + entry->field.offset);
                    break;
            }
            written++;
        }
        pos = json_append_char(out, out_size, pos, '}');
    }

    pos = json_append_char(out, out_size, pos, '}');
    out[pos] = '\0';
    return (int)pos;
}

/* ── 应用（局部 JSON 对象） ─────────────────────────────────────── */

static const ConfigFieldDef *find_field(const char *key, const char **section_out)
{
    for (int i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(k_fields[i].field.key, key) == 0) {
            if (section_out) {
                *section_out = k_fields[i].section;
            }
            return &k_fields[i].field;
        }
    }
    return NULL;
}

static int apply_eq_bands(AppConfig *cfg, const JsonValue *value)
{
    JsonReader reader;
    json_reader_init(&reader, value->start, value->length);
    JsonReader array = reader;
    if (json_reader_enter(&array, value) != 0) {
        return -1;
    }
    int index = 0;
    JsonValue element;
    while (json_array_next(&array, &element) == 1) {
        if (index >= EQ_BAND_COUNT) {
            break;
        }
        int gain = (int)json_value_int(&element, cfg->eq_band_gains[index]);
        if (gain < -12) gain = -12;
        if (gain > 12) gain = 12;
        cfg->eq_band_gains[index++] = gain;
    }
    return 0;
}

static int apply_json_impl(const char *patch_json, char *error_out,
                           size_t error_size, int reject_locked)
{
    if (error_out && error_size) {
        error_out[0] = '\0';
    }
    if (!patch_json || patch_json[0] == '\0') {
        return -1;
    }

    /* 原子应用：先改副本，全部成功后才提交（含远程连接与 EQ 频段这类
     * 复合字段），避免“改了一半”的配置被保存。 */
    AppConfig *draft = malloc(sizeof(AppConfig));
    if (!draft) {
        if (error_out && error_size) snprintf(error_out, error_size, "out of memory");
        return -1;
    }
    memcpy(draft, &g_app_config, sizeof(AppConfig));
    if (draft->keybinding_override_count < 0) {
        draft->keybinding_override_count = 0;
    }
    if (draft->keybinding_override_count > MAX_KEYBINDING_OVERRIDES) {
        draft->keybinding_override_count = MAX_KEYBINDING_OVERRIDES;
    }

    JsonReader reader;
    json_reader_init(&reader, patch_json, strlen(patch_json));
    if (!json_reader_ok(&reader)) {
        free(draft);
        if (error_out && error_size) snprintf(error_out, error_size, "malformed JSON");
        return -1;
    }

    int failed = 0;

    /* 顶层分区必须已知：旧版本前端可能仍发送已移除的分区
     * （如 remote_connections，远程音乐源已移交前端），必须整体失败，
     * 而不是“忽略它、改掉别的”。 */
    {
        JsonValue root = { JSON_VALUE_OBJECT, 0, 0.0, patch_json, strlen(patch_json) };
        JsonReader top = reader;
        if (json_reader_enter(&top, &root) == 0) {
            char section_key[128];
            JsonValue section_value;
            while (json_object_next(&top, section_key, sizeof(section_key), &section_value) == 1) {
                int known = 0;
                for (int s = 0; k_sections[s] != NULL; s++) {
                    if (strcmp(section_key, k_sections[s]) == 0) {
                        known = 1;
                        break;
                    }
                }
                if (!known) {
                    /* "version" 是核心自己输出的配置版本（config_render_json 顶层），
                     * 允许原样回灌（整表应用/镜像刷新），但它不改动任何字段。 */
                    if (strcmp(section_key, "version") == 0) {
                        continue;
                    }
                    if (error_out && error_size) {
                        snprintf(error_out, error_size, "unknown section '%s'", section_key);
                    }
                    free(draft);
                    return -1;
                }
            }
            if (!json_reader_ok(&top)) {
                free(draft);
                if (error_out && error_size) snprintf(error_out, error_size, "malformed JSON");
                return -1;
            }
        }
    }

    for (int s = 0; k_sections[s] != NULL && !failed; s++) {
        const char *section = k_sections[s];

        if (strcmp(section, "equalizer") == 0) {
            JsonValue eq;
            if (json_get_path(&reader, "equalizer", &eq) == 0 &&
                eq.type == JSON_VALUE_OBJECT) {
                JsonReader eq_reader = reader;
                if (json_reader_enter(&eq_reader, &eq) != 0) {
                    failed = 1;
                    break;
                }
                JsonValue value;
                char key[64];
                while (json_object_next(&eq_reader, key, sizeof(key), &value) == 1) {
                    if (strcmp(key, "enabled") == 0) {
                        draft->eq_enabled = json_value_bool(&value, draft->eq_enabled) ? 1 : 0;
                    } else if (strcmp(key, "preamp") == 0) {
                        int gain = (int)json_value_int(&value, draft->eq_preamp);
                        if (gain < -12) gain = -12;
                        if (gain > 12) gain = 12;
                        draft->eq_preamp = gain;
                    } else if (strcmp(key, "bands") == 0 && value.type == JSON_VALUE_ARRAY) {
                        if (apply_eq_bands(draft, &value) != 0) {
                            failed = 1;
                            break;
                        }
                    } else {
                        if (error_out && error_size) {
                            snprintf(error_out, error_size, "unknown equalizer key '%s'", key);
                        }
                        failed = 1;
                        break;
                    }
                }
            }
            continue;
        }

        if (strcmp(section, "keybindings") == 0) {
            JsonValue bindings;
            if (json_get_path(&reader, "keybindings", &bindings) != 0) {
                continue;   /* 补丁未包含 keybindings */
            }
            if (bindings.type != JSON_VALUE_OBJECT) {
                if (error_out && error_size) {
                    snprintf(error_out, error_size, "'keybindings' must be an object");
                }
                failed = 1;
                break;
            }

            JsonReader bindings_reader = reader;
            if (json_reader_enter(&bindings_reader, &bindings) != 0) {
                failed = 1;
                break;
            }

            char action[MAX_KEYBINDING_ACTION_LEN * 2];
            JsonValue value;
            while (json_object_next(&bindings_reader, action, sizeof(action), &value) == 1) {
                ActionId id = actions_from_id(action);
                if (id == ACTION_NONE) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size, "unknown keybinding action '%s'", action);
                    }
                    failed = 1;
                    break;
                }
                if (value.type != JSON_VALUE_STRING) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size,
                                 "keybinding '%s' must be a string", action);
                    }
                    failed = 1;
                    break;
                }

                char key[MAX_KEYBINDING_KEY_LEN * 2];
                json_value_string(&value, key, sizeof(key));

                if (strcmp(key, "default") == 0) {
                    int out = 0;
                    for (int i = 0; i < draft->keybinding_override_count; i++) {
                        if (strcmp(draft->keybinding_overrides[i].action, action) == 0) {
                            continue;
                        }
                        if (out != i) {
                            draft->keybinding_overrides[out] =
                                draft->keybinding_overrides[i];
                        }
                        out++;
                    }
                    draft->keybinding_override_count = out;
                    continue;
                }

                /* Config.Set 严格模式：系统保留动作不允许改绑，
                 * 但允许用 "default" 清理旧覆盖（上一分支已处理）。 */
                if (reject_locked && !actions_is_bindable(id)) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size,
                                 "action '%s' is locked and cannot be rebound", action);
                    }
                    failed = 1;
                    break;
                }

                KeyStroke stroke;
                if (keyname_parse(key, &stroke) != 0 || !keyname_is_bindable(stroke)) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size,
                                 "invalid key '%s' for action '%s'", key, action);
                    }
                    failed = 1;
                    break;
                }

                int index = -1;
                for (int i = 0; i < draft->keybinding_override_count; i++) {
                    if (strcmp(draft->keybinding_overrides[i].action, action) == 0) {
                        index = i;
                        break;
                    }
                }
                if (index < 0) {
                    if (draft->keybinding_override_count >= MAX_KEYBINDING_OVERRIDES) {
                        if (error_out && error_size) {
                            snprintf(error_out, error_size,
                                     "too many keybinding overrides");
                        }
                        failed = 1;
                        break;
                    }
                    index = draft->keybinding_override_count++;
                }
                copy_bounded(draft->keybinding_overrides[index].action,
                             sizeof(draft->keybinding_overrides[index].action),
                             action);
                copy_bounded(draft->keybinding_overrides[index].key,
                             sizeof(draft->keybinding_overrides[index].key),
                             key);
            }

            if (!failed && !json_reader_ok(&bindings_reader)) {
                if (error_out && error_size) {
                    snprintf(error_out, error_size, "malformed keybindings object");
                }
                failed = 1;
            }
            if (!failed) {
                char err[256];
                if (actions_validate_overrides(draft->keybinding_overrides,
                                               draft->keybinding_override_count,
                                               err, sizeof(err)) != 0) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size, "%s", err);
                    }
                    failed = 1;
                }
            }
            continue;
        }

        JsonValue object;
        if (json_get_path(&reader, section, &object) != 0) {
            continue;   /* 该分区未出现在补丁里 */
        }
        if (object.type != JSON_VALUE_OBJECT) {
            if (error_out && error_size) {
                snprintf(error_out, error_size, "'%s' must be an object", section);
            }
            failed = 1;
            break;
        }

        JsonReader section_reader = reader;
        if (json_reader_enter(&section_reader, &object) != 0) {
            failed = 1;
            break;
        }

        char key[128];
        JsonValue value;
        while (json_object_next(&section_reader, key, sizeof(key), &value) == 1) {
            const char *owner = NULL;
            const ConfigFieldDef *def = find_field(key, &owner);
            if (!def || strcmp(owner, section) != 0) {
                if (error_out && error_size) {
                    snprintf(error_out, error_size, "unknown key '%s' in '%s'", key, section);
                }
                failed = 1;
                break;
            }

            char *base = (char *)draft;
            switch (def->type) {
                case CFG_INT: {
                    if (value.type != JSON_VALUE_NUMBER && value.type != JSON_VALUE_BOOL) {
                        if (error_out && error_size) {
                            snprintf(error_out, error_size, "'%s' must be a number", key);
                        }
                        failed = 1;
                        break;
                    }
                    int number = (value.type == JSON_VALUE_BOOL)
                        ? value.boolean
                        : (int)value.number;
                    *(int *)(base + def->offset) = field_clamp(def, number);
                    break;
                }
                case CFG_FLOAT: {
                    if (value.type != JSON_VALUE_NUMBER) {
                        if (error_out && error_size) {
                            snprintf(error_out, error_size, "'%s' must be a number", key);
                        }
                        failed = 1;
                        break;
                    }
                    float number = (float)value.number;
                    if (number < 0.5f) number = 0.5f;
                    if (number > 3.0f) number = 3.0f;
                    *(float *)(base + def->offset) = number;
                    break;
                }
                case CFG_STRING:
                default: {
                    if (value.type != JSON_VALUE_STRING) {
                        if (error_out && error_size) {
                            snprintf(error_out, error_size, "'%s' must be a string", key);
                        }
                        failed = 1;
                        break;
                    }
                    json_value_string(&value, base + def->offset, def->size);
                    break;
                }
            }
            if (failed) {
                break;
            }
        }
    }

    if (failed) {
        free(draft);
        return -1;
    }

    memcpy(&g_app_config, draft, sizeof(AppConfig));
    free(draft);
    return 0;
}

int config_apply_json(const char *patch_json, char *error_out, size_t error_size)
{
    /* 宽松模式：用于 Config.GetAll 回填镜像，允许读入旧配置里的锁定覆盖；
     * 运行时 keymap 会忽略它们。 */
    return apply_json_impl(patch_json, error_out, error_size, 0);
}

int config_apply_json_set(const char *patch_json, char *error_out, size_t error_size)
{
    /* 严格模式：用户/前端写入路径，系统保留动作只接受 "default"。 */
    return apply_json_impl(patch_json, error_out, error_size, 1);
}
