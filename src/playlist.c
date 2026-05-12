#include "../include/defs.h"
#include "../include/pinyin_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

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

SearchState g_search_state = {0};

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
    playlist_lock();
    memset(&g_playlist, 0, sizeof(g_playlist));
    playlist_unlock();
    memset(&g_search_state, 0, sizeof(g_search_state));
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

/**
 * 获取音频元数据
 * 从文件路径提取基本元数据信息
 * 注意：实际项目中应在此处调用 taglib 或其他库读取 ID3 标签
 * 此处若无真实标签，则默认标题为文件名，艺术家为“未知艺术家”
 */
void get_audio_metadata(const char *path, char *title, char *artist, char *album) {
    static const char *const title_keys[] = {"title", "TITLE"};
    static const char *const artist_keys[] = {"artist", "ARTIST", "album_artist", "ALBUM_ARTIST"};
    static const char *const album_keys[] = {"album", "ALBUM"};

    fill_metadata_from_filename(path, title, artist, album);

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
    memset(&g_search_state, 0, sizeof(g_search_state));
    
    free(next);
    return 1;
}

int load_playlist(const char *path) {
    if (!path || path[0] == '\0') {
        return -1;
    }
    
    struct stat s;
    if (stat(path, &s) != 0) {
        return -1;
    }
    
    if (S_ISREG(s.st_mode)) {
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
    memset(&g_search_state, 0, sizeof(g_search_state));

    int total = next->count;
    free(next);
    return total;
}

int append_playlist(const char *path) {
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
        memset(&g_search_state, 0, sizeof(g_search_state));
    }

    free(next);
    return added;
}

void clear_metadata_cache(void) {
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

    Track track;
    for (int i = start; i <= end; i++) {
        int cached = 0;
        playlist_lock();
        cached = (find_in_cache_locked(i) != NULL);
        playlist_unlock();

        if (!cached) {
            get_track_metadata(i, &track);
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

int extract_album_cover(const char *audio_path, char *output_path, size_t output_size) {
    if (!audio_path || !output_path || output_size == 0) {
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
        AVDictionaryEntry *entry = av_dict_get(fmt_ctx->metadata, "metadata_block_picture", NULL, 0);
        if (!entry) {
            entry = av_dict_get(fmt_ctx->metadata, "APIC", NULL, 0);
        }
        if (!entry) {
            avformat_close_input(&fmt_ctx);
            return -1;
        }
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

    if (cover_stream_idx >= 0) {
        AVStream *stream = fmt_ctx->streams[cover_stream_idx];
        AVPacket *pkt = &stream->attached_pic;

        if (pkt && pkt->size > 0) {
            FILE *fp = fopen(temp_path, "wb");
            if (fp) {
                size_t written = fwrite(pkt->data, 1, pkt->size, fp);
                fclose(fp);
                if (written == (size_t)pkt->size) {
                    ret = 0;
                }
            }
        }
    }

    avformat_close_input(&fmt_ctx);

    if (ret == 0) {
        snprintf(output_path, output_size, "%s", temp_path);
    } else {
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
