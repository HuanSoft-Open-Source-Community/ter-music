#include "types.h"
#include "core/core.h"
#include <ncursesw/ncurses.h>
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "queue/backend_queue.h"
#include "ui/braille/braille_art.h"
#include "lyrics/lyrics.h"
#include "media/rpc.h"
#include "media/session.h"
#include "config/config.h"
#include "info/info.h"
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
#define MPRIS_ART_URL_MAX (MAX_PATH_LEN * 3 + 16)
#define INFO_PROGRESS_SIGNAL_INTERVAL_MS 1000

/* RpcPlaybackSnapshot 定义见 media/rpc.h（各 rpc_*.c 共用） */

typedef struct {
    DBusConnection *connection;
    int active;
    int has_primary_name;
    char bus_name[128];
    RpcPlaybackSnapshot last_snapshot;   /* MPRIS 属性变更检测 */
} MediaSessionState;

static MediaSessionState g_media_session = {0};

/* 1 = 曾经持有总线名，随后 D-Bus 连接断开（总线消失）。
 * 只在 media_session_tick() 的断连分支置位、永不清零：核心据此判定
 * “再也没有任何前端能联系到我”，由主循环决定退出。从未拿到总线的进程
 * （无会话总线的 systemd/纯音频场景）不会被置位。 */
static int g_session_lost = 0;

/* ── 会话访问器（供 rpc_common.c 与各 rpc_*.c 查询总线状态） ─────── */

DBusConnection *rpc_session_connection(void)
{
    return (g_media_session.active && g_media_session.connection)
        ? g_media_session.connection : NULL;
}

int rpc_session_active(void)
{
    return g_media_session.active;
}

int rpc_session_has_primary_name(void)
{
    return g_media_session.has_primary_name;
}

const char *rpc_session_bus_name(void)
{
    return g_media_session.bus_name;
}


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





static int snapshots_equal(const RpcPlaybackSnapshot *lhs,
                           const RpcPlaybackSnapshot *rhs) {
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
                                    const RpcPlaybackSnapshot *snapshot) {
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
                                    const RpcPlaybackSnapshot *snapshot) {
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
                                         const RpcPlaybackSnapshot *snapshot) {
    const RpcPlaybackSnapshot empty_snapshot = {0};
    const RpcPlaybackSnapshot *current = snapshot ? snapshot : &empty_snapshot;

    if (strcmp(property_name, "PlaybackStatus") == 0) {
        const char *value = rpc_playback_status_name(current->play_state);
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
                                        const RpcPlaybackSnapshot *snapshot) {
    const char *interface_name = NULL;
    const char *property_name = NULL;
    const char *signature = NULL;
    DBusError error;

    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_STRING, &interface_name,
                               DBUS_TYPE_STRING, &property_name,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
        dbus_error_free(&error);
        return reply;
    }
    dbus_error_free(&error);

    if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0) {
        signature = root_property_signature(property_name);
    } else if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) == 0) {
        signature = player_property_signature(property_name);
    } else {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");
    }

    if (!signature) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
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
                                     const RpcPlaybackSnapshot *snapshot) {
    const char *playback_status = rpc_playback_status_name(snapshot->play_state);
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
                                              const RpcPlaybackSnapshot *snapshot) {
    const char *interface_name = NULL;
    DBusError error;

    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_STRING, &interface_name,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
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
        return rpc_error(message, DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");
    }

    dbus_message_iter_close_container(&iter, &dict_iter);
    return reply;
}

static void apply_bus_loop_status(const char *value) {
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
        return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Missing arguments");
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Expected interface name");
    }
    dbus_message_iter_get_basic(&iter, &interface_name);

    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Expected property name");
    }
    dbus_message_iter_get_basic(&iter, &property_name);

    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
        return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Expected variant value");
    }
    dbus_message_iter_recurse(&iter, &variant_iter);

    if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) != 0) {
        return rpc_error(message, DBUS_ERROR_PROPERTY_READ_ONLY, "Property is read only");
    }

    if (strcmp(property_name, "Volume") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_DOUBLE) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Volume must be a double");
        }
        double volume = 0.0;
        dbus_message_iter_get_basic(&variant_iter, &volume);
        if (volume < 0.0) {
            volume = 0.0;
        }
        if (volume > 1.0) {
            volume = 1.0;
        }
        rpc_action_set_volume_percent((int)lrint(volume * 100.0));
    } else if (strcmp(property_name, "LoopStatus") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_STRING) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "LoopStatus must be a string");
        }
        const char *value = NULL;
        dbus_message_iter_get_basic(&variant_iter, &value);
        apply_bus_loop_status(value);
    } else if (strcmp(property_name, "Shuffle") == 0) {
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_BOOLEAN) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Shuffle must be a boolean");
        }
        dbus_bool_t enabled = FALSE;
        dbus_message_iter_get_basic(&variant_iter, &enabled);
        if (enabled) {
            g_play_mode = PLAY_MODE_SHUFFLE_REPEAT;
        } else if (play_mode_is_shuffle(g_play_mode)) {
            g_play_mode = PLAY_MODE_LIST_REPEAT;
        }
    } else {
        return rpc_error(message, DBUS_ERROR_PROPERTY_READ_ONLY, "Property is read only");
    }

    return dbus_message_new_method_return(message);
}

