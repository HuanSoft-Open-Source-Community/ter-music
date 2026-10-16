/**
 * @file backend_queue.h
 * @brief 后端播放队列：核心执行前端下发的路径队列
 *
 * 架构边界（2026-09-15）：文件系统与内容（曲库/扫描/播放列表内容/用户歌单/
 * 收藏历史/排序过滤搜索）归**前端**；核心只做播放，因此它不扫描目录、不读
 * 曲库，只维护一份"要播的本地文件路径"队列与播放游标。
 *
 * 队列内容由前端经 D-Bus 下发（`Queue.Set` 整表 / `Queue.Append` 分块），
 * 条目随带元数据（标题/艺术家/专辑/时长/CUE 偏移/歌词来源），避免核心重复
 * 扫描与打标签；元数据缺失时核心只为**当前曲目**自行提取。
 *
 * 执行顺序与游标都由本模块拥有：数组顺序即执行顺序，`bq_apply_mode()` 按
 * 播放模式（顺序/单曲/列表循环/随机/文件夹/专辑/艺术家）重建顺序并保持当前
 * 条目，`bq_advance()/bq_rewind()` 推进游标供自动续播使用。
 *
 * 线程模型：队列由**媒体循环**（`Queue.*` 写、`Queue.Get` 渲染）与**播放线程**
 * （续播 `bq_advance()`、预取 `bq_peek_next()`）共同访问，因此本模块内部用一把
 * 互斥锁保护全部共享状态；公开函数之间一律通过 `*_locked` 内部版本互调，避免
 * 自锁。条目内容、顺序或游标的任何变化都会递增修订号（`bq_revision()`），
 * 媒体循环据此补发 `QueueChanged` 广播（见 `media/rpc_queue.c`）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef BACKEND_QUEUE_H
#define BACKEND_QUEUE_H

#include <stddef.h>

#include "types.h"

/* 单次下发的条目上限（JSON 载荷受 256 KB 上限约束；更大的队列由前端
 * 先 Set 分块再 Append 补齐）。超过它**必须**失败，绝不静默截断。 */
#define BQ_SET_MAX 500

/* 载荷下发（bq_set_json / bq_append_json / bq_insert_after_json）的失败码：
 * 成功时返回写入的条目数（>=0），失败时返回下列负值之一，调用方据此
 * 决定回给前端哪个错误名。 */
#define BQ_ERR_INVALID  (-1)   /* JSON 非法 / 路径非本地 / 内存不足 */
#define BQ_ERR_TOO_MANY (-2)   /* 单发条目数超过 BQ_SET_MAX，需分块下发 */

typedef struct {
    char path[MAX_PATH_LEN];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    int duration_seconds;
    int cue_offset;      /* CUE 子轨起始秒；非子轨为 0 */
    int cue_track_number;/* CUE 子轨编号；非子轨为 0（供 MPRIS xesam:trackNumber） */
    int is_cue;          /* 1 = CUE 子轨 */
    int lyrics_source;   /* LYRICS_SOURCE_*；0 = 自动 */
} BackendQueueEntry;

void bq_init(void);
void bq_shutdown(void);

/* 核心只播放本地文件：远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）由前端下载到
 * 本地缓存后把**本地路径**交给核心。带 scheme 的路径除 file:// 外一律拒绝。 */
int bq_path_is_local(const char *path);

/* ── 内容下发（前端 → 核心） ─────────────────────────────────────── */
/* JSON 形态：{"entries":[{"path":"…","title":"…",…}]} 或裸数组。
 * 单发条目数上限是 BQ_SET_MAX，超过它整体拒绝（不截断、不报成功）。
 * @return 写入条目数（>=0）；BQ_ERR_INVALID = JSON 非法/路径非本地；
 *         BQ_ERR_TOO_MANY = 单发超过 BQ_SET_MAX，需分块 */
int bq_set_json(const char *json);
int bq_append_json(const char *json);
int bq_insert_after_json(int position, const char *json);

/* Set 的语义契约：**只替换未来曲目，当前播放不中断**。
 * 整表替换会把游标置到新表首（0），但替换前游标所在条目会被记为“在播条目”：
 * 新表比正在播放的下标短时，上层（MPRIS 属性、Info 快照）仍能用它如实上报
 * 正在出声的曲目，而不是报“无曲目”。游标被显式移动（`bq_play_at()`/
 * `bq_advance()` 等）或换曲之后该记录随之刷新，不会长期滞留旧条目。
 * @return 0 = 有在播条目（out 已填充）；-1 = 无 */
int bq_pinned_entry(BackendQueueEntry *out);

/* ── 队列编辑（前端下发编辑意图，核心镜像之） ─────────────────────── */
int bq_remove_at(int position);
int bq_move_up(int position);
int bq_move_down(int position);
int bq_clear(void);

/* ── 游标与查询 ──────────────────────────────────────────────────── */
int bq_count(void);
int bq_position(void);                      /* 当前游标，-1 = 无 */
int bq_set_position(int position);
int bq_play_at(int position);               /* 置游标；返回 0/-1 */
int bq_current(BackendQueueEntry *out);     /* 0 = 有当前条目 */
int bq_entry_at(int position, BackendQueueEntry *out);
int bq_path_at(int position, char *out, size_t size);
int bq_position_of_path(const char *path);  /* 反查位置，-1 = 不在队列 */

unsigned long long bq_revision(void);
void bq_bump_revision(void);

/* ── 执行顺序（播放模式） ────────────────────────────────────────── */
void bq_apply_mode(PlayMode mode);          /* 重建顺序，保持当前条目 */
int  bq_advance(PlayMode mode, int *out_position);   /* 1 = 前进成功，0 = 到头 */
int  bq_rewind(PlayMode mode, int *out_position);
int  bq_peek_next(PlayMode mode, int *out_position); /* 只探测，不改游标 */
void bq_shuffle_rest(void);                 /* 保持当前条目，打乱其余 */

/* ── 序列化（Queue.Get / QueueChanged） ───────────────────────────── */
size_t bq_render_get(char *out, size_t out_size, int offset, int count);
size_t bq_render_entry_json(const BackendQueueEntry *entry, char *out, size_t out_size);

#endif /* BACKEND_QUEUE_H */
