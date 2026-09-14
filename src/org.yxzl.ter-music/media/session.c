#include "types.h"
#include "core/core.h"
#include <ncursesw/ncurses.h>
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
#include "ui/braille/braille_art.h"
#include "ui/lyrics.h"
#include "media/session.h"
#include "app/open.h"
#include "config/config.h"
#include "info/info.h"
#include "remote/remote.h"
#include "ui/menus.h"
#include "util/json.h"
#include "cli/cli.h"
#include "logger/logger.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_INTROSPECTABLE_INTERFACE "org.freedesktop.DBus.Introspectable"
#define DBUS_PEER_INTERFACE "org.freedesktop.DBus.Peer"
#define LYRICS_API_INTERFACE "org.yxzl.ter_music.Lyrics"
#define INFO_API_INTERFACE "org.yxzl.ter_music.Info"
#define CONTROL_API_INTERFACE "org.yxzl.ter_music.Control"
#define MPRIS_ART_URL_MAX (MAX_PATH_LEN * 3 + 16)
#define LYRICS_API_JSON_MAX 65536
#define INFO_PROGRESS_SIGNAL_INTERVAL_MS 1000

typedef struct {
    int valid;
    int current_index;
    int playlist_total;
    PlayState play_state;
    int loop_mode;  /* PlayMode value */
    int volume_percent;
    int can_seek;
    int64_t position_us;
    int64_t length_us;
    char track_id[96];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    char art_url[MPRIS_ART_URL_MAX];
} MediaSessionSnapshot;

typedef struct {
    DBusConnection *connection;
    int active;
    int has_primary_name;
    char bus_name[128];
    MediaSessionSnapshot last_snapshot;
    /* Info 接口状态 */
    unsigned long long info_revision;
    char last_info_json[INFO_JSON_MAX];
    /* 进度信号节流 */
    unsigned long long last_progress_ms;
    PlayState last_progress_state;
    /* Info/封面变更检测 */
    int info_key_valid;
    int info_track_index;
    int info_playlist_total;
    PlayState info_state;
    PlayMode info_mode;
    int info_volume;
    float info_speed;
    int info_cover_valid;
    char info_cover_path[MAX_PATH_LEN];
    char info_track_path[MAX_PATH_LEN];
} MediaSessionState;

typedef struct {
    int active_slot;         /* -1=none, 0=A, 1=B */
    int line_index[2];       /* lyric index in g_lyrics.lines, or -1 */
    char track_id[96];
    uint64_t revision;
    char last_json[LYRICS_API_JSON_MAX];
} LyricsApiState;

static MediaSessionState g_media_session = {0};
static LyricsApiState g_lyrics_api = {0};

static const char *const k_supported_uri_schemes[] = {"file", NULL};
static const char *const k_supported_mime_types[] = {
    "audio/mpeg",
    "audio/flac",
    "audio/ogg",
    "audio/opus",
    "audio/x-wav",
    "audio/x-flac",
    "audio/mp4",
    "audio/aac",
    NULL
};

/* ── 共享动作（定义见下方 “共享动作” 段，MPRIS 处理器先行引用） ── */
static int ms_action_play(void);
static int ms_action_play_pause(void);
static int ms_action_seek_to_us(int64_t position_us);
static int ms_action_seek_by_us(int64_t delta_us);
static int ms_action_set_volume_percent(int percent);
static int ms_action_open_path(const char *path, int autoplay);

static const char *playback_status_to_mpris(PlayState state) {
    switch (state) {
        case PLAY_STATE_PLAYING:
            return "Playing";
        case PLAY_STATE_PAUSED:
            return "Paused";
        case PLAY_STATE_STOPPED:
        default:
            return "Stopped";
    }
}

static const char *root_property_signature(const char *name) {
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "SupportedUriSchemes") == 0 || strcmp(name, "SupportedMimeTypes") == 0) {
        return "as";
    }
    if (strcmp(name, "Identity") == 0 || strcmp(name, "DesktopEntry") == 0) {
        return "s";
    }
    if (strcmp(name, "CanQuit") == 0 ||
        strcmp(name, "CanRaise") == 0 ||
        strcmp(name, "HasTrackList") == 0) {
        return "b";
    }
    return NULL;
}

static const char *player_property_signature(const char *name) {
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "Metadata") == 0) {
        return "a{sv}";
    }
    if (strcmp(name, "PlaybackStatus") == 0 || strcmp(name, "LoopStatus") == 0) {
        return "s";
    }
    if (strcmp(name, "Rate") == 0 ||
        strcmp(name, "MinimumRate") == 0 ||
        strcmp(name, "MaximumRate") == 0 ||
        strcmp(name, "Volume") == 0) {
        return "d";
    }
    if (strcmp(name, "Position") == 0) {
        return "x";
    }
    if (strcmp(name, "Shuffle") == 0 ||
        strcmp(name, "CanGoNext") == 0 ||
        strcmp(name, "CanGoPrevious") == 0 ||
        strcmp(name, "CanPlay") == 0 ||
        strcmp(name, "CanPause") == 0 ||
        strcmp(name, "CanSeek") == 0 ||
        strcmp(name, "CanControl") == 0) {
        return "b";
    }
    return NULL;
}

static void media_session_send(DBusMessage *message) {
    if (!message) {
        return;
    }

    if (g_media_session.active && g_media_session.connection) {
        dbus_connection_send(g_media_session.connection, message, NULL);
        dbus_connection_flush(g_media_session.connection);
    }
    dbus_message_unref(message);
}

static DBusMessage *media_session_error(DBusMessage *message,
                                        const char *error_name,
                                        const char *text) {
    return dbus_message_new_error(message, error_name, text);
}

static int current_track_is_available(void) {
    return g_current_play_index >= 0 && g_current_play_index < playlist_count();
}

