/**
 * @file remote_parse.c
 * @brief 远程目录列表文本解析（无 libcurl、无 libxml2 依赖，可单测）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote_parse.h"

#include "remote/remote_url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ── 条目收集（容量增长、上限、按名字去重） ───────────────────────── */

typedef struct {
    RemoteDirEntry *items;
    int count;
    int cap;
    int *chain;        /* 去重哈希链：存下一条目下标，-1 结束 */
    int chain_cap;
    unsigned *buckets;
    int bucket_cap;
} EntryList;

static void list_init(EntryList *list)
{
    memset(list, 0, sizeof(*list));
}

static void list_free(EntryList *list)
{
    free(list->items);
    free(list->chain);
    free(list->buckets);
    memset(list, 0, sizeof(*list));
}

static unsigned hash_name(const char *name)
{
    unsigned hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static int list_rehash(EntryList *list, int new_cap)
{
    unsigned *buckets = calloc((size_t)new_cap, sizeof(unsigned));
    int *chain = realloc(list->chain, sizeof(int) * (size_t)(list->cap > 0 ? list->cap : 1));
    if (!buckets || !chain) {
        free(buckets);
        return -1;
    }
    for (int i = 0; i < list->chain_cap; i++) {
        chain[i] = -1;
    }
    list->chain = chain;
    if (list->chain_cap < list->cap) {
        for (int i = list->chain_cap; i < list->cap; i++) {
            list->chain[i] = -1;
        }
    }
    free(list->buckets);
    list->buckets = buckets;
    list->bucket_cap = new_cap;
    for (int i = 0; i < list->count; i++) {
        unsigned slot = hash_name(list->items[i].name) % (unsigned)new_cap;
        list->chain[i] = (int)buckets[slot] - 1;
        buckets[slot] = (unsigned)(i + 1);
    }
    return 0;
}

/* @return 0 已加入；1 重复（跳过）；-1 失败/超限 */
static int list_add(EntryList *list, const char *name, int is_dir,
                    long long size, long mtime)
{
    if (!name || !name[0]) {
        return 1;
    }
    if (list->count >= REMOTE_LISTING_MAX_ENTRIES) {
        return -1;
    }
    if (list->count >= list->cap) {
        int next = list->cap == 0 ? 64 : list->cap * 2;
        RemoteDirEntry *grown = realloc(list->items, sizeof(RemoteDirEntry) * (size_t)next);
        if (!grown) {
            return -1;
        }
        list->items = grown;
        list->cap = next;
        if (list->chain_cap < next) {
            int *chain = realloc(list->chain, sizeof(int) * (size_t)next);
            if (!chain) {
                return -1;
            }
            list->chain = chain;
            for (int i = list->chain_cap; i < next; i++) {
                list->chain[i] = -1;
            }
            list->chain_cap = next;
        }
    }
    if (list->bucket_cap == 0 || list->count * 2 > list->bucket_cap) {
        int next = list->bucket_cap == 0 ? 256 : list->bucket_cap * 2;
        if (list_rehash(list, next) != 0) {
            return -1;
        }
    }

    unsigned slot = hash_name(name) % (unsigned)list->bucket_cap;
    for (int idx = (int)list->buckets[slot] - 1; idx >= 0; idx = list->chain[idx]) {
        if (strcmp(list->items[idx].name, name) == 0) {
            return 1;                    /* 同名去重（Apache/PROPFIND 重复链接） */
        }
    }

    RemoteDirEntry *entry = &list->items[list->count];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->is_dir = is_dir ? 1 : 0;
    entry->size = size;
    entry->mtime = mtime;
    list->chain[list->count] = (int)list->buckets[slot] - 1;
    list->buckets[slot] = (unsigned)(list->count + 1);
    list->count++;
    return 0;
}

static int list_finish(EntryList *list, RemoteDirEntry **out, int *out_count)
{
    *out = list->items;
    *out_count = list->count;
    list->items = NULL;
    list_free(list);
    return 0;
}

/* ── 通用小工具 ───────────────────────────────────────────────────── */

static int is_ascii_digits(const char *text, size_t len)
{
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int month_index(const char *name, size_t len)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (len != 3) return 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(name, months[i], 3) == 0) return i + 1;
    }
    return 0;
}

/* 解析 "20241121123045"（MLSD modify） */
static long parse_mlsd_time(const char *value)
{
    if (!value || strlen(value) < 14 || !is_ascii_digits(value, 14)) {
        return 0;
    }
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    char buf[5];
    memcpy(buf, value, 4); buf[4] = '\0'; tm.tm_year = atoi(buf) - 1900;
    memcpy(buf, value + 4, 2); buf[2] = '\0'; tm.tm_mon = atoi(buf) - 1;
    memcpy(buf, value + 6, 2); buf[2] = '\0'; tm.tm_mday = atoi(buf);
    memcpy(buf, value + 8, 2); buf[2] = '\0'; tm.tm_hour = atoi(buf);
    memcpy(buf, value + 10, 2); buf[2] = '\0'; tm.tm_min = atoi(buf);
    memcpy(buf, value + 12, 2); buf[2] = '\0'; tm.tm_sec = atoi(buf);
    return (long)timegm(&tm);
}

