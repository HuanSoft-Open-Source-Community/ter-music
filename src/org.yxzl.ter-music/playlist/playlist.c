#include "types.h"
#include "playlist/playlist.h"
#include "config/config.h"
#include "search/search.h"
#include "pinyin_table.h"
#include "remote/remote.h"
#include "playlist/ape_tag.h"
#include "ui/braille/braille_art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <jpeglib.h>

const char *audio_extensions[] = {
    ".mp3", ".MP3", ".flac", ".FLAC", ".wav", ".WAV", 
    ".ogg", ".OGG", ".m4a", ".M4A", ".aac", ".AAC",
    ".wma", ".WMA", ".ape", ".APE", ".opus", ".OPUS",
    NULL
};

// 全局变量定义
Playlist g_playlist = {0};
int g_selected_index = 0;  // 当前选中的歌曲索引

// 控制区焦点状态
// 0: 列表模式 (List), 1: 控制模式 (Control)
int g_control_focus = 0; 
// 当前选中的控件索引 (0:上一曲，1:播放/暂停，2:下一曲，3:停止，4:循环，5:音量，6:进度条)
int g_current_control_idx = 1;

SortState g_sort_state = {0};

static pthread_mutex_t g_playlist_mutex = PTHREAD_MUTEX_INITIALIZER;

int is_audio_file(const char *filename);
void get_audio_metadata(const char *path, char *title, char *artist, char *album);

static int decode_utf8_codepoint(const unsigned char *str, int *len) {
    if (!str || !str[0]) {
        if (len) {
            *len = 0;
        }
        return 0;
    }

    if ((str[0] & 0x80) == 0) {
        if (len) {
            *len = 1;
        }
        return str[0];
    }

    if ((str[0] & 0xE0) == 0xC0 && str[1]) {
        if (len) {
            *len = 2;
        }
        return ((str[0] & 0x1F) << 6) | (str[1] & 0x3F);
    }

    if ((str[0] & 0xF0) == 0xE0 && str[1] && str[2]) {
        if (len) {
            *len = 3;
        }
        return ((str[0] & 0x0F) << 12) |
               ((str[1] & 0x3F) << 6) |
               (str[2] & 0x3F);
    }

    if ((str[0] & 0xF8) == 0xF0 && str[1] && str[2] && str[3]) {
        if (len) {
            *len = 4;
        }
        return ((str[0] & 0x07) << 18) |
               ((str[1] & 0x3F) << 12) |
               ((str[2] & 0x3F) << 6) |
               (str[3] & 0x3F);
    }

    if (len) {
        *len = 1;
    }
    return str[0];
}

static const char *lookup_pinyin_syllable(int codepoint) {
    if (codepoint < PINYIN_TABLE_BASE ||
        codepoint >= PINYIN_TABLE_BASE + PINYIN_TABLE_SIZE) {
        return NULL;
    }

    uint32_t offset = g_pinyin_offsets[codepoint - PINYIN_TABLE_BASE];
    if (offset == 0 || offset >= sizeof(g_pinyin_blob)) {
        return NULL;
    }

    return (const char *)&g_pinyin_blob[offset];
}

static void append_search_char(char *dest, size_t dest_size, size_t *len, char c) {
    if (!dest || !len || *len >= dest_size - 1) {
        return;
    }

    dest[*len] = c;
    (*len)++;
    dest[*len] = '\0';
}

static void append_search_text(char *dest, size_t dest_size, size_t *len, const char *text) {
    if (!dest || !len || !text) {
        return;
    }

    while (*text && *len < dest_size - 1) {
        dest[*len] = *text;
        (*len)++;
        text++;
    }
    dest[*len] = '\0';
}

static void append_search_segment(char *dest, size_t dest_size, const char *segment) {
    if (!dest || dest_size == 0 || !segment || segment[0] == '\0') {
        return;
    }

    size_t len = strlen(dest);
    if (len > 0 && len < dest_size - 1) {
        dest[len++] = ' ';
        dest[len] = '\0';
    }

    size_t segment_len = strlen(segment);
    if (segment_len > dest_size - len - 1) {
        segment_len = dest_size - len - 1;
    }

    if (segment_len > 0) {
        memcpy(dest + len, segment, segment_len);
        dest[len + segment_len] = '\0';
    }
}

static void normalize_search_token(const char *src, char *dest, size_t dest_size) {
    size_t len = 0;

    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    while (src && *src && len < dest_size - 1) {
        unsigned char c = (unsigned char)*src++;
        if (isalnum(c)) {
            dest[len++] = (char)tolower(c);
        }
    }
    dest[len] = '\0';
}

static int string_contains_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) {
        return 1;
    }
    if (!haystack || !haystack[0]) {
        return 0;
    }

    char h_lower[MAX_META_LEN * 2];
    char n_lower[MAX_META_LEN * 2];
    int hi = 0;
    int ni = 0;

    while (*haystack && hi < (int)sizeof(h_lower) - 1) {
        h_lower[hi++] = (char)tolower((unsigned char)*haystack++);
    }
    h_lower[hi] = '\0';

    while (*needle && ni < (int)sizeof(n_lower) - 1) {
        n_lower[ni++] = (char)tolower((unsigned char)*needle++);
    }
    n_lower[ni] = '\0';

    return strstr(h_lower, n_lower) != NULL;
}

