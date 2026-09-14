/**
 * @file info.c
 * @brief 播放信息快照采集与渲染（CLI `ter-music show` / D-Bus Info 接口）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "info/info.h"

#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "remote/remote.h"
#include "ui/braille/braille_art.h"
#include "ui/utf8.h"
#include "util/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define INFO_MAX_LINES 24
#define INFO_LINE_MAX 2048

/* ── 稳定名称表 ─────────────────────────────────────────────────── */

static const char *const k_play_mode_ids[PLAY_MODE_COUNT] = {
    "sequential",
    "single_repeat",
    "list_repeat",
    "shuffle_once",
    "shuffle_repeat",
    "folder_sequential",
    "folder_repeat",
    "folder_shuffle",
    "folder_shuffle_repeat",
    "album_sequential",
    "album_repeat",
    "album_shuffle",
    "album_shuffle_repeat",
    "artist_sequential",
    "artist_repeat",
    "artist_shuffle",
    "artist_shuffle_repeat"
};

/* 渲染顺序即此表顺序（state/mode 在最前，format 在 path 之前） */
static const InfoFieldDef k_info_fields[] = {
    { INFO_FIELD_STATE,  "state",  "settings.info.fields.state"  },
    { INFO_FIELD_MODE,   "mode",   "settings.info.fields.mode"   },
    { INFO_FIELD_INDEX,  "index",  "settings.info.fields.index"  },
    { INFO_FIELD_QUEUE,  "queue",  "settings.info.fields.queue"  },
    { INFO_FIELD_TITLE,  "title",  "settings.info.fields.title"  },
    { INFO_FIELD_ARTIST, "artist", "settings.info.fields.artist" },
    { INFO_FIELD_ALBUM,  "album",  "settings.info.fields.album"  },
    { INFO_FIELD_FORMAT, "format", "settings.info.fields.format" },
    { INFO_FIELD_PATH,   "path",   "settings.info.fields.path"   },
    { INFO_FIELD_VOLUME, "volume", "settings.info.fields.volume" },
    { INFO_FIELD_SPEED,  "speed",  "settings.info.fields.speed"  }
};

static const int k_info_field_count =
    (int)(sizeof(k_info_fields) / sizeof(k_info_fields[0]));

/* ── 封面文本缓存（仅主循环线程访问） ───────────────────────────── */

typedef struct {
    int valid;
    int cols;
    int rows;
    int charset;
    char path[MAX_PATH_LEN];
    char text[INFO_COVER_TEXT_MAX];
} InfoCoverCache;

static InfoCoverCache g_cover_cache;

/* ── 选项 ───────────────────────────────────────────────────────── */

void info_options_default(InfoRenderOptions *opts)
{
    if (!opts) {
        return;
    }
    opts->preset = INFO_PRESET_FULL;
    opts->fields_mask = INFO_FIELD_ALL;
    opts->show_cover = 1;
    opts->cover_cols = 16;
    opts->cover_rows = 8;
    opts->cover_charset = INFO_COVER_BRAILLE;
    opts->show_progress = 1;
    opts->progress_style = INFO_PROGRESS_BAR_TIME;
    opts->lyrics_lines = INFO_LYRICS_BOTH;
    opts->width = INFO_DEFAULT_WIDTH;
    opts->one_line = 0;
}

/* 预设只决定“显示哪些内容”，封面尺寸/字符集属于用户偏好，保持配置值 */
static void info_apply_preset(InfoRenderOptions *opts, int preset)
{
    if (preset == INFO_PRESET_FULL) {
        opts->fields_mask = INFO_FIELD_ALL;
        opts->show_cover = 1;
        opts->show_progress = 1;
        opts->progress_style = INFO_PROGRESS_BAR_TIME;
        opts->lyrics_lines = INFO_LYRICS_BOTH;
    } else if (preset == INFO_PRESET_COMPACT) {
        opts->fields_mask = INFO_FIELD_COMPACT;
        opts->show_cover = 0;
        opts->show_progress = 1;
        opts->progress_style = INFO_PROGRESS_TIME_PERCENT;
        opts->lyrics_lines = INFO_LYRICS_CURRENT;
    }
}

void info_options_from_config(InfoRenderOptions *opts)
{
    if (!opts) {
        return;
    }

    info_options_default(opts);
    opts->preset = g_app_config.info_preset;
    opts->fields_mask = g_app_config.info_fields_mask;
    opts->show_cover = g_app_config.info_show_cover;
    opts->cover_cols = g_app_config.info_cover_cols;
    opts->cover_rows = g_app_config.info_cover_rows;
    opts->cover_charset = g_app_config.info_cover_charset;
    opts->show_progress = g_app_config.info_show_progress;
    opts->progress_style = g_app_config.info_progress_style;
    opts->lyrics_lines = g_app_config.info_lyrics_lines;
    opts->width = INFO_DEFAULT_WIDTH;
    opts->one_line = 0;

    if (opts->preset == INFO_PRESET_FULL || opts->preset == INFO_PRESET_COMPACT) {
        info_apply_preset(opts, opts->preset);
    }

    /* 值域校正（配置文件可能被手工编辑） */
    if (opts->preset < 0 || opts->preset >= INFO_PRESET_COUNT) {
        opts->preset = INFO_PRESET_CUSTOM;
    }
    opts->fields_mask &= INFO_FIELD_ALL;
    opts->show_cover = opts->show_cover ? 1 : 0;
    opts->show_progress = opts->show_progress ? 1 : 0;
    if (opts->cover_charset < 0 || opts->cover_charset >= INFO_COVER_CHARSET_COUNT) {
        opts->cover_charset = INFO_COVER_BRAILLE;
    }
    if (opts->progress_style < 0 || opts->progress_style >= INFO_PROGRESS_STYLE_COUNT) {
        opts->progress_style = INFO_PROGRESS_BAR_TIME;
    }
    if (opts->lyrics_lines < 0 || opts->lyrics_lines > INFO_LYRICS_MAX) {
        opts->lyrics_lines = INFO_LYRICS_BOTH;
    }
    if (opts->cover_cols < INFO_COVER_COLS_MIN || opts->cover_cols > INFO_COVER_COLS_MAX) {
        opts->cover_cols = 16;
    }
    if (opts->cover_rows < INFO_COVER_ROWS_MIN || opts->cover_rows > INFO_COVER_ROWS_MAX) {
        opts->cover_rows = 8;
    }
}

