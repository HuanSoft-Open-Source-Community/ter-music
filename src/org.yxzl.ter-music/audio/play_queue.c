/**
 * @file play_queue.c
 * @brief 播放队列门面实现：全部转发到后端路径队列（queue/backend_queue）
 *
 * 本文件不再持有队列数据，也不认识播放列表 / 曲库 / 界面。队列内容由前端
 * 经 `Queue.Set`/`Queue.Append` 下发，执行顺序与游标由 `bq_*` 拥有。
 *
 * 播放模式的分类判定统一走 `audio/play_mode_util.h`（单一定义点），
 * 显示名（i18n）留在本模块，供前端的模式菜单复用。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "types.h"
#include "audio/play_queue.h"
#include "audio/play_mode_util.h"
#include "queue/backend_queue.h"
#include "i18n/i18n.h"
#include "logger/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 兼容句柄
 * ============================================================ */

PlayQueue g_play_queue = {0};

void play_queue_sync_mirror(int (*resolve)(const char *path, void *user), void *user)
{
    int count = bq_count();
    if (count > MAX_TRACKS) {
        count = MAX_TRACKS;
    }

    for (int i = 0; i < count; i++) {
        char path[MAX_PATH_LEN];
        if (bq_path_at(i, path, sizeof(path)) != 0) {
            g_play_queue.indices[i] = -1;
            continue;
        }
        g_play_queue.indices[i] = resolve ? resolve(path, user) : -1;
    }
    g_play_queue.count = count;
    g_play_queue.current_position = bq_position();
    g_play_queue.shuffle_generation = 0;
}