static void build_search_key(const char *src, char *dest, size_t dest_size) {
    char plain[MAX_SEARCH_KEY_LEN] = "";
    char initials[MAX_SEARCH_KEY_LEN] = "";
    char full[MAX_SEARCH_KEY_LEN] = "";
    size_t plain_len = 0;
    size_t initials_len = 0;
    size_t full_len = 0;
    int ascii_word_start = 1;
    const unsigned char *ptr = (const unsigned char *)src;

    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }

    while (*ptr) {
        int utf8_len = 1;
        int codepoint = decode_utf8_codepoint(ptr, &utf8_len);

        if (utf8_len == 1) {
            unsigned char c = *ptr;
            if (isalnum(c)) {
                char lower = (char)tolower(c);
                append_search_char(plain, sizeof(plain), &plain_len, lower);
                append_search_char(full, sizeof(full), &full_len, lower);
                if (ascii_word_start) {
                    append_search_char(initials, sizeof(initials), &initials_len, lower);
                }
                ascii_word_start = 0;
            } else {
                ascii_word_start = 1;
            }
        } else {
            const char *pinyin = lookup_pinyin_syllable(codepoint);
            if (pinyin && pinyin[0] != '\0') {
                append_search_char(initials, sizeof(initials), &initials_len, pinyin[0]);
                append_search_text(full, sizeof(full), &full_len, pinyin);
            }
            ascii_word_start = 1;
        }

        ptr += utf8_len > 0 ? utf8_len : 1;
    }

    append_search_segment(dest, dest_size, plain);
    append_search_segment(dest, dest_size, initials);
    append_search_segment(dest, dest_size, full);
}

void playlist_lock(void) {
    pthread_mutex_lock(&g_playlist_mutex);
}

void playlist_unlock(void) {
    pthread_mutex_unlock(&g_playlist_mutex);
}

int playlist_count(void) {
    int count = 0;

    playlist_lock();
    count = g_playlist.count;
    playlist_unlock();
    return count;
}

int playlist_is_loaded(void) {
    int is_loaded = 0;

    playlist_lock();
    is_loaded = g_playlist.is_loaded;
    playlist_unlock();
    return is_loaded;
}

int playlist_has_multiple_sources(void) {
    int has_multiple_sources = 0;

    playlist_lock();
    has_multiple_sources = g_playlist.has_multiple_sources;
    playlist_unlock();
    return has_multiple_sources;
}

void playlist_copy_folder_path(char *dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }

    playlist_lock();
    snprintf(dest, dest_size, "%s", g_playlist.folder_path);
    playlist_unlock();
}

int playlist_get_track_path(int index, char *dest, size_t dest_size) {
    int ok = 0;

    if (!dest || dest_size == 0) {
        return -1;
    }

    dest[0] = '\0';

    playlist_lock();
    if (index >= 0 && index < g_playlist.count) {
        snprintf(dest, dest_size, "%s", g_playlist.tracks[index]);
        ok = 1;
    }
    playlist_unlock();

    return ok ? 0 : -1;
}

int playlist_find_track_index_by_path(const char *track_path) {
    if (!track_path || track_path[0] == '\0') {
        return -1;
    }

    int found = -1;
    playlist_lock();
    for (int i = 0; i < g_playlist.count; i++) {
        if (strcmp(g_playlist.tracks[i], track_path) == 0) {
            found = i;
            break;
        }
    }
    playlist_unlock();
    return found;
}

static void clear_metadata_cache_locked(Playlist *playlist) {
    if (!playlist) {
        return;
    }

    memset(playlist->cache, 0, sizeof(playlist->cache));
    playlist->cache_count = 0;
}

void reset_playlist_state(void) {
    log_info("playlist", "Resetting playlist state");
    playlist_lock();
    memset(&g_playlist, 0, sizeof(g_playlist));
    playlist_unlock();
    search_clear();
    memset(&g_sort_state, 0, sizeof(g_sort_state));
}

static int playlist_contains_track_in(const Playlist *playlist, const char *path) {
    if (!playlist || !path || path[0] == '\0') {
        return 0;
    }

    for (int i = 0; i < playlist->count; i++) {
        if (strcmp(playlist->tracks[i], path) == 0) {
            return 1;
        }
    }

    return 0;
}

static int scan_playlist_directory_into(Playlist *playlist, const char *path, int append_mode) {
    if (!playlist) {
        return -1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    int before_count = playlist->count;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && playlist->count < MAX_TRACKS) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            char full_path[MAX_PATH_LEN];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

            if (!is_audio_file(entry->d_name)) {
                continue;
            }

            if (append_mode && playlist_contains_track_in(playlist, full_path)) {
                continue;
            }

            strncpy(playlist->tracks[playlist->count], full_path, MAX_PATH_LEN - 1);
            playlist->tracks[playlist->count][MAX_PATH_LEN - 1] = '\0';
            playlist->count++;
        }
    }

    closedir(dir);
    return playlist->count - before_count;
}

static void copy_metadata_field(char *dest, size_t dest_size, const char *value) {
    if (!dest || dest_size == 0) {
        return;
    }

    if (!value || value[0] == '\0') {
        dest[0] = '\0';
        return;
    }

    snprintf(dest, dest_size, "%s", value);
    decode_html_entities(dest);
}

