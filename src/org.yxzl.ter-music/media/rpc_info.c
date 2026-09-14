/**
 * @file rpc_info.c
 * @brief org.yxzl.ter_music.Info —— 曲目/进度/封面/配置化文本快照接口
 *
 * 本文件只负责 org.yxzl.ter_music.Info 这一个接口：处理器、变更信号与自省片段。
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
#define INFO_API_INTERFACE RPC_IFACE_INFO
/* ProgressChanged 节流：状态变化立即发，否则最多 1 Hz */
#define INFO_PROGRESS_SIGNAL_INTERVAL_MS 1000

/* ── 本接口的变更检测状态（原属 session.c 的 MediaSessionState） ─── */
static struct {
    unsigned long long revision;
    char last_json[INFO_JSON_MAX];
    unsigned long long last_progress_ms;
    PlayState last_progress_state;
    int key_valid;
    int track_index;
    int playlist_total;
    PlayState state;
    PlayMode mode;
    int volume;
    float speed;
    int cover_valid;
    char cover_path[MAX_PATH_LEN];
    char track_path[MAX_PATH_LEN];
} g_info = {0};


/* rpc_info_emit_progress()：媒体循环在快照变化后调用（声明见 media/rpc.h） */

static void emit_info_changed(const char *json) {
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  INFO_API_INTERFACE,
                                                  "InfoChanged");
    if (!signal) {
        return;
    }
    const char *value = (json && json[0] != '\0') ? json : "{}";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    rpc_send(signal);
}

static void emit_cover_changed(void) {
    InfoRenderOptions options;
    info_options_from_config(&options);

    char *text = malloc(INFO_COVER_TEXT_MAX);
    if (!text) {
        return;
    }
    int have = info_cover_text(options.cover_cols, options.cover_rows,
                               options.cover_charset, text, INFO_COVER_TEXT_MAX);

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  INFO_API_INTERFACE,
                                                  "CoverChanged");
    if (signal) {
        const char *value = have ? text : "";
        const char *charset = info_cover_charset_id(options.cover_charset);
        dbus_int32_t cols = options.cover_cols;
        dbus_int32_t rows = options.cover_rows;
        dbus_message_append_args(signal,
                                 DBUS_TYPE_STRING, &value,
                                 DBUS_TYPE_STRING, &charset,
                                 DBUS_TYPE_INT32, &cols,
                                 DBUS_TYPE_INT32, &rows,
                                 DBUS_TYPE_INVALID);
        rpc_send(signal);
    }
    free(text);
}

void rpc_info_emit_progress(const RpcPlaybackSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    uint64_t now_ms = get_ui_time_ms();
    int state_changed = (snapshot->play_state != g_info.last_progress_state);
    if (!state_changed &&
        (now_ms - g_info.last_progress_ms) < INFO_PROGRESS_SIGNAL_INTERVAL_MS) {
        return;
    }
    if (!state_changed && snapshot->play_state == PLAY_STATE_STOPPED) {
        return;
    }

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  INFO_API_INTERFACE,
                                                  "ProgressChanged");
    if (signal) {
        dbus_int64_t position_us = (dbus_int64_t)snapshot->position_us;
        dbus_int64_t length_us = (dbus_int64_t)snapshot->length_us;
        const char *status = rpc_playback_status_name(snapshot->play_state);
        dbus_message_append_args(signal,
                                 DBUS_TYPE_INT64, &position_us,
                                 DBUS_TYPE_INT64, &length_us,
                                 DBUS_TYPE_STRING, &status,
                                 DBUS_TYPE_INVALID);
        rpc_send(signal);
    }

    g_info.last_progress_ms = now_ms;
    g_info.last_progress_state = snapshot->play_state;
}

/* 检测“非进度类”信息变化：轨道、状态、模式、音量、倍速、封面 */
/* 会话关闭时清除本接口状态（等价于原先 memset(&g_media_session, 0, ...)） */
void rpc_info_reset(void)
{
    memset(&g_info, 0, sizeof(g_info));
}

