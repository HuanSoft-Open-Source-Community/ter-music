/**
 * @file remote_cache.h
 * @brief 前端远程曲目下载器：把远程目录搬到本地缓存，再把本地路径交给核心
 *
 * 核心只播放本地文件，因此“打开远程目录”在前端拆成两步：
 *   1. 后台线程列目录并逐曲下载到本地缓存（不阻塞 UI）；
 *   2. UI 线程在每帧调用 remote_cache_take_ready() 取走已下载完成的本地
 *      路径，经 player 门面交给核心播放（核心只看到普通本地文件）。
 *
 * 缓存布局（镜像远端目录，使核心的本地目录扫描天然还原远程结构）：
 *   $XDG_CACHE_HOME/ter-music/remote/<protocol>_<host>_<port>/<远端子路径>/<文件>
 * 同名 `.lrc` 一并缓存，核心按本地同名歌词加载。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef REMOTE_CACHE_H
#define REMOTE_CACHE_H

#include <stddef.h>

#include "remote/remote.h"

/* 单次会话最多下载的曲目数（避免误点巨型目录后无界下载） */
#define REMOTE_CACHE_SESSION_MAX 200

typedef enum {
    REMOTE_CACHE_IDLE = 0,
    REMOTE_CACHE_LISTING,
    REMOTE_CACHE_DOWNLOADING,
    REMOTE_CACHE_DONE,
    REMOTE_CACHE_FAILED
} RemoteCacheState;

typedef struct {
    int state;                  /* RemoteCacheState */
    int done;                   /* 已交付给核心的曲目数 */
    int total;                  /* 本次会话曲目数（列目录后确定） */
    int failed;                 /* 下载失败的曲目数 */
    int cancelled;              /* 是否被取消 */
    char message[256];          /* 最近状态/错误（UI 直接显示） */
} RemoteCacheStatus;

void remote_cache_init(void);
void remote_cache_shutdown(void);

/* 列目录（非递归，后台线程）。结果由 UI 线程经 remote_cache_take_listing()
 * 取走。@return 0 已接受；-1 = 已有任务在进行 */
int remote_cache_start_listing(const RemoteConnectionConfig *conn, const char *subpath);

/* 取走列目录结果（UI 线程）。
 * @param out_entries 收到 malloc 的条目数组（用 remote_free_entries 释放），
 *                    无结果时置 NULL
 * @param out_error   收到 1 表示列目录失败（message 里有原因）
 * @return 1 = 有结果可取；0 = 还没有结果 */
int remote_cache_take_listing(RemoteDirEntry **out_entries, int *out_count, int *out_error);

/* 开始一次“打开远程目录”会话（列目录 + 逐曲下载）。
 * @return 0 已接受；-1 = 已有任务在进行或参数无效 */
int remote_cache_start_session(const RemoteConnectionConfig *conn, const char *subpath,
                               int autoplay);

/* 取走一首已下载完成的本地路径（UI 线程）。
 * @param is_first 收到 1 表示这是本次会话的第一首（用于决定是否自动起播）
 * @param is_last  收到 1 表示这是本次会话的最后一首
 * @return 1 = 取到；0 = 暂时没有 */
int remote_cache_take_ready(char *local_path, size_t size, int *is_first, int *is_last);

/* 取走一次错误（UI 线程）；@return 1 = 取到 */
int remote_cache_take_error(char *out, size_t size);

/* 本次会话是否要求自动起播（发起时传入） */
int remote_cache_session_autoplay(void);

void remote_cache_cancel(void);
const RemoteCacheStatus *remote_cache_status(void);

/* ── 缓存维护（UI 线程调用） ──────────────────────────────────────── */
/* 缓存根目录（静态缓冲，始终可用） */
const char *remote_cache_dir(void);
long long remote_cache_usage_bytes(void);
/* 保护集（正在播放/队列内/收藏/历史的本地路径）内的文件不会被删除；
 * protected_paths 可为 NULL。@return 删除的文件数 */
int remote_cache_evict(const char *const *protected_paths, int protected_count);
int remote_cache_purge(const char *const *protected_paths, int protected_count);

#endif /* REMOTE_CACHE_H */