static const char *find_metadata_tag(AVDictionary *primary,
                                     AVDictionary *secondary,
                                     const char *const *keys,
                                     int key_count) {
    for (int i = 0; i < key_count; i++) {
        if (primary) {
            AVDictionaryEntry *entry = av_dict_get(primary, keys[i], NULL, 0);
            if (entry && entry->value && entry->value[0] != '\0') {
                return entry->value;
            }
        }

        if (secondary) {
            AVDictionaryEntry *entry = av_dict_get(secondary, keys[i], NULL, 0);
            if (entry && entry->value && entry->value[0] != '\0') {
                return entry->value;
            }
        }
    }

    return NULL;
}

static void fill_metadata_from_filename(const char *path, char *title, char *artist, char *album) {
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    // Decode percent-encoding for remote URLs (e.g. "%E8%B8%8F" -> "踏浪")
    char decoded_fname[MAX_META_LEN];
    if (remote_is_remote_path(path)) {
        remote_url_decode(fname, decoded_fname, sizeof(decoded_fname));
        fname = decoded_fname;
    }

    char temp_title[MAX_META_LEN];
    utf8_str_truncate(temp_title, fname, MAX_META_LEN - 1);
    char *dot = strrchr(temp_title, '.');
    if (dot) {
        *dot = '\0';
    }

    utf8_str_truncate(title, temp_title, MAX_META_LEN - 1);
    utf8_str_truncate(artist, "未知艺术家", MAX_META_LEN - 1);
    utf8_str_truncate(album, "未知专辑", MAX_META_LEN - 1);

    char *dash_pos = strstr(temp_title, " - ");
    if (dash_pos) {
        *dash_pos = '\0';
        utf8_str_truncate(artist, temp_title, MAX_META_LEN - 1);
        utf8_str_truncate(title, dash_pos + 3, MAX_META_LEN - 1);
    }

    decode_html_entities(title);
    decode_html_entities(artist);
    decode_html_entities(album);
}

void decode_html_entities(char *str) {
    if (!str) {
        return;
    }

    char *read = str;
    char *write = str;

    while (*read) {
        if (*read == '&') {
            if (strncmp(read, "&amp;", 5) == 0) {
                *write++ = '&';
                read += 5;
                continue;
            }
            if (strncmp(read, "&lt;", 4) == 0) {
                *write++ = '<';
                read += 4;
                continue;
            }
            if (strncmp(read, "&gt;", 4) == 0) {
                *write++ = '>';
                read += 4;
                continue;
            }
            if (strncmp(read, "&quot;", 6) == 0) {
                *write++ = '"';
                read += 6;
                continue;
            }
            if (strncmp(read, "&apos;", 6) == 0) {
                *write++ = '\'';
                read += 6;
                continue;
            }
            if (strncmp(read, "&#39;", 5) == 0) {
                *write++ = '\'';
                read += 5;
                continue;
            }
            if (strncmp(read, "&nbsp;", 6) == 0) {
                *write++ = ' ';
                read += 6;
                continue;
            }
        }

        *write++ = *read++;
    }

    *write = '\0';
}

/**
 * 检查文件是否为支持的音频格式
 * 通过文件扩展名判断
 */