/* 解析 "Nov 20 12:00" / "Nov 20 2025" */
static long parse_ls_time(const char *month, size_t month_len,
                          const char *day, size_t day_len,
                          const char *clock_or_year, size_t clock_len)
{
    int mon = month_index(month, month_len);
    if (mon <= 0 || !is_ascii_digits(day, day_len)) {
        return 0;
    }
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    tm.tm_year = local ? local->tm_year : 125;
    tm.tm_mon = mon - 1;
    tm.tm_mday = atoi(day);
    tm.tm_isdst = -1;
    if ((clock_len == 5 || clock_len == 8) &&
        clock_or_year[2] == ':' &&
        isdigit((unsigned char)clock_or_year[0]) &&
        isdigit((unsigned char)clock_or_year[1]) &&
        isdigit((unsigned char)clock_or_year[3]) &&
        isdigit((unsigned char)clock_or_year[4])) {
        tm.tm_hour = (clock_or_year[0] - '0') * 10 + (clock_or_year[1] - '0');
        tm.tm_min = (clock_or_year[3] - '0') * 10 + (clock_or_year[4] - '0');
        if (clock_len == 8) {
            tm.tm_sec = (clock_or_year[6] - '0') * 10 + (clock_or_year[7] - '0');
        }
        time_t guess = timegm(&tm);
        if (guess > now + 86400) {
            tm.tm_year -= 1;             /* 未来日期说明是去年 */
            guess = timegm(&tm);
        }
        return (long)guess;
    }
    if (clock_len == 4 && is_ascii_digits(clock_or_year, 4)) {
        tm.tm_year = atoi(clock_or_year) - 1900;
        return (long)timegm(&tm);
    }
    return 0;
}

/* ── HTML/XML 实体解码 ────────────────────────────────────────────── */

