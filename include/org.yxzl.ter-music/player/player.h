/**
 * @file player.h
 * @brief 前端唯一门面：界面只通过它读写状态与下发命令
 *
 * 目的：UI 不再直读引擎全局（g_play_queue / g_play_state / g_playlist…）、
 * 不再直调引擎命令（play_audio / play_queue_* / library_*…）。界面只依赖
 * 本头文件，后端可以是：
 *
 *   - PLAYER_BACKEND_LOCAL：直接调用进程内引擎（player_local.c，今天的
 *     行为，迁移期默认），M6 删除；
 *   - PLAYER_BACKEND_REMOTE：D-Bus 客户端（player_remote.c），核心在别的
 *     进程里，界面只是客户端（M4 起成为默认）。
 *
 * 两个后端必须给出一致的语义：同一组快照字段、同一组分页结果、同样的
 * 乐观回显与修订号语义。
 *
 * 修订号：每个可变面各有一个单调递增的 revision，`player_pump()` 更新快照
 * 后递增。界面每轮比较自己记下的值，变化才请求重绘——这样事件循环里没有
 * 回调风暴，远程后端的异步到达也能自然驱动刷新。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef PLAYER_H
#define PLAYER_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "audio/play_queue.h"
#include "core/core.h"
#include "config/config.h"
#include "info/info.h"
#include "library/library.h"
#include "playlist/playlist.h"
#include "queue/backend_queue.h"
#include "remote/remote.h"
#include "ui/lyrics.h"

/* 后端选择 */
typedef enum {
    PLAYER_BACKEND_LOCAL = 0,
    PLAYER_BACKEND_REMOTE = 1
} PlayerBackend;

/* 歌词文档单页行数上限（与 D-Bus 分页上限一致） */
#define PLAYER_LYRIC_PAGE_MAX 200

typedef struct {
    int total;                  /* 整篇行数 */
    int offset;                 /* 本页起点 */
    int count;                  /* 本页行数 */
    int current_index;          /* 当前高亮行，-1 = 无 */
    int has_lyrics;
    int has_timestamps;
    int source;                 /* LYRICS_SOURCE_* */
    char track_id[96];
    uint64_t revision;
    LyricLine lines[PLAYER_LYRIC_PAGE_MAX];
} PlayerLyricsDoc;

/* ── 生命周期 ─────────────────────────────────────────────────── */

/* 连接/初始化。backend=REMOTE 时 bus_name 可为 NULL（用主名）。
 * 远程后端在返回前完成版本握手；不兼容返回 -1。 */
int player_init(PlayerBackend backend, const char *bus_name);
void player_shutdown(void);

/* 每轮事件循环调用，非阻塞。
 * 本地后端：重算快照并与上次比较，递增变化的修订号；
 * 远程后端：读写总线、派发信号与待决调用、丢弃过期响应。
 * @return 0 正常；-1 表示与核心断开（界面应显示断线浮层） */
int player_pump(void);

int player_is_connected(void);
PlayerBackend player_backend(void);

/* 请求重启核心（断线后的一键恢复）：本地后端为空操作。 */
int player_restart_core(void);

/* ── 修订号（界面据此判断是否需要重绘） ───────────────────────── */

uint64_t player_state_revision(void);
uint64_t player_queue_revision(void);
uint64_t player_playlist_revision(void);
uint64_t player_lyrics_revision(void);
uint64_t player_config_revision(void);
uint64_t player_cover_revision(void);
uint64_t player_library_revision(void);

/* ── 快照（只读，生命周期归门面） ─────────────────────────────── */

const InfoTrack    *player_track(void);
const InfoPlayback *player_playback(void);   /* 位置已含本地单调时钟外推 */
const InfoLyrics   *player_lyrics(void);
const char         *player_status_message(void);

/* 播放位置/时长的便捷读取（秒） */
int player_track_index(void);                /* 当前曲目的播放列表下标，-1 = 无 */
int player_track_metadata(int index, Track *out);  /* 指定曲目的元数据 */
int player_position_seconds(void);
int player_duration_seconds(void);
int player_play_state(void);                 /* PlayState 值 */
int player_play_mode(void);                  /* PlayMode 值 */
int player_volume_percent(void);
float player_speed(void);

/* ── transport ────────────────────────────────────────────────── */

void player_play(int track_index);   /* < 0 = 从当前/首曲继续 */
void player_pause(void);
void player_resume(void);
void player_play_pause(void);
void player_stop(void);
void player_next(void);
void player_prev(void);
void player_seek_seconds(int seconds);
void player_set_volume(int percent);         /* 立即回显，随后以核心为准 */
void player_set_speed(float rate);
void player_set_play_mode(PlayMode mode);

