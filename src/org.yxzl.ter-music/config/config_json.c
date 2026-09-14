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
 *    paths / theme / preferences / equalizer / remote_connections。
 *  - 应用（config_apply_json）只接受已知键，任何未知键即整体失败，
 *    避免“改了一半”的配置落盘。应用是原子的：先改副本，全部成功后再提交。
 *
 * 密码：渲染只输出密文（password_encrypted）与 password_set 布尔，
 * 明文不穿越会话总线；应用时接受 password（明文，落盘时由 config 层加密）
 * 或 password_encrypted（密文，解密后存入内存，落盘时不会二次加密）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "config/config_json.h"

#include "config/config.h"

#include "config/crypto.h"
#include "config/schema.h"
#include "logger/logger.h"
#include "util/json.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    CFG_INT = 0,
    CFG_FLOAT,
    CFG_STRING
} ConfigFieldType;

typedef struct {
    const char *key;        /* JSON 键 = XML 元素名 */
    ConfigFieldType type;
    size_t offset;
    size_t size;            /* CFG_STRING 的缓冲长度 */
    int min;                /* CFG_INT / CFG_FLOAT 的钳制范围（<= max 时生效） */
    int max;
} ConfigFieldDef;

#define OFF(field) offsetof(AppConfig, field)

/* 标量字段表：顺序即 JSON 输出顺序（按 config.xml 的分区排列）。
 * section 字段决定它落在哪个分区对象里。 */
typedef struct {
    ConfigFieldDef field;
    const char *section;
} ConfigFieldEntry;

