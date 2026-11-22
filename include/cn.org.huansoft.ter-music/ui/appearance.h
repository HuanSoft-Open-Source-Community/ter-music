/**
 * @file appearance.h
 * @brief 前端运行期的终端亮/暗探测与输入安全（ncurses 侧）
 *
 * 职责：
 *   - 启动时（配置加载后、首帧前）按 appearance_mode 决定是否探测终端背景；
 *   - 主输入路径的常驻 OSC 11 应答过滤器：终端应答绝不作为按键分发；
 *   - 焦点上报（\e[?1004h）触发的**非阻塞**重探；
 *   - 变体变化时重刷主题/布局颜色与当前视图；
 *   - 模态输入框开始前清理悬挂应答。
 *
 * 探测结果只存在于本进程内存，绝不写回配置文件。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_APPEARANCE_H
#define UI_APPEARANCE_H

#include "config/appearance.h"

#include <ncursesw/ncurses.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 当前生效变体（APPEARANCE_VARIANT_*）；auto + 未知 = 暗色。 */
int appearance_variant(void);

/** @brief 最近一次探测到的背景（最高优先级信号），仅供设置页/日志。 */
int appearance_detected(void);

/** @brief 当前生效来源（APPEARANCE_SOURCE_*，显式模式时为 EXPLICIT）。 */
int appearance_source(void);

/** @brief 探测来源（忽略显式模式；用于 Settings 检测行/手动重探反馈）。 */
int appearance_detection_source(void);

/** @brief 全局默认外观模式（config.xml 的 appearance_mode）。 */
int appearance_global_mode(void);

/**
 * @brief 运行期非阻塞节拍（内部由 appearance_getch/wgetch 每约 100ms 调用）：
 * 泵 XDG portal 消息、处理 SettingChanged 去抖、必要时触发 OSC11 重探。
 */
void appearance_tick(void);

/** @brief 当前终端身份键（如 `anc:deepin-terminal`、`tp:iTerm.app`）。 */
const char *appearance_terminal_key(void);

/** @brief 当前终端的覆盖模式；-1 = Inherit（使用全局默认）。 */
int appearance_terminal_override_get(void);

/**
 * @brief 设置/清除当前终端覆盖（frontend-local JSON；不写 config.xml）。
 * @param mode APPEARANCE_MODE_AUTO/LIGHT/DARK 或 -1（清除）
 * @return 0=成功、-1=保存失败
 */
int appearance_terminal_set_override(int mode);

/**
 * @brief 从 g_app_config 同步外观模式；**首次调用**执行启动探测。
 *
 * 在配置加载之后、apply_color_theme() 之前调用（frontend_init_config 与
 * 配置重载路径）。幂等：同一进程只做一次启动同步探测；显式模式零 I/O。
 */
void appearance_sync_from_config(void);

/**
 * @brief 设置页“重新探测终端背景”：同步探测，最多约 250ms。
 * @return APPEARANCE_BG_LIGHT / DARK / UNKNOWN；变体变化时已重刷界面。
 */
int appearance_force_probe(void);

/**
 * @brief 主循环输入入口（stdscr）：pending 回放 + ESC 消歧 + OSC 11 应答过滤。
 *
 * 行为与 getch() 一致（ERR 表示当前超时内无输入）；迟到的 OSC 11 应答被
 * 吞掉并触发变体重算，绝不把应答字节当按键。第二道防线（ESC 已被别处消费、
 * 只剩 `]11;rgb:...` 余字节）只在“刚发过查询的应答窗口内、且收到完整合法
 * 应答”时才吞；用户手工输入/粘贴的字面量在该窗口内最多经过 80ms 有界预读
 * 后按原序回放，不会丢字或卡住。
 */
int appearance_getch(void);

/**
 * @brief 任意模态窗口的等价输入入口（用于弹层循环里的 wgetch）。
 *
 * 过滤逻辑与 appearance_getch() 完全相同，只是从 `win` 读取；返回前把
 * `win` 的超时恢复为 40ms（与主循环 / 既有弹层一致）。
 */
int appearance_wgetch(WINDOW *win);

/**
 * @brief 焦点上报按键处理（xterm \e[I / \e[O]）。
 * @return 1 = 已消费（\e[I 已发出非阻塞重探查询）；0 = 不是焦点键。
 */
int appearance_handle_focus(int ch);

/**
 * @brief 模态输入框开始前非阻塞清理一次悬挂的 OSC 11 应答。
 *
 * 只在“最近确实发过查询”时尝试，且只从 ESC 起始扫描；非应答字节通过
 * ungetch() 原序退回，不丢用户按键。
 */
void appearance_drop_dangling_reply(void);

/** @brief 退出路径（endwin 之前）：关闭焦点上报。 */
void appearance_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_APPEARANCE_H */