/* 无解析器时的镜像同步：只对齐长度与游标 */
static void sync_play_queue_handle(void)
{
    play_queue_sync_mirror(NULL, NULL);
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

void play_queue_clear(PlayQueue *q)
{
    (void)q;
    bq_clear();
    sync_play_queue_handle();
}

/* ============================================================
 * 重建（应用播放模式 + 复位游标）
 * ============================================================ */

void play_queue_rebuild(PlayQueue *q, PlayMode mode, const char *current_track_path)
{
    (void)q;

    if (bq_count() == 0) {
        sync_play_queue_handle();
        return;
    }

    /* 先把游标对到目标曲目，组模式 / 洗牌的“当前条目”都以它为准；
     * 目标不在队列里时保持既有游标。 */
    if (current_track_path && current_track_path[0]) {
        int position = bq_position_of_path(current_track_path);
        if (position >= 0) {
            bq_set_position(position);
        }
    }

    bq_apply_mode(mode);

    /* 洗牌模式会把当前条目挪到首位：旧实现里 current_position 也随之归 0，
     * 这里同理，但只在本就是洗牌模式时发生。 */
    if (pm_is_shuffle(mode)) {
        bq_set_position(0);
    }

    sync_play_queue_handle();
}

/* ============================================================
 * Navigation：返回/操作的都是**队列位置**（旧实现返回曲目下标）
 * ============================================================ */

int play_queue_peek_next(const PlayQueue *q, PlayMode mode)
{
    (void)q;
    int position = -1;
    if (!bq_peek_next(mode, &position)) {
        return -1;
    }
    return position;
}

int play_queue_peek_prev(const PlayQueue *q, PlayMode mode)
{
    (void)q;
    int count = bq_count();
    if (count == 0) {
        return -1;
    }
    int position = bq_position();
    if (position < 0) {
        position = 0;
    }

    int prev = position - 1;
    if (prev < 0) {
        prev = pm_repeats(mode) ? count - 1 : 0;
    }
    return prev;
}

void play_queue_advance(PlayQueue *q, PlayMode mode)
{
    (void)q;
    int position = -1;
    bq_advance(mode, &position);
    sync_play_queue_handle();
}

void play_queue_rewind(PlayQueue *q, PlayMode mode)
{
    (void)q;
    int position = -1;
    bq_rewind(mode, &position);
    sync_play_queue_handle();
}

/* ============================================================
 * Query helpers
 * ============================================================ */

int play_queue_count(void)
{
    return bq_count();
}

int play_queue_position(void)
{
    return bq_position();
}

void play_queue_set_position(int position)
{
    bq_set_position(position);
    sync_play_queue_handle();
}

int play_queue_is_active(const PlayQueue *q)
{
    (void)q;
    return bq_count() > 0 && bq_position() >= 0;
}

/* ============================================================
 * Queue editing
 * ============================================================ */

int play_queue_remove_at(PlayQueue *q, int position)
{
    (void)q;
    int rc = bq_remove_at(position);
    sync_play_queue_handle();
    return rc;
}

int play_queue_move_up(PlayQueue *q, int position)
{
    (void)q;
    int rc = bq_move_up(position);
    sync_play_queue_handle();
    return rc;
}

int play_queue_move_down(PlayQueue *q, int position)
{
    (void)q;
    int rc = bq_move_down(position);
    sync_play_queue_handle();
    return rc;
}

/* ============================================================
 * Mode query helpers（分类判定单一定义点在 audio/play_mode_util.h）
 * ============================================================ */

int play_mode_is_shuffle(PlayMode mode)      { return pm_is_shuffle(mode); }
int play_mode_is_folder_mode(PlayMode mode)  { return pm_is_folder_mode(mode); }
int play_mode_is_album_mode(PlayMode mode)   { return pm_is_album_mode(mode); }
int play_mode_is_artist_mode(PlayMode mode)  { return pm_is_artist_mode(mode); }

int play_mode_is_advanced(PlayMode mode)
{
    return play_mode_is_album_mode(mode) || play_mode_is_artist_mode(mode);
}

int play_mode_repeats(PlayMode mode)         { return pm_repeats(mode); }

/* ============================================================
 * Display names
 * ============================================================ */

const char *play_mode_display_name(PlayMode mode, int unused)
{
    (void)unused;
    switch (mode) {
        case PLAY_MODE_SEQUENTIAL:             return i18n_get("play_mode.seq");
        case PLAY_MODE_SINGLE_REPEAT:          return i18n_get("play_mode.single_repeat");
        case PLAY_MODE_LIST_REPEAT:            return i18n_get("play_mode.list_repeat");
        case PLAY_MODE_SHUFFLE_ONCE:           return i18n_get("play_mode.shuffle_once");
        case PLAY_MODE_SHUFFLE_REPEAT:         return i18n_get("play_mode.shuffle_repeat");
        case PLAY_MODE_FOLDER_SEQUENTIAL:      return i18n_get("play_mode.folder_seq");
        case PLAY_MODE_FOLDER_REPEAT:          return i18n_get("play_mode.folder_repeat");
        case PLAY_MODE_FOLDER_SHUFFLE:         return i18n_get("play_mode.folder_shuffle");
        case PLAY_MODE_FOLDER_SHUFFLE_REPEAT:  return i18n_get("play_mode.folder_shuffle_repeat");
        case PLAY_MODE_ALBUM_SEQUENTIAL:       return i18n_get("play_mode.album_seq");
        case PLAY_MODE_ALBUM_REPEAT:           return i18n_get("play_mode.album_repeat");
        case PLAY_MODE_ALBUM_SHUFFLE:          return i18n_get("play_mode.album_shuffle");
        case PLAY_MODE_ALBUM_SHUFFLE_REPEAT:   return i18n_get("play_mode.album_shuffle_repeat");
        case PLAY_MODE_ARTIST_SEQUENTIAL:      return i18n_get("play_mode.artist_seq");
        case PLAY_MODE_ARTIST_REPEAT:          return i18n_get("play_mode.artist_repeat");
        case PLAY_MODE_ARTIST_SHUFFLE:         return i18n_get("play_mode.artist_shuffle");
        case PLAY_MODE_ARTIST_SHUFFLE_REPEAT:  return i18n_get("play_mode.artist_shuffle_repeat");
        default:                               return i18n_get("play_mode.seq");
    }
}

const char *play_mode_short_name(PlayMode mode, int unused)
{
    (void)unused;
    switch (mode) {
        case PLAY_MODE_SEQUENTIAL:             return i18n_get("play_mode.seq_short");
        case PLAY_MODE_SINGLE_REPEAT:          return i18n_get("play_mode.single_repeat_short");
        case PLAY_MODE_LIST_REPEAT:            return i18n_get("play_mode.list_repeat_short");
        case PLAY_MODE_SHUFFLE_ONCE:           return i18n_get("play_mode.shuffle_once_short");
        case PLAY_MODE_SHUFFLE_REPEAT:         return i18n_get("play_mode.shuffle_repeat_short");
        case PLAY_MODE_FOLDER_SEQUENTIAL:      return i18n_get("play_mode.folder_seq_short");
        case PLAY_MODE_FOLDER_REPEAT:          return i18n_get("play_mode.folder_repeat_short");
        case PLAY_MODE_FOLDER_SHUFFLE:         return i18n_get("play_mode.folder_shuffle_short");
        case PLAY_MODE_FOLDER_SHUFFLE_REPEAT:  return i18n_get("play_mode.folder_shuffle_repeat_short");
        case PLAY_MODE_ALBUM_SEQUENTIAL:       return i18n_get("play_mode.album_seq_short");
        case PLAY_MODE_ALBUM_REPEAT:           return i18n_get("play_mode.album_repeat_short");
        case PLAY_MODE_ALBUM_SHUFFLE:          return i18n_get("play_mode.album_shuffle_short");
        case PLAY_MODE_ALBUM_SHUFFLE_REPEAT:   return i18n_get("play_mode.album_shuffle_repeat_short");
        case PLAY_MODE_ARTIST_SEQUENTIAL:      return i18n_get("play_mode.artist_seq_short");
        case PLAY_MODE_ARTIST_REPEAT:          return i18n_get("play_mode.artist_repeat_short");
        case PLAY_MODE_ARTIST_SHUFFLE:         return i18n_get("play_mode.artist_shuffle_short");
        case PLAY_MODE_ARTIST_SHUFFLE_REPEAT:  return i18n_get("play_mode.artist_shuffle_repeat_short");
        default:                               return i18n_get("play_mode.seq_short");
    }
}
