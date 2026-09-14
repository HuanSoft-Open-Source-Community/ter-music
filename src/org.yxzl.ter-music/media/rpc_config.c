/**
 * @file rpc_config.c
 * @brief org.yxzl.ter_music.Config —— 配置的唯一写入口
 *
 * 前端不再自己读写 config.xml：GetAll 返回完整配置镜像，Set 接受局部
 * JSON 补丁（键名同 config.xml），由核心原子应用、落盘并广播 ConfigChanged。
 * 运行时应用（倍速/播放模式）走 core_config_apply()，与 SIGHUP 重载同一路径。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "config/config.h"
#include "config/config_json.h"
#include "core/core.h"
#include "logger/logger.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define CONFIG_API_INTERFACE RPC_IFACE_CONFIG

void rpc_config_emit_changed(const char *patch_json)
{
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  CONFIG_API_INTERFACE,
                                                  "ConfigChanged");
    if (!signal) {
        return;
    }
    const char *value = (patch_json && patch_json[0]) ? patch_json : "{}";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    rpc_send(signal);
}

DBusMessage *rpc_config_handle(DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(member, "GetAll") == 0) {
        size_t capacity = RPC_PAYLOAD_MAX / 2;
        char *json = malloc(capacity);
        if (!json) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        int written = config_render_json(&g_app_config, json, capacity);
        if (written < 0) {
            free(json);
            return rpc_error(message, RPC_ERROR_FAILED, "failed to render the configuration");
        }

        DBusMessage *reply;
        if ((size_t)written + 1 > RPC_PAYLOAD_MAX) {
            reply = rpc_error(message, RPC_ERROR_TOO_LARGE, "configuration exceeds the payload limit");
        } else {
            reply = rpc_reply_string(message, json);
        }
        free(json);
        return reply;
    }

    if (strcmp(member, "Set") == 0) {
        const char *patch = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &patch,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        char reason[256];
        if (config_apply_json(patch, reason, sizeof(reason)) != 0) {
            /* 未知键/类型错误：整体不生效（config_apply_json 保证原子性） */
            return rpc_error(message, RPC_ERROR_UNSUPPORTED,
                             reason[0] ? reason : "invalid configuration patch");
        }

        save_config();
        core_config_apply();
        rpc_config_emit_changed(patch);
        rpc_control_emit_status(core_status_seq(), "配置已更新 / Config updated");
        log_info("rpc_config", "Configuration updated via D-Bus (patch=%zu bytes)",
                 patch ? strlen(patch) : 0);
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Reload") == 0) {
        g_config_reload_requested = 1;
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Reset") == 0) {
        init_default_config();
        save_config();
        core_config_apply();
        rpc_config_emit_changed(NULL);
        rpc_control_emit_status(core_status_seq(), "配置已重置 / Config reset");
        return rpc_reply_bool(message, 1);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown config method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_config_introspection =
    "  <interface name=\"org.yxzl.ter_music.Config\">\n"
    "    <method name=\"GetAll\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"patch\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Reload\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Reset\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <signal name=\"ConfigChanged\">\n"
    "      <arg name=\"patch\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_config_introspection(void)
{
    return k_config_introspection;
}

#endif /* HAVE_DBUS */
