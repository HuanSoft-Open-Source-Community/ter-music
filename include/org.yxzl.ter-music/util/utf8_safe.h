/**
 * @file utf8_safe.h
 * @brief 字节级 UTF-8 校验与安全复制（无 UI / 无 locale 依赖）
 *
 * 与 util/utf8.h 的宽度/截断工具不同，这里的函数只按 UTF-8 字节规则工作，
 * 不调用 mbrtowc，因此无界面线程（播放线程、daemon 媒体循环）也能安全使用。
 *
 * 主要用途：D-Bus 的 DBUS_TYPE_STRING 要求合法 UTF-8，libdbus 遇到非法
 * 序列会直接断言并 abort 整个进程。所有可能携带曲目标签、文件路径、
 * 歌词或状态文本的有界缓冲区，都应在写入 D-Bus 前经过本模块。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UTIL_UTF8_SAFE_H
#define UTIL_UTF8_SAFE_H

#include <stddef.h>

/**
 * @brief 按 D-Bus / Unicode 规则校验一段字节。
 *
 * 拒绝过长编码（C0/C1）、代理区（ED A0..BF）、超过 U+10FFFF 的码点、
 * 非法续字节与孤立续字节；允许 ASCII 与嵌入式 NUL。
 *
 * @param data   输入字节（可为 NULL，此时视为空串）
 * @param length 输入长度
 * @return 1 = 合法 UTF-8；0 = 非法
 */
int utf8_is_valid_bytes(const unsigned char *data, size_t length);

/**
 * @brief 校验一个以 NUL 结尾的字符串。
 *
 * @param text 输入字符串（NULL 视为空串）
 * @return 1 = 合法 UTF-8；0 = 非法
 */
int utf8_is_valid(const char *text);

/**
 * @brief 复制字符串，并保证目标缓冲区始终是合法 UTF-8。
 *
 * 规则：
 *   - 合法的多字节序列原样复制；
 *   - 非法字节逐个替换为 U+FFFD（EF BF BD）；
 *   - 目标空间不足时在**完整码点边界**停止，绝不写出半截序列；
 *   - 总是写结尾 NUL（dest_size > 0 时）。
 *
 * 不支持 src 与 dest 重叠；调用方需要原地清洗时请先复制到临时缓冲区。
 *
 * @param dest      目标缓冲区
 * @param dest_size 目标缓冲区大小（含结尾 NUL）
 * @param src       源字符串（可为 NULL）
 * @return 写入的字节数（不含结尾 NUL）
 */
size_t utf8_sanitize_copy(char *dest, size_t dest_size, const char *src);

#endif /* UTIL_UTF8_SAFE_H */
