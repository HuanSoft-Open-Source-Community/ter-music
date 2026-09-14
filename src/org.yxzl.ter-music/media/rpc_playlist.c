/**
 * @file rpc_playlist.c
 * @brief org.yxzl.ter_music.Playlist —— 播放列表浏览与编辑
 *
 * 载荷一律是“渲染就绪”的分页（见 playlist_page）：前端拿到即可画表，
 * 不需要逐曲目回查元数据。阻塞的目录扫描走 media/rpc_job.c 的后台任务，
 * 完成后由媒体循环广播 PlaylistChanged。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "app/open.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "core/core.h"
#include "info/info.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "search/search.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define PLAYLIST_API_INTERFACE RPC_IFACE_PLAYLIST

/* ── 核心侧过滤串（Playlist.SetFilter）：空串 = 不过滤 ───────────── */
#define PLAYLIST_FILTER_MAX 256
static char g_playlist_filter[PLAYLIST_FILTER_MAX] = "";

const char *rpc_playlist_filter(void)
{
    return g_playlist_filter;
}

void rpc_playlist_emit_changed(const char *reason)
{
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  PLAYLIST_API_INTERFACE,
                                                  "PlaylistChanged");
    if (!signal) {
        return;
    }
    const char *value = (reason && reason[0]) ? reason : "updated";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    rpc_send(signal);
}

/* 排序模式名（与 config 的 sort_mode 取值一致） */
static int rpc_sort_mode_from_id(const char *id)
{
    if (!id) return -1;
    if (strcmp(id, "default") == 0)  return SORT_DEFAULT;
    if (strcmp(id, "title") == 0)    return SORT_TITLE;
    if (strcmp(id, "artist") == 0)   return SORT_ARTIST;
    if (strcmp(id, "album") == 0)    return SORT_ALBUM;
    if (strcmp(id, "filename") == 0) return SORT_FILENAME;
    return -1;
}

static const char *rpc_sort_mode_id(int mode)
{
    switch (mode) {
        case SORT_TITLE:    return "title";
        case SORT_ARTIST:   return "artist";
        case SORT_ALBUM:    return "album";
        case SORT_FILENAME: return "filename";
        default:            return "default";
    }
}

/* 把一页渲染成 JSON 信封 */
static DBusMessage *rpc_playlist_reply_page(DBusMessage *message,
                                            int offset, int count,
                                            const char *filter)
{
    PlaylistRow *rows = calloc((size_t)count, sizeof(PlaylistRow));
    if (!rows) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    int written = playlist_page(offset, count, filter, rows, count);
    if (written < 0) {
        free(rows);
        return rpc_error(message, RPC_ERROR_INVALID_ARGS, "invalid page request");
    }

    size_t capacity = RPC_PAYLOAD_MAX / 2;
    char *json = malloc(capacity);
    if (!json) {
        free(rows);
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    size_t pos = 0;
    pos = json_append_char(json, capacity, pos, '{');
    pos = json_append_key(json, capacity, pos, "total");
    pos = json_append_int(json, capacity, pos, playlist_page_total(filter));
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "offset");
    pos = json_append_int(json, capacity, pos, offset);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "count");
    pos = json_append_int(json, capacity, pos, written);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "filter");
    pos = json_append_string_or_null(json, capacity, pos,
                                     (filter && filter[0]) ? filter : NULL);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "rows");
    pos = json_append_char(json, capacity, pos, '[');

    for (int i = 0; i < written; i++) {
        const PlaylistRow *row = &rows[i];
        if (i > 0) pos = json_append_char(json, capacity, pos, ',');
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "row");
        pos = json_append_int(json, capacity, pos, row->row);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "type");
        pos = json_append_escaped(json, capacity, pos, row->type == 1 ? "dir" : "track");
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "depth");
        pos = json_append_int(json, capacity, pos, row->depth);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "expanded");
        pos = json_append_bool(json, capacity, pos, row->expanded);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "tree_index");
        pos = json_append_int(json, capacity, pos, row->tree_index);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "track_index");
        if (row->track_index >= 0) {
            pos = json_append_int(json, capacity, pos, row->track_index);
        } else {
            pos = json_append_raw(json, capacity, pos, "null");
        }
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "name");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->name[0] ? row->name : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "title");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->title[0] ? row->title : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "artist");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->artist[0] ? row->artist : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "album");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->album[0] ? row->album : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "is_cue");
        pos = json_append_bool(json, capacity, pos, row->is_cue);
        pos = json_append_char(json, capacity, pos, '}');
    }

    pos = json_append_char(json, capacity, pos, ']');
    pos = json_append_char(json, capacity, pos, '}');
    json[pos] = '\0';

    free(rows);

    DBusMessage *reply;
    if (pos + 1 > RPC_PAYLOAD_MAX) {
        reply = rpc_error(message, RPC_ERROR_TOO_LARGE,
                          "page exceeds the payload limit; request fewer rows");
    } else {
        reply = rpc_reply_string(message, json);
    }
    free(json);
    return reply;
}

