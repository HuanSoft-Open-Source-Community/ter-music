/**
 * @file rpc_control.c
 * @brief org.yxzl.ter_music.Control —— CLI/前端控制通道
 *
 * 本文件只负责 org.yxzl.ter_music.Control 这一个接口：处理器、变更信号与自省片段。
 * 共享的发送/错误/回复助手与引擎动作见 media/rpc_common.c；
 * 连接生命周期与 MPRIS 见 media/session.c。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "audio/audio.h"
#include "queue/backend_queue.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "core/core.h"
#include "info/info.h"
#include "logger/logger.h"
#include "ui/braille/braille_art.h"
#include "lyrics/lyrics.h"
#include "util/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define CONTROL_API_INTERFACE RPC_IFACE_CONTROL


/* ── 前端可见的状态/错误信号（Control 接口） ───────────────────────
 * 状态消息的源头在 core（core_status_push），作业/远程失败在 rpc_job.c 与
 * rpc_remote.c；它们都通过这两个助手广播，避免各自拼信号。 */

void rpc_control_emit_status(unsigned long long seq, const char *message)
{
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  CONTROL_API_INTERFACE,
                                                  "StatusMessage");
    if (!signal) {
        return;
    }

    dbus_uint32_t sequence = (dbus_uint32_t)seq;
    const char *value = message ? message : "";
    dbus_message_append_args(signal,
                             DBUS_TYPE_UINT32, &sequence,
                             DBUS_TYPE_STRING, &value,
                             DBUS_TYPE_INVALID);
    rpc_send(signal);
}

void rpc_control_emit_error(const char *source, const char *name, const char *message)
{
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  CONTROL_API_INTERFACE,
                                                  "Error");
    if (!signal) {
        return;
    }

    const char *safe_source = source ? source : "";
    const char *safe_name = name ? name : RPC_ERROR_FAILED;
    const char *safe_message = message ? message : "";
    dbus_message_append_args(signal,
                             DBUS_TYPE_STRING, &safe_source,
                             DBUS_TYPE_STRING, &safe_name,
                             DBUS_TYPE_STRING, &safe_message,
                             DBUS_TYPE_INVALID);
    rpc_send(signal);
}

/* ── 前端注册表（Control.Attach / Ping / Detach / FrontendInfo） ────
 *
 * 前端（TUI/CLI/第三方应用）接入时登记，之后每 RPC_PING_INTERVAL_MS 心跳一次；
 * 超过 RPC_FRONTEND_TIMEOUT_MS 没有心跳视为离开。核心据此知道“还有没有前端”，
 * M4 的 core_exit_when_no_frontend 与 Info.GetInfo.frontends 都依赖它。
 *
 * token 只是登记标识，不做鉴权：会话总线本身是同用户信任域，且短命客户端
 * （每次调用新建连接）无法维持“token=连接名”的绑定。token 由核心分配，
 * 使用接入方连接的唯一名（dbus_message_get_sender）作为种子。 */

typedef struct {
    int active;
    char token[128];
    char role[16];
    int pid;
    unsigned long long last_ping_ms;
} RpcFrontend;

static RpcFrontend g_frontends[RPC_FRONTEND_MAX];
static int g_frontend_ever_attached = 0;

static unsigned long long rpc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* 通过会话总线查询连接的对端 PID；失败返回 0（不视为错误）。
 * 仅在 Attach 时调用一次，100ms 超时保证有界。 */
static int rpc_sender_pid(const char *sender)
{
    if (!sender || !sender[0]) {
        return 0;
    }
    DBusConnection *connection = rpc_session_connection();
    if (!connection) {
        return 0;
    }

    DBusMessage *message = dbus_message_new_method_call("org.freedesktop.DBus",
                                                        "/org/freedesktop/DBus",
                                                        "org.freedesktop.DBus",
                                                        "GetConnectionUnixProcessID");
    if (!message) {
        return 0;
    }
    dbus_message_append_args(message, DBUS_TYPE_STRING, &sender, DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, 100, &error);
    dbus_message_unref(message);

    int pid = 0;
    if (reply) {
        dbus_uint32_t value = 0;
        if (dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID)) {
            pid = (int)value;
        }
        dbus_message_unref(reply);
    }
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
    }
    return pid;
}

static int rpc_role_valid(const char *role)
{
    return role && (strcmp(role, "tui") == 0 || strcmp(role, "cli") == 0 ||
                    strcmp(role, "app") == 0);
}

static RpcFrontend *rpc_frontend_find(const char *token)
{
    if (!token || !token[0]) {
        return NULL;
    }
    for (int i = 0; i < RPC_FRONTEND_MAX; i++) {
        if (g_frontends[i].active && strcmp(g_frontends[i].token, token) == 0) {
            return &g_frontends[i];
        }
    }
    return NULL;
}