int is_audio_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    
    for (int i = 0; audio_extensions[i] != NULL; i++) {
        if (strcasecmp(ext, audio_extensions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------- ID3 tag helpers ---------- */

static int gbk_to_utf8(const char *input, size_t input_len,
                        char *output, size_t output_size) {
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return -1;

    // Use internal buffer so that input == output (in-place callers) works
    // correctly. GBK→UTF-8 expands CJK characters (2B → 3B), so writing
    // directly to output would overwrite not-yet-read input.
    char tmp[1024];
    char *in_ptr = (char *)input;
    size_t in_left = input_len;
    char *out_ptr = tmp;
    size_t out_left = sizeof(tmp) - 1;

    int ret = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    iconv_close(cd);
    if (ret == (size_t)-1) return -1;

    *out_ptr = '\0';
    strncpy(output, tmp, output_size - 1);
    output[output_size - 1] = '\0';
    return 0;
}

/*
 * Attempt GBK→UTF-8 recovery on a metadata buffer.
 *
 * FFmpeg sometimes reads GBK-encoded ID3 tag bytes as Latin-1 and
 * stores them as UTF-8: each GBK byte becomes a Unicode codepoint
 * in the U+0080-U+00FF range.  This function detects that case and
 * recovers by:
 *   1. Decoding the UTF-8 string back to Unicode codepoints
 *   2. Mapping Latin-1 codepoints (U+0080-U+00FF) back to raw bytes
 *   3. Converting those bytes from GBK to UTF-8
 *
 * Idempotent: valid UTF-8 text (containing CJK or other high-range
 * codepoints) is left unchanged; ASCII-only text is also unchanged.
 *
 * Also handles raw GBK bytes (e.g. from APE tag parser): detects
 * non-UTF-8 input and falls back to direct GBK→UTF-8 conversion.
 */
static void try_gbk_fixup(char *buf, size_t buf_size) {
    if (!buf || buf[0] == '\0' || buf_size < 2) return;

    size_t len = strlen(buf);
    if (len == 0) return;

    // Check if input is valid UTF-8. If not, it's likely raw GBK bytes
    // (e.g. from APE tags) — try direct GBK→UTF-8 conversion.
    {
        int is_valid_utf8 = 1;
        for (size_t i = 0; i < len; ) {
            unsigned char c = (unsigned char)buf[i];
            if (c < 0x80) {
                i += 1;
            } else if ((c & 0xE0) == 0xC0) {
                if (c < 0xC2 || i + 1 >= len || (buf[i+1] & 0xC0) != 0x80)
                    { is_valid_utf8 = 0; break; }
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                if (i + 2 >= len || (buf[i+1] & 0xC0) != 0x80 || (buf[i+2] & 0xC0) != 0x80)
                    { is_valid_utf8 = 0; break; }
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                if (c > 0xF4 || i + 3 >= len || (buf[i+1] & 0xC0) != 0x80 || (buf[i+2] & 0xC0) != 0x80 || (buf[i+3] & 0xC0) != 0x80)
                    { is_valid_utf8 = 0; break; }
                i += 4;
            } else {
                is_valid_utf8 = 0; break;
            }
        }
        if (!is_valid_utf8) {
            gbk_to_utf8(buf, len, buf, buf_size);
            return;
        }
    }

    // Decode UTF-8 codepoints and collect Latin-1 bytes.
    // If we encounter any codepoint > 0xFF, the text is already
    // properly decoded → return immediately (idempotent).
    unsigned char raw[1024];
    int raw_count = 0;

    for (size_t i = 0; i < len && raw_count < (int)sizeof(raw) - 1; ) {
        unsigned char c = (unsigned char)buf[i];
        unsigned int cp;

        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8: 110xxxxx 10xxxxxx → U+0080..U+07FF
            if (i + 1 >= len || (buf[i+1] & 0xC0) != 0x80) return;
            cp = ((c & 0x1F) << 6) | (buf[i+1] & 0x3F);
            if (cp > 0xFF) return;   // not a Latin-1 codepoint → already correct
            i += 2;
        } else {
            // 3+ byte UTF-8 → codepoint > 0x7FF → text is properly decoded
            return;
        }

        raw[raw_count++] = (unsigned char)cp;
    }

    // If no Latin-1 range bytes were found, nothing to fix
    int has_high = 0;
    for (int i = 0; i < raw_count; i++) {
        if (raw[i] >= 0x80) { has_high = 1; break; }
    }
    if (!has_high) return;

    raw[raw_count] = '\0';
    gbk_to_utf8((const char *)raw, raw_count, buf, buf_size);
}

/**
 * 获取音频元数据
 * 从文件路径提取基本元数据信息
 * 注意：实际项目中应在此处调用 taglib 或其他库读取 ID3 标签
 * 此处若无真实标签，则默认标题为文件名，艺术家为"未知艺术家"
 */
void get_audio_metadata(const char *path, char *title, char *artist, char *album) {
    static const char *const title_keys[] = {"title", "TITLE"};
    static const char *const artist_keys[] = {"artist", "ARTIST", "album_artist", "ALBUM_ARTIST"};
    static const char *const album_keys[] = {"album", "ALBUM"};

    fill_metadata_from_filename(path, title, artist, album);

    // 远程 URL 不通过 FFmpeg 打开（会阻塞主线程且无超时）
    if (remote_is_remote_path(path)) {
        return;
    }

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) != 0) {
        return;
    }

    AVDictionary *stream_meta = NULL;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_meta = fmt_ctx->streams[i]->metadata;
            break;
        }
    }

    const char *tag_title = find_metadata_tag(stream_meta, fmt_ctx->metadata,
                                              title_keys, (int)(sizeof(title_keys) / sizeof(title_keys[0])));
    const char *tag_artist = find_metadata_tag(stream_meta, fmt_ctx->metadata,
                                               artist_keys, (int)(sizeof(artist_keys) / sizeof(artist_keys[0])));
    const char *tag_album = find_metadata_tag(stream_meta, fmt_ctx->metadata,
                                              album_keys, (int)(sizeof(album_keys) / sizeof(album_keys[0])));

    if (tag_title) {
        copy_metadata_field(title, MAX_META_LEN, tag_title);
    }
    if (tag_artist) {
        copy_metadata_field(artist, MAX_META_LEN, tag_artist);
    }
    if (tag_album) {
        copy_metadata_field(album, MAX_META_LEN, tag_album);
    }

    avformat_close_input(&fmt_ctx);

    // 读取 APEv2 标签作为补充/覆盖源
    // APE > FFmpeg > 文件名 —— APE 标签值覆盖 FFmpeg 读取的值
    {
        APEItem ape_items[APE_MAX_ITEMS];
        int ape_count = parse_ape_tags(path, ape_items, APE_MAX_ITEMS);
        for (int i = 0; i < ape_count; i++) {
            if (ape_items[i].is_binary) continue;
            if (strcmp(ape_items[i].key, "TITLE") == 0) {
                copy_metadata_field(title, MAX_META_LEN, ape_items[i].value);
            } else if (strcmp(ape_items[i].key, "ARTIST") == 0) {
                copy_metadata_field(artist, MAX_META_LEN, ape_items[i].value);
            } else if (strcmp(ape_items[i].key, "ALBUM") == 0) {
                copy_metadata_field(album, MAX_META_LEN, ape_items[i].value);
            }
        }
    }

    // MP3 文件的 ID3v1/ID3v2.3 标签常以 GBK 编码存储中文，
    // FFmpeg 将其当作 Latin-1 解码导致乱码。尝试 GBK→UTF-8 恢复。
    // APE 标签也可能遇到类似的中文编码问题，一并处理。
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".ape") == 0)) {
        if (title[0]) try_gbk_fixup(title, MAX_META_LEN);
        if (artist[0]) try_gbk_fixup(artist, MAX_META_LEN);
        if (album[0]) try_gbk_fixup(album, MAX_META_LEN);
    }
}

