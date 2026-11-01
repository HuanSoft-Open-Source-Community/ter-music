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
#include <stdint.h>
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
static int64_t (*g_position_ms_source)(void) = NULL;

void lyrics_set_position_source(int (*source)(void))
{
    g_position_source = source;
}

void lyrics_set_position_ms_source(int64_t (*source)(void))
{
    g_position_ms_source = source;
}

int64_t lyrics_position_ms(void)
{
    if (g_position_ms_source) {
        return g_position_ms_source();
    }
    if (g_position_source) {
        /* 旧测试用秒注入：保留语义，换算成毫秒 */
        return (int64_t)g_position_source() * 1000;
    }
    return audio_get_position_ms();
}

int lyrics_position_seconds(void)
{
    return (int)(lyrics_position_ms() / 1000);
}

/* 秒 → 毫秒（四舍五入）。不引 libm：单测链接环境没有 -lm。 */
static int64_t lyric_seconds_to_ms(double seconds)
{
    if (seconds <= 0.0) {
        return 0;
    }
    return (int64_t)(seconds * 1000.0 + 0.5);
}

/* UTF-8 码点字节长度；非法/被截断的序列按 1 字节前进，保证扫描不会卡住。 */
static size_t lyric_utf8_char_len(const char *src)
{
    if (!src || src[0] == '\0') {
        return 0;
    }
    unsigned char c = (unsigned char)src[0];
    size_t need = 1;
    if ((c & 0xE0) == 0xC0) {
        need = 2;
    } else if ((c & 0xF0) == 0xE0) {
        need = 3;
    } else if ((c & 0xF8) == 0xF0) {
        need = 4;
    } else if (c >= 0x80) {
        return 1;   /* 落单续字节/非法首字节 */
    }
    for (size_t i = 1; i < need; i++) {
        unsigned char cc = (unsigned char)src[i];
        if (cc == '\0' || (cc & 0xC0) != 0x80) {
            return 1;
        }
    }
    return need;
}

static int lyric_count_chars(const char *src)
{
    int count = 0;
    if (!src) {
        return 0;
    }
    while (*src) {
        size_t len = lyric_utf8_char_len(src);
        if (len == 0) {
            break;
        }
        src += len;
        count++;
    }
    return count;
}

/* 把 src 按完整 UTF-8 字符复制到 dest（最多 dest_size-1 字节），返回写入的
 * 字符数。用于纯文本行/元数据头，避免截断点多字节字符。 */
static int lyric_copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return 0;
    }
    dest[0] = '\0';
    if (!src) {
        return 0;
    }

    size_t written = 0;
    int chars = 0;
    while (*src) {
        size_t len = lyric_utf8_char_len(src);
        if (len == 0 || written + len >= dest_size) {
            break;
        }
        memcpy(dest + written, src, len);
        written += len;
        src += len;
        chars++;
    }
    dest[written] = '\0';
    return chars;
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
    uint32_t *pool = NULL;
    pthread_mutex_lock(&g_lyrics.lock);
    pool = g_lyrics.char_times_ms;
    g_lyrics.char_times_ms = NULL;
    g_lyrics.char_times_count = 0;
    g_lyrics.count = 0;
    g_lyrics.current_index = -1;
    g_lyrics.highlight_count = 0;
    g_lyrics.has_lyrics = 0;
    g_lyrics.has_timestamps = 0;
    g_lyrics.cursor_index = -1;
    g_lyrics.source = LYRICS_SOURCE_AUTO;
    g_lyrics.current_char_sung = 0;
    g_lyrics.current_char_total = 0;
    pthread_mutex_unlock(&g_lyrics.lock);
    free(pool);
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
 * 解析 LRC 时间戳字符串（不含方括号）
 * 支持 [mm:ss]、[mm:ss.x]、[mm:ss.xx]、[mm:ss.xxx]；小数位按实际位数缩放。
 * @return 时间戳（秒，含毫秒）；-1 = 非法
 */
