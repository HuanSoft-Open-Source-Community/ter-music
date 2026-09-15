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
    /* 状态消息（Core.StatusMessage）与可视化帧（Info.VisualizerFrame） */
    unsigned long long status_seq;
    unsigned long long visualizer_revision;
    unsigned long long last_visualizer_ms;
    int visualizer_levels[VISUALIZER_BAND_COUNT];
    int visualizer_peaks[VISUALIZER_BAND_COUNT];
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

/* 状态消息：core 侧每次 push 使 seq 递增，这里转发为 Control.StatusMessage。
 * 与 InfoChanged 一样采用“比较后发送”，因此不需要额外监听器。 */
static void rpc_info_sync_status(void)
{
    unsigned long long seq = core_status_seq();
    if (seq == g_info.status_seq) {
        return;
    }
    g_info.status_seq = seq;
    rpc_control_emit_status(seq, core_status_last());
}

/* 可视化帧（Info.VisualizerFrame）：仅在采样修订号变化（音频在推进）时，
 * 按 RPC_VISUALIZER_INTERVAL_MS 节流发送；空闲/暂停时不产生总线流量。 */
static void rpc_info_sync_visualizer(void)
{
    int levels[VISUALIZER_BAND_COUNT];
    int peaks[VISUALIZER_BAND_COUNT];
    uint64_t last_update_ms = 0;

    get_visualizer_snapshot(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    if (last_update_ms == 0 || last_update_ms == g_info.last_visualizer_ms) {
        return;
    }

    uint64_t now_ms = get_ui_time_ms();
    if (g_info.visualizer_revision > 0 &&
        (now_ms - g_info.last_visualizer_ms) < RPC_VISUALIZER_INTERVAL_MS) {
        return;
    }

    g_info.last_visualizer_ms = last_update_ms;
    g_info.visualizer_revision++;
    memcpy(g_info.visualizer_levels, levels, sizeof(levels));
    memcpy(g_info.visualizer_peaks, peaks, sizeof(peaks));

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  INFO_API_INTERFACE,
                                                  "VisualizerFrame");
    if (!signal) {
        return;
    }

    DBusMessageIter iter;
    DBusMessageIter array_iter;
    dbus_uint32_t revision = (dbus_uint32_t)g_info.visualizer_revision;

    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &revision);
    for (int channel = 0; channel < 2; channel++) {
        const int *source = channel == 0 ? g_info.visualizer_levels
                                         : g_info.visualizer_peaks;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &array_iter);
        for (int i = 0; i < VISUALIZER_BAND_COUNT; i++) {
            int value = source[i];
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            unsigned char byte = (unsigned char)value;
            dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &byte);
        }
        dbus_message_iter_close_container(&iter, &array_iter);
    }

    rpc_send(signal);
}

void rpc_info_sync(void) {
    if (!rpc_session_active()) {
        return;
    }

    /* 状态消息与可视化帧：与 InfoChanged 同一轮检测（比较后发送） */
    rpc_info_sync_status();
    rpc_info_sync_visualizer();

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
        info_render_json(json, INFO_JSON_MAX, &instance, g_info.revision, rpc_core_json());
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
        info_render_json(json, INFO_JSON_MAX, &instance, g_info.revision, rpc_core_json());
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
            } else if (strcmp(charset, "half") == 0) {
                charset_value = INFO_COVER_HALF;
            } else {
                return rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                           "charset must be 'braille', 'ascii' or 'half'");
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

    if (strcmp(member, "GetVisualizer") == 0) {
        int levels[VISUALIZER_BAND_COUNT];
        int peaks[VISUALIZER_BAND_COUNT];
        uint64_t last_update_ms = 0;
        get_visualizer_snapshot(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);

        char json[4096];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "revision");
        pos = json_append_int(json, sizeof(json), pos, (long long)g_info.visualizer_revision);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "bands");
        pos = json_append_int(json, sizeof(json), pos, VISUALIZER_BAND_COUNT);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "levels");
        pos = json_append_char(json, sizeof(json), pos, '[');
        for (int i = 0; i < VISUALIZER_BAND_COUNT; i++) {
            if (i > 0) pos = json_append_char(json, sizeof(json), pos, ',');
            pos = json_append_int(json, sizeof(json), pos, levels[i]);
        }
        pos = json_append_char(json, sizeof(json), pos, ']');
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "peaks");
        pos = json_append_char(json, sizeof(json), pos, '[');
        for (int i = 0; i < VISUALIZER_BAND_COUNT; i++) {
            if (i > 0) pos = json_append_char(json, sizeof(json), pos, ',');
            pos = json_append_int(json, sizeof(json), pos, peaks[i]);
        }
        pos = json_append_char(json, sizeof(json), pos, ']');
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "GetStatus") == 0) {
        char json[CORE_STATUS_MAX + 128];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "seq");
        pos = json_append_int(json, sizeof(json), pos, (long long)core_status_seq());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "message");
        pos = json_append_string_or_null(json, sizeof(json), pos, core_status_last());
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        return rpc_reply_string(message, json);
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
    "    <method name=\"GetVisualizer\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetStatus\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
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
    "    <signal name=\"VisualizerFrame\">\n"
    "      <arg name=\"revision\" type=\"u\"/>\n"
    "      <arg name=\"levels\" type=\"ay\"/>\n"
    "      <arg name=\"peaks\" type=\"ay\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_info_introspection(void)
{
    return k_info_introspection;
}

#endif /* HAVE_DBUS */
