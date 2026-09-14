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

/* ============================================================
 * 有界 JSON 读取器
 *
 * 用途：核心侧解析 Config.Set 等外部载荷，前端侧解析 Info/Playlist/
 * Queue 等接口的响应。
 *
 * 约束（与写入器一致的设计取向）：
 *  - 不分配内存、不递归（容器跳过用显式深度计数），因此不会成为新的
 *    大栈帧来源；
 *  - 只在 [text, text+length) 内读取；缺失的键是正常情况（返回 -1 且
 *    不置 error），畸形 JSON 或未闭合容器置 error 并使后续调用失败；
 *  - 未知字段一律跳过，便于后续版本加字段。
 * ============================================================ */

typedef enum {
    JSON_VALUE_NULL = 0,
    JSON_VALUE_BOOL,
    JSON_VALUE_NUMBER,
    JSON_VALUE_STRING,
    JSON_VALUE_OBJECT,
    JSON_VALUE_ARRAY
} JsonValueType;

/* 一个值的视图：标量随结构体带出解析结果；容器只报告类型与原始范围 */
typedef struct {
    JsonValueType type;
    int boolean;            /* JSON_VALUE_BOOL */
    double number;          /* JSON_VALUE_NUMBER */
    const char *start;      /* 字符串内容起点 / 数字文本起点 / 容器 '{' 或 '[' */
    size_t length;          /* 上述范围长度（字符串为反转义前原文长度） */
} JsonValue;

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    int error;              /* 1 = 已遇到畸形输入 */
} JsonReader;

#define JSON_READER_MAX_DEPTH 32

void json_reader_init(JsonReader *reader, const char *text, size_t length);
int  json_reader_ok(const JsonReader *reader);

/* 在顶层对象中查找键；找到返回 0 并填充 out，未找到返回 -1（不算错误） */
int json_object_find(JsonReader *reader, const char *key, JsonValue *out);

/* 把 reader 定位到容器（对象/数组）内部的第一个元素之前 */
int json_reader_enter(JsonReader *reader, const JsonValue *container);

/* 依次取出容器内的元素；返回 1 = 有元素，0 = 容器结束，-1 = 错误。
 * 对象模式下 key_out 收到键名（可为 NULL），val_out 收到值。 */
int json_object_next(JsonReader *reader, char *key_out, size_t key_size,
                     JsonValue *val_out);
int json_array_next(JsonReader *reader, JsonValue *val_out);

/* 按 "a.b.c" 路径取嵌套值（仅对象层级）；未找到返回 -1 */
int json_get_path(JsonReader *reader, const char *path, JsonValue *out);

/* 取值（缺失/类型不符时使用默认值） */
long long json_get_int(JsonReader *reader, const char *path, long long def);
double    json_get_double(JsonReader *reader, const char *path, double def);
int       json_get_bool(JsonReader *reader, const char *path, int def);
/* 反转义写入 out（越界安全截断并 NUL 结尾），返回写入字节数 */
size_t    json_get_string(JsonReader *reader, const char *path,
                          char *out, size_t out_size);

/* 直接对已取得的 JsonValue 取值 */
long long json_value_int(const JsonValue *value, long long def);
double    json_value_double(const JsonValue *value, double def);
int       json_value_bool(const JsonValue *value, int def);
size_t    json_value_string(const JsonValue *value, char *out, size_t out_size);

#endif /* UTIL_JSON_H */
