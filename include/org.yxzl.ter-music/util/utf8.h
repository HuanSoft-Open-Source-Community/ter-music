/**
 * @file utf8.h
 * @brief UTF-8 字符串宽度/截断工具（无 ncurses 依赖声明）
 *
 * 实现位于 util/utf8.c。原先是 ui/ 下的头文件（因最早只服务 TUI），
 * 但后端（info/）与 CLI 都要用它做宽度计算与截断，2026-09-15 的架构
 * 调整把它搬到 util/：后端不得引用任何 ui/ 头文件。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UTIL_UTF8_H
#define UTIL_UTF8_H

#include <stddef.h>
#include <wchar.h>

/* 按终端显示列宽截断（超宽截断并追加 "..."），返回写入的列数 */
int utf8_str_truncate(char *dest, const char *src, int max_cols);

/* 字符串的终端显示列宽 */
int utf8_str_width(const char *src);

/* 从第 start_col 列起取最多 max_cols 列 */
int utf8_str_substring(char *dest, const char *src, int start_col, int max_cols);

/* 按列宽填充（不足补空格） */
int utf8_str_pad(char *dest, size_t dest_size, const char *src, int width);

/* 读取下一个字符，返回其字节长度，并输出码点与列宽 */
size_t utf8_next_char(const char *src, wchar_t *wc_out, int *width_out);

/* 终端/本地化是否需要 ASCII 回退（无颜色/无 UTF-8 时） */
int use_ascii_fallback_ui(void);

/* 确保进程 locale 支持 UTF-8（TUI 在 init_ncurses 中调用；
 * daemon / CLI 等无界面路径也必须调用，否则 mbrtowc 会按单字节
 * 处理中文，导致宽度计算与截断切断多字节字符）。 */
void ensure_utf8_locale(void);

#endif /* UTIL_UTF8_H */