/* 清理超时未心跳的登记 */
void rpc_control_tick(void)
{
    unsigned long long now = rpc_now_ms();
    for (int i = 0; i < RPC_FRONTEND_MAX; i++) {
        if (!g_frontends[i].active) {
            continue;
        }
        if (now - g_frontends[i].last_ping_ms > RPC_FRONTEND_TIMEOUT_MS) {
            log_info("rpc_control", "Frontend '%s' (%s) timed out after %llu ms",
                     g_frontends[i].token, g_frontends[i].role,
                     now - g_frontends[i].last_ping_ms);
            memset(&g_frontends[i], 0, sizeof(g_frontends[i]));
        }
    }
}

int rpc_frontend_count(void)
{
    int count = 0;
    for (int i = 0; i < RPC_FRONTEND_MAX; i++) {
        if (g_frontends[i].active) {
            count++;
        }
    }
    return count;
}

/* 是否**曾经**有前端接入过：看门狗只在“确实服务过前端”之后才考虑随最后
 * 一个前端退出——否则一个刚起步、还没人来连的核心会立刻自杀。 */
int rpc_frontend_ever_attached(void)
{
    return g_frontend_ever_attached;
}

void rpc_frontend_reset_registry(void)
{
    memset(g_frontends, 0, sizeof(g_frontends));
    g_frontend_ever_attached = 0;
}

/*
 * Attach / Ping / Detach / FrontendInfo 处理器在 rpc_control_handle 内实现：
 * 它们与既有控制方法共用同一个成员分发。
 */