/**
 * 加载指定路径下的音频文件到播放列表
 * 扫描目录，过滤支持的音频文件，并提取元数据
 */
int load_single_file(const char *file_path) {
    if (!file_path || file_path[0] == '\0') {
        return -1;
    }
    
    struct stat s;
    if (stat(file_path, &s) != 0 || !S_ISREG(s.st_mode)) {
        return -1;
    }
    
    if (!is_audio_file(file_path)) {
        return -1;
    }
    
    Playlist *next = calloc(1, sizeof(*next));
    if (!next) {
        return -1;
    }
    
    const char *slash = strrchr(file_path, '/');
    if (slash) {
        size_t length = (size_t)(slash - file_path);
        if (length > 0) {
            memcpy(next->folder_path, file_path, length);
            next->folder_path[length] = '\0';
        } else {
            next->folder_path[0] = '.';
            next->folder_path[1] = '\0';
        }
    } else {
        next->folder_path[0] = '.';
        next->folder_path[1] = '\0';
    }
    
    strncpy(next->tracks[0], file_path, MAX_PATH_LEN - 1);
    next->tracks[0][MAX_PATH_LEN - 1] = '\0';
    next->count = 1;
    next->is_loaded = 1;
    
    playlist_lock();
    g_playlist = *next;
    playlist_unlock();
    search_clear();

    free(next);
    recompute_sort_order();
    return 1;
}

int load_playlist(const char *path) {
    if (!path || path[0] == '\0') {
        return -1;
    }
    log_info("playlist", "load_playlist(path='%s') called", path);

    struct stat s;
    if (stat(path, &s) != 0) {
        log_warn("playlist", "stat() failed for path='%s'", path);
        return -1;
    }

    if (S_ISREG(s.st_mode)) {
        log_debug("playlist", "Loading single file: '%s'", path);
        return load_single_file(path);
    }

    if (!S_ISDIR(s.st_mode)) {
        return -1;
    }
    
    Playlist *next = calloc(1, sizeof(*next));
    if (!next) {
        return -1;
    }

    int added = scan_playlist_directory_into(next, path, 0);
    if (added < 0) {
        free(next);
        return -1;
    }

    if (next->count > 0) {
        snprintf(next->folder_path, sizeof(next->folder_path), "%s", path);
        next->is_loaded = 1;
    }

    playlist_lock();
    g_playlist = *next;
    playlist_unlock();
    search_clear();

    int total = next->count;
    free(next);
    log_info("playlist", "load_playlist: loaded %d tracks from '%s'", total, path);
    recompute_sort_order();
    return total;
}

int append_playlist(const char *path) {
    log_info("playlist", "append_playlist(path='%s') called", path);
    Playlist *next = calloc(1, sizeof(*next));
    if (!next) {
        return -1;
    }

    playlist_lock();
    *next = g_playlist;
    playlist_unlock();

    int had_existing_tracks = next->count > 0;
    char previous_folder[MAX_PATH_LEN];

    snprintf(previous_folder, sizeof(previous_folder), "%s", next->folder_path);

    if (!had_existing_tracks) {
        free(next);
        return load_playlist(path);
    }

    int added = scan_playlist_directory_into(next, path, 1);
    if (added < 0) {
        free(next);
        return -1;
    }

    if (added > 0) {
        next->is_loaded = 1;
        if (!next->has_multiple_sources &&
            previous_folder[0] != '\0' &&
            strcmp(previous_folder, path) != 0) {
            next->has_multiple_sources = 1;
        }

        if (next->folder_path[0] == '\0') {
            snprintf(next->folder_path, sizeof(next->folder_path), "%s", path);
        }

        playlist_lock();
        g_playlist = *next;
        playlist_unlock();
        search_clear();
    }

    free(next);
    log_info("playlist", "append_playlist: added %d new tracks (total=%d)", added, playlist_count());
    recompute_sort_order();
    return added;
}

int load_remote_playlist(const RemoteConnectionConfig *conn, const char *subpath) {
    log_info("playlist", "load_remote_playlist(protocol=%d, subpath='%s') called", conn->protocol, subpath);
    RemoteDirEntry *entries = NULL;
    int entry_count = 0;

    if (remote_list_directory(conn, subpath, &entries, &entry_count) < 0) {
        log_warn("playlist", "remote_list_directory failed");
        return -1;
    }

    Playlist *next = calloc(1, sizeof(*next));
    if (!next) {
        remote_free_entries(entries, entry_count);
        return -1;
    }

    // Build the base URL for this remote directory
    char base_url[4096];
    remote_build_url(conn, subpath, base_url, sizeof(base_url));

    int added = 0;
    for (int i = 0; i < entry_count && next->count < MAX_TRACKS; i++) {
        if (!is_audio_file(entries[i].name)) continue;

        // Construct full URL: base_url + "/" + encoded filename
        char encoded_name[768];
        remote_encode_url_path(entries[i].name, encoded_name, sizeof(encoded_name));
        char track_url[4096];
        snprintf(track_url, sizeof(track_url), "%s/%s", base_url, encoded_name);

        strncpy(next->tracks[next->count], track_url, MAX_PATH_LEN - 1);
        next->tracks[next->count][MAX_PATH_LEN - 1] = '\0';
        next->count++;
        added++;
    }

    remote_free_entries(entries, entry_count);

    if (next->count > 0) {
        strncpy(next->folder_path, base_url, sizeof(next->folder_path) - 1);
        next->is_loaded = 1;
    }

    playlist_lock();
    g_playlist = *next;
    playlist_unlock();
    search_clear();

    int total = next->count;
    free(next);
    log_info("playlist", "Remote playlist: loaded %d tracks", total);
    recompute_sort_order();
    return total;
}