static void capture_snapshot(MediaSessionSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->current_index = g_current_play_index;
    snapshot->playlist_total = playlist_count();
    snapshot->play_state = g_play_state;
    snapshot->loop_mode = g_play_mode;
    snapshot->volume_percent = get_volume_percent();
    snapshot->position_us = (int64_t)g_current_position * 1000000LL;
    snapshot->length_us = (int64_t)g_total_duration * 1000000LL;
    snapshot->can_seek = current_track_is_available() && g_total_duration > 0;

    if (!current_track_is_available()) {
        return;
    }

    char track_path[MAX_PATH_LEN];
    if (playlist_get_track_path(g_current_play_index, track_path, sizeof(track_path)) != 0) {
        return;
    }

    Track track;
    if (get_track_metadata(g_current_play_index, &track) != 0) {
        return;
    }

    snapshot->valid = 1;
    info_build_track_id(snapshot->track_id, sizeof(snapshot->track_id), track_path);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s", track.title);
    snprintf(snapshot->artist, sizeof(snapshot->artist), "%s", track.artist);
    snprintf(snapshot->album, sizeof(snapshot->album), "%s", track.album);

    char cover_path[MAX_PATH_LEN];
    if (get_current_album_cover_path(cover_path, sizeof(cover_path)) == 0) {
        info_build_file_uri(cover_path, snapshot->art_url, sizeof(snapshot->art_url));
    }
}

static int snapshots_equal(const MediaSessionSnapshot *lhs,
                           const MediaSessionSnapshot *rhs) {
    if (!lhs || !rhs) {
        return 0;
    }

    return lhs->valid == rhs->valid &&
           lhs->current_index == rhs->current_index &&
           lhs->playlist_total == rhs->playlist_total &&
           lhs->play_state == rhs->play_state &&
           lhs->loop_mode == rhs->loop_mode &&
           lhs->volume_percent == rhs->volume_percent &&
           lhs->can_seek == rhs->can_seek &&
           lhs->length_us == rhs->length_us &&
           strcmp(lhs->track_id, rhs->track_id) == 0 &&
           strcmp(lhs->title, rhs->title) == 0 &&
           strcmp(lhs->artist, rhs->artist) == 0 &&
           strcmp(lhs->album, rhs->album) == 0 &&
           strcmp(lhs->art_url, rhs->art_url) == 0;
}

/* ── Lyrics JSON API (org.yxzl.ter-music.Lyrics) ─────────────────── */

static void lyrics_api_reset_state(void) {
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
    if (current_track_is_available()) {
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
    media_session_send(signal);
}

static const char *lyrics_api_sync(void) {
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

static DBusMessage *handle_lyrics_method(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    if (strcmp(member, "GetLyrics") == 0) {
        const char *json = lyrics_api_sync();
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

    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown lyrics method");
}

static void append_string_array(DBusMessageIter *iter, const char *const *values) {
    DBusMessageIter array_iter;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &array_iter);
    if (values) {
        for (int i = 0; values[i] != NULL; i++) {
            const char *value = values[i];
            dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &value);
        }
    }
    dbus_message_iter_close_container(iter, &array_iter);
}

static void append_string_variant(DBusMessageIter *dict_iter,
                                  const char *key,
                                  const char *value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;
    const char *safe_value = value ? value : "";

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &safe_value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_boolean_variant(DBusMessageIter *dict_iter,
                                   const char *key,
                                   dbus_bool_t value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_double_variant(DBusMessageIter *dict_iter,
                                  const char *key,
                                  double value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "d", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_DOUBLE, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_int64_variant(DBusMessageIter *dict_iter,
                                 const char *key,
                                 int64_t value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "x", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_INT64, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_int_variant(DBusMessageIter *dict_iter,
                               const char *key,
                               int value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;
    dbus_int32_t out = (dbus_int32_t)value;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "i", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_INT32, &out);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_object_path_variant(DBusMessageIter *dict_iter,
                                       const char *key,
                                       const char *value) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;
    const char *safe_value = value ? value : MPRIS_OBJECT_PATH;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "o", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_OBJECT_PATH, &safe_value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_string_array_variant(DBusMessageIter *dict_iter,
                                        const char *key,
                                        const char *const *values) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
    append_string_array(&variant_iter, values);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_metadata_entries(DBusMessageIter *dict_iter,
                                    const MediaSessionSnapshot *snapshot) {
    if (!snapshot || !snapshot->valid) {
        return;
    }

    const char *artist_values[] = {snapshot->artist, NULL};
    append_object_path_variant(dict_iter, "mpris:trackid", snapshot->track_id);
    append_string_variant(dict_iter, "xesam:title", snapshot->title);
    append_string_variant(dict_iter, "xesam:album", snapshot->album);
    append_string_array_variant(dict_iter, "xesam:artist", artist_values);
    append_int64_variant(dict_iter, "mpris:length", snapshot->length_us);
    if (snapshot->art_url[0] != '\0') {
        append_string_variant(dict_iter, "mpris:artUrl", snapshot->art_url);
    }

    /* 曲目来源与序号：供第三方应用直接获取文件位置（远程曲目为原始 URL） */
    InfoTrack track;
    info_track_snapshot(&track);
    if (track.valid) {
        if (track.uri[0] != '\0') {
            append_string_variant(dict_iter, "xesam:url", track.uri);
        }
        int track_number = (track.cue_track_number > 0)
            ? track.cue_track_number : (track.index + 1);
        if (track_number > 0) {
            append_int_variant(dict_iter, "xesam:trackNumber", track_number);
        }
    }
}

static void append_metadata_variant(DBusMessageIter *dict_iter,
                                    const char *key,
                                    const MediaSessionSnapshot *snapshot) {
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;
    DBusMessageIter metadata_iter;

    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "a{sv}", &variant_iter);
    dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "{sv}", &metadata_iter);
    append_metadata_entries(&metadata_iter, snapshot);
    dbus_message_iter_close_container(&variant_iter, &metadata_iter);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);
    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_root_property_value(DBusMessageIter *iter, const char *property_name) {
    if (strcmp(property_name, "CanQuit") == 0) {
        dbus_bool_t value = FALSE;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "CanRaise") == 0) {
        dbus_bool_t value = FALSE;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "HasTrackList") == 0) {
        dbus_bool_t value = FALSE;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "Identity") == 0) {
        const char *value = APP_NAME;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);
    } else if (strcmp(property_name, "DesktopEntry") == 0) {
        const char *value = "ter-music";
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);
    } else if (strcmp(property_name, "SupportedUriSchemes") == 0) {
        append_string_array(iter, k_supported_uri_schemes);
    } else if (strcmp(property_name, "SupportedMimeTypes") == 0) {
        append_string_array(iter, k_supported_mime_types);
    }
}