static int info_parse_bool(const char *value, int *out)
{
    if (!value || !out) {
        return -1;
    }
    if (strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "false") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int info_parse_mask(const char *value, int *out)
{
    if (!value || !out) {
        return -1;
    }
    if (strcasecmp(value, "all") == 0) {
        *out = INFO_FIELD_ALL;
        return 0;
    }
    if (strcasecmp(value, "none") == 0 || value[0] == '\0') {
        *out = 0;
        return 0;
    }

    int mask = 0;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", value);
    for (char *part = strtok(buffer, ","); part != NULL; part = strtok(NULL, ",")) {
        while (*part == ' ') {
            part++;
        }
        char *end = part + strlen(part);
        while (end > part && end[-1] == ' ') {
            *--end = '\0';
        }
        int bit = info_field_bit_from_name(part);
        if (bit == 0) {
            return -1;
        }
        mask |= bit;
    }
    *out = mask;
    return 0;
}

int info_options_parse(InfoRenderOptions *opts, const char *options)
{
    if (!opts || !options || options[0] == '\0') {
        return 0;
    }

    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", options);

    for (char *pair = strtok(buffer, ";"); pair != NULL; pair = strtok(NULL, ";")) {
        while (*pair == ' ') {
            pair++;
        }
        if (*pair == '\0') {
            continue;
        }

        char *eq = strchr(pair, '=');
        char *key = pair;
        char *value = NULL;
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        }
        char *key_end = key + strlen(key);
        while (key_end > key && key_end[-1] == ' ') {
            *--key_end = '\0';
        }
        if (value) {
            while (*value == ' ') {
                value++;
            }
            char *value_end = value + strlen(value);
            while (value_end > value && value_end[-1] == ' ') {
                *--value_end = '\0';
            }
        }

        if (strcmp(key, "preset") == 0 && value) {
            if (strcmp(value, "full") == 0) {
                opts->preset = INFO_PRESET_FULL;
            } else if (strcmp(value, "compact") == 0) {
                opts->preset = INFO_PRESET_COMPACT;
            } else if (strcmp(value, "custom") == 0) {
                opts->preset = INFO_PRESET_CUSTOM;
            } else {
                return -1;
            }
            info_apply_preset(opts, opts->preset);
        } else if (strcmp(key, "fields") == 0 && value) {
            int mask = 0;
            if (info_parse_mask(value, &mask) != 0) {
                return -1;
            }
            opts->fields_mask = mask;
        } else if (strcmp(key, "cover") == 0 && value) {
            int on = 0;
            if (info_parse_bool(value, &on) != 0) {
                return -1;
            }
            opts->show_cover = on;
        } else if (strcmp(key, "cover_cols") == 0 && value) {
            int cols = atoi(value);
            if (cols < INFO_COVER_COLS_MIN || cols > INFO_COVER_COLS_MAX) {
                return -1;
            }
            opts->cover_cols = cols;
        } else if (strcmp(key, "cover_rows") == 0 && value) {
            int rows = atoi(value);
            if (rows < INFO_COVER_ROWS_MIN || rows > INFO_COVER_ROWS_MAX) {
                return -1;
            }
            opts->cover_rows = rows;
        } else if (strcmp(key, "cover_charset") == 0 && value) {
            if (strcmp(value, "braille") == 0) {
                opts->cover_charset = INFO_COVER_BRAILLE;
            } else if (strcmp(value, "ascii") == 0) {
                opts->cover_charset = INFO_COVER_ASCII;
            } else {
                return -1;
            }
        } else if (strcmp(key, "progress") == 0 && value) {
            int on = 0;
            if (info_parse_bool(value, &on) != 0) {
                return -1;
            }
            opts->show_progress = on;
        } else if (strcmp(key, "progress_style") == 0 && value) {
            if (strcmp(value, "bar") == 0) {
                opts->progress_style = INFO_PROGRESS_BAR_TIME;
            } else if (strcmp(value, "time") == 0) {
                opts->progress_style = INFO_PROGRESS_TIME;
            } else if (strcmp(value, "percent") == 0) {
                opts->progress_style = INFO_PROGRESS_PERCENT;
            } else if (strcmp(value, "time+percent") == 0) {
                opts->progress_style = INFO_PROGRESS_TIME_PERCENT;
            } else {
                return -1;
            }
        } else if (strcmp(key, "lyrics") == 0 && value) {
            int lines = atoi(value);
            if (lines < 0 || lines > INFO_LYRICS_MAX) {
                return -1;
            }
            opts->lyrics_lines = lines;
        } else if (strcmp(key, "width") == 0 && value) {
            int width = atoi(value);
            if (width < INFO_WIDTH_MIN || width > INFO_WIDTH_MAX) {
                return -1;
            }
            opts->width = width;
        } else if (strcmp(key, "one_line") == 0 && value) {
            int on = 0;
            if (info_parse_bool(value, &on) != 0) {
                return -1;
            }
            opts->one_line = on;
        } else {
            return -1;
        }
    }

    return 0;
}

/* ── 稳定名称 ───────────────────────────────────────────────────── */

const char *info_play_state_id(PlayState state)
{
    switch (state) {
        case PLAY_STATE_PLAYING: return "playing";
        case PLAY_STATE_PAUSED:  return "paused";
        case PLAY_STATE_STOPPED:
        default:                 return "stopped";
    }
}

const char *info_play_mode_id(PlayMode mode)
{
    if (mode < 0 || mode >= PLAY_MODE_COUNT) {
        return k_play_mode_ids[0];
    }
    return k_play_mode_ids[mode];
}

int info_play_mode_from_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < PLAY_MODE_COUNT; i++) {
        if (strcmp(id, k_play_mode_ids[i]) == 0) {
            return i;
        }
    }
    /* 允许纯数字索引 */
    if (id[0] >= '0' && id[0] <= '9') {
        int value = atoi(id);
        if (value >= 0 && value < PLAY_MODE_COUNT) {
            return value;
        }
    }
    return -1;
}

const char *info_preset_id(int preset)
{
    switch (preset) {
        case INFO_PRESET_FULL:    return "full";
        case INFO_PRESET_COMPACT: return "compact";
        case INFO_PRESET_CUSTOM:  return "custom";
        default:                  return "custom";
    }
}

const char *info_progress_style_id(int style)
{
    switch (style) {
        case INFO_PROGRESS_BAR_TIME:     return "bar";
        case INFO_PROGRESS_TIME:         return "time";
        case INFO_PROGRESS_PERCENT:      return "percent";
        case INFO_PROGRESS_TIME_PERCENT: return "time+percent";
        default:                         return "bar";
    }
}

const char *info_cover_charset_id(int charset)
{
    return (charset == INFO_COVER_ASCII) ? "ascii" : "braille";
}

const char *info_lyrics_source_id(int source)
{
    switch (source) {
        case LYRICS_SOURCE_EMBEDDED: return "embedded";
        case LYRICS_SOURCE_EXTERNAL: return "external";
        default:                     return "none";
    }
}

int info_field_bit_from_name(const char *name)
{
    if (!name) {
        return 0;
    }
    for (int i = 0; i < k_info_field_count; i++) {
        if (strcmp(name, k_info_fields[i].name) == 0) {
            return k_info_fields[i].bit;
        }
    }
    return 0;
}

