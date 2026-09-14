/**
 * @file json.c
 * @brief 轻量 JSON 文本构建工具实现
 *
 * 原实现位于 media/session.c（Lyrics API 专用），此处抽取为共享模块，
 * 供 D-Bus Info 接口与 CLI 信息快照复用。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t json_append_raw(char *out, size_t out_size, size_t pos,
                       const char *text)
{
    if (!out || out_size == 0 || pos >= out_size || !text) {
        return pos;
    }

    size_t available = out_size - pos;
    size_t length = strlen(text);
    if (length >= available) {
        length = available - 1;
    }
    memcpy(out + pos, text, length);
    pos += length;
    out[pos] = '\0';
    return pos;
}

size_t json_append_char(char *out, size_t out_size, size_t pos, char value)
{
    if (!out || out_size == 0 || pos + 1 >= out_size) {
        return pos;
    }
    out[pos++] = value;
    out[pos] = '\0';
    return pos;
}

size_t json_append_escaped(char *out, size_t out_size, size_t pos,
                           const char *text)
{
    pos = json_append_char(out, out_size, pos, '"');
    for (const unsigned char *ptr = (const unsigned char *)text;
         ptr && *ptr != '\0';
         ptr++) {
        unsigned char value = *ptr;
        if (value == '"' || value == '\\') {
            pos = json_append_char(out, out_size, pos, '\\');
            pos = json_append_char(out, out_size, pos, (char)value);
        } else if (value == '\b') {
            pos = json_append_raw(out, out_size, pos, "\\b");
        } else if (value == '\f') {
            pos = json_append_raw(out, out_size, pos, "\\f");
        } else if (value == '\n') {
            pos = json_append_raw(out, out_size, pos, "\\n");
        } else if (value == '\r') {
            pos = json_append_raw(out, out_size, pos, "\\r");
        } else if (value == '\t') {
            pos = json_append_raw(out, out_size, pos, "\\t");
        } else if (value < 0x20) {
            char escape[8];
            snprintf(escape, sizeof(escape), "\\u%04X", (unsigned int)value);
            pos = json_append_raw(out, out_size, pos, escape);
        } else {
            pos = json_append_char(out, out_size, pos, (char)value);
        }
    }
    return json_append_char(out, out_size, pos, '"');
}

size_t json_append_string_or_null(char *out, size_t out_size, size_t pos,
                                  const char *text)
{
    if (!text) {
        return json_append_raw(out, out_size, pos, "null");
    }
    return json_append_escaped(out, out_size, pos, text);
}

size_t json_append_number(char *out, size_t out_size, size_t pos,
                          const char *number)
{
    return json_append_raw(out, out_size, pos, number ? number : "null");
}

size_t json_append_int(char *out, size_t out_size, size_t pos,
                       long long value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    return json_append_raw(out, out_size, pos, buffer);
}

size_t json_append_double(char *out, size_t out_size, size_t pos,
                          double value, int decimals)
{
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 9) {
        decimals = 9;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    /* 某些 locale 使用逗号作为小数点，统一归一为 '.' */
    for (char *comma = strchr(buffer, ','); comma != NULL;
         comma = strchr(comma + 1, ',')) {
        *comma = '.';
    }
    return json_append_raw(out, out_size, pos, buffer);
}

size_t json_append_bool(char *out, size_t out_size, size_t pos, int value)
{
    return json_append_raw(out, out_size, pos, value ? "true" : "false");
}

size_t json_append_key(char *out, size_t out_size, size_t pos,
                       const char *key)
{
    pos = json_append_escaped(out, out_size, pos, key ? key : "");
    return json_append_char(out, out_size, pos, ':');
}

