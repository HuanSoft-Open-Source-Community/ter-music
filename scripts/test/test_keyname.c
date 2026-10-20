/*
 * util/keyname 单测：键名解析、规范化、往返、歧义拒绝。
 *
 * 运行：scripts/test/run-unit-tests.sh test_keyname
 * 依赖（见 test_keyname.srcs）：src/.../util/keyname.c
 */

#include "util/keyname.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static int parses(const char *text, KeyNameKind kind, int code)
{
    KeyStroke stroke;
    if (keyname_parse(text, &stroke) != 0) {
        return 0;
    }
    return stroke.kind == kind && stroke.code == code;
}

static int round_trips(const char *text)
{
    KeyStroke stroke;
    char out[KEYNAME_MAX_LEN];
    if (keyname_parse(text, &stroke) != 0) {
        return 0;
    }
    if (keyname_format(stroke, out, sizeof(out)) != 0) {
        return 0;
    }
    return strcmp(text, out) == 0;
}

int main(void)
{
    KeyStroke stroke;
    char out[KEYNAME_MAX_LEN];

    /* ── 合法单字符 ── */
    CHECK(parses("a", KEYNAME_CHAR, 'a'), "字符 a");
    CHECK(parses("Z", KEYNAME_CHAR, 'Z'), "字符大写 Z 保留大小写");
    CHECK(parses("0", KEYNAME_CHAR, '0'), "数字字符");
    CHECK(parses("!", KEYNAME_CHAR, '!'), "符号字符");
    CHECK(parses("~", KEYNAME_CHAR, '~'), "波浪号");

    /* ── Ctrl 组合（非歧义） ── */
    CHECK(parses("ctrl+a", KEYNAME_CTRL, 'a'), "ctrl+a");
    CHECK(parses("ctrl+y", KEYNAME_CTRL, 'y'), "ctrl+y");
    CHECK(parses("ctrl+l", KEYNAME_CTRL, 'l'), "ctrl+l");

    /* ── 命名特殊键 ── */
    CHECK(parses("up", KEYNAME_SPECIAL, KEYSPEC_UP), "up");
    CHECK(parses("down", KEYNAME_SPECIAL, KEYSPEC_DOWN), "down");
    CHECK(parses("left", KEYNAME_SPECIAL, KEYSPEC_LEFT), "left");
    CHECK(parses("right", KEYNAME_SPECIAL, KEYSPEC_RIGHT), "right");
    CHECK(parses("home", KEYNAME_SPECIAL, KEYSPEC_HOME), "home");
    CHECK(parses("end", KEYNAME_SPECIAL, KEYSPEC_END), "end");
    CHECK(parses("pgup", KEYNAME_SPECIAL, KEYSPEC_PGUP), "pgup");
    CHECK(parses("pgdn", KEYNAME_SPECIAL, KEYSPEC_PGDN), "pgdn");
    CHECK(parses("ins", KEYNAME_SPECIAL, KEYSPEC_INS), "ins");
    CHECK(parses("del", KEYNAME_SPECIAL, KEYSPEC_DEL), "del");
    CHECK(parses("backspace", KEYNAME_SPECIAL, KEYSPEC_BACKSPACE), "backspace");
    CHECK(parses("tab", KEYNAME_SPECIAL, KEYSPEC_TAB), "tab");
    CHECK(parses("btab", KEYNAME_SPECIAL, KEYSPEC_BTAB), "btab");
    CHECK(parses("enter", KEYNAME_SPECIAL, KEYSPEC_ENTER), "enter");
    CHECK(parses("esc", KEYNAME_SPECIAL, KEYSPEC_ESC), "esc");
    CHECK(parses("space", KEYNAME_SPECIAL, KEYSPEC_SPACE), "space");
    CHECK(parses("f1", KEYNAME_SPECIAL, KEYSPEC_F1), "f1");
    CHECK(parses("f9", KEYNAME_SPECIAL, KEYSPEC_F9), "f9");
    CHECK(parses("f12", KEYNAME_SPECIAL, KEYSPEC_F12), "f12");

    /* ── 非法输入 ── */
    CHECK(keyname_parse(NULL, &stroke) != 0, "NULL 失败");
    CHECK(keyname_parse("", &stroke) != 0, "空串失败");
    CHECK(keyname_parse(" ", &stroke) != 0, "裸空格失败");
    CHECK(keyname_parse("ab", &stroke) != 0, "多字符失败");
    CHECK(keyname_parse("UP", &stroke) != 0, "命名键大小写混写失败");
    CHECK(keyname_parse("Up", &stroke) != 0, "命名键首字母大写失败");
    CHECK(keyname_parse("Ctrl+a", &stroke) != 0, "ctrl 前缀大写失败");
    CHECK(keyname_parse("ctrl+A", &stroke) != 0, "ctrl 字母大写失败");
    CHECK(keyname_parse("alt+x", &stroke) != 0, "alt 组合失败");
    CHECK(keyname_parse("ctrl+c", &stroke) != 0, "ctrl+c（SIGINT）失败");
    CHECK(keyname_parse("ctrl+h", &stroke) != 0, "ctrl+h（Backspace）失败");
    CHECK(keyname_parse("ctrl+i", &stroke) != 0, "ctrl+i（Tab）失败");
    CHECK(keyname_parse("ctrl+j", &stroke) != 0, "ctrl+j（LF）失败");
    CHECK(keyname_parse("ctrl+m", &stroke) != 0, "ctrl+m（CR）失败");
    CHECK(keyname_parse("ctrl+z", &stroke) != 0, "ctrl+z（SIGTSTP）失败");
    CHECK(keyname_parse("f13", &stroke) != 0, "f13 失败");
    CHECK(keyname_parse("F1", &stroke) != 0, "F1 失败");
    CHECK(keyname_parse("esc ", &stroke) != 0, "尾随空格失败");
    CHECK(keyname_parse("\x1b", &stroke) != 0, "裸控制字符失败");
    CHECK(keyname_parse("\x7f", &stroke) != 0, "DEL 字符失败");
    CHECK(keyname_parse("\xC3\xA9", &stroke) != 0, "非 ASCII 失败");
    CHECK(keyname_parse("aaaaaaaaaaaaaaaaaaaaaaaa", &stroke) != 0, "超长失败");

    /* ── 往返：parse → format 保持规范形式 ── */
    static const char *const canonical[] = {
        "a", "Z", "0", "!", "~",
        "ctrl+a", "ctrl+y", "ctrl+l",
        "up", "down", "left", "right", "home", "end", "pgup", "pgdn",
        "ins", "del", "backspace", "tab", "btab", "enter", "esc", "space",
        "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        NULL
    };
    for (int i = 0; canonical[i]; i++) {
        CHECK(round_trips(canonical[i]), canonical[i]);
    }

    /* 手工构造的 stroke 也能格式化 */
    stroke.kind = KEYNAME_CTRL;
    stroke.code = 'l';
    CHECK(keyname_format(stroke, out, sizeof(out)) == 0 && strcmp(out, "ctrl+l") == 0,
          "format ctrl+l");

    /* ── 显示形式 ── */
    CHECK(keyname_parse("ctrl+l", &stroke) == 0 &&
          keyname_format_display(stroke, out, sizeof(out)) == 0 &&
          strcmp(out, "Ctrl+L") == 0, "display Ctrl+L");
    CHECK(keyname_parse("btab", &stroke) == 0 &&
          keyname_format_display(stroke, out, sizeof(out)) == 0 &&
          strcmp(out, "Shift+Tab") == 0, "display Shift+Tab");
    CHECK(keyname_parse("up", &stroke) == 0 &&
          keyname_format_display(stroke, out, sizeof(out)) == 0 &&
          strcmp(out, "up") == 0, "display up");
    CHECK(keyname_parse("A", &stroke) == 0 &&
          keyname_format_display(stroke, out, sizeof(out)) == 0 &&
          strcmp(out, "A") == 0, "display 保留字符大小写");

    /* ── 可绑定判定 ── */
    CHECK(keyname_parse("esc", &stroke) == 0 && keyname_is_bindable(stroke) == 0,
          "esc 不可绑定");
    stroke.kind = KEYNAME_CTRL;
    stroke.code = 'c';
    CHECK(keyname_is_bindable(stroke) == 0, "ctrl+c 不可绑定");
    CHECK(keyname_parse("tab", &stroke) == 0 && keyname_is_bindable(stroke) == 1,
          "tab 可绑定");
    CHECK(keyname_parse("ctrl+q", &stroke) == 0 && keyname_is_bindable(stroke) == 1,
          "ctrl+q 可绑定");
    CHECK(keyname_parse("a", &stroke) == 0 && keyname_is_bindable(stroke) == 1,
          "字符可绑定");

    /* ── 边界与非法参数不崩溃 ── */
    CHECK(keyname_format(stroke, NULL, 32) != 0, "format NULL 失败");
    CHECK(keyname_parse("a", &stroke) == 0 &&
          keyname_format(stroke, out, 1) != 0, "缓冲不足失败");
    CHECK(keyname_parse("a", &stroke) == 0 &&
          keyname_format(stroke, out, 2) == 0 && strcmp(out, "a") == 0,
          "单字符缓冲恰好可用");
    CHECK(keyname_parse("up", &stroke) == 0 &&
          keyname_format(stroke, out, 2) != 0, "命名键缓冲不足失败");
    stroke.kind = KEYNAME_UNKNOWN;
    stroke.code = 0;
    CHECK(keyname_format(stroke, out, sizeof(out)) != 0, "未知 stroke 格式化失败");
    CHECK(keyname_is_bindable(stroke) == 0, "未知 stroke 不可绑定");

    /* ── 相等 ── */
    KeyStroke a = { KEYNAME_CHAR, 'x' };
    KeyStroke b = { KEYNAME_CHAR, 'x' };
    KeyStroke c = { KEYNAME_CHAR, 'X' };
    CHECK(keyname_equal(a, b) == 1, "相同 stroke 相等");
    CHECK(keyname_equal(a, c) == 0, "大小写不同不相等");

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_keyname\n");
        return 0;
    }
    printf("FAIL test_keyname\n");
    return 1;
}