int info_field_count(void)
{
    return k_info_field_count;
}

const InfoFieldDef *info_field_at(int index)
{
    if (index < 0 || index >= k_info_field_count) {
        return NULL;
    }
    return &k_info_fields[index];
}

/* ── 快照采集 ───────────────────────────────────────────────────── */

void info_build_track_id(char *dest, size_t dest_size, const char *track_path)
{
    unsigned long long hash = 1469598103934665603ULL;
    const unsigned char *ptr = (const unsigned char *)(track_path ? track_path : "");

    while (*ptr != '\0') {
        hash ^= (unsigned long long)(*ptr++);
        hash *= 1099511628211ULL;
    }

    snprintf(dest, dest_size, "/org/mpris/MediaPlayer2/Track_%016llx", hash);
}

void info_build_file_uri(const char *path, char *uri, size_t uri_size)
{
    if (!uri || uri_size == 0) {
        return;
    }
    uri[0] = '\0';
    if (!path || path[0] == '\0') {
        return;
    }

    size_t position = 0;
    const char *prefix = "file://";
    for (size_t i = 0; prefix[i] != '\0' && position + 1 < uri_size; i++) {
        uri[position++] = prefix[i];
    }

    for (const unsigned char *ptr = (const unsigned char *)path;
         *ptr != '\0' && position + 1 < uri_size;
         ptr++) {
        unsigned char value = *ptr;
        if (isalnum(value) || value == '/' || value == '-' ||
            value == '_' || value == '.' || value == '~') {
            uri[position++] = (char)value;
        } else if (position + 3 < uri_size) {
            uri[position++] = '%';
            uri[position++] = "0123456789ABCDEF"[value >> 4];
            uri[position++] = "0123456789ABCDEF"[value & 0x0F];
        } else {
            break;
        }
    }
    uri[position] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int info_uri_to_path(const char *uri, char *path, size_t path_size)
{
    if (!uri || !path || path_size == 0) {
        return -1;
    }
    path[0] = '\0';

    const char *start = uri;
    if (strncmp(uri, "file://", 7) == 0) {
        start = uri + 7;
        /* 兼容 file:///path 与 file://host/path（忽略本地主机名） */
        if (*start == '/') {
            /* 绝对路径，无需处理主机名 */
        } else {
            const char *slash = strchr(start, '/');
            if (!slash) {
                return -1;
            }
            start = slash;
        }
    } else if (strstr(uri, "://") != NULL) {
        /* 远程 URL：原样返回，交由 remote/ 层处理 */
        snprintf(path, path_size, "%s", uri);
        return 0;
    }

    size_t pos = 0;
    for (const char *ptr = start; *ptr != '\0' && pos + 1 < path_size; ptr++) {
        if (*ptr == '%' && ptr[1] != '\0' && ptr[2] != '\0') {
            int hi = hex_value(ptr[1]);
            int lo = hex_value(ptr[2]);
            if (hi >= 0 && lo >= 0) {
                path[pos++] = (char)((hi << 4) | lo);
                ptr += 2;
                continue;
            }
        }
        path[pos++] = *ptr;
    }
    path[pos] = '\0';
    return 0;
}

void info_track_snapshot(InfoTrack *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->queue_position = -1;
    out->playlist_total = playlist_count();

    if (g_current_play_index < 0 || g_current_play_index >= out->playlist_total) {
        return;
    }

    char track_path[MAX_PATH_LEN];
    if (playlist_get_track_path(g_current_play_index, track_path, sizeof(track_path)) != 0) {
        return;
    }

    Track track;
    if (get_track_metadata(g_current_play_index, &track) != 0) {
        return;
    }

    out->valid = 1;
    out->index = g_current_play_index;
    info_build_track_id(out->track_id, sizeof(out->track_id), track_path);
    snprintf(out->path, sizeof(out->path), "%s", track_path);
    out->is_remote = remote_is_remote_path(track_path) ? 1 : 0;
    out->cue_track_number = track.cue_track_number;
    snprintf(out->title, sizeof(out->title), "%s", track.title);
    snprintf(out->artist, sizeof(out->artist), "%s", track.artist);
    snprintf(out->album, sizeof(out->album), "%s", track.album);

    if (out->is_remote) {
        snprintf(out->uri, sizeof(out->uri), "%s", track_path);
    } else {
        info_build_file_uri(track_path, out->uri, sizeof(out->uri));
    }

    out->queue_count = g_play_queue.count;
    for (int i = 0; i < g_play_queue.count; i++) {
        if (g_play_queue.indices[i] == g_current_play_index) {
            out->queue_position = i;
            break;
        }
    }

    if (get_current_album_cover_path(out->cover_path, sizeof(out->cover_path)) == 0) {
        out->has_cover = 1;
    }
}

void info_playback_snapshot(InfoPlayback *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = g_play_state;
    out->position_seconds = audio_get_position_seconds();
    out->duration_seconds = audio_get_duration_seconds();
    out->volume_percent = get_volume_percent();
    out->speed = g_playback_speed;
    out->play_mode = g_play_mode;
    out->can_seek = (g_current_play_index >= 0 &&
                     g_current_play_index < playlist_count() &&
                     out->duration_seconds > 0);
    if (out->duration_seconds > 0 && out->position_seconds > out->duration_seconds) {
        out->position_seconds = out->duration_seconds;
    }
}

void info_lyrics_snapshot(InfoLyrics *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->current_index = -1;
    out->next_index = -1;

    pthread_mutex_lock(&g_lyrics.lock);

    out->has_lyrics = g_lyrics.has_lyrics;
    out->has_timestamps = g_lyrics.has_timestamps;
    out->source = g_lyrics.source;

    int count = g_lyrics.count;
    if (out->has_lyrics && count > 0) {
        if (!out->has_timestamps) {
            /* 无时间戳的纯文本歌词：固定取前两行 */
            out->current_index = 0;
            out->next_index = (count > 1) ? 1 : -1;
        } else {
            /* 直接按播放位置定位，避免依赖仅由 TUI 主视图推进的 current_index */
            double position = (double)audio_get_position_seconds();
            for (int i = 0; i < count; i++) {
                if (g_lyrics.lines[i].timestamp <= position) {
                    out->current_index = i;
                } else {
                    break;
                }
            }
            if (out->current_index >= 0 && out->current_index + 1 < count) {
                out->next_index = out->current_index + 1;
            }
        }

        if (out->current_index >= 0 && out->current_index < count) {
            out->current_timestamp = g_lyrics.lines[out->current_index].timestamp;
            snprintf(out->current_text, sizeof(out->current_text), "%s",
                     g_lyrics.lines[out->current_index].text);
        }
        if (out->next_index >= 0 && out->next_index < count) {
            out->next_timestamp = g_lyrics.lines[out->next_index].timestamp;
            snprintf(out->next_text, sizeof(out->next_text), "%s",
                     g_lyrics.lines[out->next_index].text);
        }
    }

    pthread_mutex_unlock(&g_lyrics.lock);
}

/* ── 音频技术信息 ───────────────────────────────────────────────── */

void info_format_audio_fields(char *rate, size_t rate_size,
                              char *depth, size_t depth_size,
                              char *bitrate, size_t bitrate_size,
                              char *codec, size_t codec_size)
{
    if (rate && rate_size) {
        if (g_audio_sample_rate > 0) {
            snprintf(rate, rate_size, "%dHz", g_audio_sample_rate);
        } else {
            snprintf(rate, rate_size, "--");
        }
    }
    if (depth && depth_size) {
        if (g_audio_bit_depth > 0) {
            snprintf(depth, depth_size, "%dbit", g_audio_bit_depth);
        } else {
            snprintf(depth, depth_size, "--");
        }
    }
    if (bitrate && bitrate_size) {
        if (g_audio_bit_rate > 0) {
            snprintf(bitrate, bitrate_size, "%dkbps", g_audio_bit_rate / 1000);
        } else {
            snprintf(bitrate, bitrate_size, "--");
        }
    }
    if (codec && codec_size) {
        if (g_audio_codec_name[0] != '\0') {
            char upper[32];
            int i;
            for (i = 0; g_audio_codec_name[i] && i < (int)sizeof(upper) - 1; i++) {
                upper[i] = (i == 0)
                    ? (char)toupper((unsigned char)g_audio_codec_name[i])
                    : g_audio_codec_name[i];
            }
            upper[i] = '\0';
            snprintf(codec, codec_size, "%s", upper);
        } else {
            snprintf(codec, codec_size, "--");
        }
    }
}

int info_format_audio_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    char rate[32], depth[32], bitrate[32], codec[32];
    info_format_audio_fields(rate, sizeof(rate), depth, sizeof(depth),
                             bitrate, sizeof(bitrate), codec, sizeof(codec));

    size_t pos = 0;
    const char *parts[] = { codec, rate, depth, bitrate };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (strcmp(parts[i], "--") == 0) {
            continue;
        }
        int written = snprintf(out + pos, out_size - pos, "%s%s",
                               pos == 0 ? "" : " ", parts[i]);
        if (written < 0 || (size_t)written >= out_size - pos) {
            pos = out_size - 1;
            break;
        }
        pos += (size_t)written;
    }

    if (pos == 0) {
        snprintf(out, out_size, "--");
        return 3;
    }
    return (int)pos;
}