static double parse_timestamp(const char *time_str) {
    if (!time_str || time_str[0] == '\0') {
        return -1.0;
    }

    int mm = 0;
    int ss = 0;
    int consumed = 0;
    if (sscanf(time_str, "%d:%d%n", &mm, &ss, &consumed) < 2) {
        return -1.0;
    }
    if (mm < 0 || ss < 0) {
        return -1.0;
    }

    double value = (double)mm * 60.0 + (double)ss;
    const char *frac = time_str + consumed;
    if (*frac == '.') {
        frac++;
        int digits = 0;
        int frac_value = 0;
        while (*frac >= '0' && *frac <= '9' && digits < 9) {
            frac_value = frac_value * 10 + (*frac - '0');
            digits++;
            frac++;
        }
        if (digits == 0 || *frac != '\0') {
            return -1.0;
        }
        int divisor = 1;
        for (int i = 0; i < digits; i++) {
            divisor *= 10;
        }
        value += (double)frac_value / (double)divisor;
    } else if (*frac != '\0') {
        return -1.0;
    }

    return value;
}

/* 尝试把 src 处的 [..] 解析成时间标签。
 * @return 标签字节长度（含方括号）；0 = 不是时间标签
 */
static int parse_time_tag(const char *src, int64_t *out_ms) {
    if (!src || src[0] != '[') {
        return 0;
    }
    const char *end = strchr(src, ']');
    if (!end) {
        return 0;
    }
    size_t len = (size_t)(end - src - 1);
    if (len == 0 || len >= 24) {
        return 0;
    }

    char buffer[24];
    memcpy(buffer, src + 1, len);
    buffer[len] = '\0';

    double ts = parse_timestamp(buffer);
    if (ts < 0.0) {
        return 0;
    }
    if (out_ms) {
        *out_ms = lyric_seconds_to_ms(ts);
    }
    return (int)(end - src + 1);
}

/* 一行内的逐段时间边界：(本段起始字符下标, 本段起始毫秒) */
typedef struct {
    int char_index;
    int64_t time_ms;
} LyricSegment;

/* 解析中的临时歌词文档。逐字池与段表都是动态的，加载完成后一次性提交到
 * g_lyrics，避免解析期占用固定大数组。 */
typedef struct {
    LyricLine lines[MAX_LYRIC_LINES];
    int count;
    LyricSegment *segments;
    int segment_count;
    int segment_capacity;
    int line_segment_offset[MAX_LYRIC_LINES];
    int line_segment_count[MAX_LYRIC_LINES];
    int line_pool_offset[MAX_LYRIC_LINES];
    int has_timestamps;
    uint32_t *char_times;
    int char_times_count;
} ParsedLyrics;

static void parsed_lyrics_free(ParsedLyrics *parsed)
{
    if (!parsed) {
        return;
    }
    free(parsed->segments);
    free(parsed->char_times);
    parsed->segments = NULL;
    parsed->char_times = NULL;
    parsed->segment_count = 0;
    parsed->segment_capacity = 0;
    parsed->char_times_count = 0;
}

static int parsed_add_segment(ParsedLyrics *parsed, int char_index, int64_t time_ms)
{
    if (parsed->segment_count == parsed->segment_capacity) {
        int capacity = parsed->segment_capacity > 0
            ? parsed->segment_capacity * 2 : 128;
        LyricSegment *grown = realloc(parsed->segments,
                                      (size_t)capacity * sizeof(LyricSegment));
        if (!grown) {
            return -1;
        }
        parsed->segments = grown;
        parsed->segment_capacity = capacity;
    }
    parsed->segments[parsed->segment_count].char_index = char_index;
    parsed->segments[parsed->segment_count].time_ms = time_ms;
    parsed->segment_count++;
    return 0;
}

/* 解析单行。line 可被原地改写（解码实体、插入 NUL）。
 * @return 1 = 收录一行；0 = 跳过；-1 = 内存不足 */
