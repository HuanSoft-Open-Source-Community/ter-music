/**
 * @file rpc_lyrics.c
 * @brief org.yxzl.ter_music.Lyrics —— A/B 两句歌词同步接口
 *
 * 本文件只负责 org.yxzl.ter_music.Lyrics 这一个接口：处理器、变更信号与自省片段。
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
#define LYRICS_API_INTERFACE RPC_IFACE_LYRICS

#define LYRICS_API_JSON_MAX 65536

/* ── 本接口状态（原属 session.c） ─────────────────────────────────── */
typedef struct {
    int active_slot;         /* -1=none, 0=A, 1=B */
    int line_index[2];       /* lyric index in g_lyrics.lines, or -1 */
    char track_id[96];
    uint64_t revision;
    char last_json[LYRICS_API_JSON_MAX];
} LyricsApiState;

static LyricsApiState g_lyrics_api = {0};


void rpc_lyrics_reset(void) {
    g_lyrics_api.active_slot = -1;
    g_lyrics_api.line_index[0] = -1;
    g_lyrics_api.line_index[1] = -1;
    g_lyrics_api.track_id[0] = '\0';
    g_lyrics_api.revision = 0;
    g_lyrics_api.last_json[0] = '\0';
}

static void lyrics_api_prepare(char *out, size_t out_size, uint64_t revision) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    char track_id[96] = "";
    if (rpc_track_available()) {
        char track_path[MAX_PATH_LEN];
        if (playlist_get_track_path(g_current_play_index, track_path,
                                    sizeof(track_path)) == 0) {
            info_build_track_id(track_id, sizeof(track_id), track_path);
        }
    }

    int track_changed = (strcmp(g_lyrics_api.track_id, track_id) != 0);
    int has_lyrics = 0;
    int has_timestamps = 0;
    int line_a = -1;
    int line_b = -1;
    double timestamp_a = 0.0;
    double timestamp_b = 0.0;
    char text_a[MAX_LYRIC_TEXT_LEN] = "";
    char text_b[MAX_LYRIC_TEXT_LEN] = "";

    pthread_mutex_lock(&g_lyrics.lock);
    has_lyrics = g_lyrics.has_lyrics;
    has_timestamps = g_lyrics.has_timestamps;
    int current_index = g_lyrics.current_index;
    int line_count = g_lyrics.count;

    if (!has_lyrics || line_count <= 0 || current_index < 0) {
        g_lyrics_api.active_slot = -1;
        g_lyrics_api.line_index[0] = -1;
        g_lyrics_api.line_index[1] = -1;
    } else {
        if (current_index >= line_count) {
            current_index = line_count - 1;
        }
        int next_index = (current_index + 1 < line_count)
            ? current_index + 1 : -1;
        int active_slot = g_lyrics_api.active_slot;
        int current_matches_active = 0;
        int current_matches_inactive = 0;

        if (active_slot == 0 || active_slot == 1) {
            current_matches_active =
                (g_lyrics_api.line_index[active_slot] == current_index);
            current_matches_inactive =
                (g_lyrics_api.line_index[1 - active_slot] == current_index);
        }

        if (track_changed || (!current_matches_active &&
                              !current_matches_inactive)) {
            active_slot = 0;
            g_lyrics_api.line_index[0] = current_index;
            g_lyrics_api.line_index[1] = next_index;
        } else if (current_matches_active) {
            g_lyrics_api.line_index[1 - active_slot] = next_index;
        } else {
            active_slot = 1 - active_slot;
            g_lyrics_api.line_index[active_slot] = current_index;
            g_lyrics_api.line_index[1 - active_slot] = next_index;
        }
        g_lyrics_api.active_slot = active_slot;
    }

    line_a = g_lyrics_api.line_index[0];
    line_b = g_lyrics_api.line_index[1];
    if (line_a >= 0 && line_a < line_count) {
        timestamp_a = g_lyrics.lines[line_a].timestamp;
        snprintf(text_a, sizeof(text_a), "%s", g_lyrics.lines[line_a].text);
    }
    if (line_b >= 0 && line_b < line_count) {
        timestamp_b = g_lyrics.lines[line_b].timestamp;
        snprintf(text_b, sizeof(text_b), "%s", g_lyrics.lines[line_b].text);
    }
    pthread_mutex_unlock(&g_lyrics.lock);

    snprintf(g_lyrics_api.track_id, sizeof(g_lyrics_api.track_id), "%s",
             track_id);

    size_t pos = 0;
    pos = json_append_raw(out, out_size, pos, "{\"active_line\":");
    if (g_lyrics_api.active_slot == 0) {
        pos = json_append_raw(out, out_size, pos, "\"A\"");
    } else if (g_lyrics_api.active_slot == 1) {
        pos = json_append_raw(out, out_size, pos, "\"B\"");
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",\"line_a\":");
    pos = json_append_line_object(out, out_size, pos, line_a,
                                  has_timestamps, timestamp_a, text_a);
    pos = json_append_raw(out, out_size, pos, ",\"line_b\":");
    pos = json_append_line_object(out, out_size, pos, line_b,
                                  has_timestamps, timestamp_b, text_b);
    pos = json_append_raw(out, out_size, pos, ",\"track_id\":");
    pos = json_append_string_or_null(out, out_size, pos,
                                     track_id[0] ? track_id : NULL);
    pos = json_append_raw(out, out_size, pos, ",\"has_lyrics\":");
    pos = json_append_raw(out, out_size, pos, has_lyrics ? "true" : "false");
    pos = json_append_raw(out, out_size, pos, ",\"has_timestamps\":");
    pos = json_append_raw(out, out_size, pos,
                          has_timestamps ? "true" : "false");
    pos = json_append_raw(out, out_size, pos, ",\"revision\":");

    char revision_text[32];
    snprintf(revision_text, sizeof(revision_text), "%llu",
             (unsigned long long)revision);
    pos = json_append_number(out, out_size, pos, revision_text);
    pos = json_append_raw(out, out_size, pos, "}");
    out[pos] = '\0';
}