static void append_player_property_value(DBusMessageIter *iter,
                                         const char *property_name,
                                         const MediaSessionSnapshot *snapshot) {
    const MediaSessionSnapshot empty_snapshot = {0};
    const MediaSessionSnapshot *current = snapshot ? snapshot : &empty_snapshot;

    if (strcmp(property_name, "PlaybackStatus") == 0) {
        const char *value = playback_status_to_mpris(current->play_state);
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);
    } else if (strcmp(property_name, "LoopStatus") == 0) {
        const char *value = info_loop_status_mpris((PlayMode)current->loop_mode);
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);
    } else if (strcmp(property_name, "Rate") == 0 ||
               strcmp(property_name, "MinimumRate") == 0 ||
               strcmp(property_name, "MaximumRate") == 0) {
        double value = 1.0;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &value);
    } else if (strcmp(property_name, "Shuffle") == 0) {
        dbus_bool_t value = info_shuffle_mpris(current->loop_mode);
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "Volume") == 0) {
        double value = (double)current->volume_percent / 100.0;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &value);
    } else if (strcmp(property_name, "Position") == 0) {
        dbus_int64_t value = current->position_us;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_INT64, &value);
    } else if (strcmp(property_name, "CanGoNext") == 0 ||
               strcmp(property_name, "CanGoPrevious") == 0 ||
               strcmp(property_name, "CanPlay") == 0) {
        dbus_bool_t value = current->playlist_total > 0;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "CanPause") == 0) {
        dbus_bool_t value = current->play_state != PLAY_STATE_STOPPED;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "CanSeek") == 0) {
        dbus_bool_t value = current->can_seek;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    } else if (strcmp(property_name, "CanControl") == 0) {
        dbus_bool_t value = TRUE;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);
    }
}

static DBusMessage *handle_get_property(DBusMessage *message,
                                        const MediaSessionSnapshot *snapshot) {
    const char *interface_name = NULL;
    const char *property_name = NULL;
    const char *signature = NULL;
    DBusError error;

    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_STRING, &interface_name,
                               DBUS_TYPE_STRING, &property_name,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
        dbus_error_free(&error);
        return reply;
    }
    dbus_error_free(&error);

    if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0) {
        signature = root_property_signature(property_name);
    } else if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) == 0) {
        signature = player_property_signature(property_name);
    } else {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");
    }

    if (!signature) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
    }

    DBusMessage *reply = dbus_message_new_method_return(message);
    DBusMessageIter iter;
    DBusMessageIter variant_iter;

    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, signature, &variant_iter);

    if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0) {
        append_root_property_value(&variant_iter, property_name);
    } else if (strcmp(property_name, "Metadata") == 0) {
        DBusMessageIter metadata_iter;
        dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "{sv}", &metadata_iter);
        append_metadata_entries(&metadata_iter, snapshot);
        dbus_message_iter_close_container(&variant_iter, &metadata_iter);
    } else {
        append_player_property_value(&variant_iter, property_name, snapshot);
    }

    dbus_message_iter_close_container(&iter, &variant_iter);
    return reply;
}

static void append_root_properties(DBusMessageIter *dict_iter) {
    append_boolean_variant(dict_iter, "CanQuit", FALSE);
    append_boolean_variant(dict_iter, "CanRaise", FALSE);
    append_boolean_variant(dict_iter, "HasTrackList", FALSE);
    append_string_variant(dict_iter, "Identity", APP_NAME);
    append_string_variant(dict_iter, "DesktopEntry", "ter-music");
    append_string_array_variant(dict_iter, "SupportedUriSchemes", k_supported_uri_schemes);
    append_string_array_variant(dict_iter, "SupportedMimeTypes", k_supported_mime_types);
}

static void append_player_properties(DBusMessageIter *dict_iter,
                                     const MediaSessionSnapshot *snapshot) {
    const char *playback_status = playback_status_to_mpris(snapshot->play_state);
    const char *loop_status = info_loop_status_mpris((PlayMode)snapshot->loop_mode);
    dbus_bool_t can_navigate = snapshot->playlist_total > 0;
    dbus_bool_t can_pause = snapshot->play_state != PLAY_STATE_STOPPED;
    double volume = (double)snapshot->volume_percent / 100.0;

    append_string_variant(dict_iter, "PlaybackStatus", playback_status);
    append_string_variant(dict_iter, "LoopStatus", loop_status);
    append_boolean_variant(dict_iter, "Shuffle", info_shuffle_mpris(snapshot->loop_mode));
    append_metadata_variant(dict_iter, "Metadata", snapshot);
    append_double_variant(dict_iter, "Volume", volume);
    append_boolean_variant(dict_iter, "CanGoNext", can_navigate);
    append_boolean_variant(dict_iter, "CanGoPrevious", can_navigate);
    append_boolean_variant(dict_iter, "CanPlay", can_navigate);
    append_boolean_variant(dict_iter, "CanPause", can_pause);
    append_boolean_variant(dict_iter, "CanSeek", snapshot->can_seek);
    append_boolean_variant(dict_iter, "CanControl", TRUE);
}

