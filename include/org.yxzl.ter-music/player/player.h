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
#include "lyrics/lyrics.h"
#include "remote/remote.h"

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

/* 重连钩子：远端后端从断线恢复到已连接时回调一次。
 * 前端用它把内容队列补推给（可能是新起来的）核心。 */
void player_set_reconnect_hook(void (*hook)(void));
void player_notify_reconnected(void);

/* 断线状态（远端后端）：界面据此画断线浮层并提示按 R 重启核心。
 * 本地后端恒为“未断线”。 */
int player_is_offline(void);
/* 下次重连尝试还剩多少毫秒（用于提示重试节奏）；未断线时为 0 */
int player_reconnect_in_ms(void);

/* ── 修订号（界面据此判断是否需要重绘） ───────────────────────── */

uint64_t player_state_revision(void);
uint64_t player_queue_revision(void);
uint64_t player_lyrics_revision(void);
uint64_t player_config_revision(void);
uint64_t player_cover_revision(void);

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
void player_adjust_volume(int delta);        /* ± 档位音量（键盘/鼠标共用） */
void player_set_speed(float rate);
/* 倍速档位表（只读，长度写入 out_count）：界面用它渲染档位菜单 */
const float *player_speed_steps(int *out_count);
int player_speed_step_count(void);
int player_speed_index(void);                /* 当前档位下标 */
void player_set_speed_index(int index);      /* 按档位设置倍速（越界忽略） */
void player_set_play_mode(PlayMode mode);
void player_cycle_play_mode(void);           /* 顺序 → 单曲 → 列表 → 随机 → 文件夹 */

/* 播放模式显示名（i18n；界面不再直接调引擎的命名函数） */
const char *player_play_mode_name(int use_english);
const char *player_play_mode_name_of(PlayMode mode, int use_english);

/* ── 均衡器（后端在配置写入后热应用） ─────────────────────────── */
int   player_eq_enabled(void);
void  player_eq_set_enabled(int enabled);
void  player_eq_set_band_gain(int band, float gain);
float player_eq_get_band_gain(int band);
void  player_eq_set_preamp(float preamp);
void  player_eq_apply_preset(int preset);

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

/* 播放列表 / 曲库 / 收藏 / 历史都是**前端自有内容**（playlist/、library/、
 * search/），界面直接调用那些模块；门面里不再有它们的位置——门面只包播放面。 */

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

/* 后端当前的歌词状态（界面只读）：高亮行、来源、是否有时间戳 */
int player_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps);
int player_lyrics_highlight_count(void);   /* 同一时间戳一起高亮的行数（1..2） */
int player_lyrics_source(void);
int player_lyrics_total(void);               /* 歌词总行数 */
int player_lyrics_line_at(int index, LyricLine *out);   /* 取某一行（-1 = 越界） */

/* 取当前曲目的字符封面（按尺寸与字符集缓存）；返回是否有内容。
 * 远程后端首次调用会触发异步获取，未就绪时返回 0。 */
int player_cover_rows(int cols, int rows, int charset, char *out, size_t out_size);

/* 当前曲目的封面文件路径（后端筛选“当前曲目信息”时的结果）。
 * @return 0 有封面；-1 无当前曲目或无封面 */
int player_cover_path(char *out, size_t out_size);

/* 可视化：levels/peaks 各 max_levels 个（0-255） */
void player_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);
void player_set_visualizer_active(int active);

/* 门面里没有远程音乐源：远程服务器列表、目录浏览与下载都是前端自己的事
 * （remote/remote_store.c、remote/remote_cache.c），前端把下载好的本地
 * 路径经 player_playlist_load() 交给核心即可。 */

#endif /* PLAYER_H */