static int parse_one_lyric_line(char *line, int keep_metadata, int keep_plain,
                                ParsedLyrics *parsed)
{
    if (!line || line[0] == '\0' || parsed->count >= MAX_LYRIC_LINES) {
        return 0;
    }

    /* 外部 LRC 的 [ti:] / [ar:] / [al:] 头：保留旧行为，作为无时间戳行收录 */
    if (keep_metadata && line[0] == '[' &&
        (line[1] == 't' || line[1] == 'a') && line[2] != '\0' && line[3] == ':') {
        char *close = strchr(line + 4, ']');
        if (close && close > line + 4) {
            *close = '\0';
            LyricLine *out = &parsed->lines[parsed->count];
            memset(out, 0, sizeof(*out));
            out->timing = LYRIC_TIMING_NONE;
            out->char_count = lyric_copy_text(out->text, sizeof(out->text),
                                               line + 4);
            parsed->line_segment_offset[parsed->count] = parsed->segment_count;
            parsed->line_segment_count[parsed->count] = 0;
            parsed->line_pool_offset[parsed->count] = 0;
            *close = ']';
            parsed->count++;
            return 1;
        }
    }

    char *tag = strchr(line, '[');
    int64_t start_ms = 0;
    int tag_len = tag ? parse_time_tag(tag, &start_ms) : 0;

    if (tag_len <= 0) {
        if (!keep_plain) {
            return 0;       /* 外部 LRC：没有时间戳的普通行不收录 */
        }
        /* 内嵌歌词：跳过 [xx:...] 元数据头，其余按无时间戳行收录 */
        if (line[0] == '[') {
            char *colon = strchr(line, ':');
            char *close = strchr(line, ']');
            if (colon && close && colon < close) {
                return 0;
            }
        }
        LyricLine *out = &parsed->lines[parsed->count];
        memset(out, 0, sizeof(*out));
        out->timing = LYRIC_TIMING_NONE;
        out->char_count = lyric_copy_text(out->text, sizeof(out->text), line);
        parsed->line_segment_offset[parsed->count] = parsed->segment_count;
        parsed->line_segment_count[parsed->count] = 0;
        parsed->line_pool_offset[parsed->count] = 0;
        parsed->count++;
        return 1;
    }

    LyricLine *out = &parsed->lines[parsed->count];
    memset(out, 0, sizeof(*out));
    out->timestamp = (double)start_ms / 1000.0;
    out->has_timestamp = 1;
    out->timing = LYRIC_TIMING_NONE;
    parsed->has_timestamps = 1;

    int segment_start = parsed->segment_count;
    if (parsed_add_segment(parsed, 0, start_ms) != 0) {
        return -1;
    }

    const char *scan = tag + tag_len;
    char *dst = out->text;
    int dst_bytes = 0;
    int char_count = 0;
    int text_started = 0;

    while (*scan != '\0') {
        if (*scan == '[') {
            int64_t seg_ms = 0;
            int seg_len = parse_time_tag(scan, &seg_ms);
            if (seg_len > 0) {
                if (text_started &&
                    parsed_add_segment(parsed, char_count, seg_ms) != 0) {
                    return -1;
                }
                scan += seg_len;
                continue;
            }
        }

        unsigned char c = (unsigned char)*scan;
        if (!text_started && (c == ' ' || c == '\t')) {
            scan++;
            continue;
        }

        size_t char_len = lyric_utf8_char_len(scan);
        if (char_len == 0) {
            break;
        }
        if (dst_bytes + (int)char_len < (int)sizeof(out->text)) {
            memcpy(dst + dst_bytes, scan, char_len);
            dst_bytes += (int)char_len;
            dst[dst_bytes] = '\0';
            char_count++;
        }
        text_started = 1;
        scan += char_len;
    }

    /* 去掉行尾空白并重算字符数（段下标随后统一钳制） */
    while (dst_bytes > 0 && (dst[dst_bytes - 1] == ' ' || dst[dst_bytes - 1] == '\t')) {
        dst_bytes--;
    }
    dst[dst_bytes] = '\0';
    char_count = lyric_count_chars(dst);

    double end_timestamp = 0.0;
    int segment_count = parsed->segment_count - segment_start;
    if (segment_count > 1) {
        LyricSegment *last = &parsed->segments[parsed->segment_count - 1];
        if (last->char_index == char_count) {
            /* 正文之后紧跟的最后一个时间标签 = 行结束时间 */
            end_timestamp = (double)last->time_ms / 1000.0;
            parsed->segment_count--;
            segment_count--;
        }
    }

    if (char_count == 0) {
        /* 没有正文（纯音乐/空行占位）：保留旧行为，但不参与逐字 */
        (void)lyric_copy_text(out->text, sizeof(out->text),
                              i18n_get("lyrics.instrumental"));
        char_count = 0;
    }

    out->end_timestamp = end_timestamp;
    out->char_count = char_count;
    parsed->line_segment_offset[parsed->count] = segment_start;
    parsed->line_segment_count[parsed->count] = segment_count;
    parsed->line_pool_offset[parsed->count] = 0;
    parsed->count++;
    return 1;
}