static DBusMessage *handle_get_all_properties(DBusMessage *message,
                                              const MediaSessionSnapshot *snapshot) {
    const char *interface_name = NULL;
    DBusError error;

    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_STRING, &interface_name,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
        dbus_error_free(&error);
        return reply;
    }
    dbus_error_free(&error);

    DBusMessage *reply = dbus_message_new_method_return(message);
    DBusMessageIter iter;
    DBusMessageIter dict_iter;

    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);

    if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0) {
        append_root_properties(&dict_iter);
    } else if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) == 0) {
        append_player_properties(&dict_iter, snapshot);
    } else {
        dbus_message_unref(reply);
        return media_session_error(message, DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");
    }

    dbus_message_iter_close_container(&iter, &dict_iter);
    return reply;
}

static void apply_remote_loop_status(const char *value) {
    if (!value) {
        return;
    }

    if (strcmp(value, "Track") == 0) {
        g_play_mode = PLAY_MODE_SINGLE_REPEAT;
    } else if (strcmp(value, "Playlist") == 0) {
        if (!play_mode_is_shuffle(g_play_mode)) {
            g_play_mode = PLAY_MODE_LIST_REPEAT;
        }
    } else {
        g_play_mode = PLAY_MODE_SEQUENTIAL;
    }
}

static DBusMessage *handle_set_property(DBusMessage *message) {
    DBusMessageIter iter;
    DBusMessageIter variant_iter;
    const char *interface_name = NULL;
    const char *property_name = NULL;

    if (!dbus_message_iter_init(message, &iter)) {
        return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Missing arguments");
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Expected interface name");
    }
    dbus_message_iter_get_basic(&iter, &interface_name);

    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Expected property name");
    }
    dbus_message_iter_get_basic(&iter, &property_name);

    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
        return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Expected variant value");
    }
    dbus_message_iter_recurse(&iter, &variant_iter);

    if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) != 0) {
        return media_session_error(message, DBUS_ERROR_PROPERTY_READ_ONLY, "Property is read only");
    }

    if (strcmp(property_name, "Volume") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_DOUBLE) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Volume must be a double");
        }
        double volume = 0.0;
        dbus_message_iter_get_basic(&variant_iter, &volume);
        if (volume < 0.0) {
            volume = 0.0;
        }
        if (volume > 1.0) {
            volume = 1.0;
        }
        ms_action_set_volume_percent((int)lrint(volume * 100.0));
    } else if (strcmp(property_name, "LoopStatus") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_STRING) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "LoopStatus must be a string");
        }
        const char *value = NULL;
        dbus_message_iter_get_basic(&variant_iter, &value);
        apply_remote_loop_status(value);
    } else if (strcmp(property_name, "Shuffle") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_BOOLEAN) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Shuffle must be a boolean");
        }
        dbus_bool_t enabled = FALSE;
        dbus_message_iter_get_basic(&variant_iter, &enabled);
        if (enabled) {
            g_play_mode = PLAY_MODE_SHUFFLE_REPEAT;
        } else if (play_mode_is_shuffle(g_play_mode)) {
            g_play_mode = PLAY_MODE_LIST_REPEAT;
        }
    } else {
        return media_session_error(message, DBUS_ERROR_PROPERTY_READ_ONLY, "Property is read only");
    }

    return dbus_message_new_method_return(message);
}

/* @return 1 = 已开始播放，0 = 没有可播放的曲目 */
static int play_selected_or_current_track(void) {
    int playlist_total = playlist_count();

    if (!playlist_is_loaded() || playlist_total <= 0) {
        return 0;
    }

    int target_index = (g_current_play_index >= 0)
        ? g_current_play_index
        : (g_sort_state.active ? g_sort_state.sorted_indices[g_selected_index] : g_selected_index);
    if (target_index >= 0 && target_index < playlist_total) {
        play_audio(target_index);
        return 1;
    }
    return 0;
}

static DBusMessage *handle_root_method(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);

    if (!member) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }
    if (strcmp(member, "Raise") == 0) {
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Quit") == 0) {
        return media_session_error(message, DBUS_ERROR_NOT_SUPPORTED, "Quit is not supported");
    }
    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown root method");
}

static DBusMessage *handle_player_method(DBusMessage *message,
                                         const MediaSessionSnapshot *snapshot) {
    const char *member = dbus_message_get_member(message);

    if (!member) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(member, "Next") == 0) {
        next_track();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Previous") == 0) {
        prev_track();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Pause") == 0) {
        pause_audio();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "PlayPause") == 0) {
        ms_action_play_pause();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Stop") == 0) {
        stop_audio();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Play") == 0) {
        ms_action_play();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Seek") == 0) {
        DBusError error;
        dbus_int64_t delta_us = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT64, &delta_us,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (snapshot->can_seek) {
            ms_action_seek_by_us((int64_t)delta_us);
        }
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "SetPosition") == 0) {
        DBusMessageIter iter;
        const char *track_id = NULL;
        dbus_int64_t position_us = 0;

        if (!dbus_message_iter_init(message, &iter)) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Missing arguments");
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Expected track id");
        }
        dbus_message_iter_get_basic(&iter, &track_id);

        if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT64) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS, "Expected position");
        }
        dbus_message_iter_get_basic(&iter, &position_us);

        if (snapshot->can_seek &&
            snapshot->valid &&
            strcmp(track_id, snapshot->track_id) == 0) {
            ms_action_seek_to_us((int64_t)position_us);
        }
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "OpenUri") == 0) {
        DBusError error;
        const char *uri = NULL;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &uri,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!ms_action_open_path(uri, 1)) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                       "Uri could not be opened");
        }
        return dbus_message_new_method_return(message);
    }

    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown player method");
}

