/**
 * @file playlist_queue.c
 * @brief 前端 → 后端：把内容列表装配成路径队列并下发
 *
 * 架构边界（2026-09-15）：**内容**（扫描、元数据、CUE 子轨、歌词来源）归
 * 前端；**执行**（播放顺序、游标、自动续播）归后端的路径队列
 * （`queue/backend_queue.c`）。本模块是两者之间唯一的桥：
 *
 *   g_playlist（前端内容）──装配条目──▶ BackendQueueEntry 数组
 *                       ──Queue.Set / Queue.Append（分块 ≤ BQ_SET_MAX）──▶ 后端
 *
 * 后端不认识内容列表，因此每次内容变化（装载/追加/清空）都由前端重推整表；
 * 元数据随条目携带，避免后端重复扫描与打标签。
 *
 * 装配顺序 = 物理下标顺序（`g_playlist.tracks[]` 的顺序），因此「物理下标」
 * 与「队列位置」在前端始终一一对应；搜索/排序视图只影响界面显示，不影响
 * 队列构造（与迁移前的 `play_queue_rebuild` 语义一致）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"

#include "audio/audio_internal.h"
#include "audio/play_queue.h"
#include "logger/logger.h"
#include "queue/backend_queue.h"
#include "types.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 条目 JSON 的单条上限：路径与三个元数据字段转义后仍有余量 */
#define PLAYLIST_QUEUE_ENTRY_MAX (MAX_PATH_LEN * 2 + MAX_META_LEN * 6 + 128)

static void append_escaped(char *out, size_t out_size, size_t *pos, const char *text)
{
    *pos = json_append_escaped(out, out_size, *pos, text ? text : "");
}

/* 把物理下标 index 的曲目写成一条队列条目（JSON 对象）。
 * @return 写入字节数；0 = 该下标无有效路径 */
static size_t entry_json(int index, char *out, size_t out_size)
{
    char path[MAX_PATH_LEN];
    if (playlist_get_track_path(index, path, sizeof(path)) != 0 || path[0] == '\0') {
        return 0;
    }

    Track track;
    memset(&track, 0, sizeof(track));
    get_track_metadata(index, &track);

    int cue_offset = cue_get_offset(index);
    int cue_track_number = cue_get_track_number(index);
    int lyrics_source = track.lyrics_source;

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "path");
    append_escaped(out, out_size, &pos, path);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "title");
    append_escaped(out, out_size, &pos, track.title);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "artist");
    append_escaped(out, out_size, &pos, track.artist);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "album");
    append_escaped(out, out_size, &pos, track.album);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "duration_seconds");
    /* 时长由后端从文件本身取得（信息块与进度条都以它为准），此处不下发，
     * 避免前后端两份时长口径不一致。 */
    pos = json_append_int(out, out_size, pos, 0);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cue_offset");
    pos = json_append_int(out, out_size, pos, cue_offset);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cue_track_number");
    pos = json_append_int(out, out_size, pos, cue_track_number);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "is_cue");
    pos = json_append_bool(out, out_size, pos,
                           cue_offset > 0 && cue_track_number > 0);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "lyrics_source");
    pos = json_append_int(out, out_size, pos, lyrics_source);
    pos = json_append_char(out, out_size, pos, '}');
    return pos;
}

/* 内容列表下标解析器：给路径返回物理下标（-1 = 不在列表） */
static int resolve_index(const char *path, void *user)
{
    (void)user;
    return playlist_find_track_index_by_path(path);
}

/* 写一块的块头：{"entries":[ ；游标复位到 0。返回新的 pos。 */
static size_t chunk_open(char *buffer, size_t capacity)
{
    size_t pos = 0;
    pos = json_append_char(buffer, capacity, pos, '{');
    pos = json_append_key(buffer, capacity, pos, "entries");
    pos = json_append_char(buffer, capacity, pos, '[');
    return pos;
}