static const ConfigFieldEntry k_fields[] = {
    /* paths */
    {"default_startup_path", CFG_STRING, OFF(default_startup_path), MAX_PATH_LEN, 0, 0, "paths"},
    {"last_opened_path",     CFG_STRING, OFF(last_opened_path),     MAX_PATH_LEN, 0, 0, "paths"},
    {"last_played_folder_path", CFG_STRING, OFF(last_played_folder_path), MAX_PATH_LEN, 0, 0, "paths"},
    {"last_played_track_path",  CFG_STRING, OFF(last_played_track_path),  MAX_PATH_LEN, 0, 0, "paths"},

    /* theme */
    {"playlist_fg",  CFG_INT, OFF(theme.playlist_fg),  0, 0, 255, "theme"},
    {"playlist_bg",  CFG_INT, OFF(theme.playlist_bg),  0, -1, 255, "theme"},
    {"controls_fg",  CFG_INT, OFF(theme.controls_fg),  0, 0, 255, "theme"},
    {"controls_bg",  CFG_INT, OFF(theme.controls_bg),  0, -1, 255, "theme"},
    {"lyrics_fg",    CFG_INT, OFF(theme.lyrics_fg),    0, 0, 255, "theme"},
    {"lyrics_bg",    CFG_INT, OFF(theme.lyrics_bg),    0, -1, 255, "theme"},
    {"sidebar_fg",   CFG_INT, OFF(theme.sidebar_fg),   0, 0, 255, "theme"},
    {"sidebar_bg",   CFG_INT, OFF(theme.sidebar_bg),   0, -1, 255, "theme"},
    {"highlight_fg", CFG_INT, OFF(theme.highlight_fg), 0, 0, 255, "theme"},
    {"highlight_bg", CFG_INT, OFF(theme.highlight_bg), 0, 0, 255, "theme"},
    {"border_fg",    CFG_INT, OFF(theme.border_fg),    0, 0, 255, "theme"},
    {"border_bg",    CFG_INT, OFF(theme.border_bg),    0, -1, 255, "theme"},

    /* preferences */
    {"auto_play_on_start",       CFG_INT, OFF(auto_play_on_start), 0, 0, 1, "preferences"},
    {"remember_last_path",       CFG_INT, OFF(remember_last_path), 0, 0, 1, "preferences"},
    {"clear_history_on_startup", CFG_INT, OFF(clear_history_on_startup), 0, 0, 1, "preferences"},
    {"resume_last_playback",     CFG_INT, OFF(resume_last_playback), 0, 0, 1, "preferences"},
    {"last_played_position",     CFG_INT, OFF(last_played_position), 0, 0, 0, "preferences"},
    {"ui_language",              CFG_STRING, OFF(ui_language), sizeof(((AppConfig *)0)->ui_language), 0, 0, "preferences"},
    {"volume_percent",           CFG_INT, OFF(volume_percent), 0, 0, 100, "preferences"},
    {"audio_latency_ms",         CFG_INT, OFF(audio_latency_ms), 0, 0, 0, "preferences"},
    {"show_lyrics_panel",        CFG_INT, OFF(show_lyrics_panel), 0, 0, 1, "preferences"},
    {"default_loop_mode",        CFG_INT, OFF(default_play_mode), 0, 0, PLAY_MODE_COUNT - 1, "preferences"},
    {"default_play_mode",        CFG_INT, OFF(default_play_mode), 0, 0, PLAY_MODE_COUNT - 1, "preferences"},
    {"advanced_play_modes_enabled", CFG_INT, OFF(advanced_play_modes_enabled), 0, 0, 1, "preferences"},
    {"default_playback_speed",   CFG_FLOAT, OFF(default_playback_speed), 0, 0, 0, "preferences"},
    {"show_album_cover",         CFG_INT, OFF(show_album_cover), 0, 0, 1, "preferences"},
    {"seamless_preload",         CFG_INT, OFF(seamless_preload), 0, 0, 1, "preferences"},
    {"lyrics_alignment",         CFG_INT, OFF(lyrics_alignment), 0, 0, 2, "preferences"},
    {"audio_backend",            CFG_INT, OFF(audio_backend), 0, 0, 3, "preferences"},
    {"sort_mode",                CFG_INT, OFF(sort_mode), 0, 0, 4, "preferences"},
    {"cue_encoding",             CFG_INT, OFF(cue_encoding), 0, 0, 0, "preferences"},

    /* info display（config v5） */
    {"info_preset",         CFG_INT, OFF(info_preset), 0, 0, 2, "preferences"},
    {"info_fields",         CFG_INT, OFF(info_fields_mask), 0, 0, 0, "preferences"},
    {"info_show_cover",     CFG_INT, OFF(info_show_cover), 0, 0, 1, "preferences"},
    {"info_cover_cols",     CFG_INT, OFF(info_cover_cols), 0, 4, 40, "preferences"},
    {"info_cover_rows",     CFG_INT, OFF(info_cover_rows), 0, 2, 20, "preferences"},
    {"info_cover_charset",  CFG_INT, OFF(info_cover_charset), 0, 0, 2, "preferences"},
    {"info_show_progress",  CFG_INT, OFF(info_show_progress), 0, 0, 1, "preferences"},
    {"info_progress_style", CFG_INT, OFF(info_progress_style), 0, 0, 3, "preferences"},
    {"info_lyrics_lines",   CFG_INT, OFF(info_lyrics_lines), 0, 0, 2, "preferences"},
};

#define FIELD_COUNT ((int)(sizeof(k_fields) / sizeof(k_fields[0])))

static const char *const k_sections[] = {
    "paths", "theme", "preferences", "equalizer", "remote_connections", NULL
};

static int field_clamp(const ConfigFieldDef *def, int value)
{
    if (def->max >= def->min && def->max > 0) {
        if (value < def->min) return def->min;
        if (value > def->max) return def->max;
    }
    return value;
}

/* ── 渲染 ───────────────────────────────────────────────────────── */

