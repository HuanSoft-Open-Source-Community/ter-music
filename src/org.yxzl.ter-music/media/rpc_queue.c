/**
 * @file rpc_queue.c
 * @brief org.yxzl.ter_music.Queue —— 播放队列读取与编辑
 *
 * 队列写操作全部是 O(MAX_TRACKS) 的有界操作（队列长度上限即曲目上限），
 * 因此都在媒体循环内同步完成，不需要后台任务；每次写操作后广播
 * QueueChanged，前端据此重取一页。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "app/open.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define QUEUE_API_INTERFACE RPC_IFACE_QUEUE

static unsigned long long g_queue_revision = 0;

void rpc_queue_emit_changed(void)
{
    g_queue_revision++;

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  QUEUE_API_INTERFACE,
                                                  "QueueChanged");
    if (!signal) {
        return;
    }

    dbus_uint32_t revision = (dbus_uint32_t)g_queue_revision;
    dbus_int32_t count = (dbus_int32_t)play_queue_count();
    dbus_int32_t position = (dbus_int32_t)play_queue_position();

    dbus_message_append_args(signal,
                             DBUS_TYPE_UINT32, &revision,
                             DBUS_TYPE_INT32, &count,
                             DBUS_TYPE_INT32, &position,
                             DBUS_TYPE_INVALID);
    rpc_send(signal);
}

unsigned long long rpc_queue_revision(void)
{
    return g_queue_revision;
}

/* 取队列位置 position 对应的曲目元数据（越界返回 0） */
static int rpc_queue_row_json(char *out, size_t out_size, size_t pos,
                              int position, int track_index)
{
    Track track;
    memset(&track, 0, sizeof(track));
    if (track_index >= 0) {
        get_track_metadata(track_index, &track);
    }

    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "position");
    pos = json_append_int(out, out_size, pos, position);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "track_index");
    if (track_index >= 0) {
        pos = json_append_int(out, out_size, pos, track_index);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "title");
    pos = json_append_string_or_null(out, out_size, pos,
                                     track.title[0] ? track.title : NULL);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "artist");
    pos = json_append_string_or_null(out, out_size, pos,
                                     track.artist[0] ? track.artist : NULL);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "album");
    pos = json_append_string_or_null(out, out_size, pos,
                                     track.album[0] ? track.album : NULL);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "is_cue");
    pos = json_append_bool(out, out_size, pos,
                           track.cue_offset > 0 && track.cue_track_number > 0);
    return (int)json_append_char(out, out_size, pos, '}');
}

static DBusMessage *rpc_queue_reply_page(DBusMessage *message, int offset, int count)
{
    int total = play_queue_count();
    int current = play_queue_position();

    if (offset >= total) {
        count = 0;
    } else if (offset + count > total) {
        count = total - offset;
    }

    /* 单页 JSON 有界：行数 ≤ RPC_PAGE_MAX */
    size_t capacity = RPC_PAYLOAD_MAX / 2;
    char *json = malloc(capacity);
    if (!json) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    size_t pos = 0;
    pos = json_append_char(json, capacity, pos, '{');
    pos = json_append_key(json, capacity, pos, "revision");
    pos = json_append_int(json, capacity, pos, (long long)g_queue_revision);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "count");
    pos = json_append_int(json, capacity, pos, total);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "current_position");
    if (current >= 0) {
        pos = json_append_int(json, capacity, pos, current);
    } else {
        pos = json_append_raw(json, capacity, pos, "null");
    }
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "offset");
    pos = json_append_int(json, capacity, pos, offset);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "rows");
    pos = json_append_char(json, capacity, pos, '[');

    for (int i = 0; i < count; i++) {
        int position = offset + i;
        int track_index = play_queue_index_at(position);
        if (i > 0) {
            pos = json_append_char(json, capacity, pos, ',');
        }
        rpc_queue_row_json(json, capacity, pos, position, track_index);
        pos = strlen(json);
    }

    pos = json_append_char(json, capacity, pos, ']');
    pos = json_append_char(json, capacity, pos, '}');
    json[pos] = '\0';

    DBusMessage *reply = rpc_reply_string(message, json);
    free(json);
    return reply;
}

