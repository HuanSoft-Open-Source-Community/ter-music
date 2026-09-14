/**
 * @file rpc_library.c
 * @brief org.yxzl.ter_music.Library / .Favorites / .History / .DirHistory
 *
 * 曲库浏览的分页查询由 library/library_query.c 提供（SQL LIMIT/OFFSET），
 * 这里只做“参数 → 查询 → JSON”的翻译。扫描走既有异步扫描线程
 * （library_scan_directory_async / library_scan_all_roots），因此
 * Library.Rescan 不阻塞媒体循环；进度由 Library.Status 查询。
 *
 * 收藏/历史/目录历史共用 LibraryChanged 信号（reason 区分），避免为每个
 * 子接口各造一个信号。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "library/library.h"
#include "logger/logger.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"

void rpc_library_emit_changed(const char *reason)
{
    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  RPC_IFACE_LIBRARY,
                                                  "LibraryChanged");
    if (!signal) {
        return;
    }
    const char *value = (reason && reason[0]) ? reason : "updated";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    rpc_send(signal);
}

/* 视图名 → 查询类型；未知返回 -1 */
static int rpc_library_kind_from_id(const char *id)
{
    if (!id) return -1;
    if (strcmp(id, "artists") == 0) return LIBRARY_QUERY_ARTISTS;
    if (strcmp(id, "albums") == 0)  return LIBRARY_QUERY_ALBUMS;
    if (strcmp(id, "genres") == 0)  return LIBRARY_QUERY_GENRES;
    if (strcmp(id, "tracks") == 0)  return LIBRARY_QUERY_TRACKS;
    if (strcmp(id, "search") == 0)  return LIBRARY_QUERY_SEARCH;
    return -1;
}

/* 解析 filter JSON（可为空串/NULL）：{"artist":…,"album":…,"genre":…,"query":…} */
static void rpc_library_parse_filter(const char *filter_json, LibraryQuery *query)
{
    memset(query, 0, sizeof(*query));
    if (!filter_json || filter_json[0] == '\0') {
        return;
    }
    JsonReader reader;
    json_reader_init(&reader, filter_json, strlen(filter_json));
    json_get_string(&reader, "artist", query->artist, sizeof(query->artist));
    json_get_string(&reader, "album", query->album, sizeof(query->album));
    json_get_string(&reader, "genre", query->genre, sizeof(query->genre));
    json_get_string(&reader, "query", query->query, sizeof(query->query));
}

static DBusMessage *rpc_library_reply_page(DBusMessage *message,
                                           const LibraryQuery *query,
                                           int offset, int count)
{
    LibraryRow *rows = calloc((size_t)count, sizeof(LibraryRow));
    if (!rows) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    int written = library_query_page(query, offset, count, rows, count);
    if (written < 0) {
        free(rows);
        return rpc_error(message, RPC_ERROR_FAILED, "library query failed");
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
    pos = json_append_int(json, capacity, pos, library_query_count(query));
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "offset");
    pos = json_append_int(json, capacity, pos, offset);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "count");
    pos = json_append_int(json, capacity, pos, written);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "rows");
    pos = json_append_char(json, capacity, pos, '[');

    for (int i = 0; i < written; i++) {
        const LibraryRow *row = &rows[i];
        if (i > 0) pos = json_append_char(json, capacity, pos, ',');
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "name");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->name[0] ? row->name : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "artist");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->artist[0] ? row->artist : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "album");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->album[0] ? row->album : NULL);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "track_count");
        pos = json_append_int(json, capacity, pos, row->track_count);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "rowid");
        if (row->rowid > 0) {
            pos = json_append_int(json, capacity, pos, row->rowid);
        } else {
            pos = json_append_raw(json, capacity, pos, "null");
        }
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "path");
        pos = json_append_string_or_null(json, capacity, pos,
                                         row->path[0] ? row->path : NULL);
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

/* ── Library ─────────────────────────────────────────────────────── */

