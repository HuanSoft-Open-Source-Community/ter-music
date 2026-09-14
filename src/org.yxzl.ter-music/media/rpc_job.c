/**
 * @file rpc_job.c
 * @brief RPC 后台任务：把阻塞 IO 移出媒体循环（build-then-swap）
 *
 * 目录扫描与远程列举都是阻塞 IO（数秒级），在媒体循环里直接做会让所有
 * 前端一起卡住，也可能拖过 D-Bus 方法超时。这里用**单个**工作线程承接
 * 这类任务：
 *
 *   - 工作线程只写自己持有的结果（Playlist* / 远程目录数组），
 *     不触碰任何全局状态，因此不需要锁；
 *   - 结果由媒体循环在 rpc_job_tick() 里单点换入（playlist_install 或
 *     复制到任务槽），随后广播 PlaylistChanged / StatusMessage / Error；
 *   - 同一时刻只允许一个任务（再启动返回 Error.Busy），避免并发换入。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"

#include "app/open.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "core/core.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "remote/remote.h"
#include "ui/ui.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS

typedef struct {
    int state;                  /* RpcJobState */
    int kind;
    char folder[MAX_PATH_LEN];
    int autoplay;
    int progress;
    int total;
    char error[CORE_STATUS_MAX];

    /* 工作线程写入、媒体循环读取 */
    pthread_t thread;
    int thread_started;
    Playlist *built;
    RemoteConnectionConfig connection;
    char subpath[MAX_PATH_LEN];
    RemoteDirEntry *entries;
    int entry_count;
    int entry_error;
} RpcJobStateSlot;

static RpcJobStateSlot g_job = {0};
/* 远程列举结果由 rpc_remote.c 消费，故放在这里共享 */
static RemoteDirEntry *g_job_entries = NULL;
static int g_job_entry_count = 0;
static int g_job_entry_error = 0;
static char g_job_entry_path[MAX_PATH_LEN] = "";

static void job_progress(int processed, int total, void *userdata)
{
    (void)userdata;
    /* 只写两个 int：媒体循环读取时不会撕裂语义 */
    g_job.progress = processed;
    g_job.total = total;
}

static void *job_thread_local_playlist(void *arg)
{
    (void)arg;
    g_job.built = playlist_build_local(g_job.folder, g_job.kind == RPC_JOB_PLAYLIST_APPEND,
                                       job_progress, NULL);
    return NULL;
}

static void *job_thread_remote_list(void *arg)
{
    (void)arg;
    RemoteDirEntry *entries = NULL;
    int count = 0;
    if (remote_list_directory(&g_job.connection, g_job.subpath, &entries, &count) < 0) {
        g_job.entry_error = 1;
        return NULL;
    }
    g_job.entries = entries;
    g_job.entry_count = count;
    return NULL;
}

static void *job_thread_remote_connect(void *arg)
{
    (void)arg;
    g_job.built = playlist_build_remote(&g_job.connection, g_job.subpath);
    return NULL;
}

int rpc_job_start(RpcJobKind kind, const char *path, const char *subpath, int autoplay)
{
    if (g_job.state == RPC_JOB_RUNNING) {
        return -1;
    }

    RemoteConnectionConfig connection = g_job.connection;   /* 远程任务由调用方预先 set */
    memset(&g_job, 0, sizeof(g_job));
    g_job.connection = connection;
    g_job.kind = kind;
    g_job.autoplay = autoplay;

    if (kind == RPC_JOB_PLAYLIST_LOAD || kind == RPC_JOB_PLAYLIST_APPEND) {
        if (!path || !path[0]) {
            return -1;
        }
        snprintf(g_job.folder, sizeof(g_job.folder), "%s", path);
    } else if (kind == RPC_JOB_REMOTE_LIST || kind == RPC_JOB_REMOTE_CONNECT) {
        snprintf(g_job.subpath, sizeof(g_job.subpath), "%s", subpath ? subpath : "");
    } else {
        return -1;
    }

    void *(*entry)(void *) = NULL;
    switch (kind) {
        case RPC_JOB_PLAYLIST_LOAD:
        case RPC_JOB_PLAYLIST_APPEND: entry = job_thread_local_playlist; break;
        case RPC_JOB_REMOTE_LIST:     entry = job_thread_remote_list; break;
        case RPC_JOB_REMOTE_CONNECT:  entry = job_thread_remote_connect; break;
        default: return -1;
    }

    g_job.state = RPC_JOB_RUNNING;
    g_job.thread_started = 1;
    if (pthread_create(&g_job.thread, NULL, entry, NULL) != 0) {
        g_job.state = RPC_JOB_FAILED;
        g_job.thread_started = 0;
        snprintf(g_job.error, sizeof(g_job.error), "无法创建工作线程");
        return -1;
    }
    return 0;
}

