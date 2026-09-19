/**
 * @file lyrics.h
 * @brief 歌词引擎（后端）：加载、解析、来源偏好与播放推进
 *
 * 架构边界（2026-09-15）：歌词是**当前曲目信息**的一部分，归后端；界面只
 * 负责把它画出来。本模块因此不包含任何 ncurses 代码，也不认识界面状态。
 *
 * 职责：
 *   - 从音频文件内嵌标签或同名 `.lrc` 加载歌词（编码嗅探、HTML 实体解码）；
 *   - 持有歌词行与推进状态（`current_index` / `highlight_count`）；
 *   - 按播放位置推进（`lyrics_tick()`），供 `core_tick()` 每轮调用；
 *   - 提供快照（`lyrics_snapshot`）与分页文档（`lyrics_page`）给界面 / D-Bus。
 *
 * 不归本模块：渲染（`ui/lyrics.c`）、歌词光标模式与跳转（界面私有）、
 * 来源偏好的持久化（前端的内容库；后端只经 `lyrics_set_source_hook()` 回调）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef LYRICS_H
#define LYRICS_H

#include <pthread.h>
#include <stddef.h>

#include "types.h"

/* 歌词文本上限（与界面/D-Bus 分页共用） */
#define MAX_LYRIC_LINES    500
#define MAX_LYRIC_TEXT_LEN 256

/* 单页文档的行数上限（与 D-Bus 分页上限一致） */
#define LYRICS_PAGE_MAX 200

typedef struct {
    double timestamp;                 /* 时间戳（秒，含毫秒） */
    char text[MAX_LYRIC_TEXT_LEN];
} LyricLine;

typedef struct {
    LyricLine lines[MAX_LYRIC_LINES];
    int count;
    int current_index;                /* 当前高亮起始行，-1 = 无 */
    int highlight_count;              /* 当前高亮行数（同时间戳最多 2 行） */
    int has_lyrics;
    int has_timestamps;               /* 1 = LRC 时间戳，0 = 纯文本内嵌歌词 */
    int source;                       /* 实际加载来源：LYRICS_SOURCE_* */
    int cursor_index;                 /* **界面私有**：光标模式下的跳转位置 */
    pthread_mutex_t lock;
} Lyrics;

/* 全局歌词状态（由后端拥有；界面只读快照或经分页接口取数据） */
extern Lyrics g_lyrics;

/* ── 加载与推进 ─────────────────────────────────────────────────── */

/* 按来源偏好加载歌词（AUTO 先内嵌后 .lrc；EMBEDDED/EXTERNAL 只取其一）。
 * 未找到时把状态清空。 */
void load_lyrics(const char *audio_path, int lyrics_source);

/* 清空歌词状态（停止播放 / 换曲失败时调用） */
void clear_lyrics(void);

/* 用**新的来源偏好**重新加载当前曲目歌词。
 * 当前曲目路径由后端队列的游标决定，不需要调用方传入。 */
void reload_lyrics_with_source(int new_source);

/* 按播放位置推进歌词高亮；由 core_tick() 每轮调用。
 * @return 1 = 高亮行发生变化（界面据此重绘），0 = 无变化 */
int lyrics_tick(void);

/* 播放位置来源默认是音频层（`audio_get_position_seconds()`）。
 * 单测可注入自己的取值函数，从而确定性地驱动推进而不必启动音频栈；
 * 传 NULL 恢复默认。 */
void lyrics_set_position_source(int (*source)(void));
int lyrics_position_seconds(void);

/* 当前曲目路径（后端队列游标所在条目）；无当前曲目时返回 NULL */
const char *lyrics_current_track_path(void);

/* ── 来源偏好 ───────────────────────────────────────────────────── */

/* 切换来源偏好并**立即生效**（重新加载 + 回调持久化钩子）。
 * @return 0 成功；-1 = 无当前曲目 */
int lyrics_switch_source(int new_source);

/* 实际加载来源（LYRICS_SOURCE_*） */
int lyrics_source(void);

/* 把“当前曲目 + 来源偏好”交给前端持久化（内容库属前端）。
 * 后端不写内容库，只回调；未注册时静默忽略。 */
void lyrics_set_source_hook(void (*hook)(const char *track_path, int source));

/* ── 快照与分页 ─────────────────────────────────────────────────── */

/* 最近一次推进的高亮行；out_current/out_next 可为 NULL。
 * @return 1 = 有歌词 */
int lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps, int *out_source);

typedef struct {
    int total;
    int offset;
    int count;
    int current_index;
    int has_lyrics;
    int has_timestamps;
    int source;
    char track_id[96];
    LyricLine lines[LYRICS_PAGE_MAX];
} LyricsPage;

/* 取一页歌词行（供界面滚动渲染 / D-Bus Lyrics.GetDocument）。
 * @return 写入行数；-1 = 参数非法 */
int lyrics_page(int offset, int count, LyricsPage *out);

/* 当前曲目路径的稳定标识（file://… 的 hash 形式），写入 out */
void lyrics_track_id(char *out, size_t out_size);

#endif /* LYRICS_H */
