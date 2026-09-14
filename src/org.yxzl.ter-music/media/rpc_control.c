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
#include "audio/play_queue.h"
#include "config/config.h"
#include "core/core.h"
#include "info/info.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "remote/remote.h"
#include "ui/braille/braille_art.h"
#include "ui/lyrics.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "util/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define CONTROL_API_INTERFACE RPC_IFACE_Control


DBusMessage *rpc_control_handle(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
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
    if (strcmp(member, "OpenPath") == 0) {
        DBusError error;
        const char *path = NULL;
        dbus_bool_t autoplay = FALSE;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &path,
                                   DBUS_TYPE_BOOLEAN, &autoplay,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return rpc_reply_bool(message, rpc_action_open_path(path, autoplay ? 1 : 0));
    }
    if (strcmp(member, "PlayIndex") == 0) {
        DBusError error;
        dbus_int32_t index = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return rpc_reply_bool(message, rpc_action_play_index(index));
    }
    if (strcmp(member, "GetPlaylist") == 0) {
        int total = playlist_count();
        char folder[MAX_PATH_LEN];
        char json[2048];

        playlist_copy_folder_path(folder, sizeof(folder));

        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "loaded");
        pos = json_append_bool(json, sizeof(json), pos, playlist_is_loaded());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "count");
        pos = json_append_int(json, sizeof(json), pos, total);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "current_index");
        if (g_current_play_index >= 0) {
            pos = json_append_int(json, sizeof(json), pos, g_current_play_index);
        } else {
            pos = json_append_raw(json, sizeof(json), pos, "null");
        }
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "folder");
        pos = json_append_string_or_null(json, sizeof(json), pos,
                                         folder[0] ? folder : NULL);
        pos = json_append_char(json, sizeof(json), pos, '}');
        (void)pos;
        return rpc_reply_string(message, json);
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
    "    <method name=\"Play\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Pause\"><arg type=\"b\" direction=\"out\"/></method>\n"
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
    "    <method name=\"OpenPath\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"autoplay\" type=\"b\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"PlayIndex\">\n"
    "      <arg name=\"index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetPlaylist\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"ReloadConfig\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Quit\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "  </interface>\n";

const char *rpc_control_introspection(void)
{
    return k_control_introspection;
}

#endif /* HAVE_DBUS */
