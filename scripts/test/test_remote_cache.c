/*
 * remote_cache 单测：下载原子性、命中校验、失败清理、取消、线程回收、淘汰
 *
 * 覆盖审计 F-09（取消后不再交付）、F-10（缓存预算）、F-14（0=不限制）、
 * F-18（缓存键碰撞/命中校验/LRC 补齐）、F-20（worker join）、F-25（截断标记）、
 * F-26（状态快照 API）。
 *
 * 运行：scripts/test/run-unit-tests.sh test_remote_cache
 * 依赖（见 test_remote_cache.srcs）：remote/remote_cache.c + stub_logger.c
 * 远端 I/O 由本文件内的桩函数提供（不联网、不依赖 libcurl）。
 */

#include "remote/remote.h"
#include "remote/remote_cache.h"
#include "remote/remote_store.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

/* ── 远端桩 ───────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    long long size;
    int is_dir;
} StubEntry;

static StubEntry g_entries[256];
static int g_entry_count;
static int g_list_fail;
static const char *g_fail_url_fragment;
static int g_fetch_calls;
static int g_lrc_fetch_calls;
static int g_cancel_on_fetch;
static int g_cancel_after_fetches;
static int g_fetch_delay_us;

static int (*g_abort_check)(void *ctx);
static void *g_abort_ctx;
static int g_abort_hook_seen;
static int g_worker_active;

static const char *fragment(const char *url)
{
    const char *slash = strrchr(url, '/');
    return slash ? slash + 1 : url;
}

int remote_list_directory(const RemoteConnectionConfig *conn, const char *subpath,
                          RemoteDirEntry **out_entries, int *out_count)
{
    (void)conn; (void)subpath;
    *out_entries = NULL;
    *out_count = 0;
    if (g_list_fail) {
        remote_set_error("stub listing failure");
        return -1;
    }
    RemoteDirEntry *entries = calloc((size_t)(g_entry_count > 0 ? g_entry_count : 1),
                                     sizeof(RemoteDirEntry));
    if (!entries) {
        return -1;
    }
    for (int i = 0; i < g_entry_count; i++) {
        snprintf(entries[i].name, sizeof(entries[i].name), "%s", g_entries[i].name);
        entries[i].is_dir = g_entries[i].is_dir;
        entries[i].size = g_entries[i].size;
        entries[i].mtime = 0;
    }
    *out_entries = entries;
    *out_count = g_entry_count;
    return 0;
}

void remote_free_entries(RemoteDirEntry *entries, int count)
{
    (void)count;
    free(entries);
}

long long stub_size_for_name(const char *name)
{
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            return g_entries[i].size;
        }
    }
    return 16;
}

int remote_fetch_file(const RemoteConnectionConfig *conn, const char *subpath,
                      const char *dest_path)
{
    (void)conn;
    const char *name = fragment(subpath);
    int is_lrc = strstr(name, ".lrc") != NULL;
    if (is_lrc) {
        g_lrc_fetch_calls++;
    } else {
        g_fetch_calls++;
    }
    if (g_fail_url_fragment && strstr(subpath, g_fail_url_fragment) != NULL) {
        remote_set_error("stub fetch failure");
        return -1;
    }
    if (g_cancel_on_fetch) {
        remote_cache_cancel();
    }

    long long size = 16;
    if (is_lrc) {
        size = 7;
    } else {
        /* URL 里的名字是百分号编码前的名字（测试用例只用 ASCII） */
        char decoded[256];
        snprintf(decoded, sizeof(decoded), "%s", name);
        size = stub_size_for_name(decoded);
    }

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        return -1;
    }
    for (long long i = 0; i < size; i++) {
        fputc('x', fp);
    }
    fclose(fp);

    if (g_abort_ctx != NULL || g_abort_check != NULL) {
        g_abort_hook_seen = 1;
    }
    if (g_fetch_delay_us > 0) {
        struct timespec delay = {0, (long)g_fetch_delay_us * 1000L};
        nanosleep(&delay, NULL);
    }
    if (g_cancel_after_fetches > 0 && g_fetch_calls >= g_cancel_after_fetches) {
        remote_cache_cancel();
    }
    return 0;
}

const char *remote_strerror(void)
{
    return "";
}