static int g_sort_comparison_field = SORT_DEFAULT;

static int sort_comparator(const void *a, const void *b) {
    int idx_a = *(const int *)a;
    int idx_b = *(const int *)b;
    Track ta, tb;
    get_track_metadata(idx_a, &ta);
    get_track_metadata(idx_b, &tb);

    const char *fa, *fb;
    switch (g_sort_comparison_field) {
        case SORT_TITLE:
            fa = ta.title;
            fb = tb.title;
            break;
        case SORT_ARTIST:
            fa = ta.artist;
            fb = tb.artist;
            break;
        case SORT_ALBUM:
            fa = ta.album;
            fb = tb.album;
            break;
        case SORT_FILENAME:
            fa = strrchr(ta.path, '/');
            fa = fa ? fa + 1 : ta.path;
            fb = strrchr(tb.path, '/');
            fb = fb ? fb + 1 : tb.path;
            break;
        default:
            return 0;
    }

    int cmp = strcasecmp(fa, fb);
    if (cmp != 0) return cmp;
    return idx_a - idx_b;  // stable: preserve original order for equal values
}

void recompute_sort_order(void) {
    playlist_lock();
    int count = g_playlist.count;
    int mode = g_app_config.sort_mode;
    playlist_unlock();

    if (mode == SORT_DEFAULT || count <= 0) {
        g_sort_state.active = 0;
        request_ui_refresh(UI_DIRTY_PLAYLIST);
        return;
    }

    // Pre-load metadata for all tracks to warm the LRU cache before qsort
    Track tmp;
    for (int i = 0; i < count; i++) {
        get_track_metadata(i, &tmp);
    }

    for (int i = 0; i < count; i++) {
        g_sort_state.sorted_indices[i] = i;
    }

    g_sort_comparison_field = mode;
    qsort(g_sort_state.sorted_indices, count, sizeof(int), sort_comparator);
    g_sort_state.active = 1;

    request_ui_refresh(UI_DIRTY_PLAYLIST);
}

void clear_metadata_cache(void) {
    log_debug("playlist", "Clearing metadata cache");
    playlist_lock();
    clear_metadata_cache_locked(&g_playlist);
    playlist_unlock();
}

static CachedTrack *find_in_cache_locked(int index) {
    for (int i = 0; i < g_playlist.cache_count; i++) {
        if (g_playlist.cache[i].valid && g_playlist.cache[i].index == index) {
            g_playlist.cache[i].last_used = time(NULL);
            return &g_playlist.cache[i];
        }
    }
    return NULL;
}

static int evict_one_locked(void) {
    if (g_playlist.cache_count < MAX_CACHE_SIZE) {
        return g_playlist.cache_count++;
    }

    time_t oldest = g_playlist.cache[0].last_used;
    int oldest_idx = 0;

    for (int i = 1; i < g_playlist.cache_count; i++) {
        if (g_playlist.cache[i].last_used < oldest) {
            oldest = g_playlist.cache[i].last_used;
            oldest_idx = i;
        }
    }

    g_playlist.cache[oldest_idx].valid = 0;
    return oldest_idx;
}

