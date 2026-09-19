/**
 * @file config_fields.h
 * @brief 标量配置字段表（JSON 键 = config.xml 元素名）的单一来源
 *
 * 这张表同时被 `config_json.c`（Config.GetAll/Set 的读写与钳制）与
 * `config_diff.c`（把前端改动的差异算成最小补丁）使用。放在头文件里以
 * `static const` 形式共享：两个 TU 各持一份只读副本（约 2 KB），换来"一处
 * 定义、不可能漂移"。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CONFIG_FIELDS_H
#define CONFIG_FIELDS_H

#include <stddef.h>

#include "types.h"

typedef enum {
    CFG_INT = 0,
    CFG_FLOAT,
    CFG_STRING
} ConfigFieldType;

typedef struct {
    const char *key;        /* JSON 键 = XML 元素名 */
    ConfigFieldType type;
    size_t offset;
    size_t size;            /* CFG_STRING 的缓冲长度 */
    int min;                /* CFG_INT / CFG_FLOAT 的钳制范围（<= max 时生效） */
    int max;
} ConfigFieldDef;

/* 标量字段表：顺序即 JSON 输出顺序（按 config.xml 的分区排列）。
 * section 字段决定它落在哪个分区对象里。 */
typedef struct {
    ConfigFieldDef field;
    const char *section;
} ConfigFieldEntry;

#define OFF(field) offsetof(AppConfig, field)

static const ConfigFieldEntry k_fields[] = {
    /* paths */
    {"default_startup_path", CFG_STRING, OFF(default_startup_path), MAX_PATH_LEN, 0, 0, "paths"},
    {"last_opened_path",     CFG_STRING, OFF(last_opened_path),     MAX_PATH_LEN, 0, 0, "paths"},
    {"last_played_folder_path", CFG_STRING, OFF(last_played_folder_path), MAX_PATH_LEN, 0, 0, "paths"},
    {"last_played_track_path",  CFG_STRING, OFF(last_played_track_path),  MAX_PATH_LEN, 0, 0, "paths"},

    /* theme */
    {"playlist_fg",  CFG_INT, OFF(theme.playlist_fg),  0, 0, 255, "theme"},
    {"playlist_bg",  CFG_INT, OFF(theme.playlist_bg),  0, -1, 255, "theme"},
    {"controls_fg",  CFG_INT, OFF(theme.controls_fg),  0, 0, 255, "theme"},
    {"controls_bg",  CFG_INT, OFF(theme.controls_bg),  0, -1, 255, "theme"},
    {"lyrics_fg",    CFG_INT, OFF(theme.lyrics_fg),    0, 0, 255, "theme"},
    {"lyrics_bg",    CFG_INT, OFF(theme.lyrics_bg),    0, -1, 255, "theme"},
    {"sidebar_fg",   CFG_INT, OFF(theme.sidebar_fg),   0, 0, 255, "theme"},
    {"sidebar_bg",   CFG_INT, OFF(theme.sidebar_bg),   0, -1, 255, "theme"},
    {"highlight_fg", CFG_INT, OFF(theme.highlight_fg), 0, 0, 255, "theme"},
    {"highlight_bg", CFG_INT, OFF(theme.highlight_bg), 0, 0, 255, "theme"},
    {"border_fg",    CFG_INT, OFF(theme.border_fg),    0, 0, 255, "theme"},
    {"border_bg",    CFG_INT, OFF(theme.border_bg),    0, -1, 255, "theme"},

    /* preferences */
    {"auto_play_on_start",       CFG_INT, OFF(auto_play_on_start), 0, 0, 1, "preferences"},
    {"remember_last_path",       CFG_INT, OFF(remember_last_path), 0, 0, 1, "preferences"},
    {"clear_history_on_startup", CFG_INT, OFF(clear_history_on_startup), 0, 0, 1, "preferences"},
    {"resume_last_playback",     CFG_INT, OFF(resume_last_playback), 0, 0, 1, "preferences"},
    {"last_played_position",     CFG_INT, OFF(last_played_position), 0, 0, 0, "preferences"},
    {"ui_language",              CFG_STRING, OFF(ui_language), sizeof(((AppConfig *)0)->ui_language), 0, 0, "preferences"},
    {"volume_percent",           CFG_INT, OFF(volume_percent), 0, 0, 100, "preferences"},
    {"audio_latency_ms",         CFG_INT, OFF(audio_latency_ms), 0, 0, 0, "preferences"},
    {"show_lyrics_panel",        CFG_INT, OFF(show_lyrics_panel), 0, 0, 1, "preferences"},
    {"default_loop_mode",        CFG_INT, OFF(default_play_mode), 0, 0, PLAY_MODE_COUNT - 1, "preferences"},
    {"default_play_mode",        CFG_INT, OFF(default_play_mode), 0, 0, PLAY_MODE_COUNT - 1, "preferences"},
    {"advanced_play_modes_enabled", CFG_INT, OFF(advanced_play_modes_enabled), 0, 0, 1, "preferences"},
    {"default_playback_speed",   CFG_FLOAT, OFF(default_playback_speed), 0, 0, 0, "preferences"},
    {"show_album_cover",         CFG_INT, OFF(show_album_cover), 0, 0, 1, "preferences"},
    {"seamless_preload",         CFG_INT, OFF(seamless_preload), 0, 0, 1, "preferences"},
    {"lyrics_alignment",         CFG_INT, OFF(lyrics_alignment), 0, 0, 2, "preferences"},
    {"audio_backend",            CFG_INT, OFF(audio_backend), 0, 0, 3, "preferences"},
    {"sort_mode",                CFG_INT, OFF(sort_mode), 0, 0, 4, "preferences"},
    {"cue_encoding",             CFG_INT, OFF(cue_encoding), 0, 0, 0, "preferences"},

    /* info display（config v5） */
    {"info_preset",         CFG_INT, OFF(info_preset), 0, 0, 2, "preferences"},
    {"info_fields",         CFG_INT, OFF(info_fields_mask), 0, 0, 0, "preferences"},
    {"info_show_cover",     CFG_INT, OFF(info_show_cover), 0, 0, 1, "preferences"},
    {"info_cover_cols",     CFG_INT, OFF(info_cover_cols), 0, 4, 40, "preferences"},
    {"info_cover_rows",     CFG_INT, OFF(info_cover_rows), 0, 2, 20, "preferences"},
    {"info_cover_charset",  CFG_INT, OFF(info_cover_charset), 0, 0, 2, "preferences"},
    {"info_show_progress",  CFG_INT, OFF(info_show_progress), 0, 0, 1, "preferences"},
    {"info_progress_style", CFG_INT, OFF(info_progress_style), 0, 0, 3, "preferences"},
    {"info_lyrics_lines",   CFG_INT, OFF(info_lyrics_lines), 0, 0, 2, "preferences"},
};

#define FIELD_COUNT ((int)(sizeof(k_fields) / sizeof(k_fields[0])))

#endif /* CONFIG_FIELDS_H */
