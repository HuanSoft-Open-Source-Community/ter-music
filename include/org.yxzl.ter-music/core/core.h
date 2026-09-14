/**
 * @file core.h
 * @brief 核心（backend）层：与界面无关的共享进程逻辑
 *
 * TUI（main.c）与无界面 daemon 是同一进程模型的两种前端外壳，二者必须执行
 * 同一套每轮工作（core_tick）与同一套配置重载/状态语义。这些逻辑集中在本模块，
 * 避免界面模块反向承担核心职责（此前的 reload_config() 位于 ui/menus.c，
 * 却由 cli/daemon.c 调用，即为典型反例）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CORE_H
#define CORE_H

#include <signal.h>

/* ── 进程级标志 ─────────────────────────────────────────────────── */
/* 由信号处理器设置，两个主循环都检查（定义在 core/core.c） */
extern volatile sig_atomic_t g_should_exit;             /* SIGTERM / SIGINT / SIGHUP */
extern volatile sig_atomic_t g_config_reload_requested; /* SIGHUP / D-Bus ReloadConfig */

/* 状态消息缓冲长度 */
#define CORE_STATUS_MAX 256

/* ── 每轮主循环工作 ─────────────────────────────────────────────── */
/* 回收已结束的播放线程 → 执行挂起的播放动作 → 推进歌词 → D-Bus 会话 tick，
 * 并在 g_config_reload_requested 置位时应用配置重载并广播状态消息。
 * TUI 与 daemon 的事件循环都应调用它。 */
void core_tick(void);

/* ── 配置重载（非界面部分）──────────────────────────────────────── */
/* 重读配置文件并把核心可见字段应用到运行时（播放模式、倍速）。
 * 默认值/XML 读取/版本迁移由 config 层负责；主题配色与界面刷新由前端负责。 */
void core_config_apply(void);

/* ── 配置重载监听 ───────────────────────────────────────────────── */
/* 核心完成配置应用后回调前端（界面用它重刷配色与脏标记）。
 * 只在“重载”路径触发，启动时的首次应用不触发（与既有行为一致）。 */
void core_set_config_listener(void (*listener)(void));

/* ── 状态消息 ───────────────────────────────────────────────────── */
/* 核心侧记录最近一条状态消息，并转发给已注册的界面监听器；
 * 无界面（daemon）时只记录，便于后续经 D-Bus 提供给前端。 */
void core_status_push(const char *message);
void core_set_status_listener(void (*listener)(const char *message));
const char *core_status_last(void);

/* ── 歌词推进钩子 ───────────────────────────────────────────────── */
/* 歌词状态目前仍由 ui/lyrics.c 持有（其归属将在前端改造阶段迁移到核心）。
 * 核心通过注册回调调用推进函数，使 core 模块不反向依赖界面头文件。 */
void core_set_lyrics_tick(void (*tick)(void));

#endif /* CORE_H */