void rpc_info_sync(void) {
    if (!rpc_session_active()) {
        return;
    }

    char track_path[MAX_PATH_LEN] = "";
    if (rpc_track_available()) {
        if (playlist_get_track_path(g_current_play_index, track_path,
                                    sizeof(track_path)) != 0) {
            track_path[0] = '\0';
        }
    }

    int changed = 0;
    if (!g_info.key_valid) {
        changed = 1;
    } else if (g_info.track_index != g_current_play_index ||
               strcmp(g_info.track_path, track_path) != 0 ||
               g_info.playlist_total != playlist_count() ||
               g_info.state != g_play_state ||
               g_info.mode != g_play_mode ||
               g_info.volume != get_volume_percent() ||
               fabs((double)g_info.speed - (double)g_playback_speed) > 0.001 ||
               g_info.cover_valid != g_current_album_cover_valid ||
               strcmp(g_info.cover_path, g_current_album_cover_path) != 0) {
        changed = 1;
    }

    if (!changed) {
        return;
    }

    int cover_changed = (!g_info.key_valid ||
                         g_info.cover_valid != g_current_album_cover_valid ||
                         strcmp(g_info.cover_path,
                                g_current_album_cover_path) != 0);

    g_info.key_valid = 1;
    g_info.track_index = g_current_play_index;
    snprintf(g_info.track_path,
             sizeof(g_info.track_path), "%s", track_path);
    g_info.playlist_total = playlist_count();
    g_info.state = g_play_state;
    g_info.mode = g_play_mode;
    g_info.volume = get_volume_percent();
    g_info.speed = g_playback_speed;
    g_info.cover_valid = g_current_album_cover_valid;
    snprintf(g_info.cover_path,
             sizeof(g_info.cover_path), "%s",
             g_current_album_cover_path);

    g_info.revision++;

    char *json = malloc(INFO_JSON_MAX);
    if (json) {
        InfoInstance instance = rpc_instance_info();
        info_render_json(json, INFO_JSON_MAX, &instance, g_info.revision);
        snprintf(g_info.last_json, sizeof(g_info.last_json), "%s", json);
        emit_info_changed(json);
        free(json);
    }

    if (cover_changed) {
        emit_cover_changed();
    }
}


DBusMessage *rpc_info_handle(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    InfoInstance instance = rpc_instance_info();

    if (strcmp(member, "GetInfo") == 0) {
        char *json = malloc(INFO_JSON_MAX);
        if (!json) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_render_json(json, INFO_JSON_MAX, &instance, g_info.revision);
        DBusMessage *reply = rpc_reply_string(message, json);
        free(json);
        return reply;
    }

    if (strcmp(member, "GetTrackInfo") == 0) {
        char *json = malloc(INFO_JSON_MAX);
        if (!json) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_render_track_json(json, INFO_JSON_MAX);
        DBusMessage *reply = rpc_reply_string(message, json);
        free(json);
        return reply;
    }

    if (strcmp(member, "GetProgress") == 0) {
        char json[4096];
        info_render_progress_json(json, sizeof(json));
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "GetLyricsLines") == 0) {
        char json[4096];
        info_render_lyrics_json(json, sizeof(json));
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "InstanceInfo") == 0) {
        char json[512];
        info_render_instance_json(json, sizeof(json), &instance);
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "GetCoverArt") == 0) {
        DBusError error;
        const char *charset = NULL;
        dbus_int32_t cols = 0;
        dbus_int32_t rows = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &charset,
                                   DBUS_TYPE_INT32, &cols,
                                   DBUS_TYPE_INT32, &rows,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int charset_value = -1;
        if (charset && charset[0] != '\0') {
            if (strcmp(charset, "braille") == 0) {
                charset_value = INFO_COVER_BRAILLE;
            } else if (strcmp(charset, "ascii") == 0) {
                charset_value = INFO_COVER_ASCII;
            } else {
                return rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                           "charset must be 'braille' or 'ascii'");
            }
        }

        char *text = malloc(INFO_COVER_TEXT_MAX);
        if (!text) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_cover_text(cols, rows, charset_value, text, INFO_COVER_TEXT_MAX);
        DBusMessage *reply = rpc_reply_string(message, text);
        free(text);
        return reply;
    }

    if (strcmp(member, "GetDisplay") == 0) {
        DBusError error;
        const char *options_text = NULL;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &options_text,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        InfoRenderOptions options;
        info_options_from_config(&options);
        if (options_text && options_text[0] != '\0' &&
            info_options_parse(&options, options_text) != 0) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                       "Invalid display options");
        }

        char *text = malloc(INFO_TEXT_MAX);
        if (!text) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        if (info_render_text(&options, text, INFO_TEXT_MAX) < 0) {
            text[0] = '\0';
        }
        DBusMessage *reply = rpc_reply_string(message, text);
        free(text);
        return reply;
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown info method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_info_introspection =
    "  <interface name=\"org.yxzl.ter_music.Info\">\n"
    "    <method name=\"GetInfo\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetTrackInfo\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetProgress\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetLyricsLines\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"InstanceInfo\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetCoverArt\">\n"
    "      <arg name=\"charset\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"cols\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"rows\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"text\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetDisplay\">\n"
    "      <arg name=\"options\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"text\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"InfoChanged\">\n"
    "      <arg name=\"json\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ProgressChanged\">\n"
    "      <arg name=\"position_us\" type=\"x\"/>\n"
    "      <arg name=\"duration_us\" type=\"x\"/>\n"
    "      <arg name=\"playback_status\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"CoverChanged\">\n"
    "      <arg name=\"text\" type=\"s\"/>\n"
    "      <arg name=\"charset\" type=\"s\"/>\n"
    "      <arg name=\"cols\" type=\"i\"/>\n"
    "      <arg name=\"rows\" type=\"i\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_info_introspection(void)
{
    return k_info_introspection;
}

#endif /* HAVE_DBUS */
