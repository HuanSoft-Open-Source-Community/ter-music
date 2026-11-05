/**
 * @file remote_cache.c
 * @brief 前端远程曲目下载器（后台线程 + 结果队列）
 *
 * 线程模型（与核心的 rpc_job.c 同思路，但对象是前端缓存）：
 *   - 同一时刻只有一个后台任务（列目录或会话下载），状态由一个互斥量保护；
 *   - 工作线程只在缓存目录里写文件，并把“已完成的本地路径”推入队列，
 *     不触碰 UI 与核心；
 *   - UI 线程每帧调用 remote_cache_take_ready()，把路径经 player 门面交给
 *     核心（核心只看到本地文件）。
 *
 * 下载原子性：先写 `<目标>.part`，成功后 rename 成目标名；已存在的目标文件
 * 视为缓存命中，直接复用（多前端并发时也不会读到半成品）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote_cache.h"
#include "remote/remote_store.h"

#include "config/config.h"
#include "logger/logger.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 复用播放列表的扩展名判定（library.c 也这样复用） */
extern int is_audio_file(const char *filename);

#define REMOTE_CACHE_MAX_COMPONENT 256

typedef struct ReadyNode {
    char path[MAX_PATH_LEN];
    struct ReadyNode *next;
} ReadyNode;

typedef struct {
    int kind;                       /* 0 = 列目录，1 = 会话下载 */
    RemoteConnectionConfig conn;
    char subpath[512];
    int autoplay;
} CacheJob;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started = 0;
static int g_thread_running = 0;
static int g_cancel_requested = 0;

static CacheJob g_job;
static RemoteCacheStatus g_status;
static char g_last_error[256];
static int g_error_pending = 0;
static int g_session_autoplay = 0;

/* 列目录结果（一次性交付） */
static RemoteDirEntry *g_list_entries = NULL;
static int g_list_count = 0;
static int g_list_error = 0;
static int g_list_pending = 0;

/* 已下载完成的本地路径队列 */
static ReadyNode *g_ready_head = NULL;
static ReadyNode *g_ready_tail = NULL;
static int g_ready_count = 0;
static int g_delivered = 0;
static int g_popped = 0;
static int g_total_tracks = 0;

static char g_cache_root[MAX_PATH_LEN] = "";

/* ── 小工具 ───────────────────────────────────────────────────────── */

/* 只保留可安全落盘的字符：其它字节（含 '/' 与不可打印字符）替换为 '_'。
 * UTF-8 多字节序列（>= 0x80）原样保留，中文文件名不会被破坏。 */
static void cache_safe_component(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!in || in[0] == '\0' || strcmp(in, ".") == 0 || strcmp(in, "..") == 0) {
        snprintf(out, out_size, "_");
        return;
    }

    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && pos + 1 < out_size; p++) {
        unsigned char c = *p;
        int safe = (c >= 0x80) ||
                   (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')';
        out[pos++] = safe ? (char)c : '_';
    }
    out[pos] = '\0';
    if (pos == 0) {
        snprintf(out, out_size, "_");
    }
}

static int cache_mkdir_p(const char *path)
{
    char buffer[MAX_PATH_LEN];
    if (!path || path[0] == '\0') {
        return -1;
    }
    snprintf(buffer, sizeof(buffer), "%s", path);

    for (char *cursor = buffer + 1; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
        *cursor = '/';
    }
    if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void cache_build_root(void)
{
    if (g_cache_root[0]) {
        return;
    }
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        snprintf(g_cache_root, sizeof(g_cache_root), "%s/ter-music/remote", xdg);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(g_cache_root, sizeof(g_cache_root), "%s/.cache/ter-music/remote",
             (home && home[0]) ? home : "/tmp");
}

const char *remote_cache_dir(void)
{
    cache_build_root();
    return g_cache_root;
}

/* 远端子路径 → 缓存目录（逐级镜像，逐级消毒） */
static int cache_session_dir(const RemoteConnectionConfig *conn, const char *subpath,
                             char *out, size_t out_size)
{
    cache_build_root();

    char host[REMOTE_CACHE_MAX_COMPONENT];
    char conn_part[REMOTE_CACHE_MAX_COMPONENT * 2];
    char safe_host[REMOTE_CACHE_MAX_COMPONENT];

    snprintf(host, sizeof(host), "%s", conn->host);
    cache_safe_component(host, safe_host, sizeof(safe_host));
    snprintf(conn_part, sizeof(conn_part), "%d_%s_%d", conn->protocol, safe_host, conn->port);

    int written = snprintf(out, out_size, "%s/%s", g_cache_root, conn_part);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    if (!subpath || subpath[0] == '\0') {
        return 0;
    }

    const char *cursor = subpath;
    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        const char *slash = strchr(cursor, '/');
        size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (length >= REMOTE_CACHE_MAX_COMPONENT) {
            length = REMOTE_CACHE_MAX_COMPONENT - 1;
        }
        char component[REMOTE_CACHE_MAX_COMPONENT];
        memcpy(component, cursor, length);
        component[length] = '\0';

        char safe[REMOTE_CACHE_MAX_COMPONENT];
        cache_safe_component(component, safe, sizeof(safe));

        size_t used = strlen(out);
        written = snprintf(out + used, out_size - used, "/%s", safe);
        if (written < 0 || (size_t)written >= out_size - used) {
            return -1;
        }
        cursor = slash ? slash + 1 : cursor + length;
    }
    return 0;
}