/* 推导行结束时间、逐字时间池，并确定每行的 timing 模式。 */
static int finalize_parsed_lyrics(ParsedLyrics *parsed)
{
    int total_chars = 0;

    for (int i = 0; i < parsed->count; i++) {
        LyricLine *line = &parsed->lines[i];
        double explicit_end = line->end_timestamp;
        line->timing = LYRIC_TIMING_NONE;
        line->end_timestamp = 0.0;

        if (!line->has_timestamp) {
            continue;
        }

        int64_t start_ms = lyric_seconds_to_ms(line->timestamp);
        int64_t end_ms = lyric_seconds_to_ms(explicit_end);
        if (end_ms <= start_ms) {
            end_ms = 0;
            for (int j = i + 1; j < parsed->count; j++) {
                if (!parsed->lines[j].has_timestamp) {
                    continue;
                }
                int64_t next_ms = lyric_seconds_to_ms(parsed->lines[j].timestamp);
                if (next_ms > start_ms) {
                    end_ms = next_ms;
                    break;
                }
            }
        }
        if (end_ms <= start_ms) {
            /* 最后一行：沿用前一个有效行的时长，避免依赖尚未就绪的音频总时长 */
            for (int j = i - 1; j >= 0; j--) {
                if (!parsed->lines[j].has_timestamp) {
                    continue;
                }
                int64_t prev_start = lyric_seconds_to_ms(parsed->lines[j].timestamp);
                int64_t prev_end = lyric_seconds_to_ms(parsed->lines[j].end_timestamp);
                if (prev_end > prev_start) {
                    end_ms = start_ms + (prev_end - prev_start);
                    break;
                }
            }
        }

        if (end_ms <= start_ms || line->char_count <= 0) {
            continue;
        }

        line->end_timestamp = (double)end_ms / 1000.0;
        if (parsed->line_segment_count[i] >= 2) {
            line->timing = LYRIC_TIMING_CHARS;
            total_chars += line->char_count;
        } else {
            line->timing = LYRIC_TIMING_LINE;
        }
    }

    if (total_chars > 0) {
        parsed->char_times = malloc((size_t)total_chars * sizeof(uint32_t));
        if (!parsed->char_times) {
            /* 内存不足：全部降级为行级插值，歌词仍然可用 */
            for (int i = 0; i < parsed->count; i++) {
                if (parsed->lines[i].timing == LYRIC_TIMING_CHARS) {
                    parsed->lines[i].timing = LYRIC_TIMING_LINE;
                }
            }
            total_chars = 0;
        }
    }

    int pool_pos = 0;
    for (int i = 0; i < parsed->count; i++) {
        LyricLine *line = &parsed->lines[i];
        if (line->timing != LYRIC_TIMING_CHARS) {
            parsed->line_pool_offset[i] = 0;
            continue;
        }

        int offset = pool_pos;
        int count = line->char_count;
        int64_t start_ms = lyric_seconds_to_ms(line->timestamp);
        int64_t end_ms = lyric_seconds_to_ms(line->end_timestamp);
        for (int j = 0; j < count; j++) {
            parsed->char_times[offset + j] =
                (uint32_t)(start_ms > UINT32_MAX ? UINT32_MAX : start_ms);
        }

        int seg_offset = parsed->line_segment_offset[i];
        int seg_count = parsed->line_segment_count[i];
        for (int k = 0; k < seg_count; k++) {
            LyricSegment *seg = &parsed->segments[seg_offset + k];
            int c0 = seg->char_index;
            if (c0 < 0) c0 = 0;
            if (c0 > count) c0 = count;
            int c1 = count;
            if (k + 1 < seg_count) {
                c1 = parsed->segments[seg_offset + k + 1].char_index;
                if (c1 < c0) c1 = c0;
                if (c1 > count) c1 = count;
            }
            if (c1 <= c0) {
                continue;
            }

            int64_t t0 = seg->time_ms;
            if (t0 < start_ms) t0 = start_ms;
            if (t0 > end_ms) t0 = end_ms;
            int64_t t1 = (k + 1 < seg_count)
                ? parsed->segments[seg_offset + k + 1].time_ms : end_ms;
            if (t1 < t0) t1 = t0;
            if (t1 > end_ms) t1 = end_ms;

            for (int j = c0; j < c1; j++) {
                int64_t when = t0;
                if (t1 > t0) {
                    when = t0 + ((t1 - t0) * (int64_t)(j - c0)) / (int64_t)(c1 - c0);
                    if (when < t0) when = t0;
                    if (when > t1) when = t1;
                }
                if (when > UINT32_MAX) when = UINT32_MAX;
                parsed->char_times[offset + j] = (uint32_t)when;
            }
        }

        for (int j = 1; j < count; j++) {
            if (parsed->char_times[offset + j] < parsed->char_times[offset + j - 1]) {
                parsed->char_times[offset + j] = parsed->char_times[offset + j - 1];
            }
        }

        parsed->line_pool_offset[i] = offset;
        pool_pos += count;
    }
    parsed->char_times_count = pool_pos;
    return 0;
}