int get_track_metadata(int index, Track *out) {
    if (!out) {
        return -1;
    }

    char path[MAX_PATH_LEN];

    playlist_lock();
    if (index < 0 || index >= g_playlist.count) {
        playlist_unlock();
        return -1;
    }

    CachedTrack *cached = find_in_cache_locked(index);
    if (cached) {
        strncpy(out->path, g_playlist.tracks[index], MAX_PATH_LEN - 1);
        out->path[MAX_PATH_LEN - 1] = '\0';
        strncpy(out->title, cached->title, MAX_META_LEN - 1);
        out->title[MAX_META_LEN - 1] = '\0';
        strncpy(out->artist, cached->artist, MAX_META_LEN - 1);
        out->artist[MAX_META_LEN - 1] = '\0';
        strncpy(out->album, cached->album, MAX_META_LEN - 1);
        out->album[MAX_META_LEN - 1] = '\0';
        playlist_unlock();
        return 0;
    }

    strncpy(path, g_playlist.tracks[index], MAX_PATH_LEN - 1);
    path[MAX_PATH_LEN - 1] = '\0';
    playlist_unlock();

    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    char title_search[MAX_SEARCH_KEY_LEN];
    char artist_search[MAX_SEARCH_KEY_LEN];
    char album_search[MAX_SEARCH_KEY_LEN];
    get_audio_metadata(path, title, artist, album);
    build_search_key(title, title_search, sizeof(title_search));
    build_search_key(artist, artist_search, sizeof(artist_search));
    build_search_key(album, album_search, sizeof(album_search));

    strncpy(out->path, path, MAX_PATH_LEN - 1);
    out->path[MAX_PATH_LEN - 1] = '\0';
    strncpy(out->title, title, MAX_META_LEN - 1);
    out->title[MAX_META_LEN - 1] = '\0';
    strncpy(out->artist, artist, MAX_META_LEN - 1);
    out->artist[MAX_META_LEN - 1] = '\0';
    strncpy(out->album, album, MAX_META_LEN - 1);
    out->album[MAX_META_LEN - 1] = '\0';

    playlist_lock();
    if (index >= 0 &&
        index < g_playlist.count &&
        strcmp(g_playlist.tracks[index], path) == 0) {
        int cache_idx = evict_one_locked();
        CachedTrack *new_cache = &g_playlist.cache[cache_idx];
        new_cache->index = index;
        new_cache->valid = 1;
        new_cache->last_used = time(NULL);
        strncpy(new_cache->title, title, MAX_META_LEN - 1);
        new_cache->title[MAX_META_LEN - 1] = '\0';
        strncpy(new_cache->artist, artist, MAX_META_LEN - 1);
        new_cache->artist[MAX_META_LEN - 1] = '\0';
        strncpy(new_cache->album, album, MAX_META_LEN - 1);
        new_cache->album[MAX_META_LEN - 1] = '\0';
        strncpy(new_cache->title_search, title_search, MAX_SEARCH_KEY_LEN - 1);
        new_cache->title_search[MAX_SEARCH_KEY_LEN - 1] = '\0';
        strncpy(new_cache->artist_search, artist_search, MAX_SEARCH_KEY_LEN - 1);
        new_cache->artist_search[MAX_SEARCH_KEY_LEN - 1] = '\0';
        strncpy(new_cache->album_search, album_search, MAX_SEARCH_KEY_LEN - 1);
        new_cache->album_search[MAX_SEARCH_KEY_LEN - 1] = '\0';
    }
    playlist_unlock();

    return 0;
}

int track_matches_query(int index, const char *query) {
    if (!query || query[0] == '\0') {
        return 1;
    }

    Track track;
    if (get_track_metadata(index, &track) != 0) {
        return 0;
    }

    if (string_contains_ci(track.title, query) ||
        string_contains_ci(track.artist, query) ||
        string_contains_ci(track.album, query)) {
        return 1;
    }

    char normalized_query[MAX_SEARCH_KEY_LEN];
    normalize_search_token(query, normalized_query, sizeof(normalized_query));
    if (normalized_query[0] == '\0') {
        return 0;
    }

    int matched = 0;
    playlist_lock();
    CachedTrack *cached = find_in_cache_locked(index);
    if (cached) {
        matched = (strstr(cached->title_search, normalized_query) != NULL) ||
                  (strstr(cached->artist_search, normalized_query) != NULL) ||
                  (strstr(cached->album_search, normalized_query) != NULL);
    }
    playlist_unlock();

    return matched;
}

void preload_visible_tracks(int start, int end) {
    int playlist_total = playlist_count();

    if (start < 0) start = 0;
    if (end >= playlist_total) end = playlist_total - 1;
    if (start > end) return;

    // 限制每帧最多加载 2 个非缓存曲目，防止慢速文件系统阻塞 UI
    int loaded = 0;
    Track track;
    for (int i = start; i <= end && loaded < 2; i++) {
        int cached = 0;
        playlist_lock();
        cached = (find_in_cache_locked(i) != NULL);
        playlist_unlock();

        if (!cached) {
            get_track_metadata(i, &track);
            loaded++;
        }
    }
}

char g_current_album_cover_path[MAX_PATH_LEN] = "";
int g_current_album_cover_valid = 0;

static pthread_mutex_t g_album_cover_mutex = PTHREAD_MUTEX_INITIALIZER;

void cleanup_album_cover_cache(void) {
    pthread_mutex_lock(&g_album_cover_mutex);
    if (g_current_album_cover_valid && g_current_album_cover_path[0] != '\0') {
        unlink(g_current_album_cover_path);
        g_current_album_cover_path[0] = '\0';
        g_current_album_cover_valid = 0;
    }
    pthread_mutex_unlock(&g_album_cover_mutex);
}

static int write_jpeg_file(const char *path, const unsigned char *rgb_data,
                          int width, int height, int quality) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    unsigned char *row = (unsigned char *)rgb_data;
    int stride = width * 3;
    for (int y = 0; y < height; y++) {
        jpeg_write_scanlines(&cinfo, &row, 1);
        row += stride;
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 0;
}