static void cache_push_ready(const char *local_path)
{
    ReadyNode *node = malloc(sizeof(*node));
    if (!node) {
        return;
    }
    snprintf(node->path, sizeof(node->path), "%s", local_path);
    node->next = NULL;

    pthread_mutex_lock(&g_lock);
    if (g_ready_tail) {
        g_ready_tail->next = node;
    } else {
        g_ready_head = node;
    }
    g_ready_tail = node;
    g_ready_count++;
    pthread_mutex_unlock(&g_lock);
}

static void cache_set_message(const char *text)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_status.message, sizeof(g_status.message), "%s", text ? text : "");
    pthread_mutex_unlock(&g_lock);
}

static void cache_fail(const char *text)
{
    pthread_mutex_lock(&g_lock);
    g_status.state = REMOTE_CACHE_FAILED;
    snprintf(g_status.message, sizeof(g_status.message), "%s", text ? text : "");
    snprintf(g_last_error, sizeof(g_last_error), "%s", text ? text : "");
    g_error_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

/* ── 下载 ─────────────────────────────────────────────────────────── */

/* 把一首曲目（及其同名 .lrc）下载到缓存；返回 0 表示本地文件就绪 */
static int cache_fetch_track(const char *url, const char *local_path, char *error, size_t error_size)
{
    struct stat st;
    if (stat(local_path, &st) == 0 && st.st_size > 0) {
        return 0;   /* 缓存命中 */
    }

    char part[MAX_PATH_LEN];
    int written = snprintf(part, sizeof(part), "%s.part", local_path);
    if (written < 0 || (size_t)written >= sizeof(part)) {
        snprintf(error, error_size, "path too long");
        return -1;
    }
    unlink(part);

    if (remote_fetch_to_file(url, part) != 0) {
        snprintf(error, error_size, "%s", remote_strerror());
        unlink(part);
        return -1;
    }
    if (rename(part, local_path) != 0) {
        snprintf(error, error_size, "rename failed: %s", strerror(errno));
        unlink(part);
        return -1;
    }

    /* 同名歌词：尽力而为，失败不影响播放 */
    char lrc_local[MAX_PATH_LEN];
    snprintf(lrc_local, sizeof(lrc_local), "%s", local_path);
    char *dot = strrchr(lrc_local, '.');
    if (dot) {
        strcpy(dot, ".lrc");

        char lrc_url[MAX_PATH_LEN * 2];
        snprintf(lrc_url, sizeof(lrc_url), "%s", url);
        char *url_dot = strrchr(lrc_url, '.');
        if (url_dot) {
            strcpy(url_dot, ".lrc");

            char lrc_part[MAX_PATH_LEN];
            if (snprintf(lrc_part, sizeof(lrc_part), "%s.part", lrc_local) < (int)sizeof(lrc_part)) {
                unlink(lrc_part);
                if (remote_fetch_to_file(lrc_url, lrc_part) == 0) {
                    rename(lrc_part, lrc_local);
                } else {
                    unlink(lrc_part);
                }
            }
        }
    }
    return 0;
}

/* ── 工作线程 ─────────────────────────────────────────────────────── */

static void *cache_thread(void *arg)
{
    (void)arg;

    RemoteConnectionConfig conn;
    char subpath[512];
    int kind = 0;
    int autoplay = 0;

    pthread_mutex_lock(&g_lock);
    conn = g_job.conn;
    snprintf(subpath, sizeof(subpath), "%s", g_job.subpath);
    kind = g_job.kind;
    autoplay = g_job.autoplay;
    pthread_mutex_unlock(&g_lock);

    RemoteDirEntry *entries = NULL;
    int count = 0;
    if (remote_list_directory(&conn, subpath, &entries, &count) < 0) {
        char message[256];
        snprintf(message, sizeof(message), "%s", remote_strerror());
        if (kind == 0) {
            pthread_mutex_lock(&g_lock);
            g_list_error = 1;
            g_list_pending = 1;
            pthread_mutex_unlock(&g_lock);
        }
        cache_fail(message);
        goto finish;
    }

    if (kind == 0) {
        pthread_mutex_lock(&g_lock);
        if (g_list_entries) {
            remote_free_entries(g_list_entries, g_list_count);
        }
        g_list_entries = entries;
        g_list_count = count;
        g_list_error = 0;
        g_list_pending = 1;
        g_status.state = REMOTE_CACHE_DONE;
        pthread_mutex_unlock(&g_lock);
        goto finish;                      /* 所有权交给 UI 线程 */
    }

    /* 会话下载 */
    {
        char dir[MAX_PATH_LEN];
        if (cache_session_dir(&conn, subpath, dir, sizeof(dir)) != 0 ||
            cache_mkdir_p(dir) != 0) {
            remote_free_entries(entries, count);
            cache_fail("cannot create cache directory");
            goto finish;
        }

        char base_url[4096];
        remote_build_url(&conn, subpath, base_url, sizeof(base_url));

        int total = 0;
        for (int i = 0; i < count && total < REMOTE_CACHE_SESSION_MAX; i++) {
            if (!entries[i].is_dir && is_audio_file(entries[i].name)) {
                total++;
            }
        }

        pthread_mutex_lock(&g_lock);
        g_status.state = REMOTE_CACHE_DOWNLOADING;
        g_status.total = total;
        g_total_tracks = total;
        g_delivered = 0;
        g_popped = 0;
        pthread_mutex_unlock(&g_lock);

        if (total == 0) {
            remote_free_entries(entries, count);
            cache_fail("no audio files in this directory");
            goto finish;
        }

        for (int i = 0; i < count && g_delivered < total; i++) {
            if (entries[i].is_dir || !is_audio_file(entries[i].name)) {
                continue;
            }

            pthread_mutex_lock(&g_lock);
            int cancelled = g_cancel_requested;
            pthread_mutex_unlock(&g_lock);
            if (cancelled) {
                break;
            }

            char encoded[MAX_PATH_LEN];
            remote_encode_url_path(entries[i].name, encoded, sizeof(encoded));
            char url[MAX_PATH_LEN * 2];
            snprintf(url, sizeof(url), "%s/%s", base_url, encoded);

            char safe_name[REMOTE_CACHE_MAX_COMPONENT];
            cache_safe_component(entries[i].name, safe_name, sizeof(safe_name));
            char local_path[MAX_PATH_LEN];
            snprintf(local_path, sizeof(local_path), "%s/%s", dir, safe_name);

            char error[256] = "";
            if (cache_fetch_track(url, local_path, error, sizeof(error)) == 0) {
                cache_push_ready(local_path);
                pthread_mutex_lock(&g_lock);
                g_delivered++;
                g_status.done = g_delivered;
                pthread_mutex_unlock(&g_lock);
            } else {
                pthread_mutex_lock(&g_lock);
                g_status.failed++;
                snprintf(g_status.message, sizeof(g_status.message), "%.120s: %.100s",
                         entries[i].name, error);
                pthread_mutex_unlock(&g_lock);
                log_warn("remote_cache", "Download failed for '%s': %s", entries[i].name, error);

                /* 记一次错误给 UI（不中断整个会话） */
                pthread_mutex_lock(&g_lock);
                snprintf(g_last_error, sizeof(g_last_error), "%.120s: %.100s",
                         entries[i].name, error);
                g_error_pending = 1;
                pthread_mutex_unlock(&g_lock);
            }
        }

        remote_free_entries(entries, count);

        pthread_mutex_lock(&g_lock);
        g_status.state = (g_delivered > 0) ? REMOTE_CACHE_DONE : REMOTE_CACHE_FAILED;
        if (g_delivered == 0) {
            snprintf(g_status.message, sizeof(g_status.message), "no track could be downloaded");
        }
        g_session_autoplay = autoplay;
        if (g_status.total == 0) {
            g_status.total = g_delivered;
        }
        pthread_mutex_unlock(&g_lock);
    }

finish:
    pthread_mutex_lock(&g_lock);
    g_thread_running = 0;
    g_cancel_requested = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static int cache_start_job(int kind, const RemoteConnectionConfig *conn,
                           const char *subpath, int autoplay)
{
    if (!conn) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (g_thread_running) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_job.kind = kind;
    g_job.conn = *conn;
    snprintf(g_job.subpath, sizeof(g_job.subpath), "%s", subpath ? subpath : "");
    g_job.autoplay = autoplay;

    g_status.state = (kind == 0) ? REMOTE_CACHE_LISTING : REMOTE_CACHE_DOWNLOADING;
    g_status.done = 0;
    g_status.total = 0;
    g_status.failed = 0;
    g_status.cancelled = 0;
    g_status.message[0] = '\0';
    g_cancel_requested = 0;
    g_error_pending = 0;
    g_thread_running = 1;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_thread, NULL, cache_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_thread_running = 0;
        g_status.state = REMOTE_CACHE_FAILED;
        snprintf(g_status.message, sizeof(g_status.message), "cannot create worker thread");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_thread_started = 1;
    return 0;
}

void remote_cache_init(void)
{
    cache_build_root();
    pthread_mutex_lock(&g_lock);
    memset(&g_status, 0, sizeof(g_status));
    pthread_mutex_unlock(&g_lock);
}

void remote_cache_shutdown(void)
{
    remote_cache_cancel();

    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&g_lock);
        int running = g_thread_running;
        pthread_mutex_unlock(&g_lock);
        if (!running) {
            break;
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    if (g_thread_started) {
        pthread_mutex_lock(&g_lock);
        int running = g_thread_running;
        pthread_mutex_unlock(&g_lock);
        if (running) {
            log_warn("remote_cache", "Worker still busy at shutdown; leaving it detached");
            pthread_detach(g_thread);
        } else {
            pthread_join(g_thread, NULL);
        }
        g_thread_started = 0;
    }

    pthread_mutex_lock(&g_lock);
    ReadyNode *node = g_ready_head;
    g_ready_head = g_ready_tail = NULL;
    g_ready_count = 0;
    if (g_list_entries) {
        remote_free_entries(g_list_entries, g_list_count);
        g_list_entries = NULL;
        g_list_count = 0;
    }
    pthread_mutex_unlock(&g_lock);
    while (node) {
        ReadyNode *next = node->next;
        free(node);
        node = next;
    }
}

/* ── 对外接口 ─────────────────────────────────────────────────────── */

int remote_cache_start_listing(const RemoteConnectionConfig *conn, const char *subpath)
{
    return cache_start_job(0, conn, subpath, 0);
}

int remote_cache_start_session(const RemoteConnectionConfig *conn, const char *subpath,
                               int autoplay)
{
    return cache_start_job(1, conn, subpath, autoplay);
}

int remote_cache_take_listing(RemoteDirEntry **out_entries, int *out_count, int *out_error)
{
    if (out_entries) *out_entries = NULL;
    if (out_count) *out_count = 0;
    if (out_error) *out_error = 0;

    pthread_mutex_lock(&g_lock);
    if (!g_list_pending) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (out_entries) *out_entries = g_list_entries;
    if (out_count) *out_count = g_list_count;
    if (out_error) *out_error = g_list_error;
    g_list_entries = NULL;
    g_list_count = 0;
    g_list_error = 0;
    g_list_pending = 0;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int remote_cache_take_ready(char *local_path, size_t size, int *is_first, int *is_last)
{
    if (!local_path || size == 0) {
        return 0;
    }
    if (is_first) *is_first = 0;
    if (is_last) *is_last = 0;

    pthread_mutex_lock(&g_lock);
    ReadyNode *node = g_ready_head;
    if (!node) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_ready_head = node->next;
    if (!g_ready_head) {
        g_ready_tail = NULL;
    }
    g_ready_count--;
    int first = (g_popped == 0);
    int last = (g_ready_head == NULL);
    g_popped++;
    pthread_mutex_unlock(&g_lock);

    snprintf(local_path, size, "%s", node->path);
    free(node);

    if (is_first) *is_first = first;
    if (is_last) *is_last = last;
    return 1;
}

int remote_cache_take_error(char *out, size_t size)
{
    if (!out || size == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_error_pending) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    snprintf(out, size, "%s", g_last_error);
    g_error_pending = 0;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int remote_cache_session_autoplay(void)
{
    int value;
    pthread_mutex_lock(&g_lock);
    value = g_session_autoplay;
    pthread_mutex_unlock(&g_lock);
    return value;
}

void remote_cache_cancel(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_thread_running) {
        g_cancel_requested = 1;
        g_status.cancelled = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

const RemoteCacheStatus *remote_cache_status(void)
{
    return &g_status;   /* UI 只读取：字段为 int/char[]，读时可能有并发写入 */
}

/* ── 缓存维护 ─────────────────────────────────────────────────────── */

static int cache_path_protected(const char *path,
                                const char *const *protected_paths, int protected_count)
{
    for (int i = 0; i < protected_count; i++) {
        if (protected_paths[i] && strcmp(path, protected_paths[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 递归遍历缓存目录；purge 为真时删除全部未保护文件，否则按 mtime 从旧到新
 * 删除直到容量低于上限。 */
static long long cache_walk(const char *dir, int purge, long long limit_bytes,
                            const char *const *protected_paths, int protected_count,
                            int *deleted)
{
    DIR *handle = opendir(dir);
    if (!handle) {
        return 0;
    }

    long long total = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[MAX_PATH_LEN];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            total += cache_walk(path, purge, limit_bytes, protected_paths, protected_count, deleted);
            if (purge) {
                rmdir(path);
            }
            continue;
        }

        total += st.st_size;
        if (cache_path_protected(path, protected_paths, protected_count)) {
            continue;
        }
        if (purge && unlink(path) == 0) {
            (*deleted)++;
        }
    }
    closedir(handle);

    if (purge) {
        return total;
    }
    return total;
}

/* 淘汰用的文件清单（路径 + mtime + 大小） */
typedef struct {
    char path[MAX_PATH_LEN];
    time_t mtime;
    long long size;
} CacheFile;

static int cache_collect(const char *dir, CacheFile **files, int *count, int *capacity,
                         long long *total_bytes)
{
    DIR *handle = opendir(dir);
    if (!handle) {
        return -1;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[MAX_PATH_LEN];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            cache_collect(path, files, count, capacity, total_bytes);
            continue;
        }
        if (*count >= *capacity) {
            int next = (*capacity == 0) ? 128 : (*capacity * 2);
            CacheFile *grown = realloc(*files, (size_t)next * sizeof(CacheFile));
            if (!grown) {
                closedir(handle);
                return -1;
            }
            *files = grown;
            *capacity = next;
        }
        CacheFile *slot = &(*files)[*count];
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        slot->mtime = st.st_mtime;
        slot->size = st.st_size;
        (*count)++;
        *total_bytes += st.st_size;
    }
    closedir(handle);
    return 0;
}

static int cache_file_cmp_oldest_first(const void *a, const void *b)
{
    const CacheFile *fa = a;
    const CacheFile *fb = b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

long long remote_cache_usage_bytes(void)
{
    cache_build_root();
    int deleted = 0;
    return cache_walk(g_cache_root, 0, -1, NULL, 0, &deleted);
}

int remote_cache_purge(const char *const *protected_paths, int protected_count)
{
    cache_build_root();
    int deleted = 0;
    cache_walk(g_cache_root, 1, -1, protected_paths, protected_count, &deleted);
    log_info("remote_cache", "Purged %d cached file(s)", deleted);
    return deleted;
}

int remote_cache_evict(const char *const *protected_paths, int protected_count)
{
    cache_build_root();

    int limit_mb = remote_store_cache_limit_mb();
    if (limit_mb <= 0) {
        return 0;
    }
    long long limit_bytes = (long long)limit_mb * 1024 * 1024;

    CacheFile *files = NULL;
    int count = 0;
    int capacity = 0;
    long long total = 0;
    if (cache_collect(g_cache_root, &files, &count, &capacity, &total) != 0) {
        free(files);
        return 0;
    }
    if (total <= limit_bytes) {
        free(files);
        return 0;
    }

    qsort(files, (size_t)count, sizeof(CacheFile), cache_file_cmp_oldest_first);

    int deleted = 0;
    for (int i = 0; i < count && total > limit_bytes; i++) {
        if (cache_path_protected(files[i].path, protected_paths, protected_count)) {
            continue;
        }
        if (unlink(files[i].path) == 0) {
            total -= files[i].size;
            deleted++;
        }
    }
    free(files);
    if (deleted > 0) {
        log_info("remote_cache", "Evicted %d cached file(s), %lld MB left",
                 deleted, total / (1024 * 1024));
    }
    return deleted;
}
