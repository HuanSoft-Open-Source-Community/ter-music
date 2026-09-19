/**
 * @file lyrics.c
 * @brief 歌词引擎实现：加载、解析、来源偏好、按播放位置推进
 *
 * 从 ui/lyrics.c 抽出的**引擎部分**（2026-09-15）：歌词属“当前曲目信息”，
 * 归后端。本文件不含 ncurses，也不读写界面状态；渲染留在 ui/lyrics.c。
 *
 * 相对抽取前的三处有意变化：
 *   - 当前曲目路径来自后端队列游标（`bq_current`），不再问内容列表；
 *   - 来源偏好不再写内容库：后端调 `lyrics_set_source_hook()` 让前端持久化；
 *   - UTF-16 解码改为自实现（见 utf16_to_utf8 的说明），不再依赖 iconv 对
 *     locale 的隐式假设。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "lyrics/lyrics.h"

#include "audio/audio.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "queue/backend_queue.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HTML 实体解码属内容层的文本工具（playlist/playlist.c 实现）；歌词解析与
 * 曲目标签走同一套解码，故在此声明复用。 */
void decode_html_entities(char *str);

static int utf16_to_utf8(const unsigned char *input, size_t input_len,
                         int big_endian, char **out_text);

/* 全局歌词实例（后端拥有） */
Lyrics g_lyrics = {
    .count = 0,
    .current_index = -1,
    .highlight_count = 0,
    .has_lyrics = 0,
    .has_timestamps = 0,
    .source = LYRICS_SOURCE_AUTO,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

static void (*g_source_hook)(const char *track_path, int source) = NULL;
static int (*g_position_source)(void) = NULL;

void lyrics_set_position_source(int (*source)(void))
{
    g_position_source = source;
}

int lyrics_position_seconds(void)
{
    return g_position_source ? g_position_source() : audio_get_position_seconds();
}

void lyrics_set_source_hook(void (*hook)(const char *track_path, int source))
{
    g_source_hook = hook;
}

const char *lyrics_current_track_path(void)
{
    static char path[MAX_PATH_LEN];
    BackendQueueEntry entry;
    if (bq_current(&entry) != 0) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s", entry.path);
    return path;
}

int lyrics_source(void)
{
    int source;
    pthread_mutex_lock(&g_lyrics.lock);
    source = g_lyrics.source;
    pthread_mutex_unlock(&g_lyrics.lock);
    return source;
}

static void reset_loaded_lyrics(void) {
    pthread_mutex_lock(&g_lyrics.lock);
    g_lyrics.count = 0;
    g_lyrics.current_index = -1;
    g_lyrics.highlight_count = 0;
    g_lyrics.has_lyrics = 0;
    g_lyrics.has_timestamps = 0;
    g_lyrics.cursor_index = -1;
    g_lyrics.source = LYRICS_SOURCE_AUTO;
    pthread_mutex_unlock(&g_lyrics.lock);
}

static int duplicate_text_bytes(const unsigned char *data, size_t len, size_t skip, char **out_text) {
    if (!out_text) {
        return -1;
    }

    if (!data || skip > len) {
        return -1;
    }

    size_t text_len = len - skip;
    char *copy = malloc(text_len + 1);
    if (!copy) {
        return -1;
    }

    if (text_len > 0) {
        memcpy(copy, data + skip, text_len);
    }
    copy[text_len] = '\0';
    *out_text = copy;
    return 0;
}

static int read_file_bytes(const char *path, unsigned char **data_out, size_t *size_out) {
    if (!path || !data_out || !size_out) {
        return -1;
    }

    *data_out = NULL;
    *size_out = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        return -1;
    }

    rewind(fp);

    unsigned char *data = malloc((size_t)file_size + 1);
    if (!data) {
        fclose(fp);
        return -1;
    }

    size_t bytes_read = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);

    data[bytes_read] = '\0';
    *data_out = data;
    *size_out = bytes_read;
    return 0;
}

