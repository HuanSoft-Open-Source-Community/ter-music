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

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* ── 进程级标志（声明见 core.h）────────────────────────────────── */
volatile sig_atomic_t g_should_exit = 0;
volatile sig_atomic_t g_config_reload_requested = 0;

/* ── 钩子与状态 ─────────────────────────────────────────────────── */
static void (*g_config_listener)(void) = NULL;
static void (*g_status_listener)(const char *message) = NULL;
static void (*g_state_listener)(void) = NULL;

/* 状态消息：播放线程（解码/写设备失败）与媒体循环（RPC 处理器）都会 push，
 * rpc_info_sync() 则在媒体循环读，因此缓冲区与序号都在这把锁下读写。 */
static char g_status_last[CORE_STATUS_MAX] = "";
static unsigned long long g_status_seq = 0;
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 配置落盘去抖（声明见 core.h）：脏标志可能被媒体循环之外的线程置位，
 * 用 volatile 保证跨线程可见；真正写盘只发生在 core_tick()（媒体循环）里。 */
static volatile int g_config_dirty = 0;
static unsigned long long g_config_dirty_ms = 0;

static unsigned long long core_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

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
    /* 返回调用线程私有的副本：调用方拿到指针后不会再被别的线程的 push 改写，
     * 也不必在锁外读共享缓冲区。 */
    static __thread char copy[CORE_STATUS_MAX];

    pthread_mutex_lock(&g_status_mutex);
    memcpy(copy, g_status_last, sizeof(copy));
    pthread_mutex_unlock(&g_status_mutex);
    copy[sizeof(copy) - 1] = '\0';
    return copy;
}

unsigned long long core_status_seq(void)
{
    pthread_mutex_lock(&g_status_mutex);
    unsigned long long seq = g_status_seq;
    pthread_mutex_unlock(&g_status_mutex);
    return seq;
}

void core_status_push(const char *message)
{
    if (!message) {
        return;
    }

    pthread_mutex_lock(&g_status_mutex);
    strncpy(g_status_last, message, sizeof(g_status_last) - 1);
    g_status_last[sizeof(g_status_last) - 1] = '\0';
    g_status_seq++;
    pthread_mutex_unlock(&g_status_mutex);

    if (g_status_listener) {
        g_status_listener(message);
    }
}

/* ── 配置落盘去抖 ───────────────────────────────────────────────── */
void core_config_mark_dirty(void)
{
    unsigned long long now = core_now_ms();
    /* 首次置脏记下时刻；已经脏了就不刷新，避免“持续变化永远不落盘” */
    if (!g_config_dirty) {
        g_config_dirty_ms = now;
    }
    g_config_dirty = 1;
}

void core_config_flush_dirty(void)
{
    if (!g_config_dirty) {
        return;
    }
    g_config_dirty = 0;
    save_config();
}

static void core_config_flush_due(void)
{
    if (!g_config_dirty) {
        return;
    }
    if (core_now_ms() - g_config_dirty_ms < CORE_CONFIG_SAVE_DEBOUNCE_MS) {
        return;
    }
    core_config_flush_dirty();
}

/* ── 配置重载 ───────────────────────────────────────────────────── */
void core_config_apply_runtime(void)
{
    /* 运行时应用：只认内存里的配置镜像，不碰磁盘（见 core.h 的说明）。 */
    g_playback_speed = g_app_config.default_playback_speed;
    g_play_mode = (PlayMode)g_app_config.default_play_mode;
    /* 音量同属运行时字段：docs/API_DBUS_en_US.md 承诺 Config.Set 对音量
     * "visible immediately"，缺了这一步就只落盘不生效（Control.GetVolume
     * 报旧值）。走引擎唯一入口 set_volume_percent()：仅当引擎值与镜像不同
     * 才变化，变化时回写 g_app_config.volume_percent 的正是本行的源值
     * （同值幂等，不回卷镜像）；MPRIS/Control 设音量时也已同步该镜像，
     * 因此重复调用安全，去抖落盘只可能重写同一份值，不构成递归。 */
    set_volume_percent(g_app_config.volume_percent);
}

void core_config_apply(void)
{
    /* config 层负责默认值、XML 读取与版本迁移 */
    load_config();
    config_run_migrations();

    /* 运行时应用：与 load_config() 分开，便于前端在启动时静默应用一次，
     * 而只在“重载”语义下才推送状态消息 */
    core_config_apply_runtime();
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

    /* 音量/倍速这类去抖落盘的字段：攒到最小间隔之后一次写完 */
    core_config_flush_due();

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