static DBusMessage *rpc_playlist_reply_tree(DBusMessage *message)
{
    char folder[MAX_PATH_LEN];
    playlist_copy_folder_path(folder, sizeof(folder));

    char json[2048];
    size_t pos = 0;
    pos = json_append_char(json, sizeof(json), pos, '{');
    pos = json_append_key(json, sizeof(json), pos, "loaded");
    pos = json_append_bool(json, sizeof(json), pos, playlist_is_loaded());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "count");
    pos = json_append_int(json, sizeof(json), pos, playlist_count());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "visible_count");
    pos = json_append_int(json, sizeof(json), pos, playlist_visible_count());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "tree_mode");
    pos = json_append_bool(json, sizeof(json), pos, playlist_tree_is_active());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "folder");
    pos = json_append_string_or_null(json, sizeof(json), pos, folder[0] ? folder : NULL);
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "sort");
    pos = json_append_escaped(json, sizeof(json), pos, rpc_sort_mode_id(g_app_config.sort_mode));
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "filter");
    pos = json_append_string_or_null(json, sizeof(json), pos,
                                     g_playlist_filter[0] ? g_playlist_filter : NULL);
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "loading");
    pos = json_append_bool(json, sizeof(json), pos, rpc_job_state() == RPC_JOB_RUNNING);
    pos = json_append_char(json, sizeof(json), pos, '}');
    json[pos] = '\0';
    return rpc_reply_string(message, json);
}

static DBusMessage *rpc_playlist_reply_status(DBusMessage *message)
{
    char json[1024];
    size_t pos = 0;
    const char *state = "idle";
    if (rpc_job_state() == RPC_JOB_RUNNING) {
        state = "loading";
    } else if (rpc_job_state() == RPC_JOB_FAILED) {
        state = "error";
    }

    pos = json_append_char(json, sizeof(json), pos, '{');
    pos = json_append_key(json, sizeof(json), pos, "state");
    pos = json_append_escaped(json, sizeof(json), pos, state);
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "progress");
    pos = json_append_int(json, sizeof(json), pos, rpc_job_progress());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "total");
    pos = json_append_int(json, sizeof(json), pos, rpc_job_total());
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "path");
    pos = json_append_string_or_null(json, sizeof(json), pos,
                                     rpc_job_path()[0] ? rpc_job_path() : NULL);
    pos = json_append_raw(json, sizeof(json), pos, ",");
    pos = json_append_key(json, sizeof(json), pos, "error");
    pos = json_append_string_or_null(json, sizeof(json), pos,
                                     rpc_job_error()[0] ? rpc_job_error() : NULL);
    pos = json_append_char(json, sizeof(json), pos, '}');
    json[pos] = '\0';
    return rpc_reply_string(message, json);
}