/* 解析完整歌词文本（原地改写）。外部 .lrc 与内嵌 LRC 共用这一条路径。
 * @return 收录行数；-1 = 解析失败（内存不足） */
static int parse_lyrics_text(char *text, int keep_metadata, int keep_plain,
                             ParsedLyrics *parsed)
{
    memset(parsed, 0, sizeof(*parsed));
    if (!text) {
        return 0;
    }

    char *cursor = text;
    while (cursor && *cursor != '\0') {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = line + strlen(line);
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                           line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }

        decode_html_entities(line);
        int rc = parse_one_lyric_line(line, keep_metadata, keep_plain, parsed);
        if (rc < 0) {
            parsed_lyrics_free(parsed);
            return -1;
        }
    }

    if (finalize_parsed_lyrics(parsed) != 0) {
        parsed_lyrics_free(parsed);
        return -1;
    }
    return parsed->count;
}

/* 把解析结果一次性提交为全局歌词状态，并释放上一份逐字池。 */
static void commit_parsed_lyrics(ParsedLyrics *parsed, int source,
                                 int has_timestamps)
{
    uint32_t *old_pool = NULL;

    pthread_mutex_lock(&g_lyrics.lock);
    old_pool = g_lyrics.char_times_ms;

    g_lyrics.count = parsed->count;
    memcpy(g_lyrics.lines, parsed->lines,
           sizeof(LyricLine) * (size_t)parsed->count);
    for (int i = 0; i < parsed->count; i++) {
        g_lyrics.line_char_offset[i] = parsed->line_pool_offset[i];
    }
    g_lyrics.char_times_ms = parsed->char_times;
    g_lyrics.char_times_count = parsed->char_times_count;
    parsed->char_times = NULL;
    parsed->char_times_count = 0;

    g_lyrics.has_lyrics = parsed->count > 0 ? 1 : 0;
    g_lyrics.has_timestamps = has_timestamps ? 1 : 0;
    g_lyrics.current_index = has_timestamps ? -1 : 0;
    g_lyrics.highlight_count = 0;
    g_lyrics.current_char_sung = 0;
    g_lyrics.current_char_total = 0;
    g_lyrics.cursor_index = -1;
    g_lyrics.source = source;

    pthread_mutex_unlock(&g_lyrics.lock);

    free(old_pool);
    /* 段表只服务解析期，提交后不再需要（逐字池已转交全局状态）。 */
    free(parsed->segments);
    parsed->segments = NULL;
    parsed->segment_count = 0;
    parsed->segment_capacity = 0;
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

    /* ── Parse lyrics text（内嵌：保留纯文本行，跳过元数据头） ── */
    ParsedLyrics parsed;
    int count = parse_lyrics_text(lyrics_text, 0, 1, &parsed);
    free(lyrics_text);

    if (count <= 0) {
        parsed_lyrics_free(&parsed);
        return -1;
    }

    int has_timestamps = parsed.has_timestamps;
    commit_parsed_lyrics(&parsed, LYRICS_SOURCE_EMBEDDED, has_timestamps);

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
    
    /* ── Parse lyrics text（外部：保留 [ti:]/[ar:]/[al:] 头行，跳过普通无时间戳行） ── */
    ParsedLyrics parsed;
    int count = parse_lyrics_text(lyrics_text, 1, 0, &parsed);
    free(lyrics_text);

    // 如果没有解析到任何歌词
    if (count <= 0) {
        parsed_lyrics_free(&parsed);
        log_debug("lyrics", "No lyrics content in '%s'", lrc_path);
        reset_loaded_lyrics();
        return;
    }

    commit_parsed_lyrics(&parsed, LYRICS_SOURCE_EXTERNAL, 1);

    log_info("lyrics", "Loaded %d lyric lines from '%s'", count, lrc_path);
}

