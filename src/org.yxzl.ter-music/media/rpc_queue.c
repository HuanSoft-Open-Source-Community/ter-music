/**
 * @file rpc_queue.c
 * @brief org.yxzl.ter_music.Queue —— 播放队列读取与编辑
 *
 * 队列写操作全部是有界的内存操作（单发条目数上限 BQ_SET_MAX，超限整体拒绝），
 * 因此都在媒体循环内同步完成，不需要后台任务。
 *
 * QueueChanged 广播的两个来源（详见 media/rpc.h）：
 *  - 本文件的 D-Bus 写方法：编辑完成后 rpc_queue_after_edit() 立即广播；
 *  - 核心内部的游标/顺序变化（播放线程续播、随机重排、播放模式重建）：由
 *    media_session_tick() 每轮调用 rpc_queue_sync()，比对后端队列修订号后补发。
 * 修订号同源（都用 bq_revision()），因此信号里的 revision 与 Queue.Get 的
 * revision 可以直接对照，前端不必维护两套序号。
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

/* 最近一次广播出去的后端队列修订号（与 Queue.Get 的 revision 同源） */
static unsigned long long g_queue_broadcast_revision = 0;

void rpc_queue_emit_changed(void)
{
    unsigned long long revision = bq_revision();
    g_queue_broadcast_revision = revision;

    DBusMessage *signal = dbus_message_new_signal(MPRIS_OBJECT_PATH,
                                                  QUEUE_API_INTERFACE,
                                                  "QueueChanged");
    if (!signal) {
        return;
    }

    dbus_uint32_t revision_arg = (dbus_uint32_t)revision;
    dbus_int32_t count = (dbus_int32_t)bq_count();
    dbus_int32_t position = (dbus_int32_t)bq_position();

    dbus_message_append_args(signal,
                             DBUS_TYPE_UINT32, &revision_arg,
                             DBUS_TYPE_INT32, &count,
                             DBUS_TYPE_INT32, &position,
                             DBUS_TYPE_INVALID);
    rpc_send(signal);
}

/* 媒体循环每轮调用（session.c 的 media_session_tick）：核心内部改动队列
 * （播放线程续播、随机重排、播放模式重建）不经过本文件的写方法，故用
 * “比较修订号后补发”把广播补齐，模式与 rpc_info_sync() 一致。 */
void rpc_queue_sync(void)
{
    if (!rpc_session_active()) {
        return;
    }
    if (bq_revision() == g_queue_broadcast_revision) {
        return;
    }
    rpc_queue_emit_changed();
}

unsigned long long rpc_queue_revision(void)
{
    return bq_revision();
}

static DBusMessage *rpc_queue_reply_page(DBusMessage *message, int offset, int count)
{
    /* 单页 JSON 有界：行数 ≤ RPC_PAGE_MAX；渲染由后端队列模块完成
     * （revision/count/current_position/rows 一次成型，避免两处口径漂移）。
     * 缓冲区写满时追加器会静默丢弃，因此渲染后必须查溢出：绝不回半截 JSON。 */
    size_t capacity = RPC_PAYLOAD_MAX / 2;
    char *json = malloc(capacity);
    if (!json) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    size_t pos = bq_render_get(json, capacity, offset, count);
    DBusMessage *reply;
    if (json_writer_overflowed(capacity, pos)) {
        reply = rpc_error(message, RPC_ERROR_TOO_LARGE,
                          "queue page exceeds the payload limit; request fewer rows");
    } else {
        reply = rpc_reply_string(message, json);
    }
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

        /* 分页契约与其它接口同一口径：count==0 走默认页（RPC_PAGE_DEFAULT），
         * 显式超过 RPC_PAGE_MAX 一律拒绝（不静默截断），越界 offset 返回空页。 */
        int page_offset = 0;
        int page_count = 0;
        if (rpc_page_clamp(offset, count, &page_offset, &page_count) != 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "count exceeds the page limit");
        }
        return rpc_queue_reply_page(message, page_offset, page_count);
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

        if (written == BQ_ERR_TOO_MANY) {
            /* 单发条目数超过 BQ_SET_MAX：整体拒绝，绝不收下前半截再报成功 */
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "payload carries more than the per-call limit; chunk it: "
                             "one Queue.Set with at most 500 entries, then Queue.Append for the rest");
        }
        if (written < 0) {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                             "queue payload rejected (malformed JSON or a non-local path)");
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

    /* Rebuild 已在 api_version 4 撤下：执行顺序由播放模式（Control.SetPlayMode）
     * 与前端下发的数组顺序决定，核心不再单独提供“重建”入口。 */

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
    /* Get 此前只在 core.methods/文档里有、自省 XML 缺失（可调用不可描述）。
     * 参数按处理器实际签名：(i offset, i count) -> s 分页 JSON（见本文件
     * rpc_queue_handle 的 "Get" 分支与 bq_render_get）。 */
    "    <method name=\"Get\">\n"
    "      <arg name=\"offset\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"count\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
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
    /* Rebuild 的自省段随 api_version 4 一起撤下（处理器分支已删除） */
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