size_t json_append_line_object(char *out, size_t out_size, size_t pos,
                               int index, int timestamp_valid,
                               double timestamp, const char *text)
{
    pos = json_append_raw(out, out_size, pos, "{\"index\":");
    if (index >= 0) {
        pos = json_append_int(out, out_size, pos, (long long)index);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",\"timestamp\":");
    if (index >= 0 && timestamp_valid) {
        pos = json_append_double(out, out_size, pos, timestamp, 3);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",\"text\":");
    if (index >= 0) {
        pos = json_append_string_or_null(out, out_size, pos, text);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    return json_append_raw(out, out_size, pos, "}");
}

/* ============================================================
 * 有界 JSON 读取器
 * ============================================================ */

static void json_reader_fail(JsonReader *reader)
{
    if (reader) {
        reader->error = 1;
    }
}

static void json_skip_ws(JsonReader *reader)
{
    while (reader->pos < reader->length) {
        char c = reader->text[reader->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            reader->pos++;
        } else {
            break;
        }
    }
}

static int json_peek(const JsonReader *reader)
{
    if (!reader || reader->pos >= reader->length) {
        return -1;
    }
    return (unsigned char)reader->text[reader->pos];
}

/* 跳过一段字符串字面量（reader->pos 必须停在开引号上） */
static int json_skip_string(JsonReader *reader)
{
    if (json_peek(reader) != '"') {
        json_reader_fail(reader);
        return -1;
    }
    reader->pos++;
    while (reader->pos < reader->length) {
        char c = reader->text[reader->pos];
        if (c == '\\') {
            reader->pos += 2;   /* 转义序列：跳过反斜杠与其后一个字符 */
            continue;
        }
        reader->pos++;
        if (c == '"') {
            return 0;
        }
    }
    json_reader_fail(reader);
    return -1;
}

/* 跳过任意值（标量或容器）；用显式深度计数代替递归 */
static int json_skip_value(JsonReader *reader)
{
    json_skip_ws(reader);
    int c = json_peek(reader);
    if (c < 0) {
        json_reader_fail(reader);
        return -1;
    }

    if (c == '"') {
        return json_skip_string(reader);
    }

    if (c == '{' || c == '[') {
        int depth = 0;
        while (reader->pos < reader->length) {
            char ch = reader->text[reader->pos];
            if (ch == '"') {
                if (json_skip_string(reader) != 0) {
                    return -1;
                }
                continue;
            }
            reader->pos++;
            if (ch == '{' || ch == '[') {
                depth++;
                if (depth > JSON_READER_MAX_DEPTH) {
                    json_reader_fail(reader);
                    return -1;
                }
            } else if (ch == '}' || ch == ']') {
                depth--;
                if (depth == 0) {
                    return 0;
                }
            }
        }
        json_reader_fail(reader);
        return -1;
    }

    /* 字面量：true / false / null / 数字 */
    while (reader->pos < reader->length) {
        char ch = reader->text[reader->pos];
        if (ch == ',' || ch == '}' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            break;
        }
        reader->pos++;
    }
    return 0;
}

/* 解析一个标量并填充 JsonValue；容器只报告范围 */
static int json_read_value(JsonReader *reader, JsonValue *out, int capture_container);

static int json_scan_value_range(JsonReader *reader, JsonValue *out)
{
    size_t start = reader->pos;
    if (json_skip_value(reader) != 0) {
        return -1;
    }
    out->start = reader->text + start;
    out->length = reader->pos - start;
    return 0;
}

void json_reader_init(JsonReader *reader, const char *text, size_t length)
{
    if (!reader) {
        return;
    }
    reader->text = text ? text : "";
    reader->length = text ? length : 0;
    reader->pos = 0;
    reader->error = 0;
}

int json_reader_ok(const JsonReader *reader)
{
    return reader && !reader->error;
}

static int json_parse_number(JsonReader *reader, JsonValue *out)
{
    size_t start = reader->pos;
    while (reader->pos < reader->length) {
        char c = reader->text[reader->pos];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E') {
            reader->pos++;
        } else {
            break;
        }
    }
    if (reader->pos == start) {
        json_reader_fail(reader);
        return -1;
    }

    out->type = JSON_VALUE_NUMBER;
    out->start = reader->text + start;
    out->length = reader->pos - start;

    char buffer[64];
    size_t copy = out->length < sizeof(buffer) - 1 ? out->length : sizeof(buffer) - 1;
    memcpy(buffer, out->start, copy);
    buffer[copy] = '\0';
    out->number = strtod(buffer, NULL);
    return 0;
}

