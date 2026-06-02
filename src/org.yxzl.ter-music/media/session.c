#include "types.h"
#include <ncursesw/ncurses.h>
#include "audio/audio.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
#include "media/session.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

typedef struct {
    int valid;
    int current_index;
    int playlist_total;
    PlayState play_state;
    LoopMode loop_mode;
    int volume_percent;
    int can_seek;
    int64_t position_us;
    int64_t length_us;
    char track_id[96];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
} MediaSessionSnapshot;

typedef struct {
    DBusConnection *connection;
    int active;
    char bus_name[128];
    MediaSessionSnapshot last_snapshot;
} MediaSessionState;

static MediaSessionState g_media_session = {0};

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

static const char *loop_mode_to_mpris(LoopMode mode) {
    switch (mode) {
        case LOOP_SINGLE:
            return "Track";
        case LOOP_LIST:
        case LOOP_RANDOM:
            return "Playlist";
        case LOOP_OFF:
        default:
            return "None";
    }
}

static int shuffle_enabled_for_loop_mode(LoopMode mode) {
    return mode == LOOP_RANDOM;
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

static void build_track_id(char *dest, size_t dest_size, const char *track_path) {
    unsigned long long hash = 1469598103934665603ULL;
    const unsigned char *ptr = (const unsigned char *)(track_path ? track_path : "");

    while (*ptr != '\0') {
        hash ^= (unsigned long long)(*ptr++);
        hash *= 1099511628211ULL;
    }

    snprintf(dest, dest_size, "/org/mpris/MediaPlayer2/Track_%016llx", hash);
}

static void capture_snapshot(MediaSessionSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->current_index = g_current_play_index;
    snapshot->playlist_total = playlist_count();
    snapshot->play_state = g_play_state;
    snapshot->loop_mode = g_loop_mode;
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
    build_track_id(snapshot->track_id, sizeof(snapshot->track_id), track_path);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s", track.title);
    snprintf(snapshot->artist, sizeof(snapshot->artist), "%s", track.artist);
    snprintf(snapshot->album, sizeof(snapshot->album), "%s", track.album);
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
           strcmp(lhs->album, rhs->album) == 0;
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
        const char *value = loop_mode_to_mpris(current->loop_mode);
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);
    } else if (strcmp(property_name, "Rate") == 0 ||
               strcmp(property_name, "MinimumRate") == 0 ||
               strcmp(property_name, "MaximumRate") == 0) {
        double value = 1.0;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &value);
    } else if (strcmp(property_name, "Shuffle") == 0) {
        dbus_bool_t value = shuffle_enabled_for_loop_mode(current->loop_mode);
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
    const char *loop_status = loop_mode_to_mpris(snapshot->loop_mode);
    dbus_bool_t can_navigate = snapshot->playlist_total > 0;
    dbus_bool_t can_pause = snapshot->play_state != PLAY_STATE_STOPPED;
    double volume = (double)snapshot->volume_percent / 100.0;

    append_string_variant(dict_iter, "PlaybackStatus", playback_status);
    append_string_variant(dict_iter, "LoopStatus", loop_status);
    append_boolean_variant(dict_iter, "Shuffle", shuffle_enabled_for_loop_mode(snapshot->loop_mode));
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
        g_loop_mode = LOOP_SINGLE;
    } else if (strcmp(value, "Playlist") == 0) {
        if (g_loop_mode != LOOP_RANDOM) {
            g_loop_mode = LOOP_LIST;
        }
    } else {
        g_loop_mode = LOOP_OFF;
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
        set_volume_percent((int)lrint(volume * 100.0));
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
            g_loop_mode = LOOP_RANDOM;
        } else if (g_loop_mode == LOOP_RANDOM) {
            g_loop_mode = LOOP_LIST;
        }
    } else {
        return media_session_error(message, DBUS_ERROR_PROPERTY_READ_ONLY, "Property is read only");
    }

    return dbus_message_new_method_return(message);
}

static void play_selected_or_current_track(void) {
    int playlist_total = playlist_count();

    if (!playlist_is_loaded() || playlist_total <= 0) {
        return;
    }

    int target_index = (g_current_play_index >= 0)
        ? g_current_play_index
        : (g_sort_state.active ? g_sort_state.sorted_indices[g_selected_index] : g_selected_index);
    if (target_index >= 0 && target_index < playlist_total) {
        play_audio(target_index);
    }
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
        if (g_play_state == PLAY_STATE_PLAYING) {
            pause_audio();
        } else if (g_play_state == PLAY_STATE_PAUSED) {
            resume_audio();
        } else {
            play_selected_or_current_track();
        }
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Stop") == 0) {
        stop_audio();
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "Play") == 0) {
        if (g_play_state == PLAY_STATE_PAUSED) {
            resume_audio();
        } else if (g_play_state != PLAY_STATE_PLAYING) {
            play_selected_or_current_track();
        }
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
            int64_t target_us = snapshot->position_us + delta_us;
            if (target_us < 0) {
                target_us = 0;
            }
            if (snapshot->length_us > 0 && target_us > snapshot->length_us) {
                target_us = snapshot->length_us;
            }
            seek_audio((double)target_us / 1000000.0);
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
            if (position_us < 0) {
                position_us = 0;
            }
            if (snapshot->length_us > 0 && position_us > snapshot->length_us) {
                position_us = snapshot->length_us;
            }
            seek_audio((double)position_us / 1000000.0);
        }
        return dbus_message_new_method_return(message);
    }
    if (strcmp(member, "OpenUri") == 0) {
        return media_session_error(message, DBUS_ERROR_NOT_SUPPORTED, "OpenUri is not supported");
    }

    return media_session_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown player method");
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

static int request_bus_name(char *dest, size_t dest_size) {
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

    if (!request_bus_name(g_media_session.bus_name, sizeof(g_media_session.bus_name))) {
        log_warn("media_session", "Failed to acquire D-Bus bus name");
        dbus_connection_unref(g_media_session.connection);
        memset(&g_media_session, 0, sizeof(g_media_session));
        return;
    }

    g_media_session.active = 1;
    log_info("media_session", "D-Bus initialized, bus='%s'", g_media_session.bus_name);
    emit_properties_changed(MPRIS_ROOT_INTERFACE, NULL);
    sync_player_state();
}

void media_session_shutdown(void) {
    log_info("media_session", "Shutting down media session");
    if (!g_media_session.connection) {
        memset(&g_media_session, 0, sizeof(g_media_session));
        return;
    }

    if (g_media_session.active && g_media_session.bus_name[0] != '\0') {
        log_debug("media_session", "Releasing D-Bus name: '%s'", g_media_session.bus_name);
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_release_name(g_media_session.connection, g_media_session.bus_name, &error);
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
        }
    }

    dbus_connection_unref(g_media_session.connection);
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
        } else if (dbus_message_has_interface(message, MPRIS_ROOT_INTERFACE)) {
            reply = handle_root_method(message);
        } else if (dbus_message_has_interface(message, MPRIS_PLAYER_INTERFACE)) {
            reply = handle_player_method(message, &snapshot);
        }

        if (reply) {
            media_session_send(reply);
        }

        dbus_message_unref(message);
    }

    sync_player_state();
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

#endif
