/**
 * @file keyname.c
 * @brief 键名解析与格式化实现（见 util/keyname.h）
 */

#include "util/keyname.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    int code;
} SpecialName;

static const SpecialName k_special_names[] = {
    {"up",        KEYSPEC_UP},
    {"down",      KEYSPEC_DOWN},
    {"left",      KEYSPEC_LEFT},
    {"right",     KEYSPEC_RIGHT},
    {"home",      KEYSPEC_HOME},
    {"end",       KEYSPEC_END},
    {"pgup",      KEYSPEC_PGUP},
    {"pgdn",      KEYSPEC_PGDN},
    {"ins",       KEYSPEC_INS},
    {"del",       KEYSPEC_DEL},
    {"backspace", KEYSPEC_BACKSPACE},
    {"tab",       KEYSPEC_TAB},
    {"btab",      KEYSPEC_BTAB},
    {"enter",     KEYSPEC_ENTER},
    {"esc",       KEYSPEC_ESC},
    {"space",     KEYSPEC_SPACE},
    {"f1",        KEYSPEC_F1},
    {"f2",        KEYSPEC_F2},
    {"f3",        KEYSPEC_F3},
    {"f4",        KEYSPEC_F4},
    {"f5",        KEYSPEC_F5},
    {"f6",        KEYSPEC_F6},
    {"f7",        KEYSPEC_F7},
    {"f8",        KEYSPEC_F8},
    {"f9",        KEYSPEC_F9},
    {"f10",       KEYSPEC_F10},
    {"f11",       KEYSPEC_F11},
    {"f12",       KEYSPEC_F12},
};

#define SPECIAL_NAME_COUNT ((int)(sizeof(k_special_names) / sizeof(k_special_names[0])))

static int ctrl_is_ambiguous(int code)
{
    /* c=SIGINT, h=Backspace, i=Tab, j=LF, m=CR, z=SIGTSTP */
    return code == 'c' || code == 'h' || code == 'i' ||
           code == 'j' || code == 'm' || code == 'z';
}

static int stroke_is_well_formed(KeyStroke stroke)
{
    switch (stroke.kind) {
        case KEYNAME_CHAR:
            return stroke.code >= 0x21 && stroke.code <= 0x7E;
        case KEYNAME_CTRL:
            return stroke.code >= 'a' && stroke.code <= 'z';
        case KEYNAME_SPECIAL:
            return stroke.code >= KEYSPEC_UP && stroke.code <= KEYSPEC_F12;
        case KEYNAME_UNKNOWN:
        default:
            return 0;
    }
}

static const char *special_name(int code)
{
    for (int i = 0; i < SPECIAL_NAME_COUNT; i++) {
        if (k_special_names[i].code == code) {
            return k_special_names[i].name;
        }
    }
    return NULL;
}

int keyname_parse(const char *text, KeyStroke *out)
{
    if (!text || !out) {
        return -1;
    }
    out->kind = KEYNAME_UNKNOWN;
    out->code = 0;

    size_t len = strlen(text);
    if (len == 0 || len >= KEYNAME_MAX_LEN) {
        return -1;
    }

    /* 单字符可打印 ASCII（不含空格 0x20） */
    if (len == 1) {
        unsigned char ch = (unsigned char)text[0];
        if (ch < 0x21 || ch > 0x7E) {
            return -1;
        }
        out->kind = KEYNAME_CHAR;
        out->code = (int)ch;
        return 0;
    }

    /* ctrl+x：前缀必须全小写，字母必须单个且非歧义 */
    if (len == 6 && strncmp(text, "ctrl+", 5) == 0) {
        char letter = text[5];
        if (letter < 'a' || letter > 'z' || ctrl_is_ambiguous(letter)) {
            return -1;
        }
        out->kind = KEYNAME_CTRL;
        out->code = letter;
        return 0;
    }

    /* 命名特殊键：必须全小写 */
    for (int i = 0; i < SPECIAL_NAME_COUNT; i++) {
        if (strcmp(text, k_special_names[i].name) == 0) {
            out->kind = KEYNAME_SPECIAL;
            out->code = k_special_names[i].code;
            return 0;
        }
    }

    return -1;
}

int keyname_format(KeyStroke stroke, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!stroke_is_well_formed(stroke)) {
        return -1;
    }

    switch (stroke.kind) {
        case KEYNAME_CHAR:
            if (out_size < 2) {
                return -1;
            }
            out[0] = (char)stroke.code;
            out[1] = '\0';
            return 0;
        case KEYNAME_CTRL:
            if (out_size < 7) {
                return -1;
            }
            snprintf(out, out_size, "ctrl+%c", (char)stroke.code);
            return 0;
        case KEYNAME_SPECIAL: {
            const char *name = special_name(stroke.code);
            if (!name || strlen(name) + 1 > out_size) {
                return -1;
            }
            snprintf(out, out_size, "%s", name);
            return 0;
        }
        default:
            return -1;
    }
}

int keyname_format_display(KeyStroke stroke, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    if (stroke.kind == KEYNAME_CTRL &&
        stroke.code >= 'a' && stroke.code <= 'z') {
        if (out_size < 7) {
            return -1;
        }
        snprintf(out, out_size, "Ctrl+%c", (char)(stroke.code - 'a' + 'A'));
        return 0;
    }

    if (stroke.kind == KEYNAME_SPECIAL && stroke.code == KEYSPEC_BTAB) {
        if (out_size < 10) {
            return -1;
        }
        snprintf(out, out_size, "Shift+Tab");
        return 0;
    }

    return keyname_format(stroke, out, out_size);
}

int keyname_is_bindable(KeyStroke stroke)
{
    if (!stroke_is_well_formed(stroke)) {
        return 0;
    }
    if (stroke.kind == KEYNAME_SPECIAL && stroke.code == KEYSPEC_ESC) {
        return 0;
    }
    if (stroke.kind == KEYNAME_CTRL && ctrl_is_ambiguous(stroke.code)) {
        return 0;
    }
    return 1;
}

int keyname_equal(KeyStroke a, KeyStroke b)
{
    return a.kind == b.kind && a.code == b.code;
}