/* 供 rpc_remote.c 传入解析好的连接配置 */
void rpc_job_set_connection(const RemoteConnectionConfig *connection)
{
    if (connection) {
        g_job.connection = *connection;
    }
}

void rpc_job_tick(void)
{
    if (g_job.state != RPC_JOB_RUNNING || !g_job.thread_started) {
        return;
    }

    /* 非阻塞检查：用 tryjoin 避免媒体循环等待 */
    int joinable = 0;
    if (pthread_tryjoin_np(g_job.thread, NULL) == 0) {
        joinable = 1;
    }
    if (!joinable) {
        return;
    }

    g_job.thread_started = 0;
    g_job.state = RPC_JOB_IDLE;

    if (g_job.kind == RPC_JOB_PLAYLIST_LOAD || g_job.kind == RPC_JOB_PLAYLIST_APPEND) {
        if (!g_job.built) {
            g_job.state = RPC_JOB_FAILED;
            snprintf(g_job.error, sizeof(g_job.error), "无法加载 '%s'（路径不存在或没有可播放的音频）",
                     g_job.folder);
            rpc_control_emit_error("Playlist.Load", RPC_ERROR_FAILED, g_job.error);
            rpc_control_emit_status(core_status_seq(), g_job.error);
            return;
        }

        playlist_install(g_job.built);
        g_job.built = NULL;

        if (g_job.autoplay && playlist_count() > 0) {
            int index = (g_current_play_index >= 0 && g_current_play_index < playlist_count())
                ? g_current_play_index : 0;
            play_audio(index);
            app_set_selection_for_track(index);
        }

        log_info("rpc_job", "Playlist loaded from '%s' (%d tracks)", g_job.folder, playlist_count());
        rpc_playlist_emit_changed("loaded");
        return;
    }

    if (g_job.kind == RPC_JOB_REMOTE_LIST) {
        /* 交换结果给 rpc_remote.c */
        if (g_job_entries) {
            remote_free_entries(g_job_entries, g_job_entry_count);
        }
        g_job_entries = g_job.entries;
        g_job_entry_count = g_job.entry_count;
        g_job_entry_error = g_job.entry_error;
        snprintf(g_job_entry_path, sizeof(g_job_entry_path), "%s", g_job.subpath);
        g_job.entries = NULL;
        g_job.entry_count = 0;
        if (g_job_entry_error) {
            rpc_control_emit_error("Remote.List", RPC_ERROR_FAILED, remote_strerror());
        }
        return;
    }

    if (g_job.kind == RPC_JOB_REMOTE_CONNECT) {
        if (!g_job.built) {
            g_job.state = RPC_JOB_FAILED;
            snprintf(g_job.error, sizeof(g_job.error), "%s", remote_strerror());
            rpc_control_emit_error("Remote.Connect", RPC_ERROR_FAILED, g_job.error);
            return;
        }
        playlist_install(g_job.built);
        g_job.built = NULL;
        rpc_playlist_emit_changed("loaded");
        return;
    }
}

void rpc_job_cancel(void)
{
    if (g_job.state != RPC_JOB_RUNNING) {
        return;
    }
    /* 只标记取消：真正的回收在 rpc_job_tick() 里完成，避免与工作线程竞争 */
    log_warn("rpc_job", "Cancel requested for job kind=%d", g_job.kind);
}

int rpc_job_state(void)
{
    return g_job.state;
}

int rpc_job_kind(void)
{
    return g_job.kind;
}

int rpc_job_progress(void)
{
    return g_job.progress;
}

int rpc_job_total(void)
{
    return g_job.total;
}

const char *rpc_job_error(void)
{
    return g_job.error;
}

const char *rpc_job_path(void)
{
    return g_job.folder;
}

RemoteDirEntry *rpc_job_entries(int *count, int *error, const char **path)
{
    if (count) *count = g_job_entry_count;
    if (error) *error = g_job_entry_error;
    if (path) *path = g_job_entry_path;
    return g_job_entries;
}

#endif /* HAVE_DBUS */