DBusMessage *rpc_playlist_handle(DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(member, "Load") == 0 || strcmp(member, "Append") == 0) {
        const char *path = NULL;
        dbus_bool_t append = FALSE;
        dbus_bool_t autoplay = FALSE;
        DBusError error;
        dbus_error_init(&error);

        if (strcmp(member, "Append") == 0) {
            if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &path,
                                       DBUS_TYPE_INVALID)) {
                DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
                dbus_error_free(&error);
                return reply;
            }
            append = TRUE;
        } else if (!dbus_message_get_args(message, &error,
                                          DBUS_TYPE_STRING, &path,
                                          DBUS_TYPE_BOOLEAN, &append,
                                          DBUS_TYPE_BOOLEAN, &autoplay,
                                          DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        RpcJobKind kind = append ? RPC_JOB_PLAYLIST_APPEND : RPC_JOB_PLAYLIST_LOAD;
        if (rpc_job_start(kind, path, NULL, autoplay ? 1 : 0) != 0) {
            return rpc_error(message, RPC_ERROR_BUSY,
                             "another playlist operation is already running");
        }
        rpc_playlist_emit_changed("loading");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Clear") == 0) {
        playlist_install(calloc(1, sizeof(Playlist)));
        g_playlist_filter[0] = '\0';
        rpc_playlist_emit_changed("loaded");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Sort") == 0) {
        const char *mode_id = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &mode_id,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int mode = rpc_sort_mode_from_id(mode_id);
        if (mode < 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "mode must be default|title|artist|album|filename");
        }
        g_app_config.sort_mode = mode;
        save_config();
        recompute_sort_order();
        rpc_playlist_emit_changed("sorted");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "SetFilter") == 0) {
        const char *query = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &query,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        snprintf(g_playlist_filter, sizeof(g_playlist_filter), "%s", query ? query : "");
        rpc_playlist_emit_changed("filtered");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Search") == 0) {
        const char *query = NULL;
        dbus_int32_t offset = 0;
        dbus_int32_t count = RPC_PAGE_DEFAULT;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &query,
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
        /* 只读搜索：不改变当前过滤状态（有界：曲目数上限 MAX_TRACKS） */
        return rpc_playlist_reply_page(message, clamped_offset, clamped_count, query);
    }

    if (strcmp(member, "GetTree") == 0) {
        return rpc_playlist_reply_tree(message);
    }

    if (strcmp(member, "GetPage") == 0) {
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
        return rpc_playlist_reply_page(message, clamped_offset, clamped_count,
                                       g_playlist_filter);
    }

    if (strcmp(member, "ToggleExpand") == 0) {
        dbus_int32_t tree_index = -1;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &tree_index,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (tree_index < 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "tree_index must be >= 0");
        }
        playlist_toggle_directory_expand(tree_index);
        rpc_playlist_emit_changed("expanded");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "RevealIndex") == 0) {
        dbus_int32_t track_index = -1;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &track_index,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int visible = playlist_reveal_track(track_index);
        if (visible < 0) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "track_index is out of range");
        }
        rpc_playlist_emit_changed("expanded");

        DBusMessage *reply = dbus_message_new_method_return(message);
        if (!reply) {
            return NULL;
        }
        dbus_int32_t row = (dbus_int32_t)visible;
        dbus_message_append_args(reply, DBUS_TYPE_INT32, &row, DBUS_TYPE_INVALID);
        return reply;
    }

    if (strcmp(member, "Status") == 0) {
        return rpc_playlist_reply_status(message);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown playlist method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_playlist_introspection =
    "  <interface name=\"org.yxzl.ter_music.Playlist\">\n"
    "    <method name=\"Load\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"append\" type=\"b\" direction=\"in\"/>\n"
    "      <arg name=\"autoplay\" type=\"b\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Append\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Clear\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Sort\">\n"
    "      <arg name=\"mode\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"SetFilter\">\n"
    "      <arg name=\"query\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Search\">\n"
    "      <arg name=\"query\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetTree\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"GetPage\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"ToggleExpand\">\n"
    "      <arg name=\"tree_index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"RevealIndex\">\n"
    "      <arg name=\"track_index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"row\" type=\"i\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Status\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <signal name=\"PlaylistChanged\">\n"
    "      <arg name=\"reason\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_playlist_introspection(void)
{
    return k_playlist_introspection;
}

#endif /* HAVE_DBUS */
