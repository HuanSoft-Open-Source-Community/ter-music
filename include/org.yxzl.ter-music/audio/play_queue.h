#ifndef PLAY_QUEUE_H
#define PLAY_QUEUE_H

#include "types.h"

extern PlayQueue g_play_queue;

void play_queue_init(PlayQueue *q);
void play_queue_clear(PlayQueue *q);

void play_queue_rebuild(PlayQueue *q, const Playlist *playlist,
                        PlayMode mode, int current_track_index);

void play_queue_shuffle_range(PlayQueue *q, int start, int count);

int  play_queue_get_current(const PlayQueue *q);
int  play_queue_peek_next(const PlayQueue *q, PlayMode mode);
int  play_queue_peek_prev(const PlayQueue *q, PlayMode mode);
void play_queue_advance(PlayQueue *q, PlayMode mode);
void play_queue_rewind(PlayQueue *q, PlayMode mode);

int  play_queue_get_track_at(const PlayQueue *q, int position, int *track_index);

/* 全局队列（g_play_queue）的无参访问器：避免调用方直接摸字段 */
int  play_queue_count(void);         /* 队列长度 */
int  play_queue_position(void);      /* 当前播放位置，-1 = 无 */
int  play_queue_index_at(int position);  /* 该位置的曲目下标，-1 = 越界/空 */
void play_queue_set_position(int position);
int  play_queue_is_active(const PlayQueue *q);
int  play_queue_contains(const PlayQueue *q, int track_index);

/* Queue editing */
int  play_queue_append(PlayQueue *q, int track_index);
int  play_queue_remove_at(PlayQueue *q, int position);
int  play_queue_insert_after(PlayQueue *q, int track_index);
int  play_queue_move_up(PlayQueue *q, int position);
int  play_queue_move_down(PlayQueue *q, int position);

/* Persistence */
int  play_queue_save(const PlayQueue *q);
int  play_queue_load(PlayQueue *q);

int  play_mode_is_shuffle(PlayMode mode);
int  play_mode_is_folder_mode(PlayMode mode);
int  play_mode_is_album_mode(PlayMode mode);
int  play_mode_is_artist_mode(PlayMode mode);
int  play_mode_is_advanced(PlayMode mode);
int  play_mode_repeats(PlayMode mode);
const char *play_mode_display_name(PlayMode mode, int use_english);
const char *play_mode_short_name(PlayMode mode, int use_english);

#endif