static int json_read_value(JsonReader *reader, JsonValue *out, int capture_container)
{
    if (!reader || !out || reader->error) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    json_skip_ws(reader);

    int c = json_peek(reader);
    if (c < 0) {
        json_reader_fail(reader);
        return -1;
    }

    if (c == '{' || c == '[') {
        out->type = (c == '{') ? JSON_VALUE_OBJECT : JSON_VALUE_ARRAY;
        if (!capture_container) {
            /* 复用同一个 reader：只跳过，不改变 out 之外的状态 */
            if (json_scan_value_range(reader, out) != 0) {
                out->type = JSON_VALUE_NULL;
                return -1;
            }
            return 0;
        }
        out->start = reader->text + reader->pos;
        out->length = 0;
        reader->pos++;   /* 进入容器内部 */
        json_skip_ws(reader);
        return 0;
    }

    if (c == '"') {
        out->type = JSON_VALUE_STRING;
        reader->pos++;
        out->start = reader->text + reader->pos;
        while (reader->pos < reader->length) {
            char ch = reader->text[reader->pos];
            if (ch == '\\') {
                reader->pos += 2;
                continue;
            }
            if (ch == '"') {
                break;
            }
            reader->pos++;
        }
        if (reader->pos >= reader->length) {
            json_reader_fail(reader);
            return -1;
        }
        out->length = (size_t)((reader->text + reader->pos) - out->start);
        reader->pos++;   /* 跳过闭合引号 */
        return 0;
    }

    if (reader->pos + 4 <= reader->length &&
        strncmp(reader->text + reader->pos, "true", 4) == 0) {
        out->type = JSON_VALUE_BOOL;
        out->boolean = 1;
        reader->pos += 4;
        return 0;
    }
    if (reader->pos + 5 <= reader->length &&
        strncmp(reader->text + reader->pos, "false", 5) == 0) {
        out->type = JSON_VALUE_BOOL;
        out->boolean = 0;
        reader->pos += 5;
        return 0;
    }
    if (reader->pos + 4 <= reader->length &&
        strncmp(reader->text + reader->pos, "null", 4) == 0) {
        out->type = JSON_VALUE_NULL;
        reader->pos += 4;
        return 0;
    }

    return json_parse_number(reader, out);
}

/* 在对象中按键查找。consume_brace 为 1 时先吃掉开头的 '{'；
 * 为 0 时假定 reader 已停在对象内部（json_reader_enter 之后）。
 * 找到返回 0，未找到返回 -1（不算错误），畸形返回 -1 并置 error。 */
static int json_object_lookup(JsonReader *walk, int consume_brace,
                              const char *key, JsonValue *out)
{
    json_skip_ws(walk);
    if (consume_brace) {
        if (json_peek(walk) != '{') {
            return -1;
        }
        walk->pos++;
    }

    size_t key_len = strlen(key);

    for (;;) {
        json_skip_ws(walk);
        int c = json_peek(walk);
        if (c == '}') {
            return -1;
        }
        if (c < 0) {
            json_reader_fail(walk);
            return -1;
        }

        JsonValue found;
        if (json_read_value(walk, &found, 0) != 0 || found.type != JSON_VALUE_STRING) {
            json_reader_fail(walk);
            return -1;
        }

        json_skip_ws(walk);
        if (json_peek(walk) != ':') {
            json_reader_fail(walk);
            return -1;
        }
        walk->pos++;

        int matches = (found.length == key_len) &&
                      (memcmp(found.start, key, key_len) == 0);

        if (matches) {
            if (json_read_value(walk, out, 0) != 0) {
                json_reader_fail(walk);
                return -1;
            }
            return 0;
        }

        if (json_skip_value(walk) != 0) {
            json_reader_fail(walk);
            return -1;
        }

        json_skip_ws(walk);
        c = json_peek(walk);
        if (c == ',') {
            walk->pos++;
            continue;
        }
        if (c == '}') {
            return -1;
        }
        json_reader_fail(walk);
        return -1;
    }
}

int json_object_find(JsonReader *reader, const char *key, JsonValue *out)
{
    if (!reader || !key || !out || reader->error) {
        return -1;
    }

    JsonReader walk = *reader;
    json_skip_ws(&walk);

    int consume_brace = (json_peek(&walk) == '{');
    int result = json_object_lookup(&walk, consume_brace, key, out);
    if (!json_reader_ok(&walk)) {
        *reader = walk;
    }
    return result;
}