static DBusMessage *rpc_library_handle(DBusMessage *message, const char *member)
{
    if (strcmp(member, "Rescan") == 0) {
        const char *path = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &path,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!library_is_available()) {
            return rpc_error(message, RPC_ERROR_FAILED, "library database is not available");
        }
        if (library_scan_in_progress()) {
            return rpc_error(message, RPC_ERROR_BUSY, "a scan is already running");
        }

        if (!path || !path[0]) {
            /* 全根扫描（library_scan_all_roots）是同步阻塞调用，不能放在
             * 媒体循环里；这里要求调用方给出明确的扫描根。 */
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "path must name a scan root; scanning every root is not exposed");
        }

        int started = library_scan_directory_async(path);
        if (started != 0) {
            return rpc_error(message, RPC_ERROR_FAILED, "failed to start the scan");
        }
        rpc_library_emit_changed("scan_started");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Status") == 0) {
        char json[1024];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "available");
        pos = json_append_bool(json, sizeof(json), pos, library_is_available());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "tracks");
        pos = json_append_int(json, sizeof(json), pos, library_get_track_count());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "scanning");
        pos = json_append_bool(json, sizeof(json), pos, library_scan_in_progress());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "progress");
        pos = json_append_int(json, sizeof(json), pos, library_scan_progress());
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "total");
        pos = json_append_int(json, sizeof(json), pos, library_scan_total());
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        return rpc_reply_string(message, json);
    }

    if (strcmp(member, "GetTree") == 0 || strcmp(member, "GetPage") == 0) {
        const char *kind_id = NULL;
        const char *filter_json = NULL;
        dbus_int32_t offset = 0;
        dbus_int32_t count = RPC_PAGE_DEFAULT;
        DBusError error;
        dbus_error_init(&error);

        if (strcmp(member, "GetTree") == 0) {
            if (!dbus_message_get_args(message, &error,
                                       DBUS_TYPE_STRING, &kind_id,
                                       DBUS_TYPE_STRING, &filter_json,
                                       DBUS_TYPE_INVALID)) {
                DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
                dbus_error_free(&error);
                return reply;
            }
        } else if (!dbus_message_get_args(message, &error,
                                          DBUS_TYPE_STRING, &kind_id,
                                          DBUS_TYPE_STRING, &filter_json,
                                          DBUS_TYPE_INT32, &offset,
                                          DBUS_TYPE_INT32, &count,
                                          DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int kind = rpc_library_kind_from_id(kind_id);
        if (kind < 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "kind must be artists|albums|genres|tracks|search");
        }

        LibraryQuery query;
        rpc_library_parse_filter(filter_json, &query);
        query.kind = kind;

        if (!library_is_available()) {
            /* 曲库不可用时返回空结果而不是错误：前端可照常渲染空视图 */
            return rpc_library_reply_page(message, &query, 0, 1);
        }

        if (strcmp(member, "GetTree") == 0) {
            char json[1024];
            size_t pos = 0;
            pos = json_append_char(json, sizeof(json), pos, '{');
            pos = json_append_key(json, sizeof(json), pos, "kind");
            pos = json_append_escaped(json, sizeof(json), pos, kind_id);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "item_count");
            pos = json_append_int(json, sizeof(json), pos, library_query_count(&query));
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "available");
            pos = json_append_bool(json, sizeof(json), pos, library_is_available());
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "filter");
            pos = json_append_raw(json, sizeof(json), pos,
                                  (filter_json && filter_json[0]) ? filter_json : "{}");
            pos = json_append_char(json, sizeof(json), pos, '}');
            json[pos] = '\0';
            return rpc_reply_string(message, json);
        }

        int clamped_offset = 0;
        int clamped_count = 0;
        if (rpc_page_clamp(offset, count, &clamped_offset, &clamped_count) != 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }
        return rpc_library_reply_page(message, &query, clamped_offset, clamped_count);
    }

    if (strcmp(member, "Search") == 0) {
        /* 搜索即 kind=search 的 GetTree（前端随后用 GetPage 取结果） */
        const char *filter_json = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &filter_json,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        LibraryQuery query;
        rpc_library_parse_filter(filter_json, &query);
        query.kind = LIBRARY_QUERY_SEARCH;

        char json[512];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "item_count");
        pos = json_append_int(json, sizeof(json), pos, library_query_count(&query));
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        rpc_library_emit_changed("updated");
        return rpc_reply_string(message, json);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown library method");
}

/* ── Favorites / History / DirHistory ────────────────────────────── */

