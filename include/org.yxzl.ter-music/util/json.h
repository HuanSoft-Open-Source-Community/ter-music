/**
 * @file json.h
 * @brief 轻量 JSON 文本构建工具（无外部依赖）
 *
 * 供 D-Bus 接口（media/session.c）与信息快照（info/info.c）共用，
 * 避免两处各自维护一份转义/数字格式化代码。
 *
 * 所有函数采用 “追加 + 返回新偏移” 的风格：调用方维护 pos，
 * 缓冲区不足时安全截断并始终保证 NUL 结尾。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UTIL_JSON_H
#define UTIL_JSON_H

#include <stddef.h>

/* 追加原始文本（不转义） */
size_t json_append_raw(char *out, size_t out_size, size_t pos, const char *text);

/* 追加单个字符 */
size_t json_append_char(char *out, size_t out_size, size_t pos, char value);

/* 追加 JSON 字符串字面量（含两端引号，转义 " \ 与控制字符） */
size_t json_append_escaped(char *out, size_t out_size, size_t pos, const char *text);

/* text 为 NULL 时输出 null，否则输出转义后的字符串 */
size_t json_append_string_or_null(char *out, size_t out_size, size_t pos,
                                  const char *text);

/* 追加已是合法 JSON 数字的文本；number 为 NULL 时输出 null */
size_t json_append_number(char *out, size_t out_size, size_t pos,
                          const char *number);

/* 追加整数 */
size_t json_append_int(char *out, size_t out_size, size_t pos,
                       long long value);

/* 追加浮点数（固定小数位，小数点与千分位按 C locale 归一为 '.'） */
size_t json_append_double(char *out, size_t out_size, size_t pos,
                          double value, int decimals);

/* 追加布尔值 */
size_t json_append_bool(char *out, size_t out_size, size_t pos, int value);

/* 追加 "key": （键 + 冒号，不含值） */
size_t json_append_key(char *out, size_t out_size, size_t pos,
                       const char *key);

/* 追加 {"index":n,"timestamp":t,"text":s} 形式的歌词行对象。
 * index < 0 时三个字段均为 null（表示“无此行”）。 */
size_t json_append_line_object(char *out, size_t out_size, size_t pos,
                               int index, int timestamp_valid,
                               double timestamp, const char *text);

#endif /* UTIL_JSON_H */
