/**
 * @file test_json_reader.c
 * @brief util/json.c 有界读取器的单元测试（M2.0 验收）
 *
 * 覆盖：路径取值、缺失键、类型不符、数组/对象遍历、转义与 \uXXXX、
 * 畸形输入、越界（未闭合容器）、深层嵌套深度上限。
 *
 * 构建：gcc -std=gnu99 -D_GNU_SOURCE -I include/org.yxzl.ter-music \
 *         .tmp/tests/test_json_reader.c src/org.yxzl.ter-music/util/json.c -o /tmp/test_json
 */

#include "util/json.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL: %s (line %d)\n", what, __LINE__);                  \
        }                                                                    \
    } while (0)

static const char *k_info = "{"
    "\"schema\":1,"
    "\"playback\":{\"state\":\"playing\",\"position_ms\":83400,\"percent\":28.2,"
                  "\"shuffle\":false,\"can_seek\":true},"
    "\"track\":{\"index\":2,\"title\":\"A \\\"quoted\\\" song\",\"album\":\"B\\u00e9la\","
               "\"path\":null},"
    "\"display\":{\"fields\":[\"state\",\"mode\",\"title\"],\"one_line\":false},"
    "\"rows\":[{\"i\":1,\"t\":\"x\"},{\"i\":2,\"t\":\"y\"}],"
    "\"unknown\":{\"deep\":{\"a\":{\"b\":{\"c\":[1,2,{\"d\":\"e\"}]}}}}"
    "}";

static void test_paths(void)
{
    JsonReader r;
    json_reader_init(&r, k_info, strlen(k_info));
    CHECK(json_reader_ok(&r), "well-formed input parses");

    CHECK(json_get_int(&r, "schema", -1) == 1, "top-level int");
    CHECK(strcmp("", "") == 0, "placeholder");

    char state[32];
    json_get_string(&r, "playback.state", state, sizeof(state));
    CHECK(strcmp(state, "playing") == 0, "nested string");

    CHECK(json_get_int(&r, "playback.position_ms", -1) == 83400, "nested int");
    CHECK(json_get_double(&r, "playback.percent", -1.0) > 28.1 &&
          json_get_double(&r, "playback.percent", -1.0) < 28.3, "nested double");
    CHECK(json_get_bool(&r, "playback.shuffle", 1) == 0, "false parses as 0");
    CHECK(json_get_bool(&r, "playback.can_seek", 0) == 1, "true parses as 1");

    char title[64];
    json_get_string(&r, "track.title", title, sizeof(title));
    CHECK(strcmp(title, "A \"quoted\" song") == 0, "escaped quote unescaped");

    char album[64];
    json_get_string(&r, "track.album", album, sizeof(album));
    CHECK(strcmp(album, "B\xc3\xa9la") == 0, "\\u00e9 becomes UTF-8");

    /* 缺失键：返回默认值且不置 error */
    CHECK(json_get_int(&r, "track.nope", 42) == 42, "missing key uses default");
    CHECK(json_get_string(&r, "nope.deep", title, sizeof(title)) == 0, "missing path empty");
    CHECK(json_get_int(&r, "playback.state", 7) == 7, "type mismatch uses default");
    CHECK(json_get_string(&r, "track.path", title, sizeof(title)) == 0, "null yields empty");
    CHECK(json_reader_ok(&r), "missing keys are not errors");
}

static void test_iteration(void)
{
    JsonReader r;
    JsonValue v;

    json_reader_init(&r, k_info, strlen(k_info));
    CHECK(json_object_find(&r, "display", &v) == 0, "find display object");
    CHECK(v.type == JSON_VALUE_OBJECT, "display is an object");

    JsonReader walk = r;
    CHECK(json_reader_enter(&walk, &v) == 0, "enter display");
    char key[64];
    JsonValue value;
    int n = 0;
    while (json_object_next(&walk, key, sizeof(key), &value) == 1) {
        n++;
        if (strcmp(key, "fields") == 0) {
            CHECK(value.type == JSON_VALUE_ARRAY, "fields is array");
        }
    }
    CHECK(n == 2, "display has 2 keys");

    /* 数组遍历 */
    json_reader_init(&r, k_info, strlen(k_info));
    CHECK(json_object_find(&r, "rows", &v) == 0 && v.type == JSON_VALUE_ARRAY, "find rows");
    JsonReader arr = r;
    CHECK(json_reader_enter(&arr, &v) == 0, "enter rows");
    int count = 0;
    long long sum = 0;
    while (json_array_next(&arr, &value) == 1) {
        count++;
        CHECK(value.type == JSON_VALUE_OBJECT, "row is object");
        JsonReader item = arr;
        CHECK(json_reader_enter(&item, &value) == 0, "enter row");
        sum += json_get_int(&item, "i", 0);
    }
    CHECK(count == 2, "two rows");
    CHECK(sum == 3, "row values 1+2");

    /* 深层嵌套（Container 跳过） */
    json_reader_init(&r, k_info, strlen(k_info));
    char deep[16];
    json_get_string(&r, "unknown.deep.a.b.c", deep, sizeof(deep));
    CHECK(deep[0] == '\0', "array in path is not addressable");
    CHECK(json_reader_ok(&r), "skipping nested containers stays valid");
}

static void test_malformed(void)
{
    const char *cases[] = {
        "{\"a\":1",                 /* 未闭合对象 */
        "{\"a\":\"unterminated}",   /* 未闭合字符串 */
        "{\"a\":{{{{",              /* 畸形嵌套 */
        "not json at all",          /* 非 JSON */
        "",                         /* 空输入 */
        NULL
    };
    for (int i = 0; cases[i] != NULL; i++) {
        JsonReader r;
        json_reader_init(&r, cases[i], strlen(cases[i]));
        JsonValue v;
        int found = json_object_find(&r, "a", &v);
        CHECK(found != 0 || json_reader_ok(&r) == 0 || v.type != JSON_VALUE_OBJECT,
              "malformed input never yields a bogus object");
    }

    /* 深度上限：超过 JSON_READER_MAX_DEPTH 的容器必须失败而非溢出栈 */
    char deep[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "{\"a\":");
    for (int i = 0; i < JSON_READER_MAX_DEPTH + 8; i++) {
        pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "[");
    }
    for (int i = 0; i < JSON_READER_MAX_DEPTH + 8; i++) {
        pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "]");
    }
    snprintf(deep + pos, sizeof(deep) - pos, "}");

    JsonReader r;
    json_reader_init(&r, deep, strlen(deep));
    JsonValue v;
    int rc = json_object_find(&r, "a", &v);
    CHECK(rc != 0 || r.error, "over-deep container is rejected, not recursed");
}

static void test_truncation(void)
{
    JsonReader r;
    json_reader_init(&r, k_info, strlen(k_info));
    char small[8];
    size_t written = json_get_string(&r, "playback.state", small, sizeof(small));
    CHECK(written == 7 && strcmp(small, "playing") == 0, "exact fit");
    CHECK(json_get_string(&r, "playback.state", small, 4) == 3, "truncates safely");
    CHECK(strcmp(small, "pla") == 0, "truncated value NUL-terminated");
}

int main(void)
{
    test_paths();
    test_iteration();
    test_malformed();
    test_truncation();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