static DBusMessage *rpc_favorites_handle(DBusMessage *message, const char *member)
{
    if (strcmp(member, "Add") == 0 || strcmp(member, "Remove") == 0 ||
        strcmp(member, "Has") == 0) {
        const char *path = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &path,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!path || !path[0]) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "path must not be empty");
        }

        if (strcmp(member, "Add") == 0) {
            int rc = library_favorites_add(path);
            rpc_library_emit_changed("favorites");
            return rpc_reply_bool(message, rc == 0);
        }
        if (strcmp(member, "Remove") == 0) {
            int rc = library_favorites_remove(path);
            rpc_library_emit_changed("favorites");
            return rpc_reply_bool(message, rc == 0);
        }
        return rpc_reply_bool(message, library_favorites_has(path) == 1);
    }

    if (strcmp(member, "List") == 0) {
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

        int total = library_favorites_get_count();
        if (offset < 0) offset = 0;
        if (count <= 0 || count > RPC_PAGE_MAX) {
            if (count > RPC_PAGE_MAX) {
                return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
            }
            count = RPC_PAGE_DEFAULT;
        }

        Track *tracks = calloc(MAX_FAVORITES_COUNT, sizeof(Track));
        if (!tracks) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        /* 既有接口一次最多取 MAX_FAVORITES_COUNT（上限即容量），
         * 因此这里取全量后切片；曲目结构较小，不构成瓶颈 */
        int loaded = library_favorites_get_all(tracks, MAX_FAVORITES_COUNT);

        char *json = malloc(RPC_PAYLOAD_MAX / 4);
        if (!json) {
            free(tracks);
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        size_t capacity = RPC_PAYLOAD_MAX / 4;
        size_t pos = 0;
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "total");
        pos = json_append_int(json, capacity, pos, total);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "offset");
        pos = json_append_int(json, capacity, pos, offset);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "rows");
        pos = json_append_char(json, capacity, pos, '[');

        int written = 0;
        for (int i = offset; i < loaded && written < count; i++) {
            if (written > 0) pos = json_append_char(json, capacity, pos, ',');
            pos = json_append_char(json, capacity, pos, '{');
            pos = json_append_key(json, capacity, pos, "path");
            pos = json_append_escaped(json, capacity, pos, tracks[i].path);
            pos = json_append_raw(json, capacity, pos, ",");
            pos = json_append_key(json, capacity, pos, "title");
            pos = json_append_string_or_null(json, capacity, pos,
                                             tracks[i].title[0] ? tracks[i].title : NULL);
            pos = json_append_raw(json, capacity, pos, ",");
            pos = json_append_key(json, capacity, pos, "artist");
            pos = json_append_string_or_null(json, capacity, pos,
                                             tracks[i].artist[0] ? tracks[i].artist : NULL);
            pos = json_append_raw(json, capacity, pos, ",");
            pos = json_append_key(json, capacity, pos, "album");
            pos = json_append_string_or_null(json, capacity, pos,
                                             tracks[i].album[0] ? tracks[i].album : NULL);
            pos = json_append_char(json, capacity, pos, '}');
            written++;
        }

        pos = json_append_char(json, capacity, pos, ']');
        pos = json_append_char(json, capacity, pos, '}');
        json[pos] = '\0';
        free(tracks);

        DBusMessage *reply = rpc_reply_string(message, json);
        free(json);
        return reply;
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown favorites method");
}

static DBusMessage *rpc_history_handle(DBusMessage *message, const char *member)
{
    if (strcmp(member, "Add") == 0) {
        const char *path = NULL;
        dbus_int32_t position = 0;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &path,
                                   DBUS_TYPE_INT32, &position,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!path || !path[0]) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "path must not be empty");
        }
        library_history_add(path, position);
        rpc_library_emit_changed("history");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Clear") == 0) {
        library_history_clear();
        rpc_library_emit_changed("history");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "List") == 0) {
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

        if (count <= 0) count = RPC_PAGE_DEFAULT;
        if (count > RPC_PAGE_MAX) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }

        HistoryEntry *entries = calloc(MAX_HISTORY_COUNT, sizeof(HistoryEntry));
        if (!entries) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        int loaded = library_history_get_all(entries, MAX_HISTORY_COUNT);

        char json[16384];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "total");
        pos = json_append_int(json, sizeof(json), pos, loaded);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "offset");
        pos = json_append_int(json, sizeof(json), pos, offset);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "rows");
        pos = json_append_char(json, sizeof(json), pos, '[');

        int written = 0;
        if (offset < 0) offset = 0;
        for (int i = offset; i < loaded && written < count; i++) {
            if (written > 0) pos = json_append_char(json, sizeof(json), pos, ',');
            pos = json_append_char(json, sizeof(json), pos, '{');
            pos = json_append_key(json, sizeof(json), pos, "path");
            pos = json_append_escaped(json, sizeof(json), pos, entries[i].path);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "title");
            pos = json_append_string_or_null(json, sizeof(json), pos,
                                             entries[i].title[0] ? entries[i].title : NULL);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "artist");
            pos = json_append_string_or_null(json, sizeof(json), pos,
                                             entries[i].artist[0] ? entries[i].artist : NULL);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "play_time");
            pos = json_append_int(json, sizeof(json), pos, (long long)entries[i].play_time);
            pos = json_append_char(json, sizeof(json), pos, '}');
            written++;
        }
        pos = json_append_char(json, sizeof(json), pos, ']');
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        free(entries);
        return rpc_reply_string(message, json);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown history method");
}

