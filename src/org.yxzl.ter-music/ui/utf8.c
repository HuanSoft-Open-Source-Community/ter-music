/**
 * @file utf8.c — UTF-8 string utility functions
 * Extracted from the original ui.c monolith.
 */
#include "types.h"
#include <ncursesw/ncurses.h>
#include "ui/ui.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include <string.h>

extern int g_ascii_fallback_ui;
#include <wchar.h>
#include <locale.h>
#include <langinfo.h>

int use_ascii_fallback_ui(void) {
    return g_ascii_fallback_ui;
}

int utf8_str_truncate(char *dest, const char *src, int max_cols) {
    if (!dest || !src || max_cols <= 0) {
        if (dest) *dest = '\0';
        return 0;
    }

    int cols = 0;
    char *d = dest;
    const char *s = src;

    while (*s && cols < max_cols) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0 || cols + char_width > max_cols) {
            break;
        }

        memcpy(d, s, char_len);
        d += char_len;
        s += char_len;
        cols += char_width;
    }

    if (*s && cols + 3 <= max_cols) {
        memcpy(d, "...", 4);
    } else {
        *d = '\0';
    }

    return cols;
}

/**
 * 计算 UTF-8 字符串的显示宽度（列数）
 */
int utf8_str_width(const char *src) {
    if (!src) return 0;
    int cols = 0;
    const char *s = src;

    while (*s) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0) {
            break;
        }

        s += char_len;
        cols += char_width;
    }
    return cols;
}

/**
 * UTF-8 字符串从指定偏移开始截取
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param start_col 起始列偏移（从 0 开始）
 * @param max_cols 最大列数
 * @return 实际占用的列数
 */
int utf8_str_substring(char *dest, const char *src, int start_col, int max_cols) {
    if (!dest || !src || max_cols <= 0) {
        if (dest) *dest = '\0';
        return 0;
    }

    int cols = 0;
    char *d = dest;
    const char *s = src;
    int result_cols = 0;
    int leading_padding = 0;

    while (*s && cols < start_col) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0) {
            break;
        }

        if (cols + char_width > start_col) {
            leading_padding = cols + char_width - start_col;
            s += char_len;
            break;
        }

        s += char_len;
        cols += char_width;
    }

    while (leading_padding > 0 && result_cols < max_cols) {
        *d++ = ' ';
        leading_padding--;
        result_cols++;
    }

    while (*s && result_cols < max_cols) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0 || result_cols + char_width > max_cols) {
            break;
        }

        memcpy(d, s, char_len);
        d += char_len;
        s += char_len;
        result_cols += char_width;
    }

    *d = '\0';

    return result_cols;
}

int utf8_str_pad(char *dest, size_t dest_size, const char *src, int width) {
    if (!dest || dest_size == 0 || width <= 0) {
        if (dest && dest_size > 0) {
            dest[0] = '\0';
        }
        return 0;
    }

    utf8_str_truncate(dest, src ? src : "", width);

    int current_width = utf8_str_width(dest);
    size_t len = strlen(dest);
    // 使用条件表达式避免 GCC 的 -Wstringop-overflow 误报
    while (current_width < width && len < dest_size - 1) {
        if (len < dest_size - 1) {
            dest[len] = ' ';
            len++;
        }
        current_width++;
    }
    if (len < dest_size) {
        dest[len] = '\0';
    }
    return current_width;
}

int utf8_backspace(char *buffer, int pos, WINDOW *win) {
    if (!buffer || pos <= 0) {
        return pos;
    }

    WINDOW *target_win = win ? win : stdscr;
    int cx = getcurx(target_win);
    int cy = getcury(target_win);
    unsigned char last_c = (unsigned char)buffer[pos - 1];
    int bytes_to_remove = 1;

    if (last_c >= 0x80) {
        if ((last_c & 0xE0) == 0xC0) bytes_to_remove = 2;
        else if ((last_c & 0xF0) == 0xE0) bytes_to_remove = 3;
        else if ((last_c & 0xF8) == 0xF0) bytes_to_remove = 4;
        else if ((last_c & 0xC0) == 0x80) {
            bytes_to_remove = 2;
            while (pos - bytes_to_remove >= 0 &&
                   (unsigned char)buffer[pos - bytes_to_remove] >= 0x80 &&
                   (unsigned char)buffer[pos - bytes_to_remove] < 0xC0) {
                bytes_to_remove++;
            }
        }
        if (bytes_to_remove > pos) bytes_to_remove = pos;
        pos -= bytes_to_remove;

        if ((last_c & 0xF0) == 0xE0 || (last_c & 0xE0) == 0xC0) {
            wmove(target_win, cy, cx - 2);
        } else {
            wmove(target_win, cy, cx - 1);
        }
    } else {
        pos--;
        wmove(target_win, cy, cx - 1);
    }

    wclrtoeol(target_win);
    wrefresh(target_win);
    return pos;
}

/**
 * 创建和调整窗口布局
 * 设置播放列表、控制栏和歌词窗口的大小和位置
 */