/* ============================================================
 * 共享动作：MPRIS 处理器与 org.yxzl.ter_music.Control 复用
 * ============================================================ */

static int ms_action_play(void) {
    if (g_play_state == PLAY_STATE_PAUSED) {
        resume_audio();
        return 1;
    }
    if (g_play_state != PLAY_STATE_PLAYING) {
        return play_selected_or_current_track();
    }
    return 1;
}

static int ms_action_play_pause(void) {
    if (g_play_state == PLAY_STATE_PLAYING) {
        pause_audio();
        return 1;
    }
    if (g_play_state == PLAY_STATE_PAUSED) {
        resume_audio();
        return 1;
    }
    return play_selected_or_current_track();
}

static int ms_action_seek_to_us(int64_t position_us) {
    if (!current_track_is_available()) {
        return 0;
    }
    if (position_us < 0) {
        position_us = 0;
    }
    int64_t length_us = (int64_t)audio_get_duration_seconds() * 1000000LL;
    if (length_us > 0 && position_us > length_us) {
        position_us = length_us;
    }
    seek_audio((double)position_us / 1000000.0);
    return 1;
}

static int ms_action_seek_by_us(int64_t delta_us) {
    int64_t position_us = (int64_t)audio_get_position_seconds() * 1000000LL;
    return ms_action_seek_to_us(position_us + delta_us);
}

static int ms_action_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    set_volume_percent(percent);
    return 1;
}

static int ms_action_set_speed(double rate) {
    if (!(rate >= 0.5 && rate <= 3.0)) {
        return 0;
    }
    g_playback_speed = (float)rate;
    g_app_config.default_playback_speed = g_playback_speed;
    save_config();
    apply_playback_speed_change();
    return 1;
}

static int ms_action_play_index(int index) {
    int total = playlist_count();
    if (index < 0 || index >= total) {
        return 0;
    }
    play_audio(index);
    app_set_selection_for_track(index);
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    return 1;
}

/* 打开本地路径 / 远程 URL（MPRIS OpenUri 与 Control.OpenPath 共用） */
static int ms_action_open_path(const char *path, int autoplay) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    char local_path[MAX_PATH_LEN];
    if (info_uri_to_path(path, local_path, sizeof(local_path)) != 0) {
        return 0;
    }
    if (local_path[0] == '\0') {
        return 0;
    }

    if (remote_is_remote_path(local_path)) {
        RemoteConnectionConfig connection;
        if (remote_parse_url(local_path, &connection) != 0) {
            return 0;
        }
        if (load_remote_playlist(&connection, connection.base_path) <= 0) {
            return 0;
        }
        g_selected_index = 0;
    } else if (app_open_path(local_path, NULL, 0, NULL, NULL) != APP_OPEN_OK) {
        return 0;
    }

    /* 新播放列表：清空旧队列，交由 play_audio() 按当前模式重建 */
    play_queue_clear(&g_play_queue);

    if (autoplay && playlist_count() > 0) {
        int index = (g_current_play_index >= 0 && g_current_play_index < playlist_count())
            ? g_current_play_index : 0;
        play_audio(index);
        app_set_selection_for_track(index);
    }

    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    return 1;
}

/* ============================================================
 * org.yxzl.ter_music.Info —— 曲目 / 进度 / 字符封面 / 配置化文本
 * ============================================================ */

static InfoInstance ms_instance_info(void) {
    InfoInstance instance;
    memset(&instance, 0, sizeof(instance));
    instance.is_daemon = g_daemon_mode;
    instance.pid = (int)getpid();
    instance.version = APP_VERSION;
    instance.bus_name = g_media_session.bus_name;
    instance.has_primary_name = g_media_session.has_primary_name;
    return instance;
}

static void emit_info_changed(const char *json) {
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  INFO_API_INTERFACE,
                                                  "InfoChanged");
    if (!signal) {
        return;
    }
    const char *value = (json && json[0] != '\0') ? json : "{}";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    media_session_send(signal);
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
        media_session_send(signal);
    }
    free(text);
}

static void emit_progress_changed(const MediaSessionSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    uint64_t now_ms = get_ui_time_ms();
    int state_changed = (snapshot->play_state != g_media_session.last_progress_state);
    if (!state_changed &&
        (now_ms - g_media_session.last_progress_ms) < INFO_PROGRESS_SIGNAL_INTERVAL_MS) {
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
        const char *status = playback_status_to_mpris(snapshot->play_state);
        dbus_message_append_args(signal,
                                 DBUS_TYPE_INT64, &position_us,
                                 DBUS_TYPE_INT64, &length_us,
                                 DBUS_TYPE_STRING, &status,
                                 DBUS_TYPE_INVALID);
        media_session_send(signal);
    }

    g_media_session.last_progress_ms = now_ms;
    g_media_session.last_progress_state = snapshot->play_state;
}

