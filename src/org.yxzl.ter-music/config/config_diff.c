/**
 * @file config_diff.c
 * @brief 配置差异 → 最小 JSON 补丁（见 config_diff.h）
 *
 * 实现直接遍历 `config_fields.h` 的共享字段表，因此新增配置项只要进表就自动
 * 参与差异计算，不会出现"加了设置项但前端推不动"的漏键。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "config/config_diff.h"

#include "config/config_fields.h"
#include "util/json.h"

#include <string.h>

/* 字段是否与另一份配置不同 */
static int field_changed(const ConfigFieldEntry *entry,
                         const AppConfig *before, const AppConfig *after)
{
    const char *lhs = (const char *)before + entry->field.offset;
    const char *rhs = (const char *)after + entry->field.offset;

    switch (entry->field.type) {
        case CFG_INT:
            return *(const int *)lhs != *(const int *)rhs;
        case CFG_FLOAT:
            return *(const float *)lhs != *(const float *)rhs;
        case CFG_STRING:
        default:
            return strncmp(lhs, rhs, entry->field.size) != 0;
    }
}

static size_t append_field(char *out, size_t out_size, size_t pos, int *written,
                           const ConfigFieldEntry *entry, const AppConfig *after)
{
    const char *value = (const char *)after + entry->field.offset;
    if (*written > 0) {
        pos = json_append_raw(out, out_size, pos, ",");
    }
    pos = json_append_key(out, out_size, pos, entry->field.key);
    switch (entry->field.type) {
        case CFG_INT:
            pos = json_append_int(out, out_size, pos, *(const int *)value);
            break;
        case CFG_FLOAT:
            pos = json_append_double(out, out_size, pos, *(const float *)value, 2);
            break;
        case CFG_STRING:
        default:
            pos = json_append_escaped(out, out_size, pos, value);
            break;
    }
    (*written)++;
    return pos;
}

size_t config_diff_json(const AppConfig *before, const AppConfig *after,
                        char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    int sections_written = 0;

    static const char *const sections[] = { "paths", "theme", "preferences", NULL };
    for (int s = 0; sections[s] != NULL; s++) {
        const char *section = sections[s];

        /* 先算这一分区有多少变化，避免写出空对象 */
        int changed = 0;
        size_t seen_offsets[64];
        int seen_count = 0;
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (strcmp(k_fields[i].section, section) != 0) {
                continue;
            }
            /* default_loop_mode / default_play_mode 指向同一字段：只算一次 */
            int duplicate = 0;
            for (int k = 0; k < seen_count; k++) {
                if (seen_offsets[k] == k_fields[i].field.offset) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (seen_count < (int)(sizeof(seen_offsets) / sizeof(seen_offsets[0]))) {
                seen_offsets[seen_count++] = k_fields[i].field.offset;
            }
            if (field_changed(&k_fields[i], before, after)) {
                changed = 1;
                /* 保留全部 offset 记录，后面还要按同一规则去重输出 */
            }
        }
        if (!changed) {
            continue;
        }

        if (sections_written > 0) {
            pos = json_append_raw(out, out_size, pos, ",");
        }
        pos = json_append_key(out, out_size, pos, section);
        pos = json_append_char(out, out_size, pos, '{');

        int written = 0;
        seen_count = 0;
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (strcmp(k_fields[i].section, section) != 0) {
                continue;
            }
            int duplicate = 0;
            for (int k = 0; k < seen_count; k++) {
                if (seen_offsets[k] == k_fields[i].field.offset) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (seen_count < (int)(sizeof(seen_offsets) / sizeof(seen_offsets[0]))) {
                seen_offsets[seen_count++] = k_fields[i].field.offset;
            }
            if (!field_changed(&k_fields[i], before, after)) {
                continue;
            }
            pos = append_field(out, out_size, pos, &written, &k_fields[i], after);
        }
        pos = json_append_char(out, out_size, pos, '}');
        sections_written++;
    }

    /* 均衡器：enabled / preamp / bands[] 不在标量表里，单独比较 */
    {
        int eq_changed = before->eq_enabled != after->eq_enabled ||
                         before->eq_preamp != after->eq_preamp;
        for (int b = 0; !eq_changed && b < EQ_BAND_COUNT; b++) {
            if (before->eq_band_gains[b] != after->eq_band_gains[b]) {
                eq_changed = 1;
            }
        }
        if (eq_changed) {
            if (sections_written > 0) {
                pos = json_append_raw(out, out_size, pos, ",");
            }
            pos = json_append_key(out, out_size, pos, "equalizer");
            pos = json_append_char(out, out_size, pos, '{');
            int written = 0;
            if (before->eq_enabled != after->eq_enabled) {
                pos = json_append_key(out, out_size, pos, "enabled");
                pos = json_append_int(out, out_size, pos, after->eq_enabled);
                written++;
            }
            if (before->eq_preamp != after->eq_preamp) {
                if (written > 0) {
                    pos = json_append_raw(out, out_size, pos, ",");
                }
                pos = json_append_key(out, out_size, pos, "preamp");
                pos = json_append_int(out, out_size, pos, after->eq_preamp);
                written++;
            }
            int bands_differ = 0;
            for (int b = 0; b < EQ_BAND_COUNT; b++) {
                if (before->eq_band_gains[b] != after->eq_band_gains[b]) {
                    bands_differ = 1;
                    break;
                }
            }
            if (bands_differ) {
                if (written > 0) {
                    pos = json_append_raw(out, out_size, pos, ",");
                }
                pos = json_append_key(out, out_size, pos, "bands");
                pos = json_append_char(out, out_size, pos, '[');
                for (int b = 0; b < EQ_BAND_COUNT; b++) {
                    if (b > 0) {
                        pos = json_append_char(out, out_size, pos, ',');
                    }
                    pos = json_append_int(out, out_size, pos, after->eq_band_gains[b]);
                }
                pos = json_append_char(out, out_size, pos, ']');
                written++;
            }
            pos = json_append_char(out, out_size, pos, '}');
            sections_written++;
        }
    }

    pos = json_append_char(out, out_size, pos, '}');
    return pos;
}