static int is_valid_utf8_bytes(const unsigned char *data, size_t len) {
    size_t i = 0;

    while (i < len) {
        unsigned char c = data[i];
        if (c < 0x80) {
            i++;
            continue;
        }

        if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80 || c < 0xC2) {
                return 0;
            }
            i += 2;
            continue;
        }

        if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xE0 && data[i + 1] < 0xA0) {
                return 0;
            }
            if (c == 0xED && data[i + 1] >= 0xA0) {
                return 0;
            }
            i += 3;
            continue;
        }

        if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xF0 && data[i + 1] < 0x90) {
                return 0;
            }
            if (c > 0xF4 || (c == 0xF4 && data[i + 1] >= 0x90)) {
                return 0;
            }
            i += 4;
            continue;
        }

        return 0;
    }

    return 1;
}

static int looks_like_utf16_le(const unsigned char *data, size_t len) {
    if (!data || len < 4) {
        return 0;
    }

    size_t sample_len = len < 128 ? len : 128;
    int zero_odd = 0;
    int zero_even = 0;
    int pair_count = 0;

    for (size_t i = 0; i + 1 < sample_len; i += 2) {
        if (data[i] == 0x00) {
            zero_even++;
        }
        if (data[i + 1] == 0x00) {
            zero_odd++;
        }
        pair_count++;
    }

    return pair_count > 0 && zero_odd >= (pair_count / 3) && zero_odd > zero_even;
}

static int looks_like_utf16_be(const unsigned char *data, size_t len) {
    if (!data || len < 4) {
        return 0;
    }

    size_t sample_len = len < 128 ? len : 128;
    int zero_odd = 0;
    int zero_even = 0;
    int pair_count = 0;

    for (size_t i = 0; i + 1 < sample_len; i += 2) {
        if (data[i] == 0x00) {
            zero_even++;
        }
        if (data[i + 1] == 0x00) {
            zero_odd++;
        }
        pair_count++;
    }

    return pair_count > 0 && zero_even >= (pair_count / 3) && zero_even > zero_odd;
}

static int convert_text_to_utf8(const unsigned char *input,
                                size_t input_len,
                                const char *from_code,
                                char **out_text) {
    if (!input || !from_code || !out_text) {
        return -1;
    }

    iconv_t cd = iconv_open("UTF-8", from_code);
    if (cd == (iconv_t)-1) {
        return -1;
    }

    size_t out_cap = input_len * 4 + 16;
    if (out_cap < 64) {
        out_cap = 64;
    }

    char *output = malloc(out_cap);
    if (!output) {
        iconv_close(cd);
        return -1;
    }

    char *out_ptr = output;
    size_t out_left = out_cap - 1;
    char *in_ptr = (char *)input;
    size_t in_left = input_len;

    while (in_left > 0) {
        size_t ret = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (ret != (size_t)-1) {
            continue;
        }

        if (errno == E2BIG) {
            size_t used = (size_t)(out_ptr - output);
            size_t new_cap = out_cap * 2;
            char *grown = realloc(output, new_cap);
            if (!grown) {
                free(output);
                iconv_close(cd);
                return -1;
            }
            output = grown;
            out_ptr = output + used;
            out_left = new_cap - used - 1;
            out_cap = new_cap;
            continue;
        }

        free(output);
        iconv_close(cd);
        return -1;
    }

    *out_ptr = '\0';
    *out_text = output;
    iconv_close(cd);
    return 0;
}

static int process_lyrics_buffer(unsigned char *raw_data, size_t raw_size, char **out_text) {
    if (!raw_data || !out_text) return -1;
    *out_text = NULL;

    if (raw_size == 0) {
        free(raw_data);
        *out_text = calloc(1, 1);
        return *out_text ? 0 : -1;
    }

    if (raw_size >= 3 &&
        raw_data[0] == 0xEF && raw_data[1] == 0xBB && raw_data[2] == 0xBF &&
        is_valid_utf8_bytes(raw_data + 3, raw_size - 3)) {
        int rc = duplicate_text_bytes(raw_data, raw_size, 3, out_text);
        free(raw_data);
        return rc;
    }

    if (raw_size >= 2 && raw_data[0] == 0xFF && raw_data[1] == 0xFE) {
        int rc = utf16_to_utf8(raw_data + 2, raw_size - 2, 0, out_text);
        free(raw_data);
        return rc;
    }

    if (raw_size >= 2 && raw_data[0] == 0xFE && raw_data[1] == 0xFF) {
        int rc = utf16_to_utf8(raw_data + 2, raw_size - 2, 1, out_text);
        free(raw_data);
        return rc;
    }

    if (is_valid_utf8_bytes(raw_data, raw_size)) {
        int rc = duplicate_text_bytes(raw_data, raw_size, 0, out_text);
        free(raw_data);
        return rc;
    }

    if (looks_like_utf16_le(raw_data, raw_size) &&
        utf16_to_utf8(raw_data, raw_size, 0, out_text) == 0) {
        free(raw_data);
        return 0;
    }

    if (looks_like_utf16_be(raw_data, raw_size) &&
        utf16_to_utf8(raw_data, raw_size, 1, out_text) == 0) {
        free(raw_data);
        return 0;
    }

    const char *fallback_encodings[] = {"GB18030", "GBK", "BIG5", NULL};
    for (int i = 0; fallback_encodings[i] != NULL; i++) {
        if (convert_text_to_utf8(raw_data, raw_size, fallback_encodings[i], out_text) == 0) {
            free(raw_data);
            return 0;
        }
    }

    int rc = duplicate_text_bytes(raw_data, raw_size, 0, out_text);
    free(raw_data);
    return rc;
}

