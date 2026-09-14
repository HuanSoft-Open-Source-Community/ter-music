/**
 * @file info.h
 * @brief 播放信息快照与渲染（CLI `ter-music show` 与 D-Bus Info 接口共用）
 *
 * 设计要点：
 *  - 本模块只读取全局播放状态，不做任何 ncurses 渲染，因此可在
 *    无界面 daemon 中安全使用；
 *  - 文本渲染与 JSON 渲染共用同一份快照，保证 `ter-music show`、
 *    `Info.GetDisplay`、`Info.GetInfo` 三者内容一致；
 *  - 封面文本（盲文/ASCII）按 (路径, 尺寸, 字符集) 缓存，避免
 *    `show --watch` 或高频 D-Bus 调用反复解码图片。
 *
 * 线程约束：仅由持有播放状态的主循环线程（TUI 事件循环或 daemon 主循环）调用。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef INFO_H
#define INFO_H

#include <stddef.h>

#include "types.h"
#include "ui/lyrics.h"

/* ── 基本信息字段位掩码 ─────────────────────────────────────────── */
#define INFO_FIELD_STATE   0x0001
#define INFO_FIELD_MODE    0x0002
#define INFO_FIELD_INDEX   0x0004
#define INFO_FIELD_QUEUE   0x0008
#define INFO_FIELD_TITLE   0x0010
#define INFO_FIELD_ARTIST  0x0020
#define INFO_FIELD_ALBUM   0x0040
#define INFO_FIELD_FORMAT  0x0080
#define INFO_FIELD_PATH    0x0100
#define INFO_FIELD_VOLUME  0x0200
#define INFO_FIELD_SPEED   0x0400
#define INFO_FIELD_ALL     0x07FF
#define INFO_FIELD_COMPACT (INFO_FIELD_STATE | INFO_FIELD_TITLE | INFO_FIELD_ARTIST)

/* ── 预设 ───────────────────────────────────────────────────────── */
#define INFO_PRESET_FULL    0
#define INFO_PRESET_COMPACT 1
#define INFO_PRESET_CUSTOM  2
#define INFO_PRESET_COUNT   3

/* ── 进度样式 ───────────────────────────────────────────────────── */
#define INFO_PROGRESS_BAR_TIME     0
#define INFO_PROGRESS_TIME         1
#define INFO_PROGRESS_PERCENT      2
#define INFO_PROGRESS_TIME_PERCENT 3
#define INFO_PROGRESS_STYLE_COUNT  4

/* ── 封面字符集 ─────────────────────────────────────────────────── */
#define INFO_COVER_BRAILLE 0
#define INFO_COVER_ASCII   1
#define INFO_COVER_CHARSET_COUNT 2

/* ── 歌词行数 ───────────────────────────────────────────────────── */
#define INFO_LYRICS_OFF     0
#define INFO_LYRICS_CURRENT 1
#define INFO_LYRICS_BOTH    2
#define INFO_LYRICS_MAX     2

/* ── 尺寸约束 ───────────────────────────────────────────────────── */
#define INFO_COVER_COLS_MIN 4
#define INFO_COVER_COLS_MAX 40
#define INFO_COVER_ROWS_MIN 2
#define INFO_COVER_ROWS_MAX 20

#define INFO_WIDTH_MIN 40
#define INFO_WIDTH_MAX 400
#define INFO_DEFAULT_WIDTH 80

#define INFO_URI_MAX (MAX_PATH_LEN * 3 + 16)
#define INFO_TEXT_MAX 40960
#define INFO_JSON_MAX 65536
#define INFO_COVER_TEXT_MAX 8192
#define INFO_MIN_TEXT_WIDTH 40   /* 文本块最小宽度；不足时省略封面 */

/* ── 渲染选项（配置值 + CLI/D-Bus 覆盖值，-1/<=0 表示“沿用配置”） ── */
typedef struct {
    int preset;         /* INFO_PRESET_* */
    int fields_mask;    /* INFO_FIELD_* */
    int show_cover;
    int cover_cols;
    int cover_rows;
    int cover_charset;  /* INFO_COVER_* */
    int show_progress;
    int progress_style; /* INFO_PROGRESS_* */
    int lyrics_lines;   /* INFO_LYRICS_* */
    int width;          /* 输出列宽 */
    int one_line;       /* 1 = 强制单行输出 */
} InfoRenderOptions;

/* ── 快照结构 ───────────────────────────────────────────────────── */
typedef struct {
    int valid;                     /* 是否存在当前曲目 */
    int index;                     /* 播放列表物理索引（0 基） */
    int playlist_total;
    int queue_position;            /* 队列中的位置（0 基），-1 表示不在队列 */
    int queue_count;
    char track_id[96];
    char path[MAX_PATH_LEN];
    char uri[INFO_URI_MAX];
    int is_remote;
    int cue_track_number;          /* 0 = 非 CUE 子轨 */
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    int has_cover;
    char cover_path[MAX_PATH_LEN];
} InfoTrack;