/* 检测“非进度类”信息变化：轨道、状态、模式、音量、倍速、封面 */
static void info_api_sync(void) {
    if (!g_media_session.active) {
        return;
    }

    char track_path[MAX_PATH_LEN] = "";
    if (current_track_is_available()) {
        if (playlist_get_track_path(g_current_play_index, track_path,
                                    sizeof(track_path)) != 0) {
            track_path[0] = '\0';
        }
    }

    int changed = 0;
    if (!g_media_session.info_key_valid) {
        changed = 1;
    } else if (g_media_session.info_track_index != g_current_play_index ||
               strcmp(g_media_session.info_track_path, track_path) != 0 ||
               g_media_session.info_playlist_total != playlist_count() ||
               g_media_session.info_state != g_play_state ||
               g_media_session.info_mode != g_play_mode ||
               g_media_session.info_volume != get_volume_percent() ||
               fabs((double)g_media_session.info_speed - (double)g_playback_speed) > 0.001 ||
               g_media_session.info_cover_valid != g_current_album_cover_valid ||
               strcmp(g_media_session.info_cover_path, g_current_album_cover_path) != 0) {
        changed = 1;
    }

    if (!changed) {
        return;
    }

    int cover_changed = (!g_media_session.info_key_valid ||
                         g_media_session.info_cover_valid != g_current_album_cover_valid ||
                         strcmp(g_media_session.info_cover_path,
                                g_current_album_cover_path) != 0);

    g_media_session.info_key_valid = 1;
    g_media_session.info_track_index = g_current_play_index;
    snprintf(g_media_session.info_track_path,
             sizeof(g_media_session.info_track_path), "%s", track_path);
    g_media_session.info_playlist_total = playlist_count();
    g_media_session.info_state = g_play_state;
    g_media_session.info_mode = g_play_mode;
    g_media_session.info_volume = get_volume_percent();
    g_media_session.info_speed = g_playback_speed;
    g_media_session.info_cover_valid = g_current_album_cover_valid;
    snprintf(g_media_session.info_cover_path,
             sizeof(g_media_session.info_cover_path), "%s",
             g_current_album_cover_path);

    g_media_session.info_revision++;

    char *json = malloc(INFO_JSON_MAX);
    if (json) {
        InfoInstance instance = ms_instance_info();
        info_render_json(json, INFO_JSON_MAX, &instance, g_media_session.info_revision);
        snprintf(g_media_session.last_info_json,
                 sizeof(g_media_session.last_info_json), "%s", json);
        emit_info_changed(json);
        free(json);
    }

    if (cover_changed) {
        emit_cover_changed();
    }
}

static DBusMessage *info_reply_string(DBusMessage *message, const char *value) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    const char *safe = value ? value : "";
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &safe, DBUS_TYPE_INVALID);
    return reply;
}

static DBusMessage *handle_info_method(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    InfoInstance instance = ms_instance_info();

    if (strcmp(member, "GetInfo") == 0) {
        char *json = malloc(INFO_JSON_MAX);
        if (!json) {
            return media_session_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_render_json(json, INFO_JSON_MAX, &instance, g_media_session.info_revision);
        DBusMessage *reply = info_reply_string(message, json);
        free(json);
        return reply;
    }

    if (strcmp(member, "GetTrackInfo") == 0) {
        char *json = malloc(INFO_JSON_MAX);
        if (!json) {
            return media_session_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_render_track_json(json, INFO_JSON_MAX);
        DBusMessage *reply = info_reply_string(message, json);
        free(json);
        return reply;
    }

    if (strcmp(member, "GetProgress") == 0) {
        char json[4096];
        info_render_progress_json(json, sizeof(json));
        return info_reply_string(message, json);
    }

    if (strcmp(member, "GetLyricsLines") == 0) {
        char json[4096];
        info_render_lyrics_json(json, sizeof(json));
        return info_reply_string(message, json);
    }

    if (strcmp(member, "InstanceInfo") == 0) {
        char json[512];
        info_render_instance_json(json, sizeof(json), &instance);
        return info_reply_string(message, json);
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
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
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
                return media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                           "charset must be 'braille' or 'ascii'");
            }
        }

        char *text = malloc(INFO_COVER_TEXT_MAX);
        if (!text) {
            return media_session_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        info_cover_text(cols, rows, charset_value, text, INFO_COVER_TEXT_MAX);
        DBusMessage *reply = info_reply_string(message, text);
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
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        InfoRenderOptions options;
        info_options_from_config(&options);
        if (options_text && options_text[0] != '\0' &&
            info_options_parse(&options, options_text) != 0) {
            return media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                       "Invalid display options");
        }

        char *text = malloc(INFO_TEXT_MAX);
        if (!text) {
            return media_session_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        if (info_render_text(&options, text, INFO_TEXT_MAX) < 0) {
            text[0] = '\0';
        }
        DBusMessage *reply = info_reply_string(message, text);
        free(text);
        return reply;
    }

    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown info method");
}

/* ============================================================
 * org.yxzl.ter_music.Control —— CLI 控制通道
 * ============================================================ */

static DBusMessage *control_bool_reply(DBusMessage *message, int ok) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    dbus_bool_t value = ok ? TRUE : FALSE;
    dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID);
    return reply;
}

static DBusMessage *control_int_reply(DBusMessage *message, int value) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    dbus_int32_t out = (dbus_int32_t)value;
    dbus_message_append_args(reply, DBUS_TYPE_INT32, &out, DBUS_TYPE_INVALID);
    return reply;
}