/* ── 封面文本 ───────────────────────────────────────────────────── */

void info_release_cover_cache(void)
{
    g_cover_cache.valid = 0;
    g_cover_cache.path[0] = '\0';
    g_cover_cache.cols = 0;
    g_cover_cache.rows = 0;
    g_cover_cache.charset = -1;
    g_cover_cache.text[0] = '\0';
}

int info_cover_text(int cols, int rows, int charset, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    char cover_path[MAX_PATH_LEN];
    if (get_current_album_cover_path(cover_path, sizeof(cover_path)) != 0) {
        return 0;
    }

    if (cols <= 0) {
        cols = g_app_config.info_cover_cols;
    }
    if (rows <= 0) {
        rows = g_app_config.info_cover_rows;
    }
    if (charset < 0) {
        charset = g_app_config.info_cover_charset;
    }
    if (cols < INFO_COVER_COLS_MIN) cols = INFO_COVER_COLS_MIN;
    if (cols > INFO_COVER_COLS_MAX) cols = INFO_COVER_COLS_MAX;
    if (rows < INFO_COVER_ROWS_MIN) rows = INFO_COVER_ROWS_MIN;
    if (rows > INFO_COVER_ROWS_MAX) rows = INFO_COVER_ROWS_MAX;
    if (charset != INFO_COVER_ASCII) {
        charset = INFO_COVER_BRAILLE;
    }

    if (g_cover_cache.valid &&
        g_cover_cache.cols == cols &&
        g_cover_cache.rows == rows &&
        g_cover_cache.charset == charset &&
        strcmp(g_cover_cache.path, cover_path) == 0) {
        snprintf(out, out_size, "%s", g_cover_cache.text);
        return out[0] != '\0' ? 1 : 0;
    }

    int rc;
    if (charset == INFO_COVER_ASCII) {
        rc = generate_ascii_art_dynamic(cover_path, BRAILLE_DEFAULT_THRESHOLD,
                                        cols, rows, g_cover_cache.text,
                                        sizeof(g_cover_cache.text));
    } else {
        rc = generate_braille_art_dynamic(cover_path, BRAILLE_DEFAULT_THRESHOLD,
                                          cols, rows, g_cover_cache.text,
                                          sizeof(g_cover_cache.text));
    }

    if (rc != 0) {
        log_debug("info", "Cover text generation failed for '%s' (%dx%d)",
                  cover_path, cols, rows);
        g_cover_cache.valid = 0;
        g_cover_cache.text[0] = '\0';
        return 0;
    }

    g_cover_cache.valid = 1;
    g_cover_cache.cols = cols;
    g_cover_cache.rows = rows;
    g_cover_cache.charset = charset;
    snprintf(g_cover_cache.path, sizeof(g_cover_cache.path), "%s", cover_path);

    snprintf(out, out_size, "%s", g_cover_cache.text);
    info_sanitize_utf8(out);
    return out[0] != '\0' ? 1 : 0;
}

