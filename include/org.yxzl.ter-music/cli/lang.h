/**
 * @file lang.h
 * @brief CLI 语言选项解析、只读配置读取与 i18n 初始化
 *
 * 语言优先级（与已确认规格一致）：
 *   --lang > TER_MUSIC_CLI_LANG > 配置 ui_language > LC_ALL > LC_MESSAGES > LANG > zh_CN
 *
 * 本模块只读取配置文件，不触发迁移、不写盘。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CLI_LANG_H
#define CLI_LANG_H

#include <stddef.h>

/**
 * 预扫描并移除 argv 中的 --lang <id> / --lang=<id>（可写在任意位置）。
 * 结果保存在内部静态状态；重复调用幂等（不会清除先前已解析的值）。
 * 空值或超长（> 64）的值被忽略并保持未设置，不在解析阶段报错。
 */
void cli_lang_extract_option(int *argc, char **argv);

/**
 * 解析最终语言 ID（静态缓冲，返回只读指针）。
 * 配置读取为只读：文件不存在/解析失败时视为无配置语言，继续按
 * locale 环境变量回退。
 */
const char *cli_lang_resolve(void);

/**
 * 解析 + i18n_init；返回最终语言 ID。
 * 若显式 --lang / TER_MUSIC_CLI_LANG 指向未安装语言，向 stderr 输出
 * 一行警告并回退到 zh_CN。可重复调用（幂等），不泄漏内存。
 */
const char *cli_lang_init(void);

/**
 * 是否由 --lang/TER_MUSIC_CLI_LANG 显式指定（供调用方决定告警）。
 */
int cli_lang_is_explicit(void);

/**
 * 归一化 locale / 语言 ID（测试与内部解析共用）：
 *   - 去掉 @修饰 与 .编码；
 *   - 空值、C、POSIX → 无偏好（out 为空串）；
 *   - zh* → zh_CN，en* → en_US；其他保留（如 ja_JP）；
 *   - '-' 归一化为 '_'；只接受字母/数字/下划线/连字符，长度 <= 64；
 *   - 非法值回退 zh_CN。
 * @param raw       输入字符串（可为 NULL）
 * @param out       输出缓冲
 * @param out_size  输出缓冲大小
 * @return out（参数非法时返回 NULL）
 */
const char *cli_lang_normalize(const char *raw, char *out, size_t out_size);

#endif /* CLI_LANG_H */