/* 队列编辑的公共收尾：重建队列派生状态并发信号 */
static void rpc_queue_after_edit(void)
{
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS);
    rpc_queue_emit_changed();
}

DBusMessage *rpc_queue_handle(DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(member, "Get") == 0) {
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
        return rpc_queue_reply_page(message, clamped_offset, clamped_count);
    }

    if (strcmp(member, "Append") == 0 || strcmp(member, "InsertAfter") == 0) {
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

        if (track_index < 0 || track_index >= playlist_count()) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "track_index is out of range");
        }

        int ok = (strcmp(member, "Append") == 0)
            ? play_queue_append(&g_play_queue, track_index)
            : play_queue_insert_after(&g_play_queue, track_index);
        if (ok == 0) {
            rpc_queue_after_edit();
        }
        return rpc_reply_bool(message, ok == 0);
    }

    if (strcmp(member, "RemoveAt") == 0 || strcmp(member, "MoveUp") == 0 ||
        strcmp(member, "MoveDown") == 0) {
        dbus_int32_t position = -1;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &position,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (position < 0 || position >= play_queue_count()) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "position is out of range");
        }

        int ok;
        if (strcmp(member, "RemoveAt") == 0) {
            ok = play_queue_remove_at(&g_play_queue, position);
        } else if (strcmp(member, "MoveUp") == 0) {
            ok = play_queue_move_up(&g_play_queue, position);
        } else {
            ok = play_queue_move_down(&g_play_queue, position);
        }
        if (ok == 0) {
            rpc_queue_after_edit();
        }
        return rpc_reply_bool(message, ok == 0);
    }

    if (strcmp(member, "Clear") == 0) {
        play_queue_clear(&g_play_queue);
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Rebuild") == 0) {
        dbus_int32_t mode = (dbus_int32_t)g_play_mode;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &mode,
                                   DBUS_TYPE_INVALID)) {
            dbus_error_free(&error);
            mode = (dbus_int32_t)g_play_mode;   /* 可选参数：缺省沿用当前模式 */
        }
        dbus_error_free(&error);

        if (mode < 0 || mode >= PLAY_MODE_COUNT) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "mode is out of range");
        }
        int anchor = (g_current_play_index >= 0) ? g_current_play_index : 0;
        play_queue_rebuild(&g_play_queue, &g_playlist, (PlayMode)mode, anchor);
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Shuffle") == 0) {
        if (play_queue_count() > 1) {
            /* 当前曲目置首，其余洗牌（与重建时的洗牌语义一致） */
            int current = play_queue_position();
            if (current > 0) {
                int tmp = g_play_queue.indices[0];
                g_play_queue.indices[0] = g_play_queue.indices[current];
                g_play_queue.indices[current] = tmp;
                g_play_queue.current_position = 0;
            }
            play_queue_shuffle_range(&g_play_queue, 1, g_play_queue.count - 1);
            g_play_queue.shuffle_generation++;
        }
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "PlayAt") == 0) {
        dbus_int32_t position = -1;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &position,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int track_index = play_queue_index_at(position);
        if (track_index < 0) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "position is out of range");
        }

        play_queue_set_position(position);
        play_audio(track_index);
        app_set_selection_for_track(track_index);
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown queue method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_queue_introspection =
    "  <interface name=\"org.yxzl.ter_music.Queue\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Append\">\n"
    "      <arg name=\"track_index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"InsertAfter\">\n"
    "      <arg name=\"track_index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"RemoveAt\">\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"MoveUp\">\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"MoveDown\">\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Clear\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"Rebuild\">\n"
    "      <arg name=\"mode\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Shuffle\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"PlayAt\">\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"QueueChanged\">\n"
    "      <arg name=\"revision\" type=\"u\"/>\n"
    "      <arg name=\"count\" type=\"i\"/>\n"
    "      <arg name=\"current_position\" type=\"i\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

const char *rpc_queue_introspection(void)
{
    return k_queue_introspection;
}

#endif /* HAVE_DBUS */