static size_t render_remote_connections(const AppConfig *cfg, char *out, size_t out_size, size_t pos)
{
    pos = json_append_char(out, out_size, pos, '[');
    for (int i = 0; i < cfg->remote_connection_count && i < MAX_REMOTE_CONNECTIONS; i++) {
        const RemoteConnectionConfig *rc = &cfg->remote_connections[i];
        if (i > 0) pos = json_append_char(out, out_size, pos, ',');

        char encrypted[512];
        encrypted[0] = '\0';
        if (rc->password[0]) {
            crypto_encrypt(rc->password, encrypted, sizeof(encrypted));
        }

        pos = json_append_char(out, out_size, pos, '{');
        pos = json_append_key(out, out_size, pos, "index");
        pos = json_append_int(out, out_size, pos, i);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "name");
        pos = json_append_escaped(out, out_size, pos, rc->name);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "protocol");
        pos = json_append_int(out, out_size, pos, rc->protocol);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "host");
        pos = json_append_escaped(out, out_size, pos, rc->host);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "port");
        pos = json_append_int(out, out_size, pos, rc->port);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "username");
        pos = json_append_escaped(out, out_size, pos, rc->username);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "base_path");
        pos = json_append_escaped(out, out_size, pos, rc->base_path);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "private_key_path");
        pos = json_append_escaped(out, out_size, pos, rc->private_key_path);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "password_set");
        pos = json_append_bool(out, out_size, pos, rc->password[0] != '\0');
        pos = json_append_raw(out, out_size, pos, ",");
        /* 只回传密文：明文不穿越会话总线 */
        pos = json_append_key(out, out_size, pos, "password_encrypted");
        pos = json_append_string_or_null(out, out_size, pos,
                                         encrypted[0] ? encrypted : NULL);
        pos = json_append_char(out, out_size, pos, '}');
    }
    return json_append_char(out, out_size, pos, ']');
}

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

        if (strcmp(section, "remote_connections") == 0) {
            pos = render_remote_connections(cfg, out, out_size, pos);
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

static int apply_remote_connections(AppConfig *cfg, const JsonValue *value)
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
        if (element.type != JSON_VALUE_OBJECT || index >= MAX_REMOTE_CONNECTIONS) {
            continue;
        }
        JsonReader entry = array;
        if (json_reader_enter(&entry, &element) != 0) {
            return -1;
        }

        RemoteConnectionConfig *rc = &cfg->remote_connections[index];
        int was_set = (rc->name[0] != '\0' || rc->host[0] != '\0');
        (void)was_set;

        json_get_string(&entry, "name", rc->name, sizeof(rc->name));
        json_get_string(&entry, "host", rc->host, sizeof(rc->host));
        json_get_string(&entry, "username", rc->username, sizeof(rc->username));
        json_get_string(&entry, "base_path", rc->base_path, sizeof(rc->base_path));
        json_get_string(&entry, "private_key_path", rc->private_key_path,
                        sizeof(rc->private_key_path));

        JsonValue protocol;
        if (json_get_path(&entry, "protocol", &protocol) == 0) {
            int value_int = (int)json_value_int(&protocol, rc->protocol);
            if (value_int < 0 || value_int > REMOTE_PROTOCOL_HTTP) {
                return -1;
            }
            rc->protocol = value_int;
        }
        JsonValue port;
        if (json_get_path(&entry, "port", &port) == 0) {
            int value_int = (int)json_value_int(&port, rc->port);
            if (value_int >= 0 && value_int <= 65535) {
                rc->port = value_int;
            }
        }

        /* 密码：明文（重新加密落盘）或密文（解密后存内存，避免二次加密）；
         * 两者都没给则保留原有密码。 */
        JsonValue password;
        if (json_get_path(&entry, "password", &password) == 0 &&
            password.type == JSON_VALUE_STRING && password.length > 0) {
            json_value_string(&password, rc->password, sizeof(rc->password));
        } else if (json_get_path(&entry, "password_encrypted", &password) == 0 &&
                   password.type == JSON_VALUE_STRING && password.length > 0) {
            char hex[512];
            json_value_string(&password, hex, sizeof(hex));
            crypto_decrypt(hex, rc->password, sizeof(rc->password));
        }

        index++;
    }

    cfg->remote_connection_count = index;
    return 0;
}

int config_apply_json(const char *patch_json, char *error_out, size_t error_size)
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

    JsonReader reader;
    json_reader_init(&reader, patch_json, strlen(patch_json));
    if (!json_reader_ok(&reader)) {
        free(draft);
        if (error_out && error_size) snprintf(error_out, error_size, "malformed JSON");
        return -1;
    }

    int failed = 0;
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
                        snprintf(error_out, error_size, "unknown equalizer key '%s'", key);
                        failed = 1;
                        break;
                    }
                }
            }
            continue;
        }

        if (strcmp(section, "remote_connections") == 0) {
            JsonValue connections;
            if (json_get_path(&reader, "remote_connections", &connections) == 0) {
                if (connections.type != JSON_VALUE_ARRAY ||
                    apply_remote_connections(draft, &connections) != 0) {
                    if (error_out && error_size) {
                        snprintf(error_out, error_size, "invalid remote_connections");
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
