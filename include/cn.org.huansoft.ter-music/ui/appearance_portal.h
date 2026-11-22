/**
 * @file appearance_portal.h
 * @brief XDG desktop portal 的 appearance color-scheme 读取器（前端私有连接）
 *
 * 只在 TUI 前端、auto 模式下懒初始化；使用**独立 private DBusConnection**，
 * 不复用核心/远程前端的连接。`HAVE_DBUS` 未定义时全部为 stub，行为与今天一致。
 *
 * 优先级由 ui/appearance.c 的 appearance_resolve_signals() 统一裁决：
 * OSC11 > XDG > COLORFGBG > TERM > 暗色。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_APPEARANCE_PORTAL_H
#define UI_APPEARANCE_PORTAL_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 懒连接 session bus；1=可用，0=不可用（无总线/无 DBus/无地址）。 */
int appearance_portal_init(void);

/**
 * @brief 同步有界读取 `org.freedesktop.appearance color-scheme`。
 * @param timeout_ms 本次调用的硬上限（由调用方给出，绝不无限等待）
 * @param bg_out     APPEARANCE_BG_*（portal 0=无偏好 → UNKNOWN）
 * @return 1=读到值（含 UNKNOWN）、0=不可用/超时、-1=协议错误
 */
int appearance_portal_read(int timeout_ms, int *bg_out);

/**
 * @brief 发起一次**非阻塞**异步读取（复用 send_with_reply + notify）。
 *
 * 由 appearance_tick 在终端主题适配器失效、需要回退 portal 时调用；结果在
 * 后续 appearance_portal_poll() 中推进，用 appearance_portal_take_async_result()
 * 取走。绝不阻塞输入路径。
 *
 * @return 1=已发起/已有在途请求，0=连接不可用（调用方按 unknown 回退）
 */
int appearance_portal_request_async_read(void);

/**
 * @brief 取走异步读取结果（成功值或 UNKNOWN 失败）。
 * @return 1=已完成（*bg_out 有效，可为 UNKNOWN）；0=尚未完成
 */
int appearance_portal_take_async_result(int *bg_out);

/**
 * @brief 非阻塞消息泵 + 延迟异步重试；由 appearance_tick() 每约 100ms 调用。
 * 只 dispatch 已到达的消息，绝不阻塞等待。
 */
void appearance_portal_poll(void);

/**
 * @brief 取走自上次调用以来通过 SettingChanged 更新的值。
 * @return 1=有新值（*bg_out）、0=没有
 */
int appearance_portal_take_changed(int *bg_out);

/** @brief 移除 match/filter、关闭 private 连接并清理状态。 */
void appearance_portal_shutdown(void);

/** @brief 连接当前是否可用。 */
int appearance_portal_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_APPEARANCE_PORTAL_H */