void info_sanitize_utf8(char *text)
{
    if (!text) {
        return;
    }

    unsigned char *read = (unsigned char *)text;
    unsigned char *write = (unsigned char *)text;

    while (*read != '\0') {
        unsigned char lead = *read;
        int extra;

        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            *write++ = '?';
            read++;
            continue;
        }

        int valid = 1;
        for (int i = 1; i <= extra; i++) {
            if ((read[i] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            *write++ = '?';
            read++;
            continue;
        }

        for (int i = 0; i <= extra; i++) {
            *write++ = read[i];
        }
        read += extra + 1;
    }

    *write = '\0';
}

/* ── 文本渲染 ───────────────────────────────────────────────────── */

static void info_format_time(int seconds, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (seconds < 0) {
        snprintf(out, out_size, "--:--");
        return;
    }
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;
    if (hours > 0) {
        snprintf(out, out_size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(out, out_size, "%02d:%02d", minutes, secs);
    }
}

static void info_render_progress_line(const InfoPlayback *pb, int style,
                                      int width, char *out, size_t out_size)
{
    int have_duration = pb->duration_seconds > 0;
    int percent = 0;
    if (have_duration) {
        percent = (pb->position_seconds * 100) / pb->duration_seconds;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
    }

    char position_text[16];
    char duration_text[16];
    char remaining_text[16];
    if (have_duration) {
        info_format_time(pb->position_seconds, position_text, sizeof(position_text));
        info_format_time(pb->duration_seconds, duration_text, sizeof(duration_text));
        int remaining = pb->duration_seconds - pb->position_seconds;
        if (remaining < 0) remaining = 0;
        info_format_time(remaining, remaining_text, sizeof(remaining_text));
    } else {
        snprintf(position_text, sizeof(position_text), "--:--");
        snprintf(duration_text, sizeof(duration_text), "--:--");
        snprintf(remaining_text, sizeof(remaining_text), "--:--");
    }

    switch (style) {
        case INFO_PROGRESS_TIME:
            snprintf(out, out_size, "%s / %s (-%s)",
                     position_text, duration_text, remaining_text);
            break;
        case INFO_PROGRESS_PERCENT:
            if (have_duration) {
                snprintf(out, out_size, "%d%%", percent);
            } else {
                snprintf(out, out_size, "--%%");
            }
            break;
        case INFO_PROGRESS_TIME_PERCENT:
            if (have_duration) {
                snprintf(out, out_size, "%s / %s %d%%",
                         position_text, duration_text, percent);
            } else {
                snprintf(out, out_size, "%s / %s --%%",
                         position_text, duration_text);
            }
            break;
        case INFO_PROGRESS_BAR_TIME:
        default: {
            int bar_width = width - 27;
            if (bar_width < 10) bar_width = 10;
            if (bar_width > 60) bar_width = 60;

            char bar[64];
            int filled = have_duration ? (bar_width * percent) / 100 : 0;
            if (filled > bar_width) filled = bar_width;
            for (int i = 0; i < bar_width; i++) {
                char c = '-';
                if (i < filled) {
                    c = '=';
                } else if (i == filled && have_duration && percent < 100) {
                    c = '>';
                }
                bar[i] = c;
            }
            bar[bar_width] = '\0';

            if (have_duration) {
                snprintf(out, out_size, "[%s / %s] [%s] %d%%",
                         position_text, duration_text, bar, percent);
            } else {
                snprintf(out, out_size, "[%s / %s] [%s] --%%",
                         position_text, duration_text, bar);
            }
            break;
        }
    }
}

/* 返回 1 表示已写出一行，0 表示该字段当前无内容可显示 */
static int info_render_field_line(int bit, const InfoTrack *t,
                                  const InfoPlayback *pb,
                                  char *out, size_t out_size)
{
    switch (bit) {
        case INFO_FIELD_STATE: {
            const char *state_text;
            switch (pb->state) {
                case PLAY_STATE_PLAYING: state_text = i18n_get("player.playing"); break;
                case PLAY_STATE_PAUSED:  state_text = i18n_get("player.paused");  break;
                default:                 state_text = i18n_get("player.stopped"); break;
            }
            snprintf(out, out_size, "%s%s", i18n_get("player.state"), state_text);
            return 1;
        }
        case INFO_FIELD_MODE:
            snprintf(out, out_size, "%s%s", i18n_get("player.mode"),
                     play_mode_display_name(pb->play_mode, 0));
            return 1;
        case INFO_FIELD_INDEX:
            if (t->valid && t->playlist_total > 0) {
                snprintf(out, out_size, "%s%d/%d", i18n_get("info.field.index"),
                         t->index + 1, t->playlist_total);
            } else {
                snprintf(out, out_size, "%s-/-", i18n_get("info.field.index"));
            }
            return 1;
        case INFO_FIELD_QUEUE:
            if (t->queue_count > 0 && t->queue_position >= 0) {
                snprintf(out, out_size, "%s%d/%d", i18n_get("info.field.queue"),
                         t->queue_position + 1, t->queue_count);
                return 1;
            }
            return 0;
        case INFO_FIELD_TITLE:
            snprintf(out, out_size, "%s%s", i18n_get("player.title"),
                     t->valid && t->title[0] ? t->title : "--");
            return 1;
        case INFO_FIELD_ARTIST:
            snprintf(out, out_size, "%s%s", i18n_get("player.artist"),
                     t->valid && t->artist[0] ? t->artist : "--");
            return 1;
        case INFO_FIELD_ALBUM:
            snprintf(out, out_size, "%s%s", i18n_get("player.album"),
                     t->valid && t->album[0] ? t->album : "--");
            return 1;
        case INFO_FIELD_FORMAT: {
            char summary[128];
            info_format_audio_summary(summary, sizeof(summary));
            snprintf(out, out_size, "%s%s", i18n_get("info.field.format"), summary);
            return 1;
        }
        case INFO_FIELD_PATH:
            snprintf(out, out_size, "%s%s", i18n_get("info.field.path"),
                     t->valid && t->path[0] ? t->path : "--");
            return 1;
        case INFO_FIELD_VOLUME:
            snprintf(out, out_size, "%s%d%%", i18n_get("info.field.volume"),
                     pb->volume_percent);
            return 1;
        case INFO_FIELD_SPEED:
            snprintf(out, out_size, "%s%.2fx", i18n_get("info.field.speed"),
                     (double)pb->speed);
            return 1;
        default:
            return 0;
    }
}

static int info_split_cover_lines(const char *text, char *storage, size_t storage_size,
                                  char *lines[], int max_lines)
{
    if (!text || !storage || storage_size == 0 || !lines || max_lines <= 0) {
        return 0;
    }

    snprintf(storage, storage_size, "%s", text);

    int count = 0;
    char *cursor = storage;
    while (cursor && *cursor != '\0' && count < max_lines) {
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
        }
        if (*cursor != '\0') {
            lines[count++] = cursor;
        }
        cursor = newline ? newline + 1 : NULL;
    }
    return count;
}

static void info_append_line(char *out, size_t out_size, size_t *pos,
                             const char *text)
{
    size_t length = strlen(text);
    if (*pos + length + 1 >= out_size) {
        size_t available = (out_size > *pos + 1) ? (out_size - *pos - 1) : 0;
        /* 容量不足时按字节截断，但绝不切断多字节字符 */
        while (available > 0 &&
               (((unsigned char)text[available]) & 0xC0) == 0x80) {
            available--;
        }
        length = available;
    }
    if (length > 0) {
        memcpy(out + *pos, text, length);
        *pos += length;
    }
    out[*pos] = '\0';
}

int info_render_text(const InfoRenderOptions *opts_in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    InfoRenderOptions opts;
    if (opts_in) {
        opts = *opts_in;
    } else {
        info_options_from_config(&opts);
    }
    if (opts.one_line) {
        return info_render_one_line(&opts, out, out_size);
    }
    if (opts.width < INFO_WIDTH_MIN) {
        opts.width = INFO_WIDTH_MIN;
    }
    if (opts.width > INFO_WIDTH_MAX) {
        opts.width = INFO_WIDTH_MAX;
    }
    if (opts.cover_cols < INFO_COVER_COLS_MIN) opts.cover_cols = INFO_COVER_COLS_MIN;
    if (opts.cover_cols > INFO_COVER_COLS_MAX) opts.cover_cols = INFO_COVER_COLS_MAX;
    if (opts.cover_rows < INFO_COVER_ROWS_MIN) opts.cover_rows = INFO_COVER_ROWS_MIN;
    if (opts.cover_rows > INFO_COVER_ROWS_MAX) opts.cover_rows = INFO_COVER_ROWS_MAX;

    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;
    info_track_snapshot(&track);
    info_playback_snapshot(&playback);
    info_lyrics_snapshot(&lyrics);

    /* 先确定封面与文本可用宽度：封面存在时进度条按文本预算计算，
     * 否则长进度条会被后续截断（出现不完整的 "]"） */
    char cover_storage[INFO_COVER_TEXT_MAX];
    char *cover_lines[INFO_COVER_ROWS_MAX];
    int cover_line_count = 0;
    int cover_width = 0;
    int text_width = opts.width;

    if (opts.show_cover) {
        char cover_probe[INFO_COVER_TEXT_MAX];
        if (info_cover_text(opts.cover_cols, opts.cover_rows,
                            opts.cover_charset, cover_probe, sizeof(cover_probe))) {
            cover_line_count = info_split_cover_lines(cover_probe, cover_storage,
                                                      sizeof(cover_storage),
                                                      cover_lines, INFO_COVER_ROWS_MAX);
            for (int i = 0; i < cover_line_count; i++) {
                int width = utf8_str_width(cover_lines[i]);
                if (width > cover_width) {
                    cover_width = width;
                }
            }
            if (cover_line_count == 0 ||
                opts.width - cover_width - 2 < INFO_MIN_TEXT_WIDTH) {
                cover_line_count = 0;
                cover_width = 0;
            }
        }
    }
    if (cover_line_count > 0) {
        text_width = opts.width - cover_width - 2;
        if (text_width < INFO_MIN_TEXT_WIDTH) {
            text_width = INFO_MIN_TEXT_WIDTH;
        }
    }

    char lines[INFO_MAX_LINES][INFO_LINE_MAX];
    int line_count = 0;

    for (int i = 0; i < k_info_field_count; i++) {
        if ((opts.fields_mask & k_info_fields[i].bit) == 0) {
            continue;
        }
        if (line_count >= INFO_MAX_LINES - 3) {
            break;
        }
        if (info_render_field_line(k_info_fields[i].bit, &track, &playback,
                                   lines[line_count], INFO_LINE_MAX)) {
            line_count++;
        }
    }

    if (opts.show_progress && line_count < INFO_MAX_LINES - 2) {
        info_render_progress_line(&playback, opts.progress_style, text_width,
                                  lines[line_count], INFO_LINE_MAX);
        line_count++;
    }

    if (opts.lyrics_lines > INFO_LYRICS_OFF && lyrics.has_lyrics &&
        lyrics.current_index >= 0) {
        if (line_count < INFO_MAX_LINES - 1) {
            snprintf(lines[line_count], INFO_LINE_MAX, "%s%s",
                     i18n_get("info.lyrics.current"), lyrics.current_text);
            line_count++;
        }
        if (opts.lyrics_lines >= INFO_LYRICS_BOTH && lyrics.next_index >= 0 &&
            line_count < INFO_MAX_LINES) {
            snprintf(lines[line_count], INFO_LINE_MAX, "%s%s",
                     i18n_get("info.lyrics.next"), lyrics.next_text);
            line_count++;
        }
    }

    size_t pos = 0;
    int total_lines = line_count > cover_line_count ? line_count : cover_line_count;
    char composed[INFO_LINE_MAX];

    for (int i = 0; i < total_lines; i++) {
        const char *text = (i < line_count) ? lines[i] : "";

        if (cover_line_count > 0) {
            int text_budget = opts.width - cover_width - 2;
            if (text_budget < 1) {
                text_budget = 1;
            }
            char truncated[INFO_LINE_MAX];
            if (utf8_str_width(text) > text_budget) {
                utf8_str_truncate(truncated, text, text_budget);
                text = truncated;
            }

            int text_width = utf8_str_width(text);
            int padding = text_budget - text_width;
            if (padding < 0) {
                padding = 0;
            }

            int written = snprintf(composed, sizeof(composed), "%s", text);
            if (written < 0) {
                written = 0;
            }
            size_t used = (size_t)written;
            if (used > sizeof(composed) - 1) {
                used = sizeof(composed) - 1;
            }
            while (padding-- > 0 && used + 2 < sizeof(composed)) {
                composed[used++] = ' ';
            }
            composed[used] = '\0';
            snprintf(composed + used, sizeof(composed) - used, "  %s",
                     i < cover_line_count ? cover_lines[i] : "");
        } else {
            if (utf8_str_width(text) > opts.width) {
                utf8_str_truncate(composed, text, opts.width);
            } else {
                snprintf(composed, sizeof(composed), "%s", text);
            }
        }

        info_append_line(out, out_size, &pos, composed);
        info_append_line(out, out_size, &pos, "\n");
    }

    info_sanitize_utf8(out);
    return (int)pos;
}

int info_render_one_line(const InfoRenderOptions *opts_in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    InfoRenderOptions opts;
    if (opts_in) {
        opts = *opts_in;
    } else {
        info_options_from_config(&opts);
    }
    opts.one_line = 1;
    opts.show_cover = 0;
    if (opts.width < INFO_WIDTH_MIN) {
        opts.width = INFO_WIDTH_MIN;
    }

    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;
    info_track_snapshot(&track);
    info_playback_snapshot(&playback);
    info_lyrics_snapshot(&lyrics);

    size_t pos = 0;
    char part[INFO_LINE_MAX];

    for (int i = 0; i < k_info_field_count; i++) {
        if ((opts.fields_mask & k_info_fields[i].bit) == 0) {
            continue;
        }
        if (!info_render_field_line(k_info_fields[i].bit, &track, &playback,
                                    part, sizeof(part))) {
            continue;
        }
        if (pos > 0) {
            info_append_line(out, out_size, &pos, " · ");
        }
        info_append_line(out, out_size, &pos, part);
    }

    if (opts.show_progress) {
        int style = (opts.progress_style == INFO_PROGRESS_PERCENT)
            ? INFO_PROGRESS_PERCENT : INFO_PROGRESS_TIME_PERCENT;
        info_render_progress_line(&playback, style, opts.width, part, sizeof(part));
        if (pos > 0) {
            info_append_line(out, out_size, &pos, " · ");
        }
        info_append_line(out, out_size, &pos, part);
    }

    if (opts.lyrics_lines > INFO_LYRICS_OFF && lyrics.has_lyrics &&
        lyrics.current_index >= 0) {
        if (pos > 0) {
            info_append_line(out, out_size, &pos, " · ");
        }
        info_append_line(out, out_size, &pos, lyrics.current_text);
    }

    if (utf8_str_width(out) > opts.width) {
        char truncated[INFO_LINE_MAX];
        utf8_str_truncate(truncated, out, opts.width);
        snprintf(out, out_size, "%s", truncated);
        pos = strlen(out);
    }

    info_append_line(out, out_size, &pos, "\n");
    info_sanitize_utf8(out);
    return (int)pos;
}

/* ── JSON 渲染 ──────────────────────────────────────────────────── */

static size_t info_append_track_object(char *out, size_t out_size, size_t pos,
                                       const InfoTrack *t)
{
    pos = json_append_char(out, out_size, pos, '{');

    pos = json_append_key(out, out_size, pos, "id");
    pos = json_append_string_or_null(out, out_size, pos, t->valid ? t->track_id : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "index");
    if (t->valid) {
        pos = json_append_int(out, out_size, pos, t->index);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "number");
    if (t->valid) {
        pos = json_append_int(out, out_size, pos, t->index + 1);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "playlist_count");
    pos = json_append_int(out, out_size, pos, t->playlist_total);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "queue_position");
    if (t->queue_position >= 0) {
        pos = json_append_int(out, out_size, pos, t->queue_position + 1);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "queue_count");
    pos = json_append_int(out, out_size, pos, t->queue_count);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "title");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (t->valid && t->title[0]) ? t->title : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "artist");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (t->valid && t->artist[0]) ? t->artist : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "album");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (t->valid && t->album[0]) ? t->album : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "path");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (t->valid && t->path[0]) ? t->path : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "uri");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (t->valid && t->uri[0]) ? t->uri : NULL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "is_remote");
    pos = json_append_bool(out, out_size, pos, t->is_remote);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cue_track_number");
    pos = json_append_int(out, out_size, pos, t->cue_track_number);

    /* format */
    char rate[32], depth[32], bitrate[32], codec[32];
    info_format_audio_fields(rate, sizeof(rate), depth, sizeof(depth),
                             bitrate, sizeof(bitrate), codec, sizeof(codec));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "format");
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "codec");
    pos = json_append_string_or_null(out, out_size, pos,
                                     g_audio_codec_name[0] ? g_audio_codec_name : NULL);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "sample_rate");
    if (g_audio_sample_rate > 0) {
        pos = json_append_int(out, out_size, pos, g_audio_sample_rate);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "bit_depth");
    if (g_audio_bit_depth > 0) {
        pos = json_append_int(out, out_size, pos, g_audio_bit_depth);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "bit_rate");
    if (g_audio_bit_rate > 0) {
        pos = json_append_int(out, out_size, pos, g_audio_bit_rate);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "rate_display");
    pos = json_append_escaped(out, out_size, pos, rate);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "depth_display");
    pos = json_append_escaped(out, out_size, pos, depth);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "bitrate_display");
    pos = json_append_escaped(out, out_size, pos, bitrate);
    pos = json_append_char(out, out_size, pos, '}');

    return json_append_char(out, out_size, pos, '}');
}