int json_reader_enter(JsonReader *reader, const JsonValue *container)
{
    if (!reader || !container || reader->error) {
        return -1;
    }
    if (container->type != JSON_VALUE_OBJECT && container->type != JSON_VALUE_ARRAY) {
        return -1;
    }

    /* 从容器起点重新扫描并停在内部第一个元素之前 */
    size_t offset = (size_t)(container->start - reader->text);
    if (offset >= reader->length) {
        json_reader_fail(reader);
        return -1;
    }
    reader->pos = offset + 1;
    json_skip_ws(reader);
    return 0;
}

int json_object_next(JsonReader *reader, char *key_out, size_t key_size,
                     JsonValue *val_out)
{
    if (!reader || !val_out || reader->error) {
        return -1;
    }

    json_skip_ws(reader);
    int c = json_peek(reader);
    if (c == '}') {
        reader->pos++;
        return 0;
    }
    if (c < 0) {
        json_reader_fail(reader);
        return -1;
    }

    JsonValue key;
    if (json_read_value(reader, &key, 0) != 0 || key.type != JSON_VALUE_STRING) {
        json_reader_fail(reader);
        return -1;
    }
    if (key_out && key_size > 0) {
        json_value_string(&key, key_out, key_size);
    }

    json_skip_ws(reader);
    if (json_peek(reader) != ':') {
        json_reader_fail(reader);
        return -1;
    }
    reader->pos++;

    if (json_read_value(reader, val_out, 0) != 0) {
        json_reader_fail(reader);
        return -1;
    }

    json_skip_ws(reader);
    if (json_peek(reader) == ',') {
        reader->pos++;
    }
    return 1;
}

int json_array_next(JsonReader *reader, JsonValue *val_out)
{
    if (!reader || !val_out || reader->error) {
        return -1;
    }

    json_skip_ws(reader);
    int c = json_peek(reader);
    if (c == ']') {
        reader->pos++;
        return 0;
    }
    if (c < 0) {
        json_reader_fail(reader);
        return -1;
    }

    if (json_read_value(reader, val_out, 0) != 0) {
        json_reader_fail(reader);
        return -1;
    }

    json_skip_ws(reader);
    if (json_peek(reader) == ',') {
        reader->pos++;
    }
    return 1;
}

int json_get_path(JsonReader *reader, const char *path, JsonValue *out)
{
    if (!reader || !path || !out || reader->error) {
        return -1;
    }

    JsonReader walk = *reader;
    const char *cursor = path;
    char segment[128];
    int first = 1;

    for (;;) {
        const char *dot = strchr(cursor, '.');
        size_t seg_len = dot ? (size_t)(dot - cursor) : strlen(cursor);
        if (seg_len == 0 || seg_len >= sizeof(segment)) {
            return -1;
        }
        memcpy(segment, cursor, seg_len);
        segment[seg_len] = '\0';

        int consume_brace = 0;
        if (first) {
            json_skip_ws(&walk);
            consume_brace = (json_peek(&walk) == '{');
        }
        first = 0;

        JsonValue container = {0};
        if (json_object_lookup(&walk, consume_brace, segment, &container) != 0) {
            return -1;
        }

        if (!dot) {
            *out = container;
            return 0;
        }
        if (container.type != JSON_VALUE_OBJECT) {
            return -1;
        }
        if (json_reader_enter(&walk, &container) != 0) {
            *reader = walk;
            return -1;
        }
        cursor = dot + 1;
    }
}

long long json_value_int(const JsonValue *value, long long def)
{
    if (!value || value->type != JSON_VALUE_NUMBER) {
        return def;
    }
    return (long long)value->number;
}

double json_value_double(const JsonValue *value, double def)
{
    if (!value || value->type != JSON_VALUE_NUMBER) {
        return def;
    }
    return value->number;
}

int json_value_bool(const JsonValue *value, int def)
{
    if (!value || value->type != JSON_VALUE_BOOL) {
        return def;
    }
    return value->boolean;
}

