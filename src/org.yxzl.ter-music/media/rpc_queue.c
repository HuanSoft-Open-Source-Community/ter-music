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

#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "core/core.h"
#include "logger/logger.h"
#include "queue/backend_queue.h"
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
    dbus_int32_t count = (dbus_int32_t)bq_count();
    dbus_int32_t position = (dbus_int32_t)bq_position();

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

static DBusMessage *rpc_queue_reply_page(DBusMessage *message, int offset, int count)
{
    /* 单页 JSON 有界：行数 ≤ RPC_PAGE_MAX；渲染由后端队列模块完成
     * （revision/count/current_position/rows 一次成型，避免两处口径漂移）。 */
    size_t capacity = RPC_PAYLOAD_MAX / 2;
    char *json = malloc(capacity);
    if (!json) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    bq_render_get(json, capacity, offset, count);

    DBusMessage *reply = rpc_reply_string(message, json);
    free(json);
    return reply;
}

/* 队列编辑的公共收尾：重建队列派生状态并发信号 */
static void rpc_queue_after_edit(void)
{
    core_notify_state_changed();
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

        /* 分页契约：count 超过 RPC_PAGE_MAX 一律拒绝（不静默截断），
         * 越界 offset 返回空行集——与 Playlist/Library 分页同一口径。 */
        if (count < 0 || count > RPC_PAGE_MAX) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }
        int clamped_offset = offset < 0 ? 0 : offset;
        return rpc_queue_reply_page(message, clamped_offset, count);
    }

    if (strcmp(member, "Set") == 0 || strcmp(member, "Append") == 0 ||
        strcmp(member, "InsertAfter") == 0) {
        const char *payload = NULL;
        DBusError error;
        dbus_error_init(&error);
        dbus_int32_t position = -1;

        /* Set/Append：单个 JSON 载荷；InsertAfter：位置 + JSON。
         * Append 也接受 (i position, s json) 形式以外的单参调用。 */
        if (strcmp(member, "InsertAfter") == 0) {
            if (!dbus_message_get_args(message, &error,
                                       DBUS_TYPE_INT32, &position,
                                       DBUS_TYPE_STRING, &payload,
                                       DBUS_TYPE_INVALID)) {
                dbus_error_free(&error);
                dbus_error_init(&error);
                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &payload,
                                           DBUS_TYPE_INVALID)) {
                    DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
                    dbus_error_free(&error);
                    return reply;
                }
                position = play_queue_position();
            }
        } else if (!dbus_message_get_args(message, &error,
                                          DBUS_TYPE_STRING, &payload,
                                          DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int written;
        if (strcmp(member, "Set") == 0) {
            written = bq_set_json(payload);
        } else if (strcmp(member, "Append") == 0) {
            written = bq_append_json(payload);
        } else {
            written = bq_insert_after_json(position, payload);
        }

        if (written < 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "queue payload rejected (malformed JSON, over the per-call limit, or a non-local path)");
        }
        rpc_queue_after_edit();
        dbus_int32_t result = (dbus_int32_t)written;
        return rpc_reply_int(message, result);
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

        if (position < 0 || position >= bq_count()) {
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
        /* 锚点＝后端游标所在条目：后端不认识内容列表下标 */
        BackendQueueEntry current;
        const char *anchor = (bq_current(&current) == 0) ? current.path : NULL;
        play_queue_rebuild(&g_play_queue, (PlayMode)mode, anchor);
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Shuffle") == 0) {
        /* 当前条目置首、其余打乱（后端队列的洗牌语义） */
        bq_shuffle_rest();
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

        if (position < 0 || position >= bq_count()) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "position is out of range");
        }

        bq_play_at(position);
        play_audio(position);
        rpc_queue_after_edit();
        return rpc_reply_bool(message, 1);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown queue method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_queue_introspection =
    "  <interface name=\"org.yxzl.ter_music.Queue\">\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"written\" type=\"i\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Append\">\n"
    "      <arg name=\"json\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"written\" type=\"i\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"InsertAfter\">\n"
    "      <arg name=\"position\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"written\" type=\"i\" direction=\"out\"/>\n"
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