static int load_lyrics_text_utf8(const char *path, char **out_text) {
    if (!path || !out_text) {
        return -1;
    }

    *out_text = NULL;

    unsigned char *raw_data = NULL;
    size_t raw_size = 0;
    if (read_file_bytes(path, &raw_data, &raw_size) != 0) {
        return -1;
    }

    return process_lyrics_buffer(raw_data, raw_size, out_text);
}

/**
 * 解析 LRC 时间戳字符串
 * 格式：[mm:ss.xx]
 * @param time_str 时间戳字符串（不包含方括号）
 * @return 时间戳（秒，包含毫秒）
 */
static double parse_timestamp(const char *time_str) {
    int mm, ss, xx;
    if (sscanf(time_str, "%d:%d.%d", &mm, &ss, &xx) == 3) {
        return mm * 60 + ss + xx / 100.0;  // 保留毫秒精度
    }
    return -1.0;
}
/**
 * 解析单行 LRC 内容
 * @param line LRC 文件的一行
 * @param timestamp 输出：时间戳（秒，包含毫秒）
 * @param text 输出：歌词文本
 * @return 1 表示成功，0 表示失败
 */
static int parse_lrc_line(const char *line, double *timestamp, char *text) {
    if (!line || !timestamp || !text) {
        return 0;
    }
    
    // 跳过空行
    if (line[0] == '\0' || line[0] == '\n') {
        return 0;
    }
    
    // 查找第一个时间标签 [mm:ss.xx]
    const char *start = strchr(line, '[');
    if (!start) {
        return 0;
    }
    
    const char *end = strchr(start, ']');
    if (!end) {
        return 0;
    }
    
    // 提取时间戳字符串（不包含方括号）
    char time_str[16];
    int len = end - start - 1;
    if (len <= 0 || (size_t)len >= sizeof(time_str)) {
        return 0;
    }
    strncpy(time_str, start + 1, len);
    time_str[len] = '\0';
    
    // 解析时间戳
    double ts = parse_timestamp(time_str);
    if (ts < 0) {
        return 0;
    }
    *timestamp = ts;
    
    // 提取歌词文本（跳过所有时间标签）
    const char *text_start = end + 1;
    while (*text_start == '[') {
        // 跳过连续的时间标签
        const char *next_end = strchr(text_start, ']');
        if (!next_end) {
            break;
        }
        text_start = next_end + 1;
    }
    
    // 去除前导空格
    while (*text_start == ' ' || *text_start == '\t') {
        text_start++;
    }
    
    // 复制歌词文本，边复制边过滤嵌入的 [mm:ss.xx] 时间戳标签
    // 这样 MAX_LYRIC_TEXT_LEN 限制只作用于真正的歌词内容，
    // 避免卡拉OK式 LRC 因原始文本过长而把时间戳截断成碎片残留
    int dst = 0;
    const char *src = text_start;
    while (*src && dst < MAX_LYRIC_TEXT_LEN - 1) {
        if (*src == '[') {
            const char *close = strchr(src, ']');
            if (close) {
                int mm, ss, xx;
                if (sscanf(src + 1, "%d:%d.%d", &mm, &ss, &xx) == 3) {
                    src = close + 1;
                    continue;
                }
            }
        }
        text[dst++] = *src++;
    }
    text[dst] = '\0';
    len = dst;

    // 去除末尾换行符和空格
    while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r' || text[len-1] == ' ')) {
        text[--len] = '\0';
    }

    // 如果歌词文本为空，使用占位符
    if (len == 0) {
        snprintf(text, MAX_LYRIC_TEXT_LEN, "%s", i18n_get("lyrics.instrumental"));
    }
    
    return 1;
}