/* ── 队列 ─────────────────────────────────────────────────────── */

/* 队列内容归后端（前端下发路径队列），游标也由后端拥有；
 * 前端的「内容列表物理下标」与「队列位置」一一对应（装配顺序即下标顺序）。 */
int player_queue_count(void);
int player_queue_position(void);
int player_queue_index_at(int position);     /* 该位置的曲目下标，-1 = 越界 */
int player_queue_play(int position);
int player_queue_append(int track_index);
int player_queue_insert_after(int track_index);
int player_queue_remove_at(int position);
int player_queue_move_up(int position);
int player_queue_move_down(int position);
int player_queue_clear(void);
int player_queue_rebuild(void);
int player_queue_shuffle(void);
int player_queue_is_active(void);

/* 内容列表变化后整表重推（分块下发），返回下发条目数；-1 = 失败 */
int player_queue_push(void);
/* 按路径反查队列位置（-1 = 不在队列） */
int player_queue_find(const char *path);
/* 队列视图分页：一页最多 out_cap 条，返回实际写入条数 */
int player_queue_page_count(void);
int player_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap);

/* ── 播放列表 ─────────────────────────────────────────────────── */

int  player_playlist_count(void);
int  player_playlist_visible_count(void);
int  player_playlist_loaded(void);
int  player_playlist_tree_active(void);
void player_playlist_folder(char *out, size_t out_size);
const char *player_playlist_sort_id(void);
const char *player_playlist_filter(void);

/* 可见行总数与一页内容。本地后端同步填充；远程后端返回当前缓存页
 * （可能为空）并触发异步刷新，随后由 playlist revision 通知界面重取。
 * out 必须至少容纳 count 个 PlaylistRow（约 1 KB/行，堆分配）。 */
int player_playlist_page_total(void);
int player_playlist_page(int offset, int count, PlaylistRow *out, int out_cap);

int player_playlist_load(const char *path, int append, int autoplay);
int player_playlist_toggle_expand(int tree_index);
int player_playlist_reveal(int track_index);
int player_playlist_sort(SortMode mode);
int player_playlist_set_filter(const char *query);
int player_playlist_search(const char *query, int offset, int count,
                           PlaylistRow *out, int out_cap);
/* 后台加载状态（远程/本地一致）：state 写入 out_state（可 NULL） */
int player_playlist_status(int *progress, int *total);

/* ── 曲库 / 收藏 / 历史 ───────────────────────────────────────── */

int player_library_available(void);
int player_library_track_count(void);
int player_library_scan(int *scanning, int *progress, int *total);
int player_library_rescan(const char *path);
int player_library_search(const char *query);
int player_library_item_count(const char *kind, const char *filter_json);
int player_library_page(const char *kind, const char *filter_json,
                        int offset, int count, LibraryRow *out, int out_cap);

int player_favorites_count(void);
int player_favorites_get(int index, Track *out);
int player_favorites_add(const Track *track);
int player_favorites_remove(const Track *track);
int player_favorites_has(const char *track_path);

int player_history_count(void);
int player_history_get(int index, HistoryEntry *out);
int player_history_add(const Track *track);
int player_history_clear(void);

int player_dir_history_count(void);
int player_dir_history_get(int index, DirHistoryEntry *out);
int player_dir_history_add(const char *path);
int player_dir_history_clear(void);

/* ── 配置（前端持有镜像；写入是唯一入口） ─────────────────────── */

/* 从核心拉取完整配置到 g_app_config 镜像 */
int player_config_refresh(void);
/* 应用局部 JSON 补丁（键名同 config.xml）：本地镜像 + 核心落盘 */
int player_config_apply_json(const char *patch_json);
int player_config_set_int(const char *key, int value);
int player_config_set_string(const char *key, const char *value);
int player_config_set_float(const char *key, float value);
int player_config_reload(void);
int player_config_reset(void);

/* ── 歌词 / 封面 / 可视化 ─────────────────────────────────────── */

int player_lyrics_document(int offset, int count, PlayerLyricsDoc *out);
int player_lyrics_reload_source(int source);

/* 取当前曲目的字符封面（按尺寸与字符集缓存）；返回是否有内容。
 * 远程后端首次调用会触发异步获取，未就绪时返回 0。 */
int player_cover_rows(int cols, int rows, int charset, char *out, size_t out_size);

/* 可视化：levels/peaks 各 max_levels 个（0-255） */
void player_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);
void player_set_visualizer_active(int active);

/* 门面里没有远程音乐源：远程服务器列表、目录浏览与下载都是前端自己的事
 * （remote/remote_store.c、remote/remote_cache.c），前端把下载好的本地
 * 路径经 player_playlist_load() 交给核心即可。 */

#endif /* PLAYER_H */
