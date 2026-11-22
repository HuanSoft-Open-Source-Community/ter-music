/**
 * @file appearance_terminal.h
 * @brief 按终端记住外观模式：身份键 + frontend-local JSON 持久化
 *
 * 本模块只负责“前端本地事实”：
 *   - 终端身份键（TERM_PROGRAM/祖先进程名/TERM/COLORTERM 等，见
 *     config/appearance.h 的 appearance_terminal_key_build()）；
 *   - `~/.config/ter-music/appearance-terminals.json`（version 1）的读写。
 *
 * 它**绝不**写 `config.xml`：全局 `preferences.appearance_mode` 仍是默认值，
 * 本终端覆盖只在当前终端生效（Design t15 D6）。文件读写为原子 temp+rename；
 * 损坏/未知版本安全降级。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_APPEARANCE_TERMINAL_H
#define UI_APPEARANCE_TERMINAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPEARANCE_TERMINALS_MAX       64
#define APPEARANCE_TERMINAL_KEY_MAX    160

typedef struct {
    char key[APPEARANCE_TERMINAL_KEY_MAX];
    int mode;   /* APPEARANCE_MODE_*（0 auto / 1 light / 2 dark） */
} AppearanceTerminalEntry;

typedef struct {
    int version;      /* 文件版本；缺失/0 视为 1 */
    int read_only;    /* version > 1：只读，不覆盖 */
    int count;
    AppearanceTerminalEntry entries[APPEARANCE_TERMINALS_MAX];
} AppearanceTerminalMap;

/* ── 可单测的文件级 API（显式路径，不依赖 app_config_dir） ───────── */

/** @brief 读取覆盖文件；0=成功（含不存在/损坏→空表）、-1=内存错误。 */
int appearance_terminals_load_from(const char *path, AppearanceTerminalMap *map);

/** @brief 原子写（temp+rename）；0=成功、-1=失败。read_only 表拒绝写。 */
int appearance_terminals_save_to(const char *path, const AppearanceTerminalMap *map);

/** @brief 查某终端键的模式；返回 APPEARANCE_MODE_* 或 -1（Inherit）。 */
int appearance_terminals_get(const AppearanceTerminalMap *map, const char *key);

/**
 * @brief 设置/删除某终端键。
 * @param mode APPEARANCE_MODE_AUTO/LIGHT/DARK 或 -1（删除条目）
 * @return 0=成功、-1=表满/参数非法。
 */
int appearance_terminals_set(AppearanceTerminalMap *map, const char *key, int mode);

/* ── 运行期 API（由 ui/appearance.c 调用） ────────────────────────── */

/** @brief 启动时一次性初始化：身份键 + 读取覆盖文件；可重复调用（幂等）。 */
void appearance_terminal_init(void);

/** @brief 当前终端身份键（已初始化；未初始化时为 "default"）。 */
const char *appearance_terminal_key(void);

/** @brief 本终端覆盖模式；-1=Inherit（使用全局默认）。 */
int appearance_terminal_override_get(void);

/** @brief 当前终端是否为 deepin-terminal（决定终端主题适配器是否启用）。 */
int appearance_terminal_is_deepin(void);

/**
 * @brief 设置本终端覆盖并落盘（persist-first：失败不改内存）。
 * @param mode APPEARANCE_MODE_AUTO/LIGHT/DARK 或 -1（清除）
 * @return 0=成功、-1=保存失败/非法。
 */
int appearance_terminal_set_override(int mode);

/** @brief 覆盖文件路径（app_config_dir(1) 下）；只读，供状态/帮助显示。 */
const char *appearance_terminal_file_path(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_APPEARANCE_TERMINAL_H */