static DBusMessage *rpc_dirhistory_handle(DBusMessage *message, const char *member)
{
    if (strcmp(member, "Add") == 0 || strcmp(member, "Remove") == 0) {
        const char *path = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &path,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!path || !path[0]) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "path must not be empty");
        }

        int rc = (strcmp(member, "Add") == 0) ? library_dir_history_add(path)
                                              : library_dir_history_remove(path);
        rpc_library_emit_changed("dir_history");
        return rpc_reply_bool(message, rc == 0);
    }

    if (strcmp(member, "Clear") == 0) {
        library_dir_history_clear();
        rpc_library_emit_changed("dir_history");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "List") == 0) {
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

        if (count <= 0) count = RPC_PAGE_DEFAULT;
        if (count > RPC_PAGE_MAX) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }

        DirHistoryEntry *entries = calloc(MAX_DIR_HISTORY_COUNT, sizeof(DirHistoryEntry));
        if (!entries) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        int loaded = library_dir_history_get_all(entries, MAX_DIR_HISTORY_COUNT);

        char json[16384];
        size_t pos = 0;
        pos = json_append_char(json, sizeof(json), pos, '{');
        pos = json_append_key(json, sizeof(json), pos, "total");
        pos = json_append_int(json, sizeof(json), pos, loaded);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "offset");
        pos = json_append_int(json, sizeof(json), pos, offset);
        pos = json_append_raw(json, sizeof(json), pos, ",");
        pos = json_append_key(json, sizeof(json), pos, "rows");
        pos = json_append_char(json, sizeof(json), pos, '[');

        int written = 0;
        if (offset < 0) offset = 0;
        for (int i = offset; i < loaded && written < count; i++) {
            if (written > 0) pos = json_append_char(json, sizeof(json), pos, ',');
            pos = json_append_char(json, sizeof(json), pos, '{');
            pos = json_append_key(json, sizeof(json), pos, "path");
            pos = json_append_escaped(json, sizeof(json), pos, entries[i].path);
            pos = json_append_raw(json, sizeof(json), pos, ",");
            pos = json_append_key(json, sizeof(json), pos, "open_time");
            pos = json_append_int(json, sizeof(json), pos, (long long)entries[i].open_time);
            pos = json_append_char(json, sizeof(json), pos, '}');
            written++;
        }
        pos = json_append_char(json, sizeof(json), pos, ']');
        pos = json_append_char(json, sizeof(json), pos, '}');
        json[pos] = '\0';
        free(entries);
        return rpc_reply_string(message, json);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown dir history method");
}

/* ── 统一分发 ───────────────────────────────────────────────────── */

DBusMessage *rpc_library_handle_all(DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);
    const char *iface = dbus_message_get_interface(message);
    if (!member || !iface) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(iface, RPC_IFACE_LIBRARY) == 0) {
        return rpc_library_handle(message, member);
    }
    if (strcmp(iface, RPC_IFACE_FAVORITES) == 0) {
        return rpc_favorites_handle(message, member);
    }
    if (strcmp(iface, RPC_IFACE_HISTORY) == 0) {
        return rpc_history_handle(message, member);
    }
    if (strcmp(iface, RPC_IFACE_DIRHISTORY) == 0) {
        return rpc_dirhistory_handle(message, member);
    }
    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown library family method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_library_introspection =
    "  <interface name=\"org.yxzl.ter_music.Library\">\n"
    "    <method name=\"Rescan\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Status\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"GetTree\">\n"
    "      <arg name=\"kind\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"filter\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetPage\">\n"
    "      <arg name=\"kind\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"filter\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Search\">\n"
    "      <arg name=\"filter\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"LibraryChanged\">\n"
    "      <arg name=\"reason\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.yxzl.ter_music.Favorites\">\n"
    "    <method name=\"Add\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Remove\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Has\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"List\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.yxzl.ter_music.History\">\n"
    "    <method name=\"Add\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"List\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Clear\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.yxzl.ter_music.DirHistory\">\n"
    "    <method name=\"Add\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Remove\">\n"
    "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"List\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Clear\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "  </interface>\n";

const char *rpc_library_introspection(void)
{
    return k_library_introspection;
}

#endif /* HAVE_DBUS */