static DBusMessage *handle_control_method(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                   "Missing method name");
    }

    if (strcmp(member, "Play") == 0) {
        return control_bool_reply(message, ms_action_play());
    }
    if (strcmp(member, "Pause") == 0) {
        pause_audio();
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "PlayPause") == 0) {
        return control_bool_reply(message, ms_action_play_pause());
    }
    if (strcmp(member, "Stop") == 0) {
        stop_audio();
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "Next") == 0) {
        next_track();
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "Previous") == 0) {
        prev_track();
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "SeekTo") == 0 || strcmp(member, "SeekBy") == 0) {
        DBusError error;
        dbus_int64_t value = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT64, &value,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int ok = (strcmp(member, "SeekTo") == 0)
            ? ms_action_seek_to_us((int64_t)value)
            : ms_action_seek_by_us((int64_t)value);
        return control_bool_reply(message, ok);
    }
    if (strcmp(member, "SetVolume") == 0) {
        DBusError error;
        dbus_int32_t percent = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &percent,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return control_bool_reply(message, ms_action_set_volume_percent(percent));
    }
    if (strcmp(member, "GetVolume") == 0) {
        return control_int_reply(message, get_volume_percent());
    }
    if (strcmp(member, "SetSpeed") == 0) {
        DBusError error;
        double rate = 0.0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_DOUBLE, &rate,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return control_bool_reply(message, ms_action_set_speed(rate));
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
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (mode < 0 || mode >= PLAY_MODE_COUNT) {
            return control_bool_reply(message, 0);
        }
        set_play_mode((PlayMode)mode);
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "GetPlayMode") == 0) {
        return control_int_reply(message, (int)g_play_mode);
    }
    if (strcmp(member, "GetPlayModeName") == 0) {
        return info_reply_string(message, play_mode_display_name(g_play_mode, 0));
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
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return control_bool_reply(message, ms_action_open_path(path, autoplay ? 1 : 0));
    }
    if (strcmp(member, "PlayIndex") == 0) {
        DBusError error;
        dbus_int32_t index = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = media_session_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);
        return control_bool_reply(message, ms_action_play_index(index));
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
        return info_reply_string(message, json);
    }
    if (strcmp(member, "ReloadConfig") == 0) {
        g_config_reload_requested = 1;
        return control_bool_reply(message, 1);
    }
    if (strcmp(member, "Quit") == 0) {
        extern volatile sig_atomic_t g_should_exit;
        g_should_exit = 1;
        return control_bool_reply(message, 1);
    }

    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                               "Unknown control method");
}

/* ============================================================
 * Introspection / Peer
 * ============================================================ */

static const char *const k_introspection_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "    <method name=\"GetMachineId\">\n"
    "      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n"
    "      <arg name=\"interface_name\" type=\"s\"/>\n"
    "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"TrackId\" type=\"o\" direction=\"in\"/>\n"
    "      <arg name=\"Position\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"OpenUri\">\n"
    "      <arg name=\"Uri\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"Seeked\">\n"
    "      <arg name=\"Position\" type=\"x\"/>\n"
    "    </signal>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.yxzl.ter_music.Lyrics\">\n"
    "    <method name=\"GetLyrics\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"LyricsChanged\">\n"
    "      <arg name=\"json\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
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
    "  </interface>\n"
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
    "  </interface>\n"
    "</node>\n";

static DBusMessage *handle_introspect(DBusMessage *message) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    const char *xml = k_introspection_xml;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
    return reply;
}

static DBusMessage *handle_get_machine_id(DBusMessage *message) {
    char machine_id[64] = "00000000000000000000000000000000";

    FILE *file = fopen("/etc/machine-id", "r");
    if (file) {
        if (fgets(machine_id, sizeof(machine_id), file)) {
            size_t length = strlen(machine_id);
            while (length > 0 && (machine_id[length - 1] == '\n' ||
                                  machine_id[length - 1] == '\r')) {
                machine_id[--length] = '\0';
            }
        }
        fclose(file);
    }
    if (machine_id[0] == '\0') {
        snprintf(machine_id, sizeof(machine_id), "00000000000000000000000000000000");
    }

    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    const char *value = machine_id;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    return reply;
}

static void emit_properties_changed(const char *interface_name,
                                    const MediaSessionSnapshot *snapshot) {
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  DBUS_PROPERTIES_INTERFACE,
                                                  "PropertiesChanged");
    DBusMessageIter iter;
    DBusMessageIter changed_iter;
    DBusMessageIter invalidated_iter;

    if (!signal) {
        return;
    }

    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface_name);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed_iter);

    if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0) {
        append_root_properties(&changed_iter);
    } else {
        append_player_properties(&changed_iter, snapshot);
    }

    dbus_message_iter_close_container(&iter, &changed_iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated_iter);
    dbus_message_iter_close_container(&iter, &invalidated_iter);

    media_session_send(signal);
}

static void sync_player_state(void) {
    MediaSessionSnapshot snapshot;
    capture_snapshot(&snapshot);

    if (!snapshots_equal(&snapshot, &g_media_session.last_snapshot)) {
        emit_properties_changed(MPRIS_PLAYER_INTERFACE, &snapshot);
        g_media_session.last_snapshot = snapshot;
    }
}

static int request_bus_name(char *dest, size_t dest_size, int *primary_out) {
    if (primary_out) {
        *primary_out = 0;
    }
    DBusError error;
    const char *base_name = "org.mpris.MediaPlayer2.ter_music";
    int request_result = DBUS_REQUEST_NAME_REPLY_EXISTS;

    dbus_error_init(&error);
    request_result = dbus_bus_request_name(g_media_session.connection,
                                           base_name,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                           &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return 0;
    }
    if (request_result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        snprintf(dest, dest_size, "%s", base_name);
        if (primary_out) {
            *primary_out = 1;
        }
        return 1;
    }

    char fallback_name[128];
    snprintf(fallback_name, sizeof(fallback_name), "%s.instance%ld", base_name, (long)getpid());

    dbus_error_init(&error);
    request_result = dbus_bus_request_name(g_media_session.connection,
                                           fallback_name,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                           &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return 0;
    }
    if (request_result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        snprintf(dest, dest_size, "%s", fallback_name);
        return 1;
    }
    return 0;
}