static void emit_lyrics_changed(const char *json) {
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  LYRICS_API_INTERFACE,
                                                  "LyricsChanged");
    if (!signal) {
        return;
    }

    const char *value = (json && json[0] != '\0') ? json : "{}";
    dbus_message_append_args(signal,
                             DBUS_TYPE_STRING, &value,
                             DBUS_TYPE_INVALID);
    rpc_send(signal);
}

const char *rpc_lyrics_sync(void) {
    char next_json[LYRICS_API_JSON_MAX];
    lyrics_api_prepare(next_json, sizeof(next_json), g_lyrics_api.revision);

    if (strcmp(next_json, g_lyrics_api.last_json) != 0) {
        g_lyrics_api.revision++;
        lyrics_api_prepare(next_json, sizeof(next_json), g_lyrics_api.revision);
        snprintf(g_lyrics_api.last_json, sizeof(g_lyrics_api.last_json),
                 "%s", next_json);
        emit_lyrics_changed(next_json);
    }
    return g_lyrics_api.last_json;
}

DBusMessage *rpc_lyrics_handle(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    if (strcmp(member, "GetLyrics") == 0) {
        const char *json = rpc_lyrics_sync();
        DBusMessage *reply = dbus_message_new_method_return(message);
        if (!reply) {
            return NULL;
        }

        const char *value = (json && json[0] != '\0') ? json : "{}";
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING, &value,
                                 DBUS_TYPE_INVALID);
        return reply;
    }

    if (strcmp(member, "GetDocument") == 0) {
        dbus_int32_t offset = 0;
        dbus_int32_t count = RPC_PAGE_DEFAULT;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &offset,
                                   DBUS_TYPE_INT32, &count,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int clamped_offset = 0;
        int clamped_count = 0;
        if (rpc_page_clamp(offset, count, &clamped_offset, &clamped_count) != 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }

        char track_id[96] = "";
        if (rpc_track_available()) {
            char track_path[MAX_PATH_LEN];
            if (playlist_get_track_path(g_current_play_index, track_path,
                                        sizeof(track_path)) == 0) {
                info_build_track_id(track_id, sizeof(track_id), track_path);
            }
        }

        /* 单页最多 1000 行 × 每行 ~512 字节，堆分配（避免大栈帧） */
        size_t capacity = RPC_PAYLOAD_MAX / 2;
        char *json = malloc(capacity);
        if (!json) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }

        int total = 0;
        int has_lyrics = 0;
        int has_timestamps = 0;
        int current_index = -1;
        int source = LYRICS_SOURCE_AUTO;
        int written = 0;

        pthread_mutex_lock(&g_lyrics.lock);
        total = g_lyrics.count;
        has_lyrics = g_lyrics.has_lyrics;
        has_timestamps = g_lyrics.has_timestamps;
        current_index = g_lyrics.current_index;
        source = g_lyrics.source;

        size_t pos = 0;
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "revision");
        pos = json_append_int(json, capacity, pos, (long long)g_lyrics_api.revision);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "track_id");
        pos = json_append_string_or_null(json, capacity, pos, track_id[0] ? track_id : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "has_lyrics");
        pos = json_append_bool(json, capacity, pos, has_lyrics);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "has_timestamps");
        pos = json_append_bool(json, capacity, pos, has_timestamps);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "source");
        pos = json_append_escaped(json, capacity, pos, info_lyrics_source_id(source));
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "total");
        pos = json_append_int(json, capacity, pos, total);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "offset");
        pos = json_append_int(json, capacity, pos, clamped_offset);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "current_index");
        if (has_lyrics && current_index >= 0 && current_index < total) {
            pos = json_append_int(json, capacity, pos, current_index);
        } else {
            pos = json_append_raw(json, capacity, pos, "null");
        }
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "lines");
        pos = json_append_char(json, capacity, pos, '[');

        for (int i = clamped_offset; i < total && written < clamped_count; i++) {
            if (written > 0) {
                pos = json_append_char(json, capacity, pos, ',');
            }
            pos = json_append_line_object(json, capacity, pos, i, has_timestamps,
                                          g_lyrics.lines[i].timestamp,
                                          g_lyrics.lines[i].text);
            written++;
        }

        pos = json_append_char(json, capacity, pos, ']');
        pos = json_append_char(json, capacity, pos, '}');
        json[pos] = '\0';
        pthread_mutex_unlock(&g_lyrics.lock);

        DBusMessage *reply;
        if (pos + 1 > RPC_PAYLOAD_MAX) {
            reply = rpc_error(message, RPC_ERROR_TOO_LARGE,
                              "lyrics page exceeds the payload limit; request fewer lines");
        } else {
            reply = rpc_reply_string(message, json);
        }
        free(json);
        return reply;
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown lyrics method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_lyrics_introspection =
    "  <interface name=\"org.yxzl.ter_music.Lyrics\">\n"
    "    <method name=\"GetLyrics\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetDocument\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"LyricsChanged\">\n"
    "      <arg name=\"json\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_lyrics_introspection(void)
{
    return k_lyrics_introspection;
}

#endif /* HAVE_DBUS */