void clear_lyrics(void) {
    log_debug("lyrics", "clear_lyrics() called");
    reset_loaded_lyrics();
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


/* ── 逐字进度 ───────────────────────────────────────────────────
 * 所有函数都要求调用方持有 g_lyrics.lock（或由下面的公开包装器加锁）。 */

static int lyric_line_char_progress_locked(int index, int64_t position_ms,
                                           int *out_sung, int *out_total)
{
    if (out_sung) *out_sung = 0;
    if (out_total) *out_total = 0;

    if (index < 0 || index >= g_lyrics.count) {
        return 0;
    }

    const LyricLine *line = &g_lyrics.lines[index];
    int total = line->char_count;
    if (line->timing == LYRIC_TIMING_NONE || !line->has_timestamp || total <= 0) {
        return 0;
    }
    if (out_total) {
        *out_total = total;
    }

    int64_t start_ms = lyric_seconds_to_ms(line->timestamp);
    int64_t end_ms = lyric_seconds_to_ms(line->end_timestamp);
    if (end_ms <= start_ms) {
        return 0;
    }
    if (position_ms < start_ms) {
        return 1;                       /* 尚未到行首 */
    }
    if (position_ms >= end_ms) {
        if (out_sung) *out_sung = total;
        return 1;
    }

    if (line->timing == LYRIC_TIMING_CHARS) {
        int offset = g_lyrics.line_char_offset[index];
        if (g_lyrics.char_times_ms && offset >= 0 &&
            offset + total <= g_lyrics.char_times_count) {
            int sung = 0;
            while (sung < total &&
                   (int64_t)g_lyrics.char_times_ms[offset + sung] <= position_ms) {
                sung++;
            }
            if (out_sung) *out_sung = sung;
            return 1;
        }
        /* 池缺失/长度不符：回退到行级插值 */
    }

    int64_t span = end_ms - start_ms;
    int64_t elapsed = position_ms - start_ms;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > span) elapsed = span;
    int sung = (int)((elapsed * (int64_t)total) / span);
    if (sung < 0) sung = 0;
    if (sung > total) sung = total;
    if (out_sung) *out_sung = sung;
    return 1;
}

int lyrics_char_progress_at(int line_index, int64_t position_ms,
                            int *out_sung, int *out_total)
{
    if (out_sung) *out_sung = 0;
    if (out_total) *out_total = 0;
    pthread_mutex_lock(&g_lyrics.lock);
    int rc = lyric_line_char_progress_locked(line_index, position_ms,
                                             out_sung, out_total);
    pthread_mutex_unlock(&g_lyrics.lock);
    return rc;
}

