/**
 * @file play_queue.h
 * @brief 播放队列门面：后端路径队列（queue/backend_queue）的兼容转发层
 *
 * 架构边界（2026-09-15）：队列内容由**前端**下发（扫描结果与列表编辑），
 * 核心只执行；真正的实现归 `queue/backend_queue.c`，本模块只保留既有的
 * 调用点签名，让 `audio/`、`info/`、`media/` 不必逐个改写。
 *
 * 语义迁移说明：
 *  - 队列条目从「曲目下标」变为「本地文件路径 + 元数据」（`BackendQueueEntry`），
 *    因此 `play_queue_*` 里凡接受 `track_index` 的接口都由调用方先换成路径；
 *  - `play_queue_rebuild` 不再从播放列表构建队列（后端不认识内容列表），
 *    改为对**已下发的队列**应用播放模式并复位游标；
 *  - `g_play_queue` 只作为兼容句柄存在，字段不再承载队列数据。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef PLAY_QUEUE_H
#define PLAY_QUEUE_H

#include "types.h"

/* 队列视图镜像：真实数据在后端队列（queue/backend_queue.c），本结构是
 * 界面渲染队列视图所需的“队列位置 → 内容列表下标”映射，定义见 types.h */
extern PlayQueue g_play_queue;

void play_queue_clear(PlayQueue *q);

/* 对已下发的队列应用播放模式，并把游标置到 current_track_path
 * （current_track_path 为空时保持游标所在条目所在位置）。 */
void play_queue_rebuild(PlayQueue *q, PlayMode mode, const char *current_track_path);

int  play_queue_peek_next(const PlayQueue *q, PlayMode mode);   /* 返回队列位置，-1 = 无 */
int  play_queue_peek_prev(const PlayQueue *q, PlayMode mode);
void play_queue_advance(PlayQueue *q, PlayMode mode);
void play_queue_rewind(PlayQueue *q, PlayMode mode);

/* 全局队列（g_play_queue）的无参访问器：避免调用方直接摸字段 */
int  play_queue_count(void);         /* 队列长度 */
int  play_queue_position(void);      /* 当前播放位置，-1 = 无 */
void play_queue_set_position(int position);
int  play_queue_is_active(const PlayQueue *q);

/* 把队列视图镜像（g_play_queue）与后端队列重新对齐：任何绕过本模块直接
 * 改写后端队列的路径（前端重推内容列表、后端换模式）之后都应调用它。
 * @param resolve 内容列表下标解析器：给路径返回物理下标，-1 = 不在内容列表
 *                （传 NULL 时 indices 全部置 -1，界面只显示路径） */
void play_queue_sync_mirror(int (*resolve)(const char *path, void *user), void *user);

/* Queue editing：全部按队列位置操作（条目内容由前端下发） */
int  play_queue_remove_at(PlayQueue *q, int position);
int  play_queue_move_up(PlayQueue *q, int position);
int  play_queue_move_down(PlayQueue *q, int position);

/* 持久化：队列内容由前端的内容列表恢复（temp playlist + 上次打开的目录），
 * 游标由核心的 resume_last_playback 恢复，因此本模块不再读写 queue.txt。 */

int  play_mode_is_shuffle(PlayMode mode);
int  play_mode_is_folder_mode(PlayMode mode);
int  play_mode_is_album_mode(PlayMode mode);
int  play_mode_is_artist_mode(PlayMode mode);
int  play_mode_is_advanced(PlayMode mode);
int  play_mode_repeats(PlayMode mode);
const char *play_mode_display_name(PlayMode mode, int use_english);
const char *play_mode_short_name(PlayMode mode, int use_english);

#endif