const char *info_loop_status_mpris(PlayMode mode)
{
    if (mode == PLAY_MODE_SINGLE_REPEAT) {
        return "Track";
    }
    if (play_mode_repeats(mode)) {
        return "Playlist";
    }
    return "None";
}

int info_shuffle_mpris(PlayMode mode)
{
    return play_mode_is_shuffle(mode) ? 1 : 0;
}

static size_t info_append_playback_object(char *out, size_t out_size, size_t pos,
                                          const InfoPlayback *pb)
{
    int have_duration = pb->duration_seconds > 0;
    char position_text[16];
    char duration_text[16];
    char remaining_text[16];
    if (have_duration) {
        info_format_time(pb->position_seconds, position_text, sizeof(position_text));
        info_format_time(pb->duration_seconds, duration_text, sizeof(duration_text));
        int remaining = pb->duration_seconds - pb->position_seconds;
        if (remaining < 0) remaining = 0;
        info_format_time(remaining, remaining_text, sizeof(remaining_text));
    } else {
        snprintf(position_text, sizeof(position_text), "--:--");
        snprintf(duration_text, sizeof(duration_text), "--:--");
        snprintf(remaining_text, sizeof(remaining_text), "--:--");
    }

    pos = json_append_char(out, out_size, pos, '{');

    pos = json_append_key(out, out_size, pos, "state");
    pos = json_append_escaped(out, out_size, pos, info_play_state_id(pb->state));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "position_ms");
    pos = json_append_int(out, out_size, pos,
                          (long long)pb->position_seconds * 1000LL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "duration_ms");
    pos = json_append_int(out, out_size, pos,
                          (long long)pb->duration_seconds * 1000LL);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "position");
    pos = json_append_escaped(out, out_size, pos, position_text);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "duration");
    pos = json_append_escaped(out, out_size, pos, duration_text);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "remaining");
    pos = json_append_escaped(out, out_size, pos, remaining_text);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "percent");
    if (have_duration) {
        double percent = (double)pb->position_seconds * 100.0 /
                         (double)pb->duration_seconds;
        pos = json_append_double(out, out_size, pos, percent, 1);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "volume_percent");
    pos = json_append_int(out, out_size, pos, pb->volume_percent);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "speed");
    pos = json_append_double(out, out_size, pos, (double)pb->speed, 2);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "play_mode");
    pos = json_append_escaped(out, out_size, pos, info_play_mode_id(pb->play_mode));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "play_mode_index");
    pos = json_append_int(out, out_size, pos, (int)pb->play_mode);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "play_mode_name");
    pos = json_append_escaped(out, out_size, pos,
                              play_mode_display_name(pb->play_mode, 0));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "loop_status");
    pos = json_append_escaped(out, out_size, pos, info_loop_status_mpris(pb->play_mode));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "shuffle");
    pos = json_append_bool(out, out_size, pos, info_shuffle_mpris(pb->play_mode));

    return json_append_char(out, out_size, pos, '}');
}