DBusMessage *rpc_control_handle(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    if (strcmp(member, "Attach") == 0) {
        const char *role = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &role,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!rpc_role_valid(role)) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "role must be 'tui', 'cli' or 'app'");
        }

        const char *sender = dbus_message_get_sender(message);
        RpcFrontend *entry = rpc_frontend_find(sender);
        if (!entry) {
            for (int i = 0; i < RPC_FRONTEND_MAX; i++) {
                if (!g_frontends[i].active) {
                    entry = &g_frontends[i];
                    break;
                }
            }
        }
        if (!entry) {
            return rpc_error(message, RPC_ERROR_BUSY,
                             "frontend registry is full; detach an unused frontend first");
        }

        memset(entry, 0, sizeof(*entry));
        entry->active = 1;
        snprintf(entry->token, sizeof(entry->token), "%s",
                 (sender && sender[0]) ? sender : "anonymous");
        snprintf(entry->role, sizeof(entry->role), "%s", role);
        entry->pid = rpc_sender_pid(sender);
        entry->last_ping_ms = rpc_now_ms();

        log_info("rpc_control", "Frontend attached: token='%s' role='%s' pid=%d (total=%d)",
                 entry->token, entry->role, entry->pid, rpc_frontend_count());
        g_frontend_ever_attached = 1;

        char json[512];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "token");
        pos = json_append_escaped(json, sizeof(json), pos, entry->token);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "role");
        pos = json_append_escaped(json, sizeof(json), pos, entry->role);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "api_version");
        pos = json_append_int(json, sizeof(json), pos, TER_MUSIC_API_VERSION);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "ping_interval_ms");
        pos = json_append_int(json, sizeof(json), pos, RPC_PING_INTERVAL_MS);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "frontends");
        pos = json_append_int(json, sizeof(json), pos, rpc_frontend_count());
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "Ping") == 0) {
        const char *token = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &token,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        RpcFrontend *entry = rpc_frontend_find(token);
        if (!entry) {
            return rpc_reply_bool(message, 0);   /* 未知/已过期 token：前端应重新 Attach */
        }
        entry->last_ping_ms = rpc_now_ms();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Detach") == 0) {
        const char *token = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &token,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        RpcFrontend *entry = rpc_frontend_find(token);
        if (!entry) {
            return rpc_reply_bool(message, 0);
        }
        log_info("rpc_control", "Frontend detached: token='%s' (remaining=%d)",
                 entry->token, rpc_frontend_count() - 1);
        memset(entry, 0, sizeof(*entry));
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "FrontendInfo") == 0) {
        char json[2048];
        size_t pos = 0;
        unsigned long long now = rpc_now_ms();

        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "frontends");
        pos = json_append_char(json, sizeof(json), pos, '[');

        int written = 0;
        for (int i = 0; i < RPC_FRONTEND_MAX; i++) {
            if (!g_frontends[i].active) {
                continue;
            }
            if (written > 0) {
                pos = json_append_char(json, sizeof(json), pos, ',');
            }
            pos = json_append_char(json, sizeof(json), pos, '{');
            pos = json_append_key(json, sizeof(json), pos, "token");
            pos = json_append_escaped(json, sizeof(json), pos, g_frontends[i].token);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "role");
            pos = json_append_escaped(json, sizeof(json), pos, g_frontends[i].role);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "pid");
            pos = json_append_int(json, sizeof(json), pos, g_frontends[i].pid);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "last_ping_ms");
            pos = json_append_int(json, sizeof(json), pos,
                                  (long long)(now - g_frontends[i].last_ping_ms));
            pos = json_append_char(json, sizeof(json), pos, '}');
            written++;
        }

        pos = json_append_char(json, sizeof(json), pos, ']');
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "count");
        pos = json_append_int(json, sizeof(json), pos, written);
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "Play") == 0) {
        return rpc_reply_bool(message, rpc_action_play());
    }
    if (strcmp(member, "Pause") == 0) {
        pause_audio();
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "PlayPause") == 0) {
        return rpc_reply_bool(message, rpc_action_play_pause());
    }
    if (strcmp(member, "Stop") == 0) {
        stop_audio();
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "Next") == 0) {
        next_track();
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "Previous") == 0) {
        prev_track();
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "SeekTo") == 0 || strcmp(member, "SeekBy") == 0) {
        DBusError error;
        dbus_int64_t value = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT64, &value,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int ok = (strcmp(member, "SeekTo") == 0)
            ? rpc_action_seek_to_us((int64_t)value)
            : rpc_action_seek_by_us((int64_t)value);
        return rpc_reply_bool(message, ok);
    }
    if (strcmp(member, "SetVolume") == 0) {
        DBusError error;
        dbus_int32_t percent = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &percent,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return rpc_reply_bool(message, rpc_action_set_volume_percent(percent));
    }
    if (strcmp(member, "GetVolume") == 0) {
        return rpc_reply_int(message, get_volume_percent());
    }
    if (strcmp(member, "SetSpeed") == 0) {
        DBusError error;
        double rate = 0.0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_DOUBLE, &rate,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return rpc_reply_bool(message, rpc_action_set_speed(rate));
    }
    if (strcmp(member, "GetSpeed") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(message);
        if (!reply) {
            return NULL;
        }
        double rate = (double)g_playback_speed;
        dbus_message_append_args(reply, DBUS_TYPE_DOUBLE, &rate, DBUS_TYPE_INVALID);
        return reply;
    }
    if (strcmp(member, "SetPlayMode") == 0) {
        DBusError error;
        dbus_int32_t mode = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &mode,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (mode < 0 || mode >= PLAY_MODE_COUNT) {
            return rpc_reply_bool(message, 0);
        }
        set_play_mode((PlayMode)mode);
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "GetPlayMode") == 0) {
        return rpc_reply_int(message, (int)g_play_mode);
    }
    if (strcmp(member, "GetPlayModeName") == 0) {
        return rpc_reply_string(message, play_mode_display_name(g_play_mode, 0));
    }
    if (strcmp(member, "ReloadConfig") == 0) {
        g_config_reload_requested = 1;
        return rpc_reply_bool(message, 1);
    }
    if (strcmp(member, "Quit") == 0) {
        extern volatile sig_atomic_t g_should_exit;
        g_should_exit = 1;
        return rpc_reply_bool(message, 1);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown control method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_control_introspection =
    "  <interface name=\"org.yxzl.ter_music.Control\">\n"
    "    <signal name=\"StatusMessage\">\n"
    "      <arg name=\"seq\" type=\"u\"/>\n"
    "      <arg name=\"message\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"Error\">\n"
    "      <arg name=\"source\" type=\"s\"/>\n"
    "      <arg name=\"name\" type=\"s\"/>\n"
    "      <arg name=\"message\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <method name=\"Attach\">\n"
    "      <arg name=\"role\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Ping\">\n"
    "      <arg name=\"token\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Detach\">\n"
    "      <arg name=\"token\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"FrontendInfo\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Play\"><arg type=\"b\" direction=\"out\"/></method>\n"    "    <method name=\"Pause\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"PlayPause\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Stop\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Next\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Previous\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"SeekTo\">\n"
    "      <arg name=\"position_us\" type=\"x\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"SeekBy\">\n"
    "      <arg name=\"delta_us\" type=\"x\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"SetVolume\">\n"
    "      <arg name=\"percent\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetVolume\"><arg type=\"i\" direction=\"out\"/></method>\n"
    "    <method name=\"SetSpeed\">\n"
    "      <arg name=\"rate\" type=\"d\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetSpeed\"><arg type=\"d\" direction=\"out\"/></method>\n"
    "    <method name=\"SetPlayMode\">\n"
    "      <arg name=\"mode\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetPlayMode\"><arg type=\"i\" direction=\"out\"/></method>\n"
    "    <method name=\"GetPlayModeName\"><arg type=\"s\" direction=\"out\"/></method>\n"
    "    <!-- OpenPath / PlayIndex 已在 api_version 4 撤下：加载内容属前端，\n"
    "         前端扫描后经 Queue.Set/Queue.PlayAt 下发。 -->\n"
    "    <method name=\"ReloadConfig\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Quit\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "  </interface>\n";

const char *rpc_control_introspection(void)
{
    return k_control_introspection;
}

#endif /* HAVE_DBUS */