void remote_set_error(const char *msg)
{
    (void)msg;
}

void remote_set_abort_check(int (*check)(void *ctx), void *ctx)
{
    g_abort_check = check;
    g_abort_ctx = ctx;
}

void remote_set_worker_active(int active)
{
    g_worker_active += active ? 1 : -1;
}

/* 真 remote_store.c 会被链接进来（F-14 的 save/load 往返要实测），
 * 这里只提供配置目录：测试用临时目录。 */
static const char *g_config_dir_stub = NULL;

const char *get_config_dir(void)
{
    return g_config_dir_stub;
}

int is_audio_file(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
           strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".ogg") == 0;
}

/* ── 测试辅助 ─────────────────────────────────────────────────────── */

static char g_tmp_root[MAX_PATH_LEN];
static RemoteConnectionConfig g_conn;

static void drain_ready(void)
{
    char path[MAX_PATH_LEN];
    int first = 0, last = 0;
    while (remote_cache_take_ready(path, sizeof(path), &first, &last)) {
        /* discard */
    }
    RemoteDirEntry *entries = NULL;
    int count = 0, error = 0;
    while (remote_cache_take_listing(&entries, &count, &error)) {
        remote_free_entries(entries, count);
        entries = NULL;
    }
}

static void reset_stub(void)
{
    drain_ready();
    g_entry_count = 0;
    g_list_fail = 0;
    g_fail_url_fragment = NULL;
    g_fetch_calls = 0;
    g_lrc_fetch_calls = 0;
    g_cancel_on_fetch = 0;
    g_cancel_after_fetches = 0;
    g_fetch_delay_us = 0;
    remote_store_set_cache_limit_mb(2048);
    g_abort_hook_seen = 0;
    g_abort_check = NULL;
    g_abort_ctx = NULL;
}

static void add_entry(const char *name, long long size, int is_dir)
{
    if (g_entry_count >= (int)(sizeof(g_entries) / sizeof(g_entries[0]))) {
        return;
    }
    g_entries[g_entry_count].name = name;
    g_entries[g_entry_count].size = size;
    g_entries[g_entry_count].is_dir = is_dir;
    g_entry_count++;
}

static int cache_file_exists(const char *name)
{
    char path[MAX_PATH_LEN];
    char safe_name[256];
    /* 名字里可能有会被消毒的字符；这里只用于干净名字 */
    snprintf(safe_name, sizeof(safe_name), "%s", name);
    int found = 0;
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "find '%s' -name '%s' -type f 2>/dev/null | head -1",
             g_tmp_root, safe_name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[MAX_PATH_LEN];
    if (fgets(line, sizeof(line), fp)) {
        found = 1;
    }
    pclose(fp);
    return found;
}

static int part_files_left(void)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "find '%s' -name '*.part' -type f 2>/dev/null | wc -l",
             g_tmp_root);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    int count = -1;
    if (fscanf(fp, "%d", &count) != 1) {
        count = -1;
    }
    pclose(fp);
    return count;
}

static int cached_files_count(void)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "find '%s' -type f ! -name '*.lrc' 2>/dev/null | wc -l", g_tmp_root);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    int count = -1;
    if (fscanf(fp, "%d", &count) != 1) {
        count = -1;
    }
    pclose(fp);
    return count;
}

static int read_threads(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    char line[256];
    int threads = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Threads:", 8) == 0) {
            threads = atoi(line + 8);
            break;
        }
    }
    fclose(fp);
    return threads;
}