static int extract_embedded_lyrics(const char *audio_path)
{
    if (!audio_path) return -1;

    char *lyrics_text = NULL;
    int found = 0;

    /* ── Step 1: FFmpeg AVDictionary ── */
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, audio_path, NULL, NULL) == 0) {
        (void)avformat_find_stream_info(fmt_ctx, NULL);

        AVDictionary *stream_meta = NULL;
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                stream_meta = fmt_ctx->streams[i]->metadata;
                break;
            }
        }

        static const char *lyrics_keys[] = {
            "lyrics", "LYRICS", "unsyncedlyrics", "\xa9lyr", NULL
        };
        for (int k = 0; !found && lyrics_keys[k]; k++) {
            AVDictionaryEntry *entry = av_dict_get(stream_meta, lyrics_keys[k], NULL, 0);
            if (!entry || !entry->value || !entry->value[0])
                entry = av_dict_get(fmt_ctx->metadata, lyrics_keys[k], NULL, 0);
            if (entry && entry->value && entry->value[0]) {
                lyrics_text = strdup(entry->value);
                if (lyrics_text) found = 1;
                break;
            }
        }
        avformat_close_input(&fmt_ctx);
    }

    /* ── Step 2: 未同步歌词的常见元数据键 ──
     * 旧实现此处另起一套 APE 标签解析（`playlist/ape_tag.h`）只为找 "LYRICS"
     * 键；APE 标签同样会进入 FFmpeg 的元数据字典，而标签解析属**内容**职责
     * （前端）。这里改用 ffprobe 风格的元数据键，后端不再依赖内容模块。 */
    if (!found) {
        static const char *fallback_keys[] = {
            "LYRICS", "lyrics", "UNSYNCEDLYRICS", "unsyncedlyrics",
            "UNSYNCED LYRICS", "\xa9lyr", NULL
        };
        AVFormatContext *meta_ctx = NULL;
        if (avformat_open_input(&meta_ctx, audio_path, NULL, NULL) == 0) {
            (void)avformat_find_stream_info(meta_ctx, NULL);
            for (int k = 0; !found && fallback_keys[k]; k++) {
                AVDictionaryEntry *entry = av_dict_get(meta_ctx->metadata, fallback_keys[k], NULL, 0);
                if (!entry || !entry->value || !entry->value[0]) {
                    continue;
                }
                lyrics_text = strdup(entry->value);
                if (lyrics_text) found = 1;
            }
            avformat_close_input(&meta_ctx);
        }
        if (!found) {
            log_debug("lyrics", "No embedded lyrics in '%s'", audio_path);
        }
    }

    if (!found) return -1;

    /* ── Parse lyrics text into LyricLine[] ── */
    LyricLine temp_lines[MAX_LYRIC_LINES];
    int count = 0;
    int has_timestamps = 0;

    char *cursor = lyrics_text;
    while (cursor && *cursor != '\0' && count < MAX_LYRIC_LINES) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = line + strlen(line);
        }

        /* Strip trailing \r */
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' '))
            line[--llen] = '\0';

        if (line[0] == '\0') continue;

        /* Try LRC parsing */
        double ts;
        char lrc_text[MAX_LYRIC_TEXT_LEN];
        if (parse_lrc_line(line, &ts, lrc_text)) {
            temp_lines[count].timestamp = ts;
            decode_html_entities(lrc_text);
            strncpy(temp_lines[count].text, lrc_text, MAX_LYRIC_TEXT_LEN - 1);
            temp_lines[count].text[MAX_LYRIC_TEXT_LEN - 1] = '\0';
            has_timestamps = 1;
            count++;
        } else {
            /* Skip LRC metadata headers like [ti:...], [ar:...], [by:...] */
            if (line[0] == '[') {
                const char *colon = strchr(line, ':');
                const char *close_bracket = strchr(line, ']');
                if (colon && close_bracket && colon < close_bracket) {
                    continue;  /* metadata header — skip */
                }
            }
            /* Plain text line */
            temp_lines[count].timestamp = 0.0;
            decode_html_entities(line);
            strncpy(temp_lines[count].text, line, MAX_LYRIC_TEXT_LEN - 1);
            temp_lines[count].text[MAX_LYRIC_TEXT_LEN - 1] = '\0';
            count++;
        }
    }

    free(lyrics_text);

    if (count == 0) return -1;

    /* Store parsed lyrics */
    pthread_mutex_lock(&g_lyrics.lock);
    g_lyrics.count = count;
    memcpy(g_lyrics.lines, temp_lines, sizeof(LyricLine) * count);
    g_lyrics.has_lyrics = 1;
    g_lyrics.has_timestamps = has_timestamps;
    g_lyrics.current_index = has_timestamps ? -1 : 0;
    g_lyrics.highlight_count = 0;
    g_lyrics.source = LYRICS_SOURCE_EMBEDDED;
    pthread_mutex_unlock(&g_lyrics.lock);

    log_info("lyrics", "Loaded %d embedded lyric lines from '%s' (timestamps=%d)",
             count, audio_path, has_timestamps);
    return 0;
}

