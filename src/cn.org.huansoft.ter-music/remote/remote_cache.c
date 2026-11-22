/**
 * @file remote_cache.c
 * @brief 前端远程曲目下载器（后台线程 + 结果队列）
 *
 * 线程模型（与核心的 rpc_job.c 同思路，但对象是前端缓存）：
 *   - 同一时刻只有一个后台任务（列目录或会话下载），状态由一个互斥量保护；
 *   - 工作线程只在缓存目录里写文件，并把“已完成的本地路径”推入队列，
 *     不触碰 UI 与核心；
 *   - UI 线程每帧调用 remote_cache_take_ready()，把路径经 player 门面交给
 *     核心（核心只看到本地文件）；
 *   - 取消通过 remote_set_abort_check() 进入 curl 传输内部，在途下载会立刻
 *     中止（F-09）；worker 结束后由下一次任务或 shutdown join，绝不 detach
 *     （F-20）。
 *
 * 下载原子性：先写 `<目标>.part`，成功后 rename 成目标名；命中时校验远端
 * 大小，远端变化会重新下载（F-18）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote_cache.h"
#include "remote/remote_store.h"
#include "remote/remote_url.h"

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

/* 单曲硬上限（与 remote.c 的 REMOTE_FETCH_MAX_BYTES 一致）：远端列表把
 * size 谎报成 0/很小也不能绕过，下载前先拒绝明显超限的条目（R-03）。 */
#define REMOTE_CACHE_TRACK_MAX_BYTES (1024LL * 1024LL * 1024LL)

/* 复用播放列表的扩展名判定（library.c 也这样复用） */
extern int is_audio_file(const char *filename);

/* 单个缓存路径分量上限（留给 "<name>-<hash>.<ext>" 足够空间） */
#define REMOTE_CACHE_MAX_COMPONENT 200
#define REMOTE_CACHE_HASH_LEN 8

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
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
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
static int g_popped = 0;

static char g_cache_root[MAX_PATH_LEN] = "";

/* ── 小工具 ───────────────────────────────────────────────────────── */