/* 收尾并下发一块：buffer 当前是 {"entries":[ …，补上 ]} 再回调。
 * @return 0 成功；非 0 = 回调判定失败（调用方据此中止） */
static int chunk_close_and_emit(char *buffer, size_t capacity, size_t pos,
                                int chunk_index, int in_chunk,
                                PlaylistQueueChunkCb cb, void *user)
{
    pos = json_append_char(buffer, capacity, pos, ']');
    pos = json_append_char(buffer, capacity, pos, '}');
    buffer[pos] = '\0';
    return cb(buffer, chunk_index, in_chunk, user);
}

int playlist_queue_foreach_chunk(PlaylistQueueChunkCb cb, void *user)
{
    if (!cb) {
        return -1;
    }
    int total = playlist_count();
    if (total <= 0) {
        return 0;   /* 无条目：不下发任何块，由调用方决定是否清空核心队列 */
    }

    /* 一块最多 BQ_SET_MAX 条；容量预留每条后的逗号，杜绝块内溢出 */
    size_t capacity = (size_t)BQ_SET_MAX * (PLAYLIST_QUEUE_ENTRY_MAX + 2) + 64;
    char *buffer = malloc(capacity);
    if (!buffer) {
        log_error("playlist_queue", "out of memory assembling %d queue entries", total);
        return -1;
    }

    int dispatched = 0;    /* 已成功下发的条目总数 */
    int chunk_index = 0;
    int in_chunk = 0;
    int failed = 0;
    size_t pos = chunk_open(buffer, capacity);

    for (int i = 0; i < total && !failed; i++) {
        char entry[PLAYLIST_QUEUE_ENTRY_MAX];
        size_t entry_len = entry_json(i, entry, sizeof(entry));
        if (entry_len == 0) {
            continue;   /* 无路径（目录行等）→ 不进队列 */
        }

        /* 先判溢出（不含本条的逗号）：满了就另起一块，避免留下悬挂逗号 */
        if (in_chunk > 0 && pos + entry_len + 8 >= capacity) {
            if (chunk_close_and_emit(buffer, capacity, pos, chunk_index, in_chunk,
                                     cb, user) != 0) {
                failed = 1;
                break;
            }
            dispatched += in_chunk;
            chunk_index++;
            pos = chunk_open(buffer, capacity);
            in_chunk = 0;
        }

        if (in_chunk > 0) {
            pos = json_append_char(buffer, capacity, pos, ',');
        }
        if (pos + entry_len + 8 >= capacity) {
            /* 单条就把容量顶爆：entry_json 已按 PLAYLIST_QUEUE_ENTRY_MAX 截断，
             * 走到这里说明缓冲分配异常，按失败处理而不是写出畸形 JSON */
            log_error("playlist_queue", "entry buffer overflow at index %d", i);
            failed = 1;
            break;
        }
        memcpy(buffer + pos, entry, entry_len);
        pos += entry_len;
        in_chunk++;

        /* 装满 BQ_SET_MAX 条：立即下发一块（对齐核心单次上限） */
        if (in_chunk >= BQ_SET_MAX) {
            if (chunk_close_and_emit(buffer, capacity, pos, chunk_index, in_chunk,
                                     cb, user) != 0) {
                failed = 1;
                break;
            }
            dispatched += in_chunk;
            chunk_index++;
            pos = chunk_open(buffer, capacity);
            in_chunk = 0;
        }
    }

    /* 收尾未满的一块 */
    if (!failed && in_chunk > 0) {
        if (chunk_close_and_emit(buffer, capacity, pos, chunk_index, in_chunk,
                                 cb, user) != 0) {
            failed = 1;
        } else {
            dispatched += in_chunk;
        }
    }

    free(buffer);
    return failed ? -1 : dispatched;
}

/* 取当前游标所在条目的路径（作为重建播放模式时的锚点）。
 * 后端不认识内容列表下标，锚点只能用路径给。 */
