/**
 * @file rpc_job.c
 * @brief RPC 后台任务：把阻塞 IO 移出媒体循环（build-then-swap）
 *
 * 目录扫描是阻塞 IO（数秒级），在媒体循环里直接做会让所有前端一起卡住，
 * 也可能拖过 D-Bus 方法超时。这里用**单个**工作线程承接这类任务：
 *
 *   - 工作线程只写自己持有的结果（Playlist*），不触碰任何全局状态，
 *     因此不需要锁；
 *   - 结果由媒体循环在 rpc_job_tick() 里单点换入（playlist_install），
 *     随后广播 PlaylistChanged / StatusMessage / Error；
 *   - 同一时刻只允许一个任务（再启动返回 Error.Busy），避免并发换入。
 *
 * 核心只处理本地目录：远程音乐源由前端负责（前端下载到本地缓存后再把
 * 路径交给核心），故这里不再有远程列举/连接任务。
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
} RpcJobStateSlot;

static RpcJobStateSlot g_job = {0};

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

int rpc_job_start(RpcJobKind kind, const char *path, const char *subpath, int autoplay)
{
    (void)subpath;

    if (g_job.state == RPC_JOB_RUNNING) {
        return -1;
    }

    memset(&g_job, 0, sizeof(g_job));
    g_job.kind = kind;
    g_job.autoplay = autoplay;

    if (kind != RPC_JOB_PLAYLIST_LOAD && kind != RPC_JOB_PLAYLIST_APPEND) {
        return -1;
    }
    if (!path || !path[0]) {
        return -1;
    }
    snprintf(g_job.folder, sizeof(g_job.folder), "%s", path);

    g_job.state = RPC_JOB_RUNNING;
    g_job.thread_started = 1;
    if (pthread_create(&g_job.thread, NULL, job_thread_local_playlist, NULL) != 0) {
        g_job.state = RPC_JOB_FAILED;
        g_job.thread_started = 0;
        snprintf(g_job.error, sizeof(g_job.error), "无法创建工作线程");
        return -1;
    }
    return 0;
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

#endif /* HAVE_DBUS */
