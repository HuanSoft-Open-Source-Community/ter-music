/**
 * @file utf8_safe.c
 * @brief 字节级 UTF-8 校验与安全复制的实现
 *
 * 这些函数不依赖 ncurses、locale 或配置，核心进程与单元测试都能直接使用。
 * 校验规则与 libdbus 的 UTF-8 检查保持一致，避免“我们认为合法、libdbus
 * 却断言 abort”的边界差异。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "util/utf8_safe.h"

#include <string.h>

/* U+FFFD REPLACEMENT CHARACTER 的 UTF-8 编码 */
#define UTF8_REPLACEMENT "\xEF\xBF\xBD"
#define UTF8_REPLACEMENT_LEN 3

/**
 * 判断从 p 开始、剩余 remaining 字节的位置是否是一个合法 UTF-8 序列。
 * @return 合法时返回序列长度（1-4）；非法或长度不足返回 0。
 */
static size_t utf8_valid_sequence_length(const unsigned char *p, size_t remaining)
{
    if (!p || remaining == 0) {
        return 0;
    }

    unsigned char c = p[0];
    if (c < 0x80) {
        return 1;
    }

    if ((c & 0xE0) == 0xC0) {
        if (c < 0xC2 || remaining < 2) {
            return 0;
        }
        if ((p[1] & 0xC0) != 0x80) {
            return 0;
        }
        return 2;
    }

    if ((c & 0xF0) == 0xE0) {
        if (remaining < 3) {
            return 0;
        }
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
            return 0;
        }
        if (c == 0xE0 && p[1] < 0xA0) {
            return 0;   /* 过长编码 */
        }
        if (c == 0xED && p[1] >= 0xA0) {
            return 0;   /* UTF-16 代理区 */
        }
        return 3;
    }

    if ((c & 0xF8) == 0xF0) {
        if (remaining < 4) {
            return 0;
        }
        if ((p[1] & 0xC0) != 0x80 ||
            (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) {
            return 0;
        }
        if (c == 0xF0 && p[1] < 0x90) {
            return 0;   /* 过长编码 */
        }
        if (c > 0xF4 || (c == 0xF4 && p[1] >= 0x90)) {
            return 0;   /* 超过 U+10FFFF */
        }
        return 4;
    }

    return 0;
}

int utf8_is_valid_bytes(const unsigned char *data, size_t length)
{
    if (!data) {
        return 1;
    }

    size_t offset = 0;
    while (offset < length) {
        size_t sequence = utf8_valid_sequence_length(data + offset, length - offset);
        if (sequence == 0) {
            return 0;
        }
        offset += sequence;
    }
    return 1;
}

int utf8_is_valid(const char *text)
{
    if (!text) {
        return 1;
    }
    return utf8_is_valid_bytes((const unsigned char *)text, strlen(text));
}

size_t utf8_sanitize_copy(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return 0;
    }

    dest[0] = '\0';
    if (!src) {
        return 0;
    }

    const unsigned char *input = (const unsigned char *)src;
    size_t input_length = strlen(src);
    size_t input_offset = 0;
    size_t output_offset = 0;
    size_t capacity = dest_size - 1;   /* 留给结尾 NUL */

    while (input_offset < input_length) {
        size_t sequence = utf8_valid_sequence_length(input + input_offset,
                                                     input_length - input_offset);
        if (sequence != 0) {
            if (output_offset + sequence > capacity) {
                break;
            }
            memcpy(dest + output_offset, input + input_offset, sequence);
            output_offset += sequence;
            input_offset += sequence;
            continue;
        }

        /* 非法字节：逐个替换，避免把一个坏序列误判成多个合法字符 */
        if (output_offset + UTF8_REPLACEMENT_LEN > capacity) {
            break;
        }
        memcpy(dest + output_offset, UTF8_REPLACEMENT, UTF8_REPLACEMENT_LEN);
        output_offset += UTF8_REPLACEMENT_LEN;
        input_offset++;
    }

    dest[output_offset] = '\0';
    return output_offset;
}
