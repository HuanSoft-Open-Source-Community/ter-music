/**
 * @file test_utf8_safe.c
 * @brief util/utf8_safe.c 的单元测试
 *
 * 覆盖：
 *   - 字节级 UTF-8 校验的严格边界（过长编码、代理区、超范围、孤立续字节）；
 *   - 有界复制在完整码点边界截断；
 *   - 非法字节替换为 U+FFFD；
 *   - 本次《四重罪孽》崩溃的 75 字节状态消息放入 64 字节缓冲区的回归用例。
 *
 * 运行：scripts/test/run-unit-tests.sh test_utf8_safe
 * 依赖（见同名 .srcs）：src/cn.org.huansoft.ter-music/util/utf8_safe.c
 */

#include "util/utf8_safe.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static void test_validator(void)
{
    CHECK(utf8_is_valid(NULL), "NULL is treated as empty");
    CHECK(utf8_is_valid(""), "empty string is valid");
    CHECK(utf8_is_valid("abc"), "ASCII is valid");
    CHECK(utf8_is_valid("四重罪孽"), "CJK is valid");
    CHECK(utf8_is_valid("\xF0\x9F\x98\x80"), "4-byte emoji is valid");
    CHECK(utf8_is_valid("\xF4\x8F\xBF\xBF"), "U+10FFFF is valid");

    CHECK(!utf8_is_valid("\x80"), "lone continuation is invalid");
    CHECK(!utf8_is_valid("\xC0\x80"), "overlong 2-byte is invalid");
    CHECK(!utf8_is_valid("\xE0\x80\x80"), "overlong 3-byte is invalid");
    CHECK(!utf8_is_valid("\xED\xA0\x80"), "UTF-16 surrogate is invalid");
    CHECK(!utf8_is_valid("\xF4\x90\x80\x80"), "beyond U+10FFFF is invalid");
    CHECK(!utf8_is_valid("\xE6"), "truncated 3-byte is invalid");
    CHECK(!utf8_is_valid("\xE6\xAD"), "truncated 3-byte tail is invalid");
    CHECK(!utf8_is_valid("\xE6\x28\x80"), "bad continuation is invalid");

    CHECK(utf8_is_valid_bytes((const unsigned char *)"ok", 2), "bytes validator accepts ASCII");
    CHECK(!utf8_is_valid_bytes((const unsigned char *)"\xE6", 1), "bytes validator rejects truncated");
}

static void test_truncation(void)
{
    /* 本次崩溃的原始消息：75 字节；写入 64 字节缓冲区时第 63 字节落在
     * “正”的 3 字节序列中间，旧实现会留下非法 UTF-8。 */
    static const char message[] =
        "正在播放：四重罪孽 - 洛天依Official、言和、乐正绫、DELA";
    CHECK(strlen(message) == 75, "reproducer message is 75 bytes");

    char buffer[64];
    size_t written = utf8_sanitize_copy(buffer, sizeof(buffer), message);
    CHECK(written == 62, "copies up to the last complete code point");
    CHECK(utf8_is_valid(buffer), "truncated buffer stays valid UTF-8");
    CHECK(buffer[63] == '\0', "buffer is NUL terminated");

    char expected[63];
    memcpy(expected, message, 62);
    expected[62] = '\0';
    CHECK(strcmp(buffer, expected) == 0, "drops the partial character whole");

    /* 短缓冲区与码点边界 */
    char small[5];
    written = utf8_sanitize_copy(small, sizeof(small), "四重");
    CHECK(written == 3, "second CJK char does not fit");
    CHECK(strcmp(small, "四") == 0, "keeps only the complete first char");
    CHECK(utf8_is_valid(small), "short buffer remains valid");

    char one[1];
    written = utf8_sanitize_copy(one, sizeof(one), "四");
    CHECK(written == 0, "size 1 can only hold NUL");
    CHECK(one[0] == '\0', "size 1 writes only NUL");

    char untouched[4] = "xyz";
    written = utf8_sanitize_copy(untouched, 0, "四");
    CHECK(written == 0, "size 0 is a no-op");
    CHECK(strcmp(untouched, "xyz") == 0, "size 0 does not write");
}

static void test_replacement(void)
{
    char buffer[32];

    size_t written = utf8_sanitize_copy(buffer, sizeof(buffer), "A\xE6\xAD");
    CHECK(written == 7, "two invalid bytes become two replacements");
    CHECK(utf8_is_valid(buffer), "replacement output is valid UTF-8");
    CHECK(strstr(buffer, "\xEF\xBF\xBD") != NULL, "U+FFFD is present");

    written = utf8_sanitize_copy(buffer, sizeof(buffer), "\xC0\x80");
    CHECK(written == 6, "overlong pair becomes two replacements");
    CHECK(utf8_is_valid(buffer), "overlong output is valid UTF-8");

    written = utf8_sanitize_copy(buffer, sizeof(buffer), "中文\x80");
    CHECK(utf8_is_valid(buffer), "valid prefix plus bad byte stays valid");
    CHECK(written == 6 + 3, "valid prefix preserved, bad byte replaced");

    written = utf8_sanitize_copy(buffer, sizeof(buffer), NULL);
    CHECK(written == 0, "NULL source yields empty output");
    CHECK(buffer[0] == '\0', "NULL source writes NUL");
}

int main(void)
{
    test_validator();
    test_truncation();
    test_replacement();

    if (g_failures == 0) {
        printf("PASS utf8_safe (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL utf8_safe (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