static unsigned fnv1a32(const char *text)
{
    unsigned hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

/* 把远端名字映射成安全缓存文件名：
 *   - 干净名字（无路径分隔符/控制字符、长度够）原样保留，便于用户识别；
 *   - 需要消毒或超长时，保留可读前缀 + "-<8 位 hash>" + 扩展名。
 *     hash 来自原始名字，使 "a:b" 与 "a?b" 落到不同文件（F-18），
 *     截断也只在 UTF-8 字符边界发生。 */
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

    int needs_hash = 0;
    size_t in_len = strlen(in);
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int safe = (c >= 0x80) ||
                   (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')';
        if (!safe) {
            needs_hash = 1;
        }
    }
    if (!needs_hash && in_len + 1 < out_size) {
        snprintf(out, out_size, "%s", in);
        return;
    }

    /* 拆出扩展名（.mp3/.flac/.lrc ...），hash 插在扩展名前，保持可播放性 */
    const char *dot = strrchr(in, '.');
    size_t stem_len = in_len;
    const char *ext = "";
    if (dot && dot != in && strlen(dot) <= 10) {
        stem_len = (size_t)(dot - in);
        ext = dot;
    }

    size_t budget = out_size - 1 - (REMOTE_CACHE_HASH_LEN + 1) - strlen(ext);
    if (budget > REMOTE_CACHE_MAX_COMPONENT) {
        budget = REMOTE_CACHE_MAX_COMPONENT;
    }
    if (stem_len > budget) {
        stem_len = budget;
        /* 不要切断 UTF-8 多字节序列 */
        while (stem_len > 0 && ((unsigned char)in[stem_len] & 0xC0) == 0x80) {
            stem_len--;
        }
    }

    size_t pos = 0;
    for (size_t i = 0; i < stem_len && pos + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        int safe = (c >= 0x80) ||
                   (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')';
        out[pos++] = safe ? (char)c : '_';
    }
    unsigned hash = fnv1a32(in);
    pos += (size_t)snprintf(out + pos, out_size - pos, "-%08x", hash);
    snprintf(out + pos, out_size - pos, "%s", ext);
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
        char component[REMOTE_CACHE_MAX_COMPONENT * 3];
        if (length >= sizeof(component)) {
            length = sizeof(component) - 1;
        }
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

static void cache_fail(const char *text)
{
    log_warn("remote_cache", "Remote job failed: %s", text ? text : "(unknown)");
    pthread_mutex_lock(&g_lock);
    g_status.state = REMOTE_CACHE_FAILED;
    snprintf(g_status.message, sizeof(g_status.message), "%s", text ? text : "");
    snprintf(g_last_error, sizeof(g_last_error), "%s", text ? text : "");
    g_error_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

/* 取消钩子：由 remote 层在 curl 传输期间回调（F-09） */
static int cache_abort_check(void *ctx)
{
    (void)ctx;
    int cancelled;
    pthread_mutex_lock(&g_lock);
    cancelled = g_cancel_requested;
    pthread_mutex_unlock(&g_lock);
    return cancelled;
}

/* ── 下载 ─────────────────────────────────────────────────────────── */

/* 缓存命中判定：大小必须与远端声明一致（远端未给 size 时只要求非空）。
 * 远端文件被替换后旧缓存不再“永久命中”（F-18）。 */
static int cache_cached_ok(const char *local_path, long long expected_size)
{
    struct stat st;
    if (stat(local_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    if (expected_size >= 0 && st.st_size != expected_size) {
        return 0;
    }
    return 1;
}

/* 尝试下载同名歌词；失败不影响播放（F-18：音频已命中时也补一次缺失的 LRC） */
static void cache_fetch_lrc(const RemoteConnectionConfig *conn,
                            const char *audio_subpath, const char *audio_local)
{
    char lrc_local[MAX_PATH_LEN];
    snprintf(lrc_local, sizeof(lrc_local), "%s", audio_local);
    char *local_dot = strrchr(lrc_local, '.');
    if (!local_dot) {
        return;
    }
    strcpy(local_dot, ".lrc");
    struct stat st;
    if (stat(lrc_local, &st) == 0 && st.st_size > 0) {
        return;
    }

    char lrc_subpath[MAX_PATH_LEN];
    snprintf(lrc_subpath, sizeof(lrc_subpath), "%s", audio_subpath);
    char *sub_dot = strrchr(lrc_subpath, '.');
    if (!sub_dot || strchr(sub_dot, '/')) {
        return;
    }
    strcpy(sub_dot, ".lrc");

    char lrc_part[MAX_PATH_LEN];
    int written = snprintf(lrc_part, sizeof(lrc_part), "%s.part", lrc_local);
    if (written < 0 || (size_t)written >= sizeof(lrc_part)) {
        return;
    }
    unlink(lrc_part);
    if (remote_fetch_file(conn, lrc_subpath, lrc_part) == 0) {
        rename(lrc_part, lrc_local);
    } else {
        unlink(lrc_part);
    }
}

/* 把一首曲目（及其同名 .lrc）下载到缓存；返回 0 表示本地文件就绪。
 * 凭据随 conn 传递，URL 文本里没有密码（F-05）。 */
static int cache_fetch_track(const RemoteConnectionConfig *conn, const char *file_subpath,
                             const char *local_path, long long expected_size,
                             char *error, size_t error_size)
{
    if (!cache_cached_ok(local_path, expected_size)) {
        char part[MAX_PATH_LEN];
        int written = snprintf(part, sizeof(part), "%s.part", local_path);
        if (written < 0 || (size_t)written >= sizeof(part)) {
            snprintf(error, error_size, "path too long");
            return -1;
        }
        unlink(part);

        if (remote_fetch_file(conn, file_subpath, part) != 0) {
            snprintf(error, error_size, "%s", remote_strerror());
            unlink(part);
            return -1;
        }
        if (rename(part, local_path) != 0) {
            snprintf(error, error_size, "rename failed: %s", strerror(errno));
            unlink(part);
            return -1;
        }
    }
    cache_fetch_lrc(conn, file_subpath, local_path);
    return 0;
}

/* ── 工作线程 ─────────────────────────────────────────────────────── */

static void cache_job_finish(int cancelled)
{
    /* 先清 abort hook/worker 计数，再发布 idle：否则 UI 可能在 idle 后立刻
     * 启动新任务并安装新 hook，随后旧 worker 收尾把新 hook 清掉（R-07）。 */
    remote_set_abort_check(NULL, NULL);
    remote_set_worker_active(0);

    pthread_mutex_lock(&g_lock);
    if (cancelled) {
        g_status.state = REMOTE_CACHE_CANCELLED;
        snprintf(g_status.message, sizeof(g_status.message), "cancelled");
    } else if (g_status.state == REMOTE_CACHE_DOWNLOADING ||
               g_status.state == REMOTE_CACHE_LISTING) {
        g_status.state = (g_status.done > 0) ? REMOTE_CACHE_DONE : REMOTE_CACHE_FAILED;
    }
    g_thread_running = 0;
    g_cancel_requested = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

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

    int cancelled = 0;

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
        pthread_mutex_lock(&g_lock);
        cancelled = g_cancel_requested;
        pthread_mutex_unlock(&g_lock);
        if (!cancelled) {
            cache_fail(message);
        }
        cache_job_finish(cancelled);
        return NULL;
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
        cache_job_finish(0);
        return NULL;
    }

    /* 会话下载 */
    {
        char dir[MAX_PATH_LEN];
        if (cache_session_dir(&conn, subpath, dir, sizeof(dir)) != 0 ||
            cache_mkdir_p(dir) != 0) {
            remote_free_entries(entries, count);
            cache_fail("cannot create cache directory");
            cache_job_finish(0);
            return NULL;
        }

        int total_available = 0;
        for (int i = 0; i < count; i++) {
            if (!entries[i].is_dir && is_audio_file(entries[i].name)) {
                total_available++;
            }
        }
        int to_fetch = total_available;
        if (to_fetch > REMOTE_CACHE_SESSION_MAX) {
            to_fetch = REMOTE_CACHE_SESSION_MAX;
        }

        pthread_mutex_lock(&g_lock);
        g_status.state = REMOTE_CACHE_DOWNLOADING;
        g_status.total = total_available;
        g_status.truncated = total_available > REMOTE_CACHE_SESSION_MAX;
        g_status.done = 0;
        g_status.failed = 0;
        pthread_mutex_unlock(&g_lock);

        if (total_available == 0) {
            remote_free_entries(entries, count);
            cache_fail("no audio files in this directory");
            cache_job_finish(0);
            return NULL;
        }

        int limit_mb = remote_store_cache_limit_mb();
        long long limit_bytes = limit_mb > 0 ? (long long)limit_mb * 1024 * 1024 : 0;
        long long usage = limit_bytes > 0 ? remote_cache_usage_bytes() : 0;
        int delivered = 0;

        for (int i = 0; i < count && delivered < to_fetch; i++) {
            if (entries[i].is_dir || !is_audio_file(entries[i].name)) {
                continue;
            }

            pthread_mutex_lock(&g_lock);
            cancelled = g_cancel_requested;
            pthread_mutex_unlock(&g_lock);
            if (cancelled) {
                break;
            }

            if (entries[i].size > REMOTE_CACHE_TRACK_MAX_BYTES) {
                pthread_mutex_lock(&g_lock);
                g_status.failed++;
                snprintf(g_status.message, sizeof(g_status.message),
                         "%.120s: remote file exceeds the 1 GiB limit",
                         entries[i].name);
                snprintf(g_last_error, sizeof(g_last_error),
                         "%.120s: remote file exceeds the 1 GiB limit",
                         entries[i].name);
                g_error_pending = 1;
                pthread_mutex_unlock(&g_lock);
                continue;
            }

            if (limit_bytes > 0 && entries[i].size > 0 && usage + entries[i].size > limit_bytes) {
                /* 超预算：先淘汰旧缓存（不含本次会话新文件，它们尚未受保护），
                 * 仍放不下就停止并明确报错，而不是把磁盘写满（F-10）。 */
                remote_cache_evict(NULL, 0);
                usage = remote_cache_usage_bytes();
                if (usage + entries[i].size > limit_bytes) {
                    cache_fail("cache limit reached - raise the cache limit or clear the cache");
                    break;
                }
            }

            /* 远端完整路径：目录 subpath + 文件名，避免出现 "//" */
            char file_subpath[MAX_PATH_LEN * 2];
            size_t sub_len = strlen(subpath);
            int written;
            if (sub_len == 0) {
                written = snprintf(file_subpath, sizeof(file_subpath), "/%s",
                                   entries[i].name);
            } else if (subpath[sub_len - 1] == '/') {
                written = snprintf(file_subpath, sizeof(file_subpath), "%s%s",
                                   subpath, entries[i].name);
            } else {
                written = snprintf(file_subpath, sizeof(file_subpath), "%s/%s",
                                   subpath, entries[i].name);
            }
            if (written < 0 || (size_t)written >= sizeof(file_subpath)) {
                remote_free_entries(entries, count);
                cache_fail("remote path is too long");
                cache_job_finish(0);
                return NULL;
            }

            char safe_name[REMOTE_CACHE_MAX_COMPONENT];
            cache_safe_component(entries[i].name, safe_name, sizeof(safe_name));
            char local_path[MAX_PATH_LEN];
            written = snprintf(local_path, sizeof(local_path), "%s/%s", dir, safe_name);
            if (written < 0 || (size_t)written >= sizeof(local_path)) {
                remote_free_entries(entries, count);
                cache_fail("cache path is too long");
                cache_job_finish(0);
                return NULL;
            }

            char error[256] = "";
            if (cache_fetch_track(&conn, file_subpath, local_path, entries[i].size,
                                  error, sizeof(error)) == 0) {
                cache_push_ready(local_path);
                delivered++;
                if (entries[i].size > 0) {
                    usage += entries[i].size;
                }
                pthread_mutex_lock(&g_lock);
                g_status.done = delivered;
                pthread_mutex_unlock(&g_lock);
            } else {
                pthread_mutex_lock(&g_lock);
                cancelled = g_cancel_requested;
                if (!cancelled) {
                    g_status.failed++;
                    snprintf(g_status.message, sizeof(g_status.message), "%.120s: %.100s",
                             entries[i].name, error);
                    snprintf(g_last_error, sizeof(g_last_error), "%.120s: %.100s",
                             entries[i].name, error);
                    g_error_pending = 1;
                }
                pthread_mutex_unlock(&g_lock);
                if (cancelled) {
                    break;
                }
                log_warn("remote_cache", "Download failed for '%s': %s",
                         entries[i].name, error);
            }
        }

        remote_free_entries(entries, count);

        pthread_mutex_lock(&g_lock);
        g_session_autoplay = autoplay;
        if (g_status.state == REMOTE_CACHE_DOWNLOADING) {
            g_status.state = (delivered > 0) ? REMOTE_CACHE_DONE : REMOTE_CACHE_FAILED;
            if (delivered == 0 && g_status.message[0] == '\0') {
                snprintf(g_status.message, sizeof(g_status.message),
                         "no track could be downloaded");
            }
        }
        pthread_mutex_unlock(&g_lock);
    }

    pthread_mutex_lock(&g_lock);
    cancelled = g_cancel_requested;
    pthread_mutex_unlock(&g_lock);
    cache_job_finish(cancelled);
    return NULL;
}

/* 启动新任务前先回收上一个已结束的 worker（F-20：不再依赖 detach/泄漏）。 */
static int cache_reap_previous(void)
{
    pthread_t previous;
    int have_previous = 0;

    pthread_mutex_lock(&g_lock);
    if (g_thread_running) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    if (g_thread_started) {
        previous = g_thread;
        have_previous = 1;
        g_thread_started = 0;
    }
    pthread_mutex_unlock(&g_lock);

    if (have_previous) {
        pthread_join(previous, NULL);
    }
    return 0;
}

static int cache_start_job(int kind, const RemoteConnectionConfig *conn,
                           const char *subpath, int autoplay)
{
    if (!conn) {
        return -1;
    }
    if (cache_reap_previous() != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_job.kind = kind;
    g_job.conn = *conn;
    snprintf(g_job.subpath, sizeof(g_job.subpath), "%s", subpath ? subpath : "");
    g_job.autoplay = autoplay;

    g_status.state = (kind == 0) ? REMOTE_CACHE_LISTING : REMOTE_CACHE_DOWNLOADING;
    g_status.done = 0;
    g_status.total = 0;
    g_status.failed = 0;
    g_status.cancelled = 0;
    g_status.truncated = 0;
    g_status.message[0] = '\0';
    g_cancel_requested = 0;
    g_error_pending = 0;
    /* 新会话必须重新编号 ready 队列：否则同进程第二次打开同一目录时
     * is_first 恒为 0，UI 会把首曲走 append 而不是替换整表，出现 a,b,a,b
     * 重复队列（t5 V-01 / F-15 回归）。在锁内重置，与 take_ready 同步。 */
    g_popped = 0;
    g_thread_running = 1;
    pthread_mutex_unlock(&g_lock);

    remote_set_abort_check(cache_abort_check, NULL);
    remote_set_worker_active(1);

    if (pthread_create(&g_thread, NULL, cache_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_thread_running = 0;
        g_status.state = REMOTE_CACHE_FAILED;
        snprintf(g_status.message, sizeof(g_status.message), "cannot create worker thread");
        pthread_mutex_unlock(&g_lock);
        remote_set_abort_check(NULL, NULL);
        remote_set_worker_active(0);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_thread_started = 1;
    pthread_mutex_unlock(&g_lock);
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
    pthread_t thread;
    int started;
    int still_running;

    remote_cache_cancel();

    pthread_mutex_lock(&g_lock);
    if (g_thread_started) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 2;
        while (g_thread_running) {
            if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT) {
                break;
            }
        }
    }
    started = g_thread_started;
    still_running = g_thread_running;
    thread = g_thread;
    pthread_mutex_unlock(&g_lock);

    if (started) {
        if (!still_running) {
            pthread_join(thread, NULL);      /* 正常路径：worker 已结束，join 立即返回 */
            pthread_mutex_lock(&g_lock);
            g_thread_started = 0;
            pthread_mutex_unlock(&g_lock);
        } else {
            /* 不在超时后 detach（会和 curl_global_cleanup 竞态）；保持 joinable，
             * remote_cleanup() 检测到在途传输时会跳过全局清理，进程随即退出。 */
            log_warn("remote_cache", "Worker still busy at shutdown; skipping join");
        }
    }

    remote_set_abort_check(NULL, NULL);

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
    /* 队列暂时清空不代表“会话最后一首”：下载仍在进行时新歌随时会到。
     * 真正语义见调用方：这里只在 worker 结束且队列为空时才置 last。 */
    int last = (g_ready_head == NULL) && !g_thread_running;
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

int remote_cache_wait_idle(int timeout_ms)
{
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    int idle = 1;
    pthread_mutex_lock(&g_lock);
    while (g_thread_running) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT) {
            idle = !g_thread_running;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return idle ? 0 : -1;
}

void remote_cache_get_status(RemoteCacheStatus *out)
{
    if (!out) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}

/* ── 缓存维护 ─────────────────────────────────────────────────────── */

static int cache_path_protected(const char *path,
                                const char *const *protected_paths, int protected_count)
{
    if (!protected_paths) {
        return 0;
    }
    for (int i = 0; i < protected_count; i++) {
        if (protected_paths[i] && strcmp(path, protected_paths[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 递归累计目录大小；purge 为真时删除未保护文件。 */
static long long cache_walk(const char *dir, int purge,
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
            total += cache_walk(path, purge, protected_paths, protected_count, deleted);
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
    return cache_walk(g_cache_root, 0, NULL, 0, &deleted);
}

int remote_cache_purge(const char *const *protected_paths, int protected_count)
{
    cache_build_root();
    int deleted = 0;
    cache_walk(g_cache_root, 1, protected_paths, protected_count, &deleted);
    log_info("remote_cache", "Purged %d cached file(s)", deleted);
    return deleted;
}

int remote_cache_evict(const char *const *protected_paths, int protected_count)
{
    cache_build_root();

    int limit_mb = remote_store_cache_limit_mb();
    if (limit_mb <= 0) {
        return 0;                        /* 0 = 不限制（F-14 语义） */
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
