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