/* @return 1 = 已开始播放，0 = 没有可播放的曲目 */

static DBusMessage *handle_root_method(DBusMessage *message) {
    const char *member = dbus_message_get_member(message);

    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }
    if (strcmp(member, "Raise") == 0) {
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Quit") == 0) {
        return rpc_error(message, DBUS_ERROR_NOT_SUPPORTED, "Quit is not supported");
    }
    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown root method");
}

static DBusMessage *handle_player_method(DBusMessage *message,
                                         const RpcPlaybackSnapshot *snapshot) {
    const char *member = dbus_message_get_member(message);

    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
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
        rpc_action_play_pause();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Stop") == 0) {
        stop_audio();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Play") == 0) {
        rpc_action_play();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Seek") == 0) {
        DBusError error;
        dbus_int64_t delta_us = 0;

        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT64, &delta_us,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (snapshot->can_seek) {
            rpc_action_seek_by_us((int64_t)delta_us);
        }
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "SetPosition") == 0) {
        DBusMessageIter iter;
        const char *track_id = NULL;
        dbus_int64_t position_us = 0;

        if (!dbus_message_iter_init(message, &iter)) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Missing arguments");
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Expected track id");
        }
        dbus_message_iter_get_basic(&iter, &track_id);

        if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT64) {
            return rpc_error(message, DBUS_ERROR_INVALID_ARGS, "Expected position");
        }
        dbus_message_iter_get_basic(&iter, &position_us);

        if (snapshot->can_seek &&
            snapshot->valid &&
            strcmp(track_id, snapshot->track_id) == 0) {
            rpc_action_seek_to_us((int64_t)position_us);
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
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS,
                                                     error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        /* api_version 4：加载内容属前端，核心不再扫描目录、也不再构建内容
         * 列表，因此 OpenUri 无法在核心里完成——前端接入后应当自行加载并把
         * 结果经 Queue.Set/Queue.PlayAt 下发。这里明确回“不支持”而不是假装成功。
         * 非本地 URI 仍然按老规矩单独说明（远程音乐源属前端）。 */
        if (!bq_path_is_local(uri)) {
            return rpc_error(message, DBUS_ERROR_NOT_SUPPORTED,
                             "OpenUri accepts local files only; remote sources belong to the front end");
        }
        return rpc_error(message, DBUS_ERROR_NOT_SUPPORTED,
                         "The core does not load content any more; the front end scans and delivers a queue via Queue.Set/Queue.PlayAt");
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown player method");
}

/* ============================================================
 * 共享动作：MPRIS 处理器与 org.yxzl.ter_music.Control 复用
 * ============================================================ */


/* ── 自省 XML ───────────────────────────────────────────────────────
 * 本文件提供 MPRIS / Properties / Peer 三段的头部片段；org.yxzl.ter_music.*
 * 各接口的片段由所属 rpc_*.c 提供（rpc_lyrics_introspection() 等），
 * 启动时拼装一次，保证接口与其自省描述始终相邻。 */

static const char *const k_introspection_head =
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
    "  </interface>\n";
static const char *const k_introspection_tail =
    "</node>\n";

#define RPC_INTROSPECTION_MAX 65536
static char g_introspection[RPC_INTROSPECTION_MAX] = "";