static const char *current_anchor_path(void)
{
    static char anchor[MAX_PATH_LEN];
    anchor[0] = '\0';

    BackendQueueEntry entry;
    if (bq_current(&entry) == 0) {
        snprintf(anchor, sizeof(anchor), "%s", entry.path);
    }
    return anchor[0] ? anchor : NULL;
}

char *playlist_queue_render(void)
{
    int total = playlist_count();
    if (total <= 0) {
        return NULL;
    }

    size_t capacity = (size_t)total * PLAYLIST_QUEUE_ENTRY_MAX + 64;
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    size_t pos = 0;
    pos = json_append_char(buffer, capacity, pos, '{');
    pos = json_append_key(buffer, capacity, pos, "entries");
    pos = json_append_char(buffer, capacity, pos, '[');

    int written = 0;
    for (int i = 0; i < total; i++) {
        char entry[PLAYLIST_QUEUE_ENTRY_MAX];
        size_t entry_len = entry_json(i, entry, sizeof(entry));
        if (entry_len == 0) {
            continue;
        }
        if (written > 0) {
            buffer[pos++] = ',';
        }
        memcpy(buffer + pos, entry, entry_len);
        pos += entry_len;
        written++;
    }

    pos = json_append_char(buffer, capacity, pos, ']');
    pos = json_append_char(buffer, capacity, pos, '}');
    buffer[pos] = '\0';

    if (written == 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

char *playlist_queue_render_entry(int index)
{
    char entry[PLAYLIST_QUEUE_ENTRY_MAX];
    size_t entry_len = entry_json(index, entry, sizeof(entry));
    if (entry_len == 0) {
        return NULL;
    }

    size_t capacity = entry_len + 32;
    char *payload = malloc(capacity);
    if (!payload) {
        return NULL;
    }
    snprintf(payload, capacity, "{\"entries\":[%s]}", entry);
    return payload;
}

/* 本地整表同步的分块回调：首块 bq_set_json（替换），其余 bq_append_json。
 * bq_*_json 同时接受 {"entries":[…]} 与裸数组，这里喂统一的块对象形态。 */
static int local_sync_chunk(const char *chunk_json, int chunk_index,
                            int entries_in_chunk, void *user)
{
    (void)entries_in_chunk;
    (void)user;
    int result = (chunk_index == 0) ? bq_set_json(chunk_json)
                                    : bq_append_json(chunk_json);
    return result < 0 ? -1 : 0;
}

int playlist_queue_sync(void)
{
    int total = playlist_count();
    if (total <= 0) {
        bq_clear();
        play_queue_sync_mirror(resolve_index, NULL);
        return 0;
    }

    /* 装配与分块下发复用 playlist_queue_foreach_chunk：单块 ≤ BQ_SET_MAX，
     * 首块替换、余块追加，行为与逐块手写下发一致。 */
    int written = playlist_queue_foreach_chunk(local_sync_chunk, NULL);
    if (written < 0) {
        log_error("playlist_queue", "Queue.Set rejected while pushing %d entries", total);
        return -1;
    }

    /* 内容列表换了：按当前播放模式重建执行顺序，锚点保持原曲不动 */
    const char *anchor = current_anchor_path();
    play_queue_rebuild(&g_play_queue, g_play_mode, anchor);
    play_queue_sync_mirror(resolve_index, NULL);
    return written;
}

int playlist_queue_push_entry(int index, int insert_after)
{
    char entry[PLAYLIST_QUEUE_ENTRY_MAX];
    size_t entry_len = entry_json(index, entry, sizeof(entry));
    if (entry_len == 0) {
        return -1;
    }

    char payload[PLAYLIST_QUEUE_ENTRY_MAX + 32];
    snprintf(payload, sizeof(payload), "{\"entries\":[%s]}", entry);

    int written = insert_after
        ? bq_insert_after_json(bq_position(), payload)
        : bq_append_json(payload);
    if (written < 0) {
        return -1;
    }
    play_queue_sync_mirror(resolve_index, NULL);
    return written;
}