static size_t info_append_lyrics_object(char *out, size_t out_size, size_t pos,
                                        const InfoLyrics *ly)
{
    pos = json_append_char(out, out_size, pos, '{');

    pos = json_append_key(out, out_size, pos, "has_lyrics");
    pos = json_append_bool(out, out_size, pos, ly->has_lyrics);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "has_timestamps");
    pos = json_append_bool(out, out_size, pos, ly->has_timestamps);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "source");
    pos = json_append_escaped(out, out_size, pos,
                              ly->has_lyrics ? info_lyrics_source_id(ly->source) : "none");

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "current");
    pos = json_append_line_object(out, out_size, pos, ly->current_index,
                                  ly->has_timestamps, ly->current_timestamp,
                                  ly->current_text);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "next");
    pos = json_append_line_object(out, out_size, pos, ly->next_index,
                                  ly->has_timestamps, ly->next_timestamp,
                                  ly->next_text);

    return json_append_char(out, out_size, pos, '}');
}

static size_t info_append_display_object(char *out, size_t out_size, size_t pos,
                                         const InfoRenderOptions *opts)
{
    pos = json_append_char(out, out_size, pos, '{');

    pos = json_append_key(out, out_size, pos, "preset");
    pos = json_append_escaped(out, out_size, pos, info_preset_id(opts->preset));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "fields");
    pos = json_append_char(out, out_size, pos, '[');
    {
        int first = 1;
        for (int i = 0; i < k_info_field_count; i++) {
            if ((opts->fields_mask & k_info_fields[i].bit) == 0) {
                continue;
            }
            if (!first) {
                pos = json_append_char(out, out_size, pos, ',');
            }
            pos = json_append_escaped(out, out_size, pos, k_info_fields[i].name);
            first = 0;
        }
    }
    pos = json_append_char(out, out_size, pos, ']');

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "show_cover");
    pos = json_append_bool(out, out_size, pos, opts->show_cover);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cover_cols");
    pos = json_append_int(out, out_size, pos, opts->cover_cols);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cover_rows");
    pos = json_append_int(out, out_size, pos, opts->cover_rows);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cover_charset");
    pos = json_append_escaped(out, out_size, pos,
                              info_cover_charset_id(opts->cover_charset));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "show_progress");
    pos = json_append_bool(out, out_size, pos, opts->show_progress);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "progress_style");
    pos = json_append_escaped(out, out_size, pos,
                              info_progress_style_id(opts->progress_style));

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "lyrics_lines");
    pos = json_append_int(out, out_size, pos, opts->lyrics_lines);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "width");
    pos = json_append_int(out, out_size, pos, opts->width);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "one_line");
    pos = json_append_bool(out, out_size, pos, opts->one_line);

    return json_append_char(out, out_size, pos, '}');
}