void load_lyrics(const char *audio_path, int lyrics_source) {
    if (!audio_path) {
        return;
    }
    log_debug("lyrics", "load_lyrics(path='%s', source=%d) called", audio_path, lyrics_source);

    /* Phase 1: Embedded lyrics */
    if (lyrics_source == LYRICS_SOURCE_AUTO || lyrics_source == LYRICS_SOURCE_EMBEDDED) {
        if (extract_embedded_lyrics(audio_path) == 0) {
            log_debug("lyrics", "Using embedded lyrics for '%s'", audio_path);
            return;
        }
        /* AUTO mode: fall through to external LRC. EMBEDDED mode: stop here. */
        if (lyrics_source == LYRICS_SOURCE_EMBEDDED) {
            log_debug("lyrics", "Embedded-only mode, no lyrics found for '%s'", audio_path);
            reset_loaded_lyrics();
            return;
        }
    }

    /* EXTERNAL-only mode: skip embedded entirely */
    if (lyrics_source == LYRICS_SOURCE_EXTERNAL) {
        log_debug("lyrics", "External-only mode, skipping embedded for '%s'", audio_path);
    }
    
    // 构造 LRC 文件路径
    char lrc_path[MAX_PATH_LEN];
    strncpy(lrc_path, audio_path, MAX_PATH_LEN - 1);
    lrc_path[MAX_PATH_LEN - 1] = '\0';
    
    // 替换扩展名为 .lrc
    char *ext = strrchr(lrc_path, '.');
    if (ext) {
        strcpy(ext, ".lrc");
    } else {
        strcat(lrc_path, ".lrc");
    }
    
    char *lyrics_text = NULL;
    if (load_lyrics_text_utf8(lrc_path, &lyrics_text) != 0 || !lyrics_text) {
        log_debug("lyrics", "No LRC file found for '%s'", lrc_path);
        reset_loaded_lyrics();
        return;
    }
    
    // 临时缓冲区存储解析后的歌词
    LyricLine temp_lines[MAX_LYRIC_LINES];
    int count = 0;

    char *cursor = lyrics_text;
    while (cursor && *cursor != '\0' && count < MAX_LYRIC_LINES) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = line + strlen(line);
        }

        if (line[0] == '\0') {
            continue;
        }

        // 检测头部元数据行 [ti:曲名]、[ar:歌手]、[al:专辑]
        if (line[0] == '[' &&
            (line[1] == 't' || line[1] == 'a') && line[3] == ':') {
            char *close = strchr(line + 4, ']');
            if (close && close > line + 4) {
                *close = '\0';
                temp_lines[count].timestamp = 0.0;
                strncpy(temp_lines[count].text, line + 4, MAX_LYRIC_TEXT_LEN - 1);
                temp_lines[count].text[MAX_LYRIC_TEXT_LEN - 1] = '\0';
                *close = ']';
                count++;
                continue;
            }
        }

        double timestamp;
        char text[MAX_LYRIC_TEXT_LEN];

        if (parse_lrc_line(line, &timestamp, text)) {
            temp_lines[count].timestamp = timestamp;
            decode_html_entities(text);
            strncpy(temp_lines[count].text, text, MAX_LYRIC_TEXT_LEN - 1);
            temp_lines[count].text[MAX_LYRIC_TEXT_LEN - 1] = '\0';
            count++;
        }
    }

    free(lyrics_text);
    
    // 如果没有解析到任何歌词
    if (count == 0) {
        log_debug("lyrics", "No lyrics content in '%s'", lrc_path);
        reset_loaded_lyrics();
        return;
    }

    // 锁定并更新全局歌词数据
    pthread_mutex_lock(&g_lyrics.lock);
    g_lyrics.count = count;
    memcpy(g_lyrics.lines, temp_lines, sizeof(LyricLine) * count);
    g_lyrics.has_lyrics = 1;
    g_lyrics.has_timestamps = 1;
    g_lyrics.current_index = -1;
    g_lyrics.highlight_count = 0;
    g_lyrics.source = LYRICS_SOURCE_EXTERNAL;
    pthread_mutex_unlock(&g_lyrics.lock);

    log_info("lyrics", "Loaded %d lyric lines from '%s'", count, lrc_path);
}