int lyrics_char_progress(int *out_sung, int *out_total)
{
    if (out_sung) *out_sung = 0;
    if (out_total) *out_total = 0;
    int64_t position_ms = lyrics_position_ms();
    pthread_mutex_lock(&g_lyrics.lock);
    int rc = lyric_line_char_progress_locked(g_lyrics.current_index, position_ms,
                                             out_sung, out_total);
    pthread_mutex_unlock(&g_lyrics.lock);
    return rc;
}

void lyrics_snapshot(LyricsSnapshot *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->current_index = -1;
    out->next_index = -1;

    int64_t position_ms = lyrics_position_ms();

    pthread_mutex_lock(&g_lyrics.lock);

    out->has_lyrics = g_lyrics.has_lyrics;
    out->has_timestamps = g_lyrics.has_timestamps;
    out->source = g_lyrics.source;

    int count = g_lyrics.count;
    if (out->has_lyrics && count > 0) {
        if (!out->has_timestamps) {
            out->current_index = 0;
            out->next_index = (count > 1) ? 1 : -1;
        } else {
            for (int i = 0; i < count; i++) {
                if (lyric_seconds_to_ms(g_lyrics.lines[i].timestamp) <= position_ms) {
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
            const LyricLine *line = &g_lyrics.lines[out->current_index];
            out->current_timestamp = line->timestamp;
            snprintf(out->current_text, sizeof(out->current_text), "%s", line->text);
            (void)lyric_line_char_progress_locked(out->current_index, position_ms,
                                                  &out->current_char_sung,
                                                  &out->current_char_total);
        }
        if (out->next_index >= 0 && out->next_index < count) {
            const LyricLine *line = &g_lyrics.lines[out->next_index];
            out->next_timestamp = line->timestamp;
            snprintf(out->next_text, sizeof(out->next_text), "%s", line->text);
        }
    }

    pthread_mutex_unlock(&g_lyrics.lock);
}

/* ── 推进（由 core_tick 每轮调用） ─────────────────────────────── */

int lyrics_tick(void)
{
    int64_t position_ms = lyrics_position_ms();

    pthread_mutex_lock(&g_lyrics.lock);

    if (!g_lyrics.has_lyrics || g_lyrics.count == 0) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int changed = 0;

    /* 无时间戳的纯文本内嵌歌词：固定停在第 0 行 */
    if (!g_lyrics.has_timestamps) {
        if (g_lyrics.current_index != 0 || g_lyrics.current_char_sung != 0 ||
            g_lyrics.current_char_total != 0) {
            g_lyrics.current_index = 0;
            g_lyrics.highlight_count = 0;
            g_lyrics.current_char_sung = 0;
            g_lyrics.current_char_total = 0;
            changed = 1;
        }
        pthread_mutex_unlock(&g_lyrics.lock);
        return changed;
    }

    int new_index = -1;
    int new_highlight_count = 0;

    /* 找到最后一个 timestamp <= 播放位置的行；该行之后若还有**同一时间戳**
     * 的行，则一起高亮（最多两行，与 LRC 里一行时间戳带多行文本的写法对应）。 */
    for (int i = 0; i < g_lyrics.count; i++) {
        if (lyric_seconds_to_ms(g_lyrics.lines[i].timestamp) > position_ms) {
            break;
        }
        new_index = i;
        new_highlight_count = 1;
        if (i + 1 < g_lyrics.count &&
            g_lyrics.lines[i + 1].timestamp == g_lyrics.lines[i].timestamp) {
            new_highlight_count = 2;
        }
    }

    int new_char_sung = 0;
    int new_char_total = 0;
    (void)lyric_line_char_progress_locked(new_index, position_ms,
                                          &new_char_sung, &new_char_total);

    if (new_index != g_lyrics.current_index ||
        new_highlight_count != g_lyrics.highlight_count ||
        new_char_sung != g_lyrics.current_char_sung ||
        new_char_total != g_lyrics.current_char_total) {
        g_lyrics.current_index = new_index;
        g_lyrics.highlight_count = new_highlight_count;
        g_lyrics.current_char_sung = new_char_sung;
        g_lyrics.current_char_total = new_char_total;
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
