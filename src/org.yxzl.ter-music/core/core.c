/**
 * @file core.c
 * @brief 核心层实现：共享主循环工作、配置重载、状态消息与歌词推进钩子
 *
 * 本文件不得包含任何界面（ncurses）头文件：TUI 与 daemon 共用同一份逻辑，
 * 界面专属行为（配色、脏标记、渲染）由前端在调用 core_* 之后自行处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "core/core.h"

#include "audio/audio.h"
#include "config/config.h"
#include "lyrics/lyrics.h"
#include "media/session.h"
#include "logger/logger.h"

#include <stddef.h>
#include <string.h>

/* ── 进程级标志（声明见 core.h）────────────────────────────────── */
volatile sig_atomic_t g_should_exit = 0;
volatile sig_atomic_t g_config_reload_requested = 0;

/* ── 钩子与状态 ─────────────────────────────────────────────────── */
static void (*g_config_listener)(void) = NULL;
static void (*g_status_listener)(const char *message) = NULL;
static void (*g_state_listener)(void) = NULL;
static char g_status_last[CORE_STATUS_MAX] = "";
static unsigned long long g_status_seq = 0;

void core_set_state_listener(void (*listener)(void))
{
    g_state_listener = listener;
}

void core_notify_state_changed(void)
{
    if (g_state_listener) {
        g_state_listener();
    }
}

void core_set_config_listener(void (*listener)(void))
{
    g_config_listener = listener;
}

void core_set_status_listener(void (*listener)(const char *message))
{
    g_status_listener = listener;
}

const char *core_status_last(void)
{
    return g_status_last;
}

unsigned long long core_status_seq(void)
{
    return g_status_seq;
}

void core_status_push(const char *message)
{
    if (!message) {
        return;
    }

    strncpy(g_status_last, message, sizeof(g_status_last) - 1);
    g_status_last[sizeof(g_status_last) - 1] = '\0';
    g_status_seq++;

    if (g_status_listener) {
        g_status_listener(message);
    }
}

/* ── 配置重载 ───────────────────────────────────────────────────── */
void core_config_apply(void)
{
    /* config 层负责默认值、XML 读取与版本迁移 */
    load_config();
    config_run_migrations();

    /* 运行时应用：与 load_config() 分开，便于前端在启动时静默应用一次，
     * 而只在“重载”语义下才推送状态消息 */
    g_playback_speed = g_app_config.default_playback_speed;
    g_play_mode = (PlayMode)g_app_config.default_play_mode;
}

/* ── 每轮主循环工作 ─────────────────────────────────────────────── */
void core_tick(void)
{
    reap_finished_playback_thread();
    process_pending_playback_action();

    /* 歌词高亮由后端按播放位置推进（数据与推进都归后端） */
    if (lyrics_tick()) {
        core_notify_state_changed();
    }

    media_session_tick();

    if (g_config_reload_requested) {
        g_config_reload_requested = 0;
        log_info("core", "Reloading configuration");
        core_config_apply();
        if (g_config_listener) {
            g_config_listener();   /* 前端重刷配色/脏标记 */
        }
        core_status_push("配置已重新加载 / Config reloaded");
    }
}