void media_session_init(void) {
    log_info("media_session", "Initializing media session (MPRIS/D-Bus)");
    DBusError error;

    memset(&g_media_session, 0, sizeof(g_media_session));
    lyrics_api_reset_state();

    dbus_error_init(&error);
    g_media_session.connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        log_warn("media_session", "D-Bus connection failed: %s", error.message);
        dbus_error_free(&error);
        return;
    }
    if (!g_media_session.connection) {
        log_warn("media_session", "D-Bus connection is NULL");
        return;
    }

    dbus_connection_set_exit_on_disconnect(g_media_session.connection, FALSE);

    int has_primary_name = 0;
    if (!request_bus_name(g_media_session.bus_name, sizeof(g_media_session.bus_name),
                          &has_primary_name)) {
        log_warn("media_session", "Failed to acquire D-Bus bus name");
        dbus_connection_unref(g_media_session.connection);
        memset(&g_media_session, 0, sizeof(g_media_session));
        return;
    }

    g_media_session.active = 1;
    g_media_session.has_primary_name = has_primary_name;
    log_info("media_session", "D-Bus initialized, bus='%s' (primary=%d)",
             g_media_session.bus_name, has_primary_name);
    emit_properties_changed(MPRIS_ROOT_INTERFACE, NULL);
    sync_player_state();
    lyrics_api_sync();
    info_api_sync();

    {
        MediaSessionSnapshot progress_snapshot;
        capture_snapshot(&progress_snapshot);
        emit_progress_changed(&progress_snapshot);
    }
}

int media_session_has_primary_name(void) {
    return g_media_session.active && g_media_session.has_primary_name;
}

const char *media_session_bus_name(void) {
    return g_media_session.active ? g_media_session.bus_name : "";
}

void media_session_shutdown(void) {
    log_info("media_session", "Shutting down media session");
    if (!g_media_session.connection) {
        lyrics_api_reset_state();
        memset(&g_media_session, 0, sizeof(g_media_session));
        return;
    }

    if (g_media_session.active && g_media_session.bus_name[0] != '\0') {
        /* Quit 等“以回复触发退出”的调用：先把待发消息冲刷到总线并留出极短
         * 窗口，避免连接关闭早于回复送达，使客户端收到 NoReply。 */
        dbus_connection_flush(g_media_session.connection);
        usleep(50000);
        log_debug("media_session", "Releasing D-Bus name: '%s'", g_media_session.bus_name);
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_release_name(g_media_session.connection, g_media_session.bus_name, &error);
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
        }
    }

    dbus_connection_unref(g_media_session.connection);
    lyrics_api_reset_state();
    memset(&g_media_session, 0, sizeof(g_media_session));
}

void media_session_notify_seek(uint64_t position_ms) {
    if (!g_media_session.active || !g_media_session.connection || !current_track_is_available()) {
        return;
    }

    log_debug("media_session", "Notifying seek: position=%llu ms", (unsigned long long)position_ms);

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  MPRIS_PLAYER_INTERFACE,
                                                  "Seeked");
    if (!signal) {
        return;
    }

    dbus_int64_t position_us = (dbus_int64_t)position_ms * 1000LL;
    dbus_message_append_args(signal,
                             DBUS_TYPE_INT64, &position_us,
                             DBUS_TYPE_INVALID);
    media_session_send(signal);
}

void media_session_tick(void) {
    if (!g_media_session.active || !g_media_session.connection) {
        return;
    }
    if (!dbus_connection_get_is_connected(g_media_session.connection)) {
        log_warn("media_session", "D-Bus connection lost, shutting down");
        media_session_shutdown();
        return;
    }

    dbus_connection_read_write(g_media_session.connection, 0);

    DBusMessage *message = NULL;
    while ((message = dbus_connection_pop_message(g_media_session.connection)) != NULL) {
        const char *path = dbus_message_get_path(message);
        DBusMessage *reply = NULL;
        MediaSessionSnapshot snapshot;

        if (!path || strcmp(path, MPRIS_OBJECT_PATH) != 0) {
            dbus_message_unref(message);
            continue;
        }

        capture_snapshot(&snapshot);

        if (dbus_message_is_method_call(message, DBUS_PROPERTIES_INTERFACE, "Get")) {
            reply = handle_get_property(message, &snapshot);
        } else if (dbus_message_is_method_call(message, DBUS_PROPERTIES_INTERFACE, "GetAll")) {
            reply = handle_get_all_properties(message, &snapshot);
        } else if (dbus_message_is_method_call(message, DBUS_PROPERTIES_INTERFACE, "Set")) {
            reply = handle_set_property(message);
        } else if (dbus_message_is_method_call(message, DBUS_INTROSPECTABLE_INTERFACE, "Introspect")) {
            reply = handle_introspect(message);
        } else if (dbus_message_is_method_call(message, DBUS_PEER_INTERFACE, "Ping")) {
            reply = dbus_message_new_method_return(message);
        } else if (dbus_message_is_method_call(message, DBUS_PEER_INTERFACE, "GetMachineId")) {
            reply = handle_get_machine_id(message);
        } else if (dbus_message_has_interface(message, MPRIS_ROOT_INTERFACE)) {
            reply = handle_root_method(message);
        } else if (dbus_message_has_interface(message, MPRIS_PLAYER_INTERFACE)) {
            reply = handle_player_method(message, &snapshot);
        } else if (dbus_message_has_interface(message, LYRICS_API_INTERFACE)) {
            reply = handle_lyrics_method(message);
        } else if (dbus_message_has_interface(message, INFO_API_INTERFACE)) {
            reply = handle_info_method(message);
        } else if (dbus_message_has_interface(message, CONTROL_API_INTERFACE)) {
            reply = handle_control_method(message);
        }

        if (reply) {
            media_session_send(reply);
        }

        dbus_message_unref(message);
    }

    sync_player_state();
    lyrics_api_sync();
    info_api_sync();

    {
        MediaSessionSnapshot progress_snapshot;
        capture_snapshot(&progress_snapshot);
        emit_progress_changed(&progress_snapshot);
    }
}

#else

void media_session_init(void) {
    log_debug("media_session", "D-Bus not available, media session stubs used");
}

void media_session_tick(void) {
}

void media_session_shutdown(void) {
}

void media_session_notify_seek(uint64_t position_ms) {
    (void)position_ms;
}

int media_session_has_primary_name(void) {
    return 0;
}

const char *media_session_bus_name(void) {
    return "";
}

#endif
