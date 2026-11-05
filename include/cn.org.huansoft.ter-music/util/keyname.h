/**
 * @file keyname.h
 * @brief 键名解析与格式化（不依赖 ncurses / 配置）
 *
 * 键名语法（v1）：
 *  - ASCII 可打印字符 0x21..0x7E，单字符，大小写敏感；空格只能写 "space"；
 *  - ctrl+a .. ctrl+z 的非歧义子集（禁止 c/h/i/j/m）；
 *  - 命名特殊键：up down left right home end pgup pgdn ins del backspace
 *    tab btab enter esc space f1..f12（必须全小写）。
 *
 * parse 与 format 互为逆运算：parse(format(x)) == x，且 format(parse(s)) 为 s
 * 的规范形式。
 */

#ifndef UTIL_KEYNAME_H
#define UTIL_KEYNAME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYNAME_MAX_LEN 24

typedef enum {
    KEYNAME_UNKNOWN = 0,
    KEYNAME_CHAR,
    KEYNAME_CTRL,
    KEYNAME_SPECIAL
} KeyNameKind;

typedef enum {
    KEYSPEC_UP = 1,
    KEYSPEC_DOWN,
    KEYSPEC_LEFT,
    KEYSPEC_RIGHT,
    KEYSPEC_HOME,
    KEYSPEC_END,
    KEYSPEC_PGUP,
    KEYSPEC_PGDN,
    KEYSPEC_INS,
    KEYSPEC_DEL,
    KEYSPEC_BACKSPACE,
    KEYSPEC_TAB,
    KEYSPEC_BTAB,
    KEYSPEC_ENTER,
    KEYSPEC_ESC,
    KEYSPEC_SPACE,
    KEYSPEC_F1,
    KEYSPEC_F2,
    KEYSPEC_F3,
    KEYSPEC_F4,
    KEYSPEC_F5,
    KEYSPEC_F6,
    KEYSPEC_F7,
    KEYSPEC_F8,
    KEYSPEC_F9,
    KEYSPEC_F10,
    KEYSPEC_F11,
    KEYSPEC_F12 = KEYSPEC_F1 + 11
} KeySpecial;

typedef struct {
    KeyNameKind kind;
    int code;   /* CHAR: ASCII 0x21..0x7E；CTRL: 'a'..'z'；SPECIAL: KeySpecial */
} KeyStroke;

/**
 * 解析键名。成功返回 0，失败返回 -1（空/NULL/超长/非法一律失败）。
 * 输出仅在成功时有效。
 */
int keyname_parse(const char *text, KeyStroke *out);

/**
 * 规范形式输出（可打印字符原样；ctrl+x；up/down/.../btab/enter/esc/space/f1..f12）。
 * 返回 0 成功，-1 表示参数非法或缓冲不足。
 */
int keyname_format(KeyStroke stroke, char *out, size_t out_size);

/**
 * 显示形式：ctrl+x -> Ctrl+X；btab -> Shift+Tab；其他同 keyname_format。
 */
int keyname_format_display(KeyStroke stroke, char *out, size_t out_size);

/**
 * 是否可作为用户绑定值。KEYSPEC_ESC（系统保留）与无法解析的控制键返回 0。
 */
int keyname_is_bindable(KeyStroke stroke);

/** 结构相等（kind 与 code 均相同）。 */
int keyname_equal(KeyStroke a, KeyStroke b);

#ifdef __cplusplus
}
#endif

#endif /* UTIL_KEYNAME_H */