static int decode_image_to_jpeg(const unsigned char *data, int size,
                                const char *output_path) {
    // 将原始数据写入临时文件，让 ffmpeg 自动探测真实格式（WebP/PNG/BMP/JPEG等）
    char tmp_img[MAX_PATH_LEN];
    snprintf(tmp_img, sizeof(tmp_img), "%sXXXXXX", ALBUM_COVER_TEMP_PREFIX);
    int tmp_fd = mkstemp(tmp_img);
    if (tmp_fd < 0) return -1;

    int wrote = 0;
    while (wrote < size) {
        int n = write(tmp_fd, data + wrote, size - wrote);
        if (n <= 0) { close(tmp_fd); unlink(tmp_img); return -1; }
        wrote += n;
    }
    close(tmp_fd);

    AVFormatContext *img_ctx = NULL;
    int ret = -1;
    if (avformat_open_input(&img_ctx, tmp_img, NULL, NULL) != 0) {
        unlink(tmp_img);
        return -1;
    }
    if (avformat_find_stream_info(img_ctx, NULL) < 0) {
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }

    int vstream = -1;
    for (unsigned int i = 0; i < img_ctx->nb_streams; i++) {
        if (img_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = (int)i;
            break;
        }
    }
    if (vstream < 0) {
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }

    const AVCodec *codec = avcodec_find_decoder(
        img_ctx->streams[vstream]->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }
    if (avcodec_parameters_to_context(codec_ctx,
            img_ctx->streams[vstream]->codecpar) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }

    // 读取 packet 并解码
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&img_ctx);
        unlink(tmp_img);
        return -1;
    }

    while (av_read_frame(img_ctx, pkt) == 0) {
        if (pkt->stream_index == vstream) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                if (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // 解码成功，转换为 RGB24
                    struct SwsContext *sws = sws_getContext(
                        frame->width, frame->height, frame->format,
                        frame->width, frame->height, AV_PIX_FMT_RGB24,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    if (sws) {
                        unsigned char *rgb = malloc(
                            frame->width * frame->height * 3);
                        if (rgb) {
                            uint8_t *dst_slice[1] = { rgb };
                            int dst_stride[1] = { frame->width * 3 };
                            sws_scale(sws, (const uint8_t **)frame->data,
                                      frame->linesize, 0, frame->height,
                                      dst_slice, dst_stride);
                            if (write_jpeg_file(output_path, rgb,
                                    frame->width, frame->height, 85) == 0) {
                                ret = 0;
                            }
                            free(rgb);
                        }
                        sws_freeContext(sws);
                    }
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&img_ctx);
    unlink(tmp_img);
    return ret;
}

int extract_album_cover(const char *audio_path, char *output_path, size_t output_size) {
    if (!audio_path || !output_path || output_size == 0) {
        return -1;
    }

    log_debug("playlist", "extract_album_cover('%s')", audio_path);

    // 远程 URL 的封面无法在不阻塞的情况下提取
    if (remote_is_remote_path(audio_path)) {
        log_debug("playlist", "Skipping cover extract for remote path");
        return -1;
    }

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, audio_path, NULL, NULL) != 0) {
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int cover_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            cover_stream_idx = (int)i;
            break;
        }
    }

    if (cover_stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream *stream = fmt_ctx->streams[cover_stream_idx];
    AVPacket *pkt = &stream->attached_pic;
    if (!pkt || pkt->size <= 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    char temp_path[MAX_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%sXXXXXX.jpg", ALBUM_COVER_TEMP_PREFIX);
    int fd = mkstemps(temp_path, 4);
    if (fd < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    close(fd);

    int ret = -1;

    // 情况1：数据本身是 JPEG，直接写入
    if (pkt->data[0] == 0xFF && pkt->data[1] == 0xD8) {
        FILE *fp = fopen(temp_path, "wb");
        if (fp) {
            size_t written = fwrite(pkt->data, 1, pkt->size, fp);
            fclose(fp);
            if (written == (size_t)pkt->size) {
                ret = 0;
            }
        }
    } else {
        // 情况2：其他格式（WebP/PNG/BMP等），解码后重编码为 JPEG
        if (decode_image_to_jpeg(pkt->data, pkt->size, temp_path) == 0) {
            ret = 0;
        }
    }

    avformat_close_input(&fmt_ctx);

    if (ret == 0) {
        snprintf(output_path, output_size, "%s", temp_path);
        log_info("playlist", "Album cover extracted: '%s' -> '%s'", audio_path, temp_path);
    } else {
        log_debug("playlist", "No album cover found in '%s'", audio_path);
        unlink(temp_path);
    }

    return ret;
}

int get_current_album_cover_path(char *path, size_t path_size) {
    if (!path || path_size == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_album_cover_mutex);
    if (g_current_album_cover_valid && g_current_album_cover_path[0] != '\0') {
        snprintf(path, path_size, "%s", g_current_album_cover_path);
        pthread_mutex_unlock(&g_album_cover_mutex);
        return 0;
    }
    pthread_mutex_unlock(&g_album_cover_mutex);

    return -1;
}

void update_album_cover_for_track(const char *track_path) {
    if (!track_path || track_path[0] == '\0') {
        return;
    }

    cleanup_album_cover_cache();

    // 清空字符画缓存，以便新歌曲加载时重新生成
    g_braille_art_buffer[0] = '\0';
    g_album_cover_size = 0;

    char temp_path[MAX_PATH_LEN];
    if (extract_album_cover(track_path, temp_path, sizeof(temp_path)) == 0) {
        pthread_mutex_lock(&g_album_cover_mutex);
        snprintf(g_current_album_cover_path, sizeof(g_current_album_cover_path), "%s", temp_path);
        g_current_album_cover_valid = 1;
        pthread_mutex_unlock(&g_album_cover_mutex);
    }
}

void reset_album_cover_cache(void) {
    cleanup_album_cover_cache();
}
