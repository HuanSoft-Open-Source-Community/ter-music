/**
 * @file appearance_terminal_theme.h
 * @brief 终端主题适配器（首个：deepin-terminal）运行期接口
 *
 * 只在当前终端身份匹配 deepin-terminal 时启用；只读其 config.conf 与
 * color-schemes/*.colorscheme，绝不写入。appearance tick 每 ≤2s 仅 stat
 * 配置/主题文件（mtime+size），变化时才重新读取判定，然后由
 * appearance.c 重新裁决并重绘。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_APPEARANCE_TERMINAL_THEME_H
#define UI_APPEARANCE_TERMINAL_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 适配器是否适用于当前终端（deepin-terminal 身份）。 */
int appearance_terminal_theme_supported(void);

/**
 * @brief 立即完整探测一次（读取 config.conf + 主题文件）。
 * @param bg_out APPEARANCE_BG_*（unknown 时为 UNKNOWN）
 * @return 1=成功判定亮暗；0=不适用/unknown
 */
int appearance_terminal_theme_probe(int *bg_out);

/** @brief 低频轮询（≤2s 一次，仅 stat；变化时才读文件）。 */
void appearance_terminal_theme_tick(void);

/**
 * @brief 取走自上次调用以来适配器结果的变化。
 * @param bg_out 最新结果（UNKNOWN 表示适配器现在无法判定）
 * @return 1=有变化、0=无
 */
int appearance_terminal_theme_take_changed(int *bg_out);

#ifdef __cplusplus
}
#endif

#endif /* UI_APPEARANCE_TERMINAL_THEME_H */
