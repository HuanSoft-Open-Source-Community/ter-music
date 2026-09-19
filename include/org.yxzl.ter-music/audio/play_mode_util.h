/**
 * @file play_mode_util.h
 * @brief 播放模式的分类判定（单一定义点）
 *
 * 原先这些判定写在 `audio/play_queue.c` 里；后端路径队列
 * （`queue/backend_queue.c`）也要用同一套语义，故抽成 static inline 头，
 * 两处共用，避免各自维护一份枚举分类而漂移。前缀 pm_ 以区别于
 * `audio/play_queue.c` 里同语义的对外符号（那些保留给既有调用方）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef PLAY_MODE_UTIL_H
#define PLAY_MODE_UTIL_H

#include "types.h"

static inline int pm_is_shuffle(PlayMode mode)
{
    switch (mode) {
        case PLAY_MODE_SHUFFLE_ONCE:
        case PLAY_MODE_SHUFFLE_REPEAT:
        case PLAY_MODE_FOLDER_SHUFFLE:
        case PLAY_MODE_FOLDER_SHUFFLE_REPEAT:
        case PLAY_MODE_ALBUM_SHUFFLE:
        case PLAY_MODE_ALBUM_SHUFFLE_REPEAT:
        case PLAY_MODE_ARTIST_SHUFFLE:
        case PLAY_MODE_ARTIST_SHUFFLE_REPEAT:
            return 1;
        default:
            return 0;
    }
}

static inline int pm_is_folder_mode(PlayMode mode)
{
    return mode >= PLAY_MODE_FOLDER_SEQUENTIAL && mode <= PLAY_MODE_FOLDER_SHUFFLE_REPEAT;
}

static inline int pm_is_album_mode(PlayMode mode)
{
    return mode >= PLAY_MODE_ALBUM_SEQUENTIAL && mode <= PLAY_MODE_ALBUM_SHUFFLE_REPEAT;
}

static inline int pm_is_artist_mode(PlayMode mode)
{
    return mode >= PLAY_MODE_ARTIST_SEQUENTIAL && mode <= PLAY_MODE_ARTIST_SHUFFLE_REPEAT;
}

/* 列表/组内循环（到尾后回到开头） */
static inline int pm_repeats(PlayMode mode)
{
    return mode == PLAY_MODE_LIST_REPEAT ||
           mode == PLAY_MODE_SHUFFLE_REPEAT ||
           mode == PLAY_MODE_FOLDER_REPEAT ||
           mode == PLAY_MODE_FOLDER_SHUFFLE_REPEAT ||
           mode == PLAY_MODE_ALBUM_REPEAT ||
           mode == PLAY_MODE_ALBUM_SHUFFLE_REPEAT ||
           mode == PLAY_MODE_ARTIST_REPEAT ||
           mode == PLAY_MODE_ARTIST_SHUFFLE_REPEAT;
}

#endif /* PLAY_MODE_UTIL_H */