void clear_lyrics(void) {
    log_debug("lyrics", "clear_lyrics() called");
    pthread_mutex_lock(&g_lyrics.lock);
    g_lyrics.count = 0;
    g_lyrics.current_index = -1;
    g_lyrics.highlight_count = 0;
    g_lyrics.has_lyrics = 0;
    g_lyrics.has_timestamps = 0;
    g_lyrics.source = LYRICS_SOURCE_AUTO;
    pthread_mutex_unlock(&g_lyrics.lock);
}

/* ── UTF-16 → UTF-8（自实现，不用 iconv） ──────────────────────────
 * glibc 的 "UTF-16LE"/"UTF-16BE" 转换器在非 UTF-8 locale 下会走“按宿主字节序
 * 直读 UCS-2”的捷径：实测 C locale 下同一份 UTF-16LE 输入，14 字节时正确、
 * 34 字节时把 U+4F60 转成了 3 个 Latin-1 字符。进程启动虽然会
 * ensure_utf8_locale()，但库层不该押注在 locale 上，因此这里直接按码元
 * 解码（含代理对），行为与 locale 无关。 */
static int utf16_to_utf8(const unsigned char *input, size_t input_len,
                         int big_endian, char **out_text)
{
    if (!input || !out_text) {
        return -1;
    }

    size_t units = input_len / 2;            /* 末尾落单的字节忽略 */
    char *output = malloc(units * 4 + 1);
    if (!output) {
        return -1;
    }

    size_t written = 0;
    for (size_t i = 0; i < units; i++) {
        unsigned int unit = big_endian
            ? ((unsigned int)input[i * 2] << 8) | input[i * 2 + 1]
            : ((unsigned int)input[i * 2 + 1] << 8) | input[i * 2];
        unsigned int cp = unit;

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            /* 高代理：需要紧随其后的低代理组成增补平面字符 */
            if (i + 1 >= units) {
                free(output);
                return -1;
            }
            unsigned int low = big_endian
                ? ((unsigned int)input[(i + 1) * 2] << 8) | input[(i + 1) * 2 + 1]
                : ((unsigned int)input[(i + 1) * 2 + 1] << 8) | input[(i + 1) * 2];
            if (low < 0xDC00 || low > 0xDFFF) {
                free(output);
                return -1;
            }
            cp = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
            i++;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            free(output);                    /* 落单的低代理：非法 */
            return -1;
        }

        if (cp < 0x80) {
            output[written++] = (char)cp;
        } else if (cp < 0x800) {
            output[written++] = (char)(0xC0 | (cp >> 6));
            output[written++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            output[written++] = (char)(0xE0 | (cp >> 12));
            output[written++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            output[written++] = (char)(0x80 | (cp & 0x3F));
        } else {
            output[written++] = (char)(0xF0 | (cp >> 18));
            output[written++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            output[written++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            output[written++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    output[written] = '\0';

    *out_text = output;
    return 0;
}


/* ── 推进（由 core_tick 每轮调用） ─────────────────────────────── */

int lyrics_tick(void)
{
    pthread_mutex_lock(&g_lyrics.lock);

    if (!g_lyrics.has_lyrics || g_lyrics.count == 0) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int changed = 0;

    /* 无时间戳的纯文本内嵌歌词：固定停在第 0 行 */
    if (!g_lyrics.has_timestamps) {
        if (g_lyrics.current_index != 0) {
            g_lyrics.current_index = 0;
            g_lyrics.highlight_count = 0;
            changed = 1;
        }
        pthread_mutex_unlock(&g_lyrics.lock);
        return changed;
    }

    double position = (double)lyrics_position_seconds();
    int new_index = -1;
    int new_highlight_count = 0;

    /* 找到最后一个 timestamp <= 播放位置的行；该行之后若还有**同一时间戳**
     * 的行，则一起高亮（最多两行，与 LRC 里一行时间戳带多行文本的写法对应）。 */
    for (int i = 0; i < g_lyrics.count; i++) {
        if (g_lyrics.lines[i].timestamp > position) {
            break;
        }
        new_index = i;
        new_highlight_count = 1;
        if (i + 1 < g_lyrics.count &&
            g_lyrics.lines[i + 1].timestamp == g_lyrics.lines[i].timestamp) {
            new_highlight_count = 2;
        }
    }

    if (new_index != g_lyrics.current_index ||
        new_highlight_count != g_lyrics.highlight_count) {
        g_lyrics.current_index = new_index;
        g_lyrics.highlight_count = new_highlight_count;
        changed = 1;
    }

    pthread_mutex_unlock(&g_lyrics.lock);
    return changed;
}

int lyrics_switch_source(int new_source)
{
    const char *track_path = lyrics_current_track_path();
    if (!track_path) {
        log_warn("lyrics", "lyrics_switch_source: no current track");
        return -1;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s", track_path);

    /* 偏好先持久化（前端持有内容库），再重新加载 */
    if (g_source_hook) {
        g_source_hook(path, new_source);
    }
    load_lyrics(path, new_source);
    return 0;
}

int lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps, int *out_source)
{
    pthread_mutex_lock(&g_lyrics.lock);

    int has = g_lyrics.has_lyrics && g_lyrics.count > 0;
    if (out_current) *out_current = has ? g_lyrics.current_index : -1;
    if (out_next) {
        *out_next = (has && g_lyrics.current_index >= 0 &&
                     g_lyrics.current_index + 1 < g_lyrics.count)
            ? g_lyrics.current_index + 1 : -1;
    }
    if (out_has_timestamps) *out_has_timestamps = g_lyrics.has_timestamps;
    if (out_source) *out_source = g_lyrics.source;

    pthread_mutex_unlock(&g_lyrics.lock);
    return has ? 1 : 0;
}

int lyrics_highlight_count(void)
{
    pthread_mutex_lock(&g_lyrics.lock);
    int count = g_lyrics.highlight_count;
    pthread_mutex_unlock(&g_lyrics.lock);
    return count;
}

int lyrics_page(int offset, int count, LyricsPage *out)
{
    if (!out || offset < 0 || count < 0 || count > LYRICS_PAGE_MAX) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_lyrics.lock);

    int total = g_lyrics.count;
    int end = offset + count;
    if (end > total) end = total;

    out->total = total;
    out->offset = offset;
    out->current_index = g_lyrics.current_index;
    out->has_lyrics = g_lyrics.has_lyrics;
    out->has_timestamps = g_lyrics.has_timestamps;
    out->source = g_lyrics.source;

    int written = 0;
    for (int i = offset; i < end && written < LYRICS_PAGE_MAX; i++) {
        out->lines[written++] = g_lyrics.lines[i];
    }
    out->count = written;

    pthread_mutex_unlock(&g_lyrics.lock);
    return written;
}

void lyrics_track_id(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    const char *path = lyrics_current_track_path();
    if (!path) {
        return;
    }

    /* 稳定标识 = 路径的 64 位 FNV-1a 十六进制（前端与 D-Bus 共用） */
    unsigned long long hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        hash ^= (unsigned long long)*p;
        hash *= 1099511628211ULL;
    }
    snprintf(out, out_size, "%016llx", hash);
}