int info_render_instance_json(char *out, size_t out_size,
                              const InfoInstance *instance)
{
    if (!out || out_size == 0) {
        return -1;
    }

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "mode");
    pos = json_append_escaped(out, out_size, pos,
                              (instance && instance->is_daemon) ? "daemon" : "tui");
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "pid");
    pos = json_append_int(out, out_size, pos,
                          (instance && instance->pid > 0) ? instance->pid : (long long)getpid());
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "version");
    pos = json_append_escaped(out, out_size, pos,
                              (instance && instance->version) ? instance->version : APP_VERSION);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "bus");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (instance && instance->bus_name &&
                                      instance->bus_name[0]) ? instance->bus_name : NULL);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "has_primary_name");
    pos = json_append_bool(out, out_size, pos,
                           (instance && instance->has_primary_name) ? 1 : 0);
    pos = json_append_char(out, out_size, pos, '}');
    info_sanitize_utf8(out);
    return (int)pos;
}

int info_render_track_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    InfoTrack track;
    info_track_snapshot(&track);
    int written = (int)info_append_track_object(out, out_size, 0, &track);
    info_sanitize_utf8(out);
    return written;
}

int info_render_progress_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    InfoPlayback playback;
    info_playback_snapshot(&playback);
    int written = (int)info_append_playback_object(out, out_size, 0, &playback);
    info_sanitize_utf8(out);
    return written;
}

int info_render_lyrics_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    InfoLyrics lyrics;
    info_lyrics_snapshot(&lyrics);
    int written = (int)info_append_lyrics_object(out, out_size, 0, &lyrics);
    info_sanitize_utf8(out);
    return written;
}

int info_render_json(char *out, size_t out_size, const InfoInstance *instance,
                     unsigned long long revision)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    InfoRenderOptions opts;
    info_options_from_config(&opts);

    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;
    info_track_snapshot(&track);
    info_playback_snapshot(&playback);
    info_lyrics_snapshot(&lyrics);

    char cover_text[INFO_COVER_TEXT_MAX];
    int have_cover = 0;
    if (opts.show_cover) {
        have_cover = info_cover_text(opts.cover_cols, opts.cover_rows,
                                     opts.cover_charset, cover_text,
                                     sizeof(cover_text));
    } else {
        cover_text[0] = '\0';
    }

    /* 复用文本渲染（使用同一份配置，保证与 ter-music show 一致） */
    char *text_buffer = malloc(INFO_TEXT_MAX);
    if (text_buffer) {
        if (info_render_text(&opts, text_buffer, INFO_TEXT_MAX) < 0) {
            text_buffer[0] = '\0';
        }
    }

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');

    pos = json_append_key(out, out_size, pos, "schema");
    pos = json_append_int(out, out_size, pos, 1);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "revision");
    pos = json_append_int(out, out_size, pos, (long long)revision);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "running");
    pos = json_append_bool(out, out_size, pos, 1);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "instance");
    {
        char instance_json[512];
        if (info_render_instance_json(instance_json, sizeof(instance_json), instance) > 0) {
            pos = json_append_raw(out, out_size, pos, instance_json);
        } else {
            pos = json_append_raw(out, out_size, pos, "{}");
        }
    }

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "playback");
    pos = info_append_playback_object(out, out_size, pos, &playback);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "track");
    pos = info_append_track_object(out, out_size, pos, &track);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cover");
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "available");
    pos = json_append_bool(out, out_size, pos, have_cover);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "art_url");
    if (track.has_cover) {
        char art_url[INFO_URI_MAX];
        info_build_file_uri(track.cover_path, art_url, sizeof(art_url));
        pos = json_append_escaped(out, out_size, pos, art_url);
    } else {
        pos = json_append_raw(out, out_size, pos, "null");
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "charset");
    pos = json_append_escaped(out, out_size, pos,
                              info_cover_charset_id(opts.cover_charset));
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cols");
    pos = json_append_int(out, out_size, pos, opts.cover_cols);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "rows");
    pos = json_append_int(out, out_size, pos, opts.cover_rows);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "text");
    pos = json_append_string_or_null(out, out_size, pos,
                                     have_cover ? cover_text : NULL);
    pos = json_append_char(out, out_size, pos, '}');

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "lyrics");
    pos = info_append_lyrics_object(out, out_size, pos, &lyrics);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "display");
    pos = info_append_display_object(out, out_size, pos, &opts);

    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "text");
    pos = json_append_string_or_null(out, out_size, pos,
                                     (text_buffer && text_buffer[0]) ? text_buffer : NULL);

    pos = json_append_char(out, out_size, pos, '}');

    free(text_buffer);
    info_sanitize_utf8(out);
    return (int)pos;
}