size_t json_value_string(const JsonValue *value, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!value || (value->type != JSON_VALUE_STRING && value->type != JSON_VALUE_NUMBER)) {
        return 0;
    }

    size_t written = 0;
    const char *cursor = value->start;
    const char *end = value->start + value->length;

    while (cursor < end && written + 1 < out_size) {
        if (*cursor != '\\') {
            out[written++] = *cursor++;
            continue;
        }

        cursor++;
        if (cursor >= end) {
            break;
        }

        char escape = *cursor++;
        switch (escape) {
            case 'n': out[written++] = '\n'; break;
            case 't': out[written++] = '\t'; break;
            case 'r': out[written++] = '\r'; break;
            case 'b': out[written++] = '\b'; break;
            case 'f': out[written++] = '\f'; break;
            case '"': out[written++] = '"'; break;
            case '\\': out[written++] = '\\'; break;
            case '/': out[written++] = '/'; break;
            case 'u': {
                unsigned int code = 0;
                int valid = 1;
                for (int i = 0; i < 4; i++) {
                    if (cursor >= end) { valid = 0; break; }
                    char hex = *cursor++;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') code |= (unsigned int)(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') code |= (unsigned int)(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') code |= (unsigned int)(hex - 'A' + 10);
                    else { valid = 0; break; }
                }
                if (!valid) break;
                if (code < 0x80) {
                    if (written + 1 < out_size) out[written++] = (char)code;
                } else if (code < 0x800) {
                    if (written + 2 < out_size) {
                        out[written++] = (char)(0xC0 | (code >> 6));
                        out[written++] = (char)(0x80 | (code & 0x3F));
                    }
                } else if (code >= 0xD800 && code <= 0xDFFF) {
                    /* 代理对：读取低半区合成，失败时退化为 '?' */
                    unsigned int low = 0;
                    int have_low = 0;
                    if (cursor + 1 < end && cursor[0] == '\\' && cursor[1] == 'u') {
                        cursor += 2;
                        for (int i = 0; i < 4; i++) {
                            if (cursor >= end) { have_low = 0; break; }
                            char hex = *cursor++;
                            low <<= 4;
                            if (hex >= '0' && hex <= '9') low |= (unsigned int)(hex - '0');
                            else if (hex >= 'a' && hex <= 'f') low |= (unsigned int)(hex - 'a' + 10);
                            else if (hex >= 'A' && hex <= 'F') low |= (unsigned int)(hex - 'A' + 10);
                            else { have_low = 0; break; }
                            have_low = 1;
                        }
                    }
                    unsigned int combined = 0;
                    if (have_low && low >= 0xDC00 && low <= 0xDFFF) {
                        combined = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    if (combined == 0) {
                        if (written + 1 < out_size) out[written++] = '?';
                    } else if (written + 4 < out_size) {
                        out[written++] = (char)(0xF0 | (combined >> 18));
                        out[written++] = (char)(0x80 | ((combined >> 12) & 0x3F));
                        out[written++] = (char)(0x80 | ((combined >> 6) & 0x3F));
                        out[written++] = (char)(0x80 | (combined & 0x3F));
                    }
                } else if (written + 3 < out_size) {
                    out[written++] = (char)(0xE0 | (code >> 12));
                    out[written++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[written++] = (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default:
                out[written++] = escape;
                break;
        }
    }

    out[written] = '\0';
    return written;
}

long long json_get_int(JsonReader *reader, const char *path, long long def)
{
    JsonValue value;
    if (json_get_path(reader, path, &value) != 0) {
        return def;
    }
    return json_value_int(&value, def);
}

double json_get_double(JsonReader *reader, const char *path, double def)
{
    JsonValue value;
    if (json_get_path(reader, path, &value) != 0) {
        return def;
    }
    return json_value_double(&value, def);
}

int json_get_bool(JsonReader *reader, const char *path, int def)
{
    JsonValue value;
    if (json_get_path(reader, path, &value) != 0) {
        return def;
    }
    return json_value_bool(&value, def);
}

size_t json_get_string(JsonReader *reader, const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    JsonValue value;
    if (json_get_path(reader, path, &value) != 0) {
        return 0;
    }
    return json_value_string(&value, out, out_size);
}