static void build_introspection(void)
{
    const char *fragments[] = {
        k_introspection_head,
        rpc_lyrics_introspection(),
        rpc_info_introspection(),
        rpc_control_introspection(),
        rpc_queue_introspection(),
        rpc_config_introspection(),
        k_introspection_tail,
        NULL
    };

    size_t used = 0;
    g_introspection[0] = '\0';
    for (int i = 0; fragments[i] != NULL; i++) {
        size_t length = strlen(fragments[i]);
        if (used + length + 1 > sizeof(g_introspection)) {
            log_error("media_session", "Introspection XML exceeds the %d byte buffer",
                      (int)sizeof(g_introspection));
            g_introspection[0] = '\0';
            return;
        }
        memcpy(g_introspection + used, fragments[i], length);
        used += length;
        g_introspection[used] = '\0';
    }
}

static DBusMessage *handle_introspect(DBusMessage *message) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    const char *xml = g_introspection;
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
                                    const RpcPlaybackSnapshot *snapshot) {
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

    rpc_send(signal);
}

static void sync_player_state(void) {
    RpcPlaybackSnapshot snapshot;
    rpc_capture_snapshot(&snapshot);

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
    build_introspection();
    rpc_lyrics_reset();
    rpc_info_reset();

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
    rpc_lyrics_sync();
    rpc_info_sync();

    {
        RpcPlaybackSnapshot progress_snapshot;
        rpc_capture_snapshot(&progress_snapshot);
        rpc_info_emit_progress(&progress_snapshot);
    }
}

int media_session_has_primary_name(void) {
    return g_media_session.active && g_media_session.has_primary_name;
}

int media_session_lost(void) {
    return g_session_lost;
}

const char *media_session_bus_name(void) {
    return g_media_session.active ? g_media_session.bus_name : "";
}

void media_session_shutdown(void) {
    log_info("media_session", "Shutting down media session");
    if (!g_media_session.connection) {
        rpc_lyrics_reset();
        rpc_info_reset();
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
    rpc_lyrics_reset();
    rpc_info_reset();
    memset(&g_media_session, 0, sizeof(g_media_session));
}

void media_session_notify_seek(uint64_t position_ms) {
    if (!g_media_session.active || !g_media_session.connection || !rpc_track_available()) {
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
    rpc_send(signal);
}

void media_session_tick(void) {
    if (!g_media_session.active || !g_media_session.connection) {
        return;
    }
    if (!dbus_connection_get_is_connected(g_media_session.connection)) {
        log_warn("media_session", "D-Bus connection lost, shutting down");
        /* 闩锁先置位：media_session_shutdown() 会把整个 session 结构清零，
         * 而“曾经持有总线名、随后连接断开”这件事必须活到进程决定退出为止。 */
        g_session_lost = 1;
        media_session_shutdown();
        return;
    }

    dbus_connection_read_write(g_media_session.connection, 0);

    DBusMessage *message = NULL;
    while ((message = dbus_connection_pop_message(g_media_session.connection)) != NULL) {
        const char *path = dbus_message_get_path(message);
        DBusMessage *reply = NULL;
        RpcPlaybackSnapshot snapshot;

        if (!path || strcmp(path, MPRIS_OBJECT_PATH) != 0) {
            dbus_message_unref(message);
            continue;
        }

        rpc_capture_snapshot(&snapshot);

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
        } else if (dbus_message_has_interface(message, RPC_IFACE_LYRICS)) {
            reply = rpc_lyrics_handle(message);
        } else if (dbus_message_has_interface(message, RPC_IFACE_INFO)) {
            reply = rpc_info_handle(message);
        } else if (dbus_message_has_interface(message, RPC_IFACE_CONTROL)) {
            reply = rpc_control_handle(message);
        } else if (dbus_message_has_interface(message, RPC_IFACE_QUEUE)) {
            reply = rpc_queue_handle(message);
        } else if (dbus_message_has_interface(message, RPC_IFACE_CONFIG)) {
            reply = rpc_config_handle(message);
        }

        /* 未匹配任何接口/方法时必须回 UnknownMethod：否则前端（尤其已升级到
         * 新接口的前端）会对一个不存在的调用一直等到超时，而不是立刻拿到
         * “该方法不存在”的明确错误。 */
        if (!reply && dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
            reply = rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                              "Unknown interface or method");
        }

        if (reply) {
            rpc_send(reply);
        }

        dbus_message_unref(message);
    }

    sync_player_state();
    rpc_lyrics_sync();
    rpc_info_sync();
    rpc_control_tick();

    {
        RpcPlaybackSnapshot progress_snapshot;
        rpc_capture_snapshot(&progress_snapshot);
        rpc_info_emit_progress(&progress_snapshot);
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

int media_session_lost(void) {
    return 0;
}

const char *media_session_bus_name(void) {
    return "";
}

#endif