static RemoteCacheStatus await_status(void)
{
    RemoteCacheStatus status;
    for (int i = 0; i < 400; i++) {
        remote_cache_get_status(&status);
        if (status.state == REMOTE_CACHE_DONE ||
            status.state == REMOTE_CACHE_FAILED ||
            status.state == REMOTE_CACHE_CANCELLED) {
            break;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (remote_cache_wait_idle(5000) != 0) {
        printf("  FAIL worker did not go idle\n");
        failures++;
    }
    remote_cache_get_status(&status);
    return status;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

static void test_session_download_and_hit(void)
{
    reset_stub();
    add_entry("a.wav", 10, 0);
    add_entry("b.mp3", 20, 0);
    add_entry("notes.txt", 5, 0);
    add_entry("sub", 0, 1);

    CHECK(remote_cache_start_session(&g_conn, "/music", 0) == 0, "会话启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_DONE, "会话完成状态");
    CHECK(status.done == 2 && status.total == 2 && status.failed == 0, "2 首全部下载");
    CHECK(g_fetch_calls == 2, "只下载音频（非音频/目录跳过）");
    CHECK(cached_files_count() == 2, "缓存落盘 2 个音频");
    CHECK(part_files_left() == 0, ".part 全部 rename 完成");
    CHECK(g_lrc_fetch_calls == 2, "每首尝试同名 LRC");
    CHECK(g_abort_hook_seen == 1, "下载期间安装了取消钩子");
    CHECK(g_abort_check == NULL, "任务结束后卸载取消钩子");

    char path[MAX_PATH_LEN];
    int first = 0, last = 0, ready = 0;
    while (remote_cache_take_ready(path, sizeof(path), &first, &last)) {
        CHECK(strstr(path, g_tmp_root) == path, "ready 路径是缓存本地路径");
        ready++;
    }
    CHECK(ready == 2, "ready 队列交付 2 首");

    /* 第二次：全部命中，不再产生音频下载（LRC 已存在也不下载） */
    g_fetch_calls = 0;
    g_lrc_fetch_calls = 0;
    CHECK(remote_cache_start_session(&g_conn, "/music", 0) == 0, "第二次会话启动");
    status = await_status();
    CHECK(status.state == REMOTE_CACHE_DONE && status.done == 2, "第二次会话完成");
    CHECK(g_fetch_calls == 0, "缓存命中不重复下载（F-18）");
    CHECK(g_lrc_fetch_calls == 0, "LRC 已存在不重复下载");
}

static void test_failure_cleanup(void)
{
    reset_stub();
    g_fail_url_fragment = "broken.wav";
    add_entry("broken.wav", 10, 0);
    add_entry("good.wav", 10, 0);

    CHECK(remote_cache_start_session(&g_conn, "/fail", 0) == 0, "失败场景会话启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_DONE, "部分失败仍算完成");
    CHECK(status.failed == 1 && status.done == 1, "失败 1 首成功 1 首");
    CHECK(part_files_left() == 0, "失败后 .part 被清理");
    CHECK(cache_file_exists("broken.wav") == 0, "失败曲目没有留下半成品");
    CHECK(cache_file_exists("good.wav") == 1, "成功曲目落盘");
}

static void test_cancel(void)
{
    reset_stub();
    g_cancel_after_fetches = 1;
    add_entry("one.wav", 10, 0);
    add_entry("two.wav", 10, 0);
    add_entry("three.wav", 10, 0);

    CHECK(remote_cache_start_session(&g_conn, "/cancel", 0) == 0, "取消场景会话启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_CANCELLED, "取消后状态为 CANCELLED");
    CHECK(status.done == 1, "取消后不再交付后续曲目（F-09）");
    CHECK(g_fetch_calls == 1, "第一首之后不再发起下载");
    CHECK(part_files_left() == 0, "取消后没有 .part 残留");

    char path[MAX_PATH_LEN];
    int first = 0, last = 0;
    CHECK(remote_cache_take_ready(path, sizeof(path), &first, &last) == 1, "取消前已完成的 1 首可取");
    CHECK(remote_cache_take_ready(path, sizeof(path), &first, &last) == 0, "取消后没有更多交付");
}

/* R-07：完成→立即新任务→取消。旧实现先发布 idle 再清 abort hook，新任务
 * 的取消钩子会被旧 worker 收尾清掉；重复多轮验证取消始终生效。 */
static void test_finish_then_immediate_cancel(void)
{
    for (int round = 0; round < 20; round++) {
        reset_stub();
        g_fetch_delay_us = 20 * 1000;
        add_entry("fast.wav", 10, 0);

        CHECK(remote_cache_start_session(&g_conn, "/race", 0) == 0,
              "竞态回归：首个会话启动");
        RemoteCacheStatus status = await_status();
        CHECK(status.state == REMOTE_CACHE_DONE, "竞态回归：首个会话完成");

        CHECK(remote_cache_start_session(&g_conn, "/race", 0) == 0,
              "竞态回归：完成后立即启动新会话");
        remote_cache_cancel();
        status = await_status();
        CHECK(status.state == REMOTE_CACHE_CANCELLED,
              "完成→立即新任务→取消仍生效（R-07 abort hook 顺序）");
    }
}

/* R-03：列表声明 size 超过 1 GiB 时下载前拒绝，不发起 fetch。 */
static void test_oversize_listing_rejected(void)
{
    reset_stub();
    add_entry("huge.wav", 1024LL * 1024LL * 1024LL + 1, 0);
    add_entry("small.wav", 10, 0);
    CHECK(remote_cache_start_session(&g_conn, "/oversize", 0) == 0,
          "超大文件场景会话启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.failed == 1, "超大文件计入失败");
    CHECK(status.done == 1, "同目录正常曲目仍下载");
    CHECK(g_fetch_calls == 1, "超大文件没有发起 fetch（列表 size 预检）");
    CHECK(cache_file_exists("huge.wav") == 0, "超大文件没有落盘");
}

static void test_size_mismatch_redownload(void)
{
    reset_stub();
    add_entry("stale.wav", 40, 0);
    RemoteConnectionConfig conn = g_conn;
    snprintf(conn.host, sizeof(conn.host), "stalehost");
    CHECK(remote_cache_start_session(&conn, "/stale", 0) == 0, "陈旧缓存场景启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_DONE, "陈旧缓存场景完成");
    CHECK(g_fetch_calls == 1, "首次下载 1 首");

    /* 本地文件被截短 → 下次会话应重新下载 */
    char path[MAX_PATH_LEN];
    int first = 0, last = 0;
    CHECK(remote_cache_take_ready(path, sizeof(path), &first, &last) == 1, "取到缓存路径");
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL, "改写缓存文件");
    if (fp) {
        fputs("short", fp);
        fclose(fp);
    }
    g_fetch_calls = 0;
    CHECK(remote_cache_start_session(&conn, "/stale", 0) == 0, "二次会话启动");
    status = await_status();
    CHECK(status.state == REMOTE_CACHE_DONE, "二次会话完成");
    CHECK(g_fetch_calls == 1, "大小不符时重新下载（F-18）");
    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_size == 40, "重新下载后大小与远端一致");
}

static void test_no_audio_and_truncation(void)
{
    reset_stub();
    add_entry("readme.txt", 3, 0);
    add_entry("sub", 0, 1);
    CHECK(remote_cache_start_session(&g_conn, "/empty", 0) == 0, "无音频场景启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_FAILED, "无音频按失败处理");
    CHECK(strstr(status.message, "no audio files") != NULL, "无音频错误信息");
    CHECK(g_fetch_calls == 0, "无音频不发起下载");

    /* 超过单会话上限：total 报真实值、truncated=1（F-25） */
    static char names[210][32];
    reset_stub();
    for (int i = 0; i < 205; i++) {
        snprintf(names[i], sizeof(names[i]), "t%03d.wav", i);
        add_entry(names[i], 4, 0);
    }
    CHECK(remote_cache_start_session(&g_conn, "/big", 0) == 0, "大目录场景启动");
    status = await_status();
    CHECK(status.total == 205, "status.total 报目录真实音频数（F-25）");
    CHECK(status.truncated == 1, "超过上限时 truncated=1");
    CHECK(status.done == REMOTE_CACHE_SESSION_MAX, "只下载单会话上限");
    CHECK(g_fetch_calls == REMOTE_CACHE_SESSION_MAX, "下载次数等于上限");
}

static void test_cache_budget(void)
{
    reset_stub();
    remote_store_set_cache_limit_mb(1);      /* 1 MB */
    add_entry("huge.wav", 2LL * 1024 * 1024, 0);
    RemoteConnectionConfig conn = g_conn;
    snprintf(conn.host, sizeof(conn.host), "budgethost");
    CHECK(remote_cache_start_session(&conn, "/budget", 0) == 0, "预算场景启动");
    RemoteCacheStatus status = await_status();
    CHECK(status.state == REMOTE_CACHE_FAILED, "超预算按失败处理");
    CHECK(strstr(status.message, "cache limit") != NULL, "超预算错误可读");
    CHECK(g_fetch_calls == 0, "超预算不写磁盘");
}

static void test_eviction(void)
{
    reset_stub();
    remote_store_set_cache_limit_mb(0);      /* 0 = 不限制（F-14） */
    CHECK(remote_cache_evict(NULL, 0) == 0, "0=不限制时不淘汰");

    /* 造两个文件：旧的大文件 + 新的小文件；上限 1MB，保护新的 */
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/evicttest", remote_cache_dir());
    char cmd[1600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && dd if=/dev/zero of='%s/old.wav' bs=1024 count=800 2>/dev/null && "
             "dd if=/dev/zero of='%s/new.wav' bs=1024 count=800 2>/dev/null && "
             "touch -d '2020-01-01' '%s/old.wav' && touch '%s/new.wav'",
             dir, dir, dir, dir, dir);
    CHECK(system(cmd) == 0, "准备淘汰测试文件");

    remote_store_set_cache_limit_mb(1);
    char old_path[MAX_PATH_LEN], new_path[MAX_PATH_LEN];
    snprintf(old_path, sizeof(old_path), "%s/old.wav", dir);
    snprintf(new_path, sizeof(new_path), "%s/new.wav", dir);
    const char *protected_paths[] = { new_path };
    int deleted = remote_cache_evict(protected_paths, 1);
    CHECK(deleted >= 1, "超出上限时淘汰旧文件");
    struct stat st;
    CHECK(stat(old_path, &st) != 0, "最旧文件被删除");
    CHECK(stat(new_path, &st) == 0, "保护集内文件保留（F-19）");
}

static void test_safe_component_collision(void)
{
    reset_stub();
    add_entry("song.wav", 8, 0);

    RemoteConnectionConfig a = g_conn;
    snprintf(a.host, sizeof(a.host), "host:colon");
    RemoteConnectionConfig b = g_conn;
    snprintf(b.host, sizeof(b.host), "host?question");
    CHECK(remote_cache_start_session(&a, "/", 0) == 0, "碰撞场景 A");
    CHECK(await_status().state == REMOTE_CACHE_DONE, "碰撞场景 A 完成");
    CHECK(remote_cache_start_session(&b, "/", 0) == 0, "碰撞场景 B");
    CHECK(await_status().state == REMOTE_CACHE_DONE, "碰撞场景 B 完成");

    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "find '%s' -name 'song.wav' -type f 2>/dev/null | wc -l", g_tmp_root);
    FILE *fp = popen(cmd, "r");
    int count = 0;
    if (fp) {
        if (fscanf(fp, "%d", &count) != 1) count = 0;
        pclose(fp);
    }
    CHECK(count == 2, "消毒后的主机名不再碰撞（F-18）");
}

static void write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

/* F-14：remote.xml 的 <cache> 用属性写出、用属性/子元素都能读回；0=不限制 */
/* t5 V-01：同进程第二次打开同一目录时，首曲必须重新标记 is_first=1，
 * 否则 UI 会把首曲 append 到旧队列（a,b,a,b）而不是替换整表。 */
static void test_repeat_open_first_flag(void)
{
    reset_stub();
    add_entry("r1.wav", 5, 0);
    add_entry("r2.wav", 6, 0);
    RemoteConnectionConfig conn = g_conn;
    snprintf(conn.host, sizeof(conn.host), "repeathost");

    for (int round = 1; round <= 2; round++) {
        CHECK(remote_cache_start_session(&conn, "/repeat", 0) == 0, "重复打开：会话启动");
        RemoteCacheStatus status = await_status();
        CHECK(status.state == REMOTE_CACHE_DONE && status.done == 2,
              "重复打开：全部下载完成");

        int ready = 0;
        int first_seen = 0;
        int duplicate = 0;
        char seen[8][MAX_PATH_LEN];
        int seen_count = 0;
        char path[MAX_PATH_LEN];
        int first = 0, last = 0;
        while (remote_cache_take_ready(path, sizeof(path), &first, &last)) {
            if (first) first_seen++;
            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], path) == 0) duplicate = 1;
            }
            if (seen_count < 8) {
                snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", path);
                seen_count++;
            }
            ready++;
        }
        CHECK(ready == 2, "重复打开：交付数=成功下载数");
        CHECK(first_seen == 1, "重复打开：首曲标记只出现一次（g_popped 已按会话重置）");
        CHECK(duplicate == 0, "重复打开：交付路径无重复行");
        (void)round;
    }
}

static void test_store_cache_roundtrip(void)
{
    char dir[] = "/tmp/tm-remote-store-XXXXXX";
    char *root = mkdtemp(dir);
    CHECK(root != NULL, "创建 store 测试目录");
    if (!root) {
        return;
    }
    g_config_dir_stub = root;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/remote.xml", root);
    write_text_file(path,
                    "<?xml version=\"1.0\"?><ter-music-remote version=\"1\">"
                    "<connections/><cache limit_mb=\"1234\"/></ter-music-remote>");
    CHECK(remote_store_load() >= 0, "加载属性形状的 <cache>");
    CHECK(remote_store_cache_limit_mb() == 1234, "属性形状 limit_mb=1234 生效（F-14）");

    remote_store_set_cache_limit_mb(0);
    CHECK(remote_store_save() == 0, "保存缓存设置");
    char saved[4096];
    FILE *fp = fopen(path, "r");
    size_t got = fp ? fread(saved, 1, sizeof(saved) - 1, fp) : 0;
    if (fp) {
        fclose(fp);
    }
    saved[got] = '\0';
    CHECK(strstr(saved, "limit_mb=\"0\"") != NULL, "save 用属性写出 limit_mb=0");
    CHECK(remote_store_load() >= 0, "重新加载保存后的文件");
    CHECK(remote_store_cache_limit_mb() == 0, "save→load 保持 0=不限制（F-14）");

    write_text_file(path,
                    "<ter-music-remote version=\"1\"><connections/>"
                    "<cache><limit_mb>4096</limit_mb></cache></ter-music-remote>");
    CHECK(remote_store_load() >= 0, "加载旧子元素形状的 <cache>");
    CHECK(remote_store_cache_limit_mb() == 4096, "旧子元素形状仍可读（向后兼容）");

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", root);
    if (system(cleanup) != 0) {
        /* 清理失败不影响断言 */
    }
    g_config_dir_stub = NULL;
    remote_store_set_cache_limit_mb(2048);
}

static void test_thread_reclaim(void)
{
    reset_stub();
    add_entry("x.wav", 4, 0);
    int baseline = read_threads();
    for (int i = 0; i < 5; i++) {
        CHECK(remote_cache_start_listing(&g_conn, "/") == 0, "连续列目录启动");
        CHECK(await_status().state == REMOTE_CACHE_DONE, "连续列目录完成");
        RemoteDirEntry *entries = NULL;
        int count = 0, error = 0;
        CHECK(remote_cache_take_listing(&entries, &count, &error) == 1, "取回列目录结果");
        remote_free_entries(entries, count);
    }
    remote_cache_shutdown();
    int after = read_threads();
    CHECK(after <= baseline, "worker 线程被 join 回收（F-20）");
}

int main(void)
{
    char template[] = "/tmp/tm-remote-cache-XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        printf("FAIL cannot create temp dir\n");
        return 1;
    }
    snprintf(g_tmp_root, sizeof(g_tmp_root), "%s", root);
    char cache_home[MAX_PATH_LEN];
    snprintf(cache_home, sizeof(cache_home), "%s/cache", root);
    setenv("XDG_CACHE_HOME", cache_home, 1);
    setenv("HOME", root, 1);

    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.protocol = REMOTE_PROTOCOL_HTTP;
    snprintf(g_conn.host, sizeof(g_conn.host), "cachehost");
    g_conn.port = 8080;
    snprintf(g_conn.base_path, sizeof(g_conn.base_path), "/music");

    remote_cache_init();

    test_session_download_and_hit();
    test_failure_cleanup();
    test_cancel();
    test_size_mismatch_redownload();
    test_no_audio_and_truncation();
    test_cache_budget();
    test_eviction();
    test_safe_component_collision();
    test_repeat_open_first_flag();
    test_finish_then_immediate_cancel();
    test_oversize_listing_rejected();
    test_store_cache_roundtrip();
    test_thread_reclaim();

    remote_cache_shutdown();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_remote_cache\n");
        return 0;
    }
    printf("FAIL test_remote_cache\n");
    return 1;
}
