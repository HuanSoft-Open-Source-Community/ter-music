/**
 * @file keymap.h
 * @brief UI 键位运行时：覆盖表加载、键码解析与动作解析
 *
 * 依赖 ncursesw（调用方需已初始化 keypad/termios）与 g_app_config。
 */

#ifndef UI_KEYMAP_H
#define UI_KEYMAP_H

#include <stddef.h>

#include "keymap/actions.h"
#include "util/keyname.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 从 g_app_config.keybinding_overrides 重建运行时表；非法项跳过并告警。 */
void keymap_reload(void);

/** 首次调用时自动 reload（供输入解析入口使用）。 */
void keymap_ensure_loaded(void);

/**
 * 按上下文父链查找按键对应的动作：先具体上下文，再逐级回退；
 * 无命中返回 ACTION_NONE。
 */
ActionId keymap_resolve(KeyContext ctx, int ch);

/** 最终有效键（覆盖优先，否则默认），最多写入 max 条；返回条数。 */
int keymap_action_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max);

/** 默认键（忽略覆盖），最多写入 max 条；返回条数。 */
int keymap_action_default_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max);

/** 该动作当前是否有生效的覆盖项。 */
int keymap_action_has_override(ActionId id);

/**
 * 设置覆盖：校验（键名/冲突）→ 写入 g_app_config → reload。
 * 原子：失败时 g_app_config 不变。成功返回 0。
 */
int keymap_set_override(ActionId id, const char *key_name,
                        char *err, size_t err_size);

/** 删除该动作的覆盖并 reload；成功返回 0（不存在也返回 0）。 */
int keymap_remove_override(ActionId id);

/** 只清空可绑定动作的覆盖；系统保留动作的旧覆盖条目保留在配置中。 */
void keymap_remove_all_bindable_overrides(void);

/**
 * ncurses 键码 → 规范键名。无法识别返回 -1。
 * Esc / Ctrl+C 也会被识别（是否允许绑定由上层判断）。
 */
int keymap_keycode_to_name(int ch, char *out, size_t out_size);

/** ncurses 键码 → KeyStroke；无法识别返回 -1。 */
int keymap_keycode_to_stroke(int ch, KeyStroke *out);

/** 系统保留键码（Esc=27、Ctrl+C=3）返回 1。 */
int keymap_is_reserved_keycode(int ch);

/** 动作 i18n 名称；未翻译时回退动作 ID。 */
const char *keymap_action_label(ActionId id);

#ifdef __cplusplus
}
#endif

#endif /* UI_KEYMAP_H */