typedef struct {
    PlayState state;
    int position_seconds;
    int duration_seconds;
    int volume_percent;
    float speed;
    PlayMode play_mode;
    int can_seek;
} InfoPlayback;

typedef struct {
    int has_lyrics;
    int has_timestamps;
    int source;                    /* LYRICS_SOURCE_* */
    int current_index;             /* -1 = 无当前行 */
    int next_index;                /* -1 = 无下一行 */
    double current_timestamp;
    double next_timestamp;
    char current_text[MAX_LYRIC_TEXT_LEN];
    char next_text[MAX_LYRIC_TEXT_LEN];
} InfoLyrics;

typedef struct {
    int is_daemon;
    int pid;
    const char *version;
    const char *bus_name;          /* 可为 NULL */
    int has_primary_name;
} InfoInstance;

/* ── 选项处理 ───────────────────────────────────────────────────── */

/* 全部字段“沿用配置” */
void info_options_default(InfoRenderOptions *opts);

/* 用 g_app_config 中的 info_* 配置填充选项 */
void info_options_from_config(InfoRenderOptions *opts);

/* 解析 "key=value;key=value" 形式的覆盖串（空串 / NULL = 不做覆盖）。
 * 可用键：preset, fields, cover, cover_cols, cover_rows, cover_charset,
 *         progress, progress_style, lyrics, width, one_line
 * @return 0 成功，-1 存在无法识别的键或非法取值 */
int info_options_parse(InfoRenderOptions *opts, const char *options);

/* ── 稳定标识（JSON/D-Bus/CLI 参数共用的机器可读名称） ──────────── */
const char *info_play_state_id(PlayState state);
const char *info_play_mode_id(PlayMode mode);
int info_play_mode_from_id(const char *id);        /* -1 = 未知 */
const char *info_preset_id(int preset);
const char *info_progress_style_id(int style);
const char *info_cover_charset_id(int charset);
const char *info_lyrics_source_id(int source);
int info_field_bit_from_name(const char *name);    /* 0 = 未知 */

/* MPRIS LoopStatus / Shuffle 映射（session.c 与 Info JSON 共用） */
const char *info_loop_status_mpris(PlayMode mode);
int info_shuffle_mpris(PlayMode mode);

/* 基本信息字段表（顺序即渲染顺序，供设置页多选菜单复用） */
typedef struct {
    int bit;
    const char *name;       /* 机器名，例如 "title" */
    const char *label_key;  /* i18n 键，例如 "settings.info.fields.title" */
} InfoFieldDef;

int info_field_count(void);
const InfoFieldDef *info_field_at(int index);       /* 越界返回 NULL */

/* ── 快照采集 ───────────────────────────────────────────────────── */
void info_track_snapshot(InfoTrack *out);
void info_playback_snapshot(InfoPlayback *out);
void info_lyrics_snapshot(InfoLyrics *out);

/* 轨道 ID 与 file:// URI 互转（MPRIS 与 Info 接口共用） */
void info_build_track_id(char *dest, size_t dest_size, const char *track_path);
void info_build_file_uri(const char *path, char *uri, size_t uri_size);
int  info_uri_to_path(const char *uri, char *path, size_t path_size);

/* ── 音频技术信息（TUI 信息栏与 CLI 信息块共用） ────────────────── */
void info_format_audio_fields(char *rate, size_t rate_size,
                              char *depth, size_t depth_size,
                              char *bitrate, size_t bitrate_size,
                              char *codec, size_t codec_size);
int  info_format_audio_summary(char *out, size_t out_size);

/* ── 封面文本 ───────────────────────────────────────────────────── */

/* cols/rows <= 0、charset < 0 时使用配置值。
 * @return 1 = 有封面文本，0 = 无封面/解码失败（out 为空串） */
int info_cover_text(int cols, int rows, int charset, char *out, size_t out_size);
void info_release_cover_cache(void);

/* 把非法 UTF-8 字节替换为 '?'（原地修改）。
 * 渲染结果会送入 D-Bus，而 libdbus 遇到非法 UTF-8 会直接 abort，
 * 因此所有对外输出在返回前都做一次净化，作为最后一道防线。 */
void info_sanitize_utf8(char *text);

/* ── 渲染 ───────────────────────────────────────────────────────── */

/* 多行文本渲染，等价于 `ter-music show`。out 需至少 INFO_TEXT_MAX 字节。
 * @return 写入的字节数（不含结尾 NUL），失败返回 -1 */
int info_render_text(const InfoRenderOptions *opts, char *out, size_t out_size);

/* 单行渲染（--one-line / daemon status） */
int info_render_one_line(const InfoRenderOptions *opts, char *out, size_t out_size);

/* 完整 JSON 快照（含 text 字段）。out 建议 INFO_JSON_MAX 字节 */
int info_render_json(char *out, size_t out_size, const InfoInstance *instance,
                     unsigned long long revision);
int info_render_track_json(char *out, size_t out_size);
int info_render_progress_json(char *out, size_t out_size);
int info_render_lyrics_json(char *out, size_t out_size);
int info_render_instance_json(char *out, size_t out_size, const InfoInstance *instance);

#endif /* INFO_H */