void remote_parse_unescape_entities(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_size; ) {
        if (in[i] == '&') {
            if (strncmp(in + i, "&amp;", 5) == 0) { out[j++] = '&'; i += 5; continue; }
            if (strncmp(in + i, "&lt;", 4) == 0) { out[j++] = '<'; i += 4; continue; }
            if (strncmp(in + i, "&gt;", 4) == 0) { out[j++] = '>'; i += 4; continue; }
            if (strncmp(in + i, "&quot;", 6) == 0) { out[j++] = '"'; i += 6; continue; }
            if (strncmp(in + i, "&apos;", 6) == 0) { out[j++] = '\''; i += 6; continue; }
            if (strncmp(in + i, "&nbsp;", 6) == 0) { out[j++] = ' '; i += 6; continue; }
            if (in[i + 1] == '#') {
                const char *p = in + i + 2;
                int base = 10;
                if (*p == 'x' || *p == 'X') { base = 16; p++; }
                char *end = NULL;
                long value = strtol(p, &end, base);
                if (end && end > p && *end == ';' && value > 0 && value < 0x110000) {
                    /* 只处理 BMP 内的码点；UTF-8 编码 */
                    if (value < 0x80) {
                        if (j + 1 >= out_size) break;
                        out[j++] = (char)value;
                    } else if (value < 0x800) {
                        if (j + 2 >= out_size) break;
                        out[j++] = (char)(0xC0 | (value >> 6));
                        out[j++] = (char)(0x80 | (value & 0x3F));
                    } else if (value < 0x10000) {
                        if (j + 3 >= out_size) break;
                        out[j++] = (char)(0xE0 | (value >> 12));
                        out[j++] = (char)(0x80 | ((value >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (value & 0x3F));
                    } else {
                        if (j + 4 >= out_size) break;
                        out[j++] = (char)(0xF0 | (value >> 18));
                        out[j++] = (char)(0x80 | ((value >> 12) & 0x3F));
                        out[j++] = (char)(0x80 | ((value >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (value & 0x3F));
                    }
                    i = (size_t)(end - in) + 1;
                    continue;
                }
            }
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
}

/* ── 路径解析（autoindex / PROPFIND 的 href → 名字） ──────────────── */

/* 把 base（目录 URL 路径）与 href 归一成绝对路径；越出 base 的父目录链接
 * 返回 -1。成功写 out_path（保留尾斜杠语义由 out_is_dir 表示）。 */
static int resolve_href(const char *base_path, const char *href,
                        char *out_path, size_t out_size, int *out_trailing_slash)
{
    char raw[2048];
    size_t href_len = strlen(href);
    if (href_len >= sizeof(raw)) {
        return -1;
    }
    snprintf(raw, sizeof(raw), "%s", href);
    /* 去掉 query/fragment */
    char *cut = strpbrk(raw, "?#");
    if (cut) {
        *cut = '\0';
    }
    if (raw[0] == '\0') {
        return -1;
    }

    /* 绝对 URL：只取 path 部分 */
    const char *path = raw;
    const char *scheme = strstr(raw, "://");
    if (scheme) {
        const char *slash = strchr(scheme + 3, '/');
        path = slash ? slash : "/";
    }

    char combined[4096];
    if (path[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", path);
    } else {
        char base[2048];
        size_t base_len = strlen(base_path ? base_path : "/");
        if (base_len == 0 || base_len >= sizeof(base)) {
            return -1;
        }
        snprintf(base, sizeof(base), "%s", base_path);
        if (base[base_len - 1] != '/') {
            if (base_len + 1 >= sizeof(base)) return -1;
            base[base_len++] = '/';
            base[base_len] = '\0';
        }
        if (strlen(base) + strlen(path) + 1 >= sizeof(combined)) {
            return -1;
        }
        snprintf(combined, sizeof(combined), "%s%s", base, path);
    }

    int trailing = (combined[strlen(combined) - 1] == '/');
    /* 归一化 . 与 ..（不得越过根） */
    char normalized[4096];
    size_t out_len = 0;
    normalized[0] = '\0';
    const char *segment = combined;
    while (*segment) {
        while (*segment == '/') {
            segment++;
        }
        if (!*segment) {
            break;
        }
        const char *slash = strchr(segment, '/');
        size_t seg_len = slash ? (size_t)(slash - segment) : strlen(segment);
        if (seg_len == 1 && segment[0] == '.') {
            /* 当前目录：跳过 */
        } else if (seg_len == 2 && segment[0] == '.' && segment[1] == '.') {
            char *last = strrchr(normalized, '/');
            if (!last) {
                return -1;               /* 越过根：父目录链接，忽略 */
            }
            /* 弹出父目录必须同步 out_len，否则后续写入落在旧 NUL 之后，
             * 合法条目（sub/../tone.wav）会被静默截断（R-06/F-12）。 */
            *last = '\0';
            out_len = (size_t)(last - normalized);
        } else {
            if (out_len + 1 + seg_len + 1 >= sizeof(normalized)) {
                return -1;
            }
            normalized[out_len++] = '/';
            memcpy(normalized + out_len, segment, seg_len);
            out_len += seg_len;
            normalized[out_len] = '\0';
        }
        segment = slash ? slash + 1 : segment + seg_len;
    }
    if (out_len == 0) {
        return -1;
    }

    /* 与 base 相同 = 目录自身，跳过。href 是百分号编码的，比较前先解码一次，
     * 否则非 ASCII 目录会把“自身”当成条目列出来。 */
    char base_norm[2048];
    size_t base_norm_len = strlen(base_path ? base_path : "/");
    if (base_norm_len >= sizeof(base_norm)) return -1;
    snprintf(base_norm, sizeof(base_norm), "%s", base_path ? base_path : "/");
    while (base_norm_len > 1 && base_norm[base_norm_len - 1] == '/') {
        base_norm[--base_norm_len] = '\0';
    }
    char decoded_norm[4096];
    if (remote_url_decode(normalized, decoded_norm, sizeof(decoded_norm)) != 0) {
        return -1;
    }
    if (strcmp(decoded_norm, base_norm) == 0) {
        return -1;
    }
    /* 真正的父目录（../file）不应越过 listing base；否则会把父目录内容
     * 当成当前目录条目（R-06）。根目录除外。 */
    if (strcmp(base_norm, "/") != 0) {
        size_t blen = strlen(base_norm);
        if (strncmp(decoded_norm, base_norm, blen) != 0 ||
            (decoded_norm[blen] != '/' && decoded_norm[blen] != '\0')) {
            return -1;
        }
    }

    if (strlen(normalized) >= out_size) {
        return -1;
    }
    snprintf(out_path, out_size, "%s", normalized);
    if (out_trailing_slash) *out_trailing_slash = trailing;
    return 0;
}

/* 从归一化路径取末段并做百分号解码 */
static int path_last_name(const char *path, char *out, size_t out_size)
{
    const char *last = strrchr(path, '/');
    const char *name = last ? last + 1 : path;
    if (!name[0]) {
        return -1;
    }
    char decoded[512];
    if (remote_url_decode(name, decoded, sizeof(decoded)) != 0) {
        return -1;
    }
    if (decoded[0] == '\0' || strcmp(decoded, ".") == 0 || strcmp(decoded, "..") == 0) {
        return -1;
    }
    if (strlen(decoded) >= out_size) {
        return -1;
    }
    snprintf(out, out_size, "%s", decoded);
    return 0;
}

/* ── FTP / SFTP（ls -l 形状） ─────────────────────────────────────── */

/* 解析一行 ls -l；返回 1 已解析，0 跳过，-1 不是该格式 */
static int parse_ls_line(const char *line, size_t len, RemoteDirEntry *out)
{
    if (len < 10) {
        return -1;
    }
    if (line[0] == 'l') {
        return 0;                        /* 符号链接：跳过 */
    }
    if (line[0] != '-' && line[0] != 'd') {
        return -1;
    }

    size_t pos = 0;
    size_t field_starts[8];
    size_t field_lens[8];
    int fields = 0;
    while (fields < 8 && pos < len) {
        while (pos < len && line[pos] == ' ') pos++;
        if (pos >= len) break;
        field_starts[fields] = pos;
        while (pos < len && line[pos] != ' ') pos++;
        field_lens[fields] = pos - field_starts[fields];
        fields++;
    }
    if (fields < 8) {
        return -1;
    }
    while (pos < len && line[pos] == ' ') pos++;
    if (pos >= len) {
        return 0;                        /* 只有属性没有名字 */
    }

    out->is_dir = (line[0] == 'd');
    out->size = -1;
    if (is_ascii_digits(line + field_starts[4], field_lens[4])) {
        char size_buf[32];
        size_t copy = field_lens[4] < sizeof(size_buf) - 1 ? field_lens[4] : sizeof(size_buf) - 1;
        memcpy(size_buf, line + field_starts[4], copy);
        size_buf[copy] = '\0';
        out->size = strtoll(size_buf, NULL, 10);
    }
    out->mtime = parse_ls_time(line + field_starts[5], field_lens[5],
                               line + field_starts[6], field_lens[6],
                               line + field_starts[7], field_lens[7]);

    size_t name_len = len - pos;
    while (name_len > 0 && (line[pos + name_len - 1] == '\r' || line[pos + name_len - 1] == '\n')) {
        name_len--;
    }
    if (name_len == 0 || name_len >= sizeof(out->name)) {
        return 0;
    }
    memcpy(out->name, line + pos, name_len);
    out->name[name_len] = '\0';
    if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) {
        return 0;
    }
    return 1;
}

static int parse_mlsd_line(const char *line, size_t len, RemoteDirEntry *out)
{
    const char *space = memchr(line, ' ', len);
    if (!space) {
        return -1;
    }
    size_t facts_len = (size_t)(space - line);
    if (facts_len == 0 || memchr(line, '=', facts_len) == NULL) {
        return -1;
    }
    /* facts 必须以 ';' 结尾 */
    if (line[facts_len - 1] != ';') {
        return -1;
    }

    int type = 0;                        /* 0 未知/文件，1 目录，2 跳过 */
    long long size = -1;
    long mtime = 0;

    const char *p = line;
    const char *facts_end = line + facts_len;
    while (p < facts_end) {
        const char *semi = memchr(p, ';', (size_t)(facts_end - p));
        if (!semi) break;
        const char *eq = memchr(p, '=', (size_t)(semi - p));
        if (eq) {
            size_t key_len = (size_t)(eq - p);
            size_t value_len = (size_t)(semi - eq - 1);
            const char *value = eq + 1;
            if (key_len == 4 && strncmp(p, "type", 4) == 0) {
                if (value_len == 3 && strncmp(value, "dir", 3) == 0) {
                    type = 1;
                } else if (value_len >= 4 &&
                           (strncmp(value, "cdir", 4) == 0 || strncmp(value, "pdir", 4) == 0)) {
                    type = 2;
                } else if (value_len >= 4 && strncmp(value, "OS.u", 4) == 0) {
                    type = 2;        /* OS.unix=slink / symlink：跳过 */
                } else if (value_len >= 4 && strncmp(value, "OS.", 3) == 0) {
                    type = 2;
                } else {
                    type = 0;
                }
            } else if (key_len == 4 && strncmp(p, "size", 4) == 0 &&
                       is_ascii_digits(value, value_len)) {
                char buf[32];
                size_t copy = value_len < sizeof(buf) - 1 ? value_len : sizeof(buf) - 1;
                memcpy(buf, value, copy);
                buf[copy] = '\0';
                size = strtoll(buf, NULL, 10);
            } else if (key_len == 6 && strncmp(p, "modify", 6) == 0) {
                char buf[32];
                size_t copy = value_len < sizeof(buf) - 1 ? value_len : sizeof(buf) - 1;
                memcpy(buf, value, copy);
                buf[copy] = '\0';
                mtime = parse_mlsd_time(buf);
            }
        }
        p = semi + 1;
    }
    if (type == 2) {
        return 0;
    }

    /* facts 结束于第一个空格；文件名 = 其后全部字节（可含 ';' 与空格） */
    const char *name = space + 1;
    size_t name_len = len - (size_t)(name - line);
    while (name_len > 0 && (name[name_len - 1] == '\r' || name[name_len - 1] == '\n')) {
        name_len--;
    }
    if (name_len == 0 || name_len >= sizeof(out->name)) {
        return 0;
    }
    memcpy(out->name, name, name_len);
    out->name[name_len] = '\0';
    if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) {
        return 0;
    }
    out->is_dir = (type == 1);
    out->size = size;
    out->mtime = mtime;
    return 1;
}

/* FTP 列表：MLSD / LIST / NLST 混合也能解析（按行形状判定） */
int remote_parse_ftp_listing(const char *data, size_t len,
                             RemoteDirEntry **out, int *out_count)
{
    if (!out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    if (!data || len == 0) {
        return 0;                        /* 空目录：合法成功（F-01） */
    }

    EntryList list;
    list_init(&list);

    size_t pos = 0;
    while (pos < len) {
        size_t line_end = pos;
        while (line_end < len && data[line_end] != '\n' && data[line_end] != '\r') {
            line_end++;
        }
        size_t line_len = line_end - pos;
        if (line_len > 0 && line_len < 4096) {
            char line[4096];
            memcpy(line, data + pos, line_len);
            line[line_len] = '\0';

            RemoteDirEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.size = -1;
            int parsed = parse_mlsd_line(line, line_len, &entry);
            if (parsed < 0) {
                parsed = parse_ls_line(line, line_len, &entry);
            }
            if (parsed < 0) {
                /* NLST：整行就是文件名 */
                while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t')) {
                    line_len--;
                }
                line[line_len] = '\0';
                const char *name = line;
                while (*name == ' ') name++;
                if (*name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                    strcmp(name, "total") != 0 && strncmp(name, "total ", 6) != 0) {
                    snprintf(entry.name, sizeof(entry.name), "%s", name);
                    entry.is_dir = 0;
                    entry.size = -1;
                    parsed = 1;
                } else {
                    parsed = 0;
                }
            }
            if (parsed == 1) {
                if (list_add(&list, entry.name, entry.is_dir, entry.size, entry.mtime) < 0) {
                    list_free(&list);
                    return -1;
                }
            }
        }
        pos = line_end;
        while (pos < len && (data[pos] == '\r' || data[pos] == '\n')) {
            pos++;
        }
    }

    return list_finish(&list, out, out_count);
}

int remote_parse_sftp_listing(const char *data, size_t len,
                              RemoteDirEntry **out, int *out_count)
{
    if (!out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    if (!data || len == 0) {
        return 0;
    }

    EntryList list;
    list_init(&list);

    size_t pos = 0;
    while (pos < len) {
        size_t line_end = pos;
        while (line_end < len && data[line_end] != '\n' && data[line_end] != '\r') {
            line_end++;
        }
        size_t line_len = line_end - pos;
        if (line_len > 0 && line_len < 4096) {
            char line[4096];
            memcpy(line, data + pos, line_len);
            line[line_len] = '\0';
            RemoteDirEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.size = -1;
            int parsed = parse_ls_line(line, line_len, &entry);
            if (parsed == 1) {
                if (list_add(&list, entry.name, entry.is_dir, entry.size, entry.mtime) < 0) {
                    list_free(&list);
                    return -1;
                }
            }
        }
        pos = line_end;
        while (pos < len && (data[pos] == '\r' || data[pos] == '\n')) {
            pos++;
        }
    }

    return list_finish(&list, out, out_count);
}

/* ── WebDAV（命名空间无关的标签扫描） ─────────────────────────────── */

static int tag_local_name(const char *tag, size_t len, char *out, size_t out_size,
                          int *is_end, int *is_self_closing)
{
    size_t i = 0;
    *is_end = 0;
    *is_self_closing = 0;
    if (i < len && tag[i] == '/') {
        *is_end = 1;
        i++;
    }
    while (i < len && isspace((unsigned char)tag[i])) i++;
    size_t start = i;
    while (i < len && tag[i] != '>' && !isspace((unsigned char)tag[i]) && tag[i] != '/') {
        i++;
    }
    size_t name_len = i - start;
    if (name_len == 0) {
        return -1;
    }
    const char *colon = memchr(tag + start, ':', name_len);
    if (colon) {
        name_len -= (size_t)(colon - (tag + start)) + 1;
        start = (size_t)(colon - tag) + 1;
    }
    if (name_len >= out_size) {
        return -1;
    }
    memcpy(out, tag + start, name_len);
    out[name_len] = '\0';
    /* 自闭合：'>' 前最后一个非空白字符是 '/' */
    size_t j = len;
    while (j > 0 && isspace((unsigned char)tag[j - 1])) j--;
    if (j > 0 && tag[j - 1] == '/') {
        *is_self_closing = 1;
    }
    return 0;
}

/* 简化实现：从 start 起找 <local> 的内容与结束标签（命名空间前缀可有可无）。 */
static int extract_tag_text(const char *data, size_t len, size_t start,
                            const char *local, char *out, size_t out_size,
                            size_t *next_pos)
{
    size_t pos = start;
    char name[64];
    while (pos < len) {
        const char *lt = memchr(data + pos, '<', len - pos);
        if (!lt) return -1;
        size_t tag_start = (size_t)(lt - data);
        const char *gt = memchr(lt, '>', len - tag_start);
        if (!gt) return -1;
        size_t tag_end = (size_t)(gt - data);
        int is_end = 0, self = 0;
        if (tag_local_name(data + tag_start + 1, tag_end - tag_start - 1,
                           name, sizeof(name), &is_end, &self) != 0) {
            pos = tag_end + 1;
            continue;
        }
        if (!is_end && strcmp(name, local) == 0) {
            if (self) {
                out[0] = '\0';
                *next_pos = tag_end + 1;
                return 0;
            }
            size_t text_start = tag_end + 1;
            size_t scan = text_start;
            while (scan < len) {
                const char *lt2 = memchr(data + scan, '<', len - scan);
                if (!lt2) return -1;
                size_t tag2_start = (size_t)(lt2 - data);
                const char *gt2 = memchr(lt2, '>', len - tag2_start);
                if (!gt2) return -1;
                size_t tag2_end = (size_t)(gt2 - data);
                char name2[64];
                int is_end2 = 0, self2 = 0;
                if (tag_local_name(data + tag2_start + 1, tag2_end - tag2_start - 1,
                                   name2, sizeof(name2), &is_end2, &self2) == 0 &&
                    is_end2 && strcmp(name2, local) == 0) {
                    size_t text_len = tag2_start - text_start;
                    if (text_len >= out_size) {
                        return -1;
                    }
                    memcpy(out, data + text_start, text_len);
                    out[text_len] = '\0';
                    *next_pos = tag2_end + 1;
                    return 0;
                }
                scan = tag2_end + 1;
            }
            return -1;
        }
        pos = tag_end + 1;
    }
    return -1;
}

static int finalize_webdav_entry(EntryList *list, const char *request_path,
                                 const char *href_raw, int has_collection,
                                 long long size, long mtime)
{
    char unescaped[2048];
    remote_parse_unescape_entities(href_raw, unescaped, sizeof(unescaped));
    const char *trim = unescaped;
    while (*trim && isspace((unsigned char)*trim)) trim++;

    char resolved[4096];
    int trailing = 0;
    if (resolve_href(request_path, trim, resolved, sizeof(resolved), &trailing) != 0) {
        return 0;
    }
    char name[256];
    if (path_last_name(resolved, name, sizeof(name)) != 0) {
        return 0;
    }
    int is_dir = has_collection || trailing;
    return list_add(list, name, is_dir, size, mtime) < 0 ? -1 : 0;
}

int remote_parse_webdav_listing(const char *data, size_t len,
                                const char *request_path,
                                RemoteDirEntry **out, int *out_count)
{
    if (!out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    if (!data || len == 0) {
        return -1;                       /* 没有 multistatus：不是合法 WebDAV 响应 */
    }

    EntryList list;
    list_init(&list);

    int seen_multistatus = 0;
    int in_response = 0;
    char href[2048] = "";
    int has_collection = 0;
    long long size = -1;
    long mtime = 0;

    size_t pos = 0;
    while (pos < len) {
        const char *lt = memchr(data + pos, '<', len - pos);
        if (!lt) break;
        size_t tag_start = (size_t)(lt - data);
        const char *gt = memchr(lt, '>', len - tag_start);
        if (!gt) break;
        size_t tag_end = (size_t)(gt - data);

        /* 跳过注释/声明/处理指令 */
        if (tag_start + 1 < len &&
            (data[tag_start + 1] == '!' || data[tag_start + 1] == '?')) {
            pos = tag_end + 1;
            continue;
        }

        char name[64];
        int is_end = 0, self = 0;
        if (tag_local_name(data + tag_start + 1, tag_end - tag_start - 1,
                           name, sizeof(name), &is_end, &self) != 0) {
            pos = tag_end + 1;
            continue;
        }

        if (!is_end && strcmp(name, "multistatus") == 0 && !self) {
            seen_multistatus = 1;
            pos = tag_end + 1;
            continue;
        }

        if (!is_end && strcmp(name, "response") == 0 && !self) {
            if (in_response) {
                if (finalize_webdav_entry(&list, request_path, href, has_collection,
                                          size, mtime) < 0) {
                    list_free(&list);
                    return -1;
                }
            }
            in_response = 1;
            href[0] = '\0';
            has_collection = 0;
            size = -1;
            mtime = 0;
            pos = tag_end + 1;
            continue;
        }
        if (is_end && strcmp(name, "response") == 0 && in_response) {
            if (finalize_webdav_entry(&list, request_path, href, has_collection,
                                      size, mtime) < 0) {
                list_free(&list);
                return -1;
            }
            in_response = 0;
            href[0] = '\0';
            has_collection = 0;
            size = -1;
            mtime = 0;
            pos = tag_end + 1;
            continue;
        }

        if (in_response) {
            if (!is_end && strcmp(name, "href") == 0 && href[0] == '\0') {
                char value[2048];
                size_t next = tag_end + 1;
                if (extract_tag_text(data, len, pos, "href", value,
                                     sizeof(value), &next) == 0) {
                    snprintf(href, sizeof(href), "%s", value);
                    pos = next;
                    continue;
                }
            } else if (!is_end && strcmp(name, "collection") == 0) {
                /* 以当前 response 为界（F-03：不再跨 response 泄漏） */
                has_collection = 1;
            } else if (!is_end && strcmp(name, "getcontentlength") == 0) {
                char value[64];
                size_t next = tag_end + 1;
                if (extract_tag_text(data, len, pos, name, value,
                                     sizeof(value), &next) == 0) {
                    char *end = NULL;
                    long long parsed = strtoll(value, &end, 10);
                    if (end && end != value) {
                        size = parsed;
                    }
                    pos = next;
                    continue;
                }
            } else if (!is_end && strcmp(name, "getlastmodified") == 0) {
                char value[128];
                size_t next = tag_end + 1;
                if (extract_tag_text(data, len, pos, name, value,
                                     sizeof(value), &next) == 0) {
                    struct tm tm;
                    memset(&tm, 0, sizeof(tm));
                    if (strptime(value, "%a, %d %b %Y %H:%M:%S", &tm)) {
                        mtime = (long)timegm(&tm);
                    }
                    pos = next;
                    continue;
                }
            }
        }

        pos = tag_end + 1;
    }

    if (in_response) {
        if (finalize_webdav_entry(&list, request_path, href, has_collection,
                                  size, mtime) < 0) {
            list_free(&list);
            return -1;
        }
    }

    if (!seen_multistatus) {
        list_free(&list);
        return -1;                       /* HTML 错误页/非 WebDAV 响应 */
    }
    return list_finish(&list, out, out_count);
}

/* ── HTTP autoindex ───────────────────────────────────────────────── */

static int html_attr_value(const char *tag, size_t len, const char *attr,
                           char *out, size_t out_size)
{
    size_t attr_len = strlen(attr);
    for (size_t i = 0; i + attr_len < len; i++) {
        if (strncasecmp(tag + i, attr, attr_len) != 0) {
            continue;
        }
        if (i > 0 && (isalnum((unsigned char)tag[i - 1]) || tag[i - 1] == '-' ||
                      tag[i - 1] == '_')) {
            continue;
        }
        size_t j = i + attr_len;
        while (j < len && isspace((unsigned char)tag[j])) j++;
        if (j >= len || tag[j] != '=') {
            continue;
        }
        j++;
        while (j < len && isspace((unsigned char)tag[j])) j++;
        if (j >= len) return -1;
        char quote = 0;
        if (tag[j] == '"' || tag[j] == '\'') {
            quote = tag[j++];
        }
        size_t start = j;
        while (j < len && (quote ? tag[j] != quote
                                 : (tag[j] != ' ' && tag[j] != '>'))) {
            j++;
        }
        size_t value_len = j - start;
        if (value_len >= out_size) return -1;
        memcpy(out, tag + start, value_len);
        out[value_len] = '\0';
        return 0;
    }
    return -1;
}

int remote_parse_html_listing(const char *data, size_t len,
                              const char *request_path,
                              RemoteDirEntry **out, int *out_count)
{
    if (!out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    if (!data || len == 0) {
        return 0;                        /* 空响应：空目录 */
    }
    /* 二进制（例如把文件 URL 当目录）不是 autoindex */
    if (memchr(data, '\0', len) != NULL || memchr(data, '<', len) == NULL) {
        return -1;
    }

    EntryList list;
    list_init(&list);

    size_t pos = 0;
    while (pos < len) {
        const char *lt = memchr(data + pos, '<', len - pos);
        if (!lt) break;
        size_t tag_start = (size_t)(lt - data);
        const char *gt = memchr(lt, '>', len - tag_start);
        if (!gt) break;
        size_t tag_end = (size_t)(gt - data);
        size_t content_start = tag_start + 1;

        if (content_start < tag_end &&
            (data[content_start] == 'a' || data[content_start] == 'A') &&
            (content_start + 1 == tag_end || isspace((unsigned char)data[content_start + 1]) ||
             data[content_start + 1] == '>')) {
            char tag[4096];
            size_t tag_len = tag_end - content_start;
            if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
            memcpy(tag, data + content_start, tag_len);
            tag[tag_len] = '\0';

            char href[1024];
            if (html_attr_value(tag, tag_len, "href", href, sizeof(href)) == 0) {
                char unescaped[1024];
                remote_parse_unescape_entities(href, unescaped, sizeof(unescaped));
                const char *trim = unescaped;
                while (*trim == ' ') trim++;
                if (*trim && *trim != '?' && *trim != '#' &&
                    strncasecmp(trim, "mailto:", 7) != 0 &&
                    strncasecmp(trim, "javascript:", 11) != 0) {
                    char resolved[4096];
                    int trailing = 0;
                    if (resolve_href(request_path, trim, resolved, sizeof(resolved),
                                     &trailing) == 0) {
                        char name[256];
                        if (path_last_name(resolved, name, sizeof(name)) == 0) {
                            if (list_add(&list, name, trailing, -1, 0) < 0) {
                                list_free(&list);
                                return -1;
                            }
                        }
                    }
                }
            }
        }
        pos = tag_end + 1;
    }

    return list_finish(&list, out, out_count);
}

/* ── SMB（smbclient ls 输出） ─────────────────────────────────────── */

/* 真实格式（宿主 smbclient 字符串）：
 *   "  %-30s%7.7s %8.0f  %s"  —— name 左对齐 30、类型 7、size 8、日期
 * 另有 "blocks of size ..." 汇总/磁盘行与 NT_STATUS_* 错误行。 */
static int smb_is_time(const char *token, size_t len)
{
    return len == 8 && token[2] == ':' && token[5] == ':' &&
           isdigit((unsigned char)token[0]) && isdigit((unsigned char)token[1]) &&
           isdigit((unsigned char)token[3]) && isdigit((unsigned char)token[4]) &&
           isdigit((unsigned char)token[6]) && isdigit((unsigned char)token[7]);
}

static int smb_is_weekday(const char *token, size_t len)
{
    return len == 3 && isalpha((unsigned char)token[0]) &&
           isalpha((unsigned char)token[1]) && isalpha((unsigned char)token[2]);
}

static int smb_is_attrs(const char *token, size_t len)
{
    if (len == 0 || len > 8) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (!strchr("DHSNARCEX", token[i])) {
            return 0;
        }
    }
    return 1;
}

static int parse_smb_line(const char *line, size_t len, RemoteDirEntry *out)
{
    if (len == 0) {
        return 0;
    }
    if (strstr(line, "NT_STATUS_") || strstr(line, "session setup failed") ||
        strstr(line, "Unable to connect") || strstr(line, "failed to connect")) {
        return -1;
    }
    if (strstr(line, "blocks of size") || strstr(line, "blocks available") ||
        strstr(line, "Disk space") || strncmp(line, "Domain=", 7) == 0 ||
        strncmp(line, "Password:", 9) == 0 || strncmp(line, "Try ", 4) == 0) {
        return 0;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }

    /* 从右往左取最多 8 个 token */
    struct { size_t start; size_t len; } toks[8];
    int tok_count = 0;
    size_t i = len;
    while (i > 0 && tok_count < 8) {
        while (i > 0 && line[i - 1] == ' ') i--;
        if (i == 0) break;
        size_t end = i;
        while (i > 0 && line[i - 1] != ' ') i--;
        toks[tok_count].start = i;
        toks[tok_count].len = end - i;
        tok_count++;
    }

    /* 标准形状：日期 5 段 + size + 属性 + name（从右往左切） */
    if (tok_count >= 7) {
        int ok = is_ascii_digits(line + toks[0].start, toks[0].len) &&
                 toks[0].len == 4 &&
                 smb_is_time(line + toks[1].start, toks[1].len) &&
                 is_ascii_digits(line + toks[2].start, toks[2].len) &&
                 toks[2].len <= 2 &&
                 month_index(line + toks[3].start, toks[3].len) > 0 &&
                 smb_is_weekday(line + toks[4].start, toks[4].len) &&
                 is_ascii_digits(line + toks[5].start, toks[5].len) &&
                 smb_is_attrs(line + toks[6].start, toks[6].len);
        if (ok) {
            size_t name_end = toks[6].start;
            while (name_end > 0 && line[name_end - 1] == ' ') name_end--;
            size_t name_start = 0;
            while (name_start < name_end && line[name_start] == ' ') name_start++;
            size_t name_len = name_end - name_start;
            if (name_len == 0 || name_len >= sizeof(out->name)) {
                return 0;
            }
            memcpy(out->name, line + name_start, name_len);
            out->name[name_len] = '\0';
            if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) {
                return 0;
            }
            out->is_dir = memchr(line + toks[6].start, 'D', toks[6].len) != NULL;
            char size_buf[32];
            size_t copy = toks[5].len < sizeof(size_buf) - 1 ? toks[5].len : sizeof(size_buf) - 1;
            memcpy(size_buf, line + toks[5].start, copy);
            size_buf[copy] = '\0';
            out->size = strtoll(size_buf, NULL, 10);
            out->mtime = parse_ls_time(line + toks[3].start, toks[3].len,
                                       line + toks[2].start, toks[2].len,
                                       line + toks[1].start, toks[1].len);
            return 1;
        }
    }

    /* 退化形状：只有 size + 属性（测试/旧版本 smbclient） */
    if (tok_count >= 2 &&
        is_ascii_digits(line + toks[0].start, toks[0].len) &&
        smb_is_attrs(line + toks[1].start, toks[1].len)) {
        size_t name_end = toks[1].start;
        while (name_end > 0 && line[name_end - 1] == ' ') name_end--;
        size_t name_start = 0;
        while (name_start < name_end && line[name_start] == ' ') name_start++;
        size_t name_len = name_end - name_start;
        if (name_len == 0 || name_len >= sizeof(out->name)) {
            return 0;
        }
        memcpy(out->name, line + name_start, name_len);
        out->name[name_len] = '\0';
        if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) {
            return 0;
        }
        out->is_dir = memchr(line + toks[1].start, 'D', toks[1].len) != NULL;
        char size_buf[32];
        size_t copy = toks[0].len < sizeof(size_buf) - 1 ? toks[0].len : sizeof(size_buf) - 1;
        memcpy(size_buf, line + toks[0].start, copy);
        size_buf[copy] = '\0';
        out->size = strtoll(size_buf, NULL, 10);
        return 1;
    }
    return 0;
}

int remote_parse_smb_listing(const char *data, size_t len,
                             RemoteDirEntry **out, int *out_count)
{
    if (!out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    if (!data || len == 0) {
        return 0;                        /* 空共享：空目录 */
    }

    EntryList list;
    list_init(&list);
    int saw_error = 0;

    size_t pos = 0;
    while (pos < len) {
        size_t line_end = pos;
        while (line_end < len && data[line_end] != '\n' && data[line_end] != '\r') {
            line_end++;
        }
        size_t line_len = line_end - pos;
        if (line_len > 0 && line_len < 4096) {
            char line[4096];
            memcpy(line, data + pos, line_len);
            line[line_len] = '\0';
            RemoteDirEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.size = -1;
            int parsed = parse_smb_line(line, line_len, &entry);
            if (parsed < 0) {
                saw_error = 1;
                break;
            }
            if (parsed == 1) {
                if (list_add(&list, entry.name, entry.is_dir, entry.size, entry.mtime) < 0) {
                    list_free(&list);
                    return -1;
                }
            }
        }
        pos = line_end;
        while (pos < len && (data[pos] == '\r' || data[pos] == '\n')) {
            pos++;
        }
    }

    if (saw_error) {
        list_free(&list);
        return -1;
    }
    return list_finish(&list, out, out_count);
}

void remote_free_entries(RemoteDirEntry *entries, int count)
{
    (void)count;
    free(entries);
}
