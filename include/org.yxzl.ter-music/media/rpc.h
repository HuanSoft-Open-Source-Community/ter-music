/**
 * @file rpc.h
 * @brief 核心 RPC 接口面的共享契约（M2）
 *
 * 核心（TUI 或 daemon 主实例）在 /org/mpris/MediaPlayer2 上除 MPRIS 外还发布
 * org.yxzl.ter_music.* 一组接口。本头文件集中定义这些接口共用的版本号、
 * 载荷上限、分页参数与错误名，避免各 rpc_*.c 各写一份常量。
 *
 * 约定（详见 docs/API_DBUS_en_US.md）：
 *  - 复杂载荷一律 JSON 字符串，用 util/json.c 的追加式写入器生成；
 *  - 单响应硬上限 RPC_PAYLOAD_MAX，超出返回 Error.TooLarge，绝不截断；
 *  - 分页默认 RPC_PAGE_DEFAULT 行、单次最多 RPC_PAGE_MAX 行；
 *  - 方法必须在媒体循环内非阻塞有界：目录扫描、远程列目录等走
 *    media/rpc_job.c 的后台任务，网络/磁盘结果经信号通知前端。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef MEDIA_RPC_H
#define MEDIA_RPC_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "info/info.h"
#include "remote/remote.h"

#ifdef HAVE_DBUS
#include <dbus/dbus.h>
#endif

/* ── 版本与握手 ─────────────────────────────────────────────────────
 * 1 = M2 之前的接口面（Info/Control/Lyrics 第一版，Info JSON 里 schema=1）；
 * 2 = 本里程碑定义的接口面：Info 增加 core 对象，新增 Playlist/Queue/
 *     Library/Favorites/History/DirHistory/Config/Remote 与增量信号。
 * 前端在接入时校验 core.api_version >= 2，不兼容则提示升级并退出。 */
#define TER_MUSIC_API_VERSION 2

/* 单响应硬上限（256 KB）：任何 JSON 回复超过它都返回 Error.TooLarge */
#define RPC_PAYLOAD_MAX 262144

/* 分页：未指定 count 时用默认值；超过上限回 Error.InvalidArgs */
#define RPC_PAGE_DEFAULT 200
#define RPC_PAGE_MAX 1000

/* 前端注册表（Control.Attach/Ping/FrontendInfo） */
#define RPC_FRONTEND_MAX 8
#define RPC_PING_INTERVAL_MS 2000
#define RPC_FRONTEND_TIMEOUT_MS 6000

/* 可视化帧：最多 20 Hz，且仅在采样修订号变化时发送 */
#define RPC_VISUALIZER_INTERVAL_MS 50

/* 远程目录列举的单页上限（受 RPC_PAGE_MAX 约束） */
#define RPC_REMOTE_ENTRY_NAME_MAX 256

/* ── 错误名 ─────────────────────────────────────────────────────── */
#define RPC_ERROR_INVALID_ARGS "org.yxzl.ter_music.Error.InvalidArgs"
#define RPC_ERROR_OUT_OF_RANGE "org.yxzl.ter_music.Error.OutOfRange"
#define RPC_ERROR_TOO_LARGE    "org.yxzl.ter_music.Error.TooLarge"
#define RPC_ERROR_BUSY         "org.yxzl.ter_music.Error.Busy"
#define RPC_ERROR_UNSUPPORTED  "org.yxzl.ter_music.Error.Unsupported"
#define RPC_ERROR_FAILED       "org.yxzl.ter_music.Error.Failed"

/* ── 接口名 ─────────────────────────────────────────────────────── */
#define RPC_IFACE_INFO        "org.yxzl.ter_music.Info"
#define RPC_IFACE_CONTROL     "org.yxzl.ter_music.Control"
#define RPC_IFACE_LYRICS      "org.yxzl.ter_music.Lyrics"
#define RPC_IFACE_PLAYLIST    "org.yxzl.ter_music.Playlist"
#define RPC_IFACE_QUEUE       "org.yxzl.ter_music.Queue"
#define RPC_IFACE_LIBRARY     "org.yxzl.ter_music.Library"
#define RPC_IFACE_FAVORITES   "org.yxzl.ter_music.Favorites"
#define RPC_IFACE_HISTORY     "org.yxzl.ter_music.History"
#define RPC_IFACE_DIRHISTORY  "org.yxzl.ter_music.DirHistory"
#define RPC_IFACE_CONFIG      "org.yxzl.ter_music.Config"
#define RPC_IFACE_REMOTE      "org.yxzl.ter_music.Remote"

/* ── 分页参数钳制 ───────────────────────────────────────────────────
 * 把调用方给出的 offset/count 收敛到合法区间。
 * @return 0 成功；-1 = count 显式超过 RPC_PAGE_MAX（应回 InvalidArgs） */
int rpc_page_clamp(long long offset, long long count, int *offset_out, int *count_out);

/* ── 方法清单（握手） ───────────────────────────────────────────────
 * 前端用 Info.GetInfo 的 core.methods 判断核心是否具备它要用的方法。
 * 实现见 media/rpc_common.c；表与实际自省 XML 的一致性由
 * scripts/test/dbus-rpc-check.sh 核对。 */
int rpc_method_count(void);
const char *rpc_method_at(int index);
const char *rpc_methods_json(void);
const char *rpc_core_json(void);   /* Info.GetInfo 的 core 字段 */

#ifdef HAVE_DBUS

/* ── 核心侧播放快照 ─────────────────────────────────────────────────
 * MPRIS 属性变更检测与 Info 信号都由它派生，故定义在此共用。 */
#define RPC_ART_URL_MAX (MAX_PATH_LEN * 3 + 16)

typedef struct {
    int valid;
    int current_index;
    int playlist_total;
    PlayState play_state;
    int loop_mode;  /* PlayMode value */
    int volume_percent;
    int can_seek;
    int64_t position_us;
    int64_t length_us;
    char track_id[96];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    char art_url[RPC_ART_URL_MAX];
} RpcPlaybackSnapshot;

/* ── 会话访问器（定义于 session.c） ─────────────────────────────── */
DBusConnection *rpc_session_connection(void);  /* 未激活时为 NULL */
int rpc_session_active(void);
int rpc_session_has_primary_name(void);
const char *rpc_session_bus_name(void);

/* ── 共享发送 / 错误 / 回复（定义于 rpc_common.c） ───────────────── */
void rpc_send(DBusMessage *message);           /* 发送并 unref；未激活时仅 unref */
DBusMessage *rpc_error(DBusMessage *message, const char *error_name, const char *text);
DBusMessage *rpc_reply_string(DBusMessage *message, const char *value);
DBusMessage *rpc_reply_bool(DBusMessage *message, int ok);
DBusMessage *rpc_reply_int(DBusMessage *message, int value);

/* ── 共享引擎动作（MPRIS 与 Control 复用） ──────────────────────── */
int rpc_track_available(void);
int rpc_action_play(void);
int rpc_action_play_pause(void);
int rpc_action_play_selected(void);
int rpc_action_seek_to_us(int64_t position_us);
int rpc_action_seek_by_us(int64_t delta_us);
int rpc_action_set_volume_percent(int percent);
int rpc_action_set_speed(double rate);
int rpc_action_play_index(int index);
int rpc_action_open_path(const char *path, int autoplay);

/* ── 共享取值 ───────────────────────────────────────────────────── */
const char *rpc_playback_status_name(PlayState state);      /* MPRIS 状态名 */
void rpc_capture_snapshot(RpcPlaybackSnapshot *snapshot);
InfoInstance rpc_instance_info(void);

/* ── 接口处理器与同步钩子 ───────────────────────────────────────── */
DBusMessage *rpc_lyrics_handle(DBusMessage *message);
DBusMessage *rpc_playlist_handle(DBusMessage *message);
DBusMessage *rpc_queue_handle(DBusMessage *message);
DBusMessage *rpc_info_handle(DBusMessage *message);
DBusMessage *rpc_control_handle(DBusMessage *message);

/* 自省片段：各接口提供自己的 <interface> 段，session.c 负责拼装 */
const char *rpc_lyrics_introspection(void);
const char *rpc_playlist_introspection(void);
const char *rpc_queue_introspection(void);
const char *rpc_info_introspection(void);
const char *rpc_control_introspection(void);

/* ── 后台任务（media/rpc_job.c） ────────────────────────────────────
 * 阻塞 IO（目录扫描、远程列举/连接）走单工作线程；结果由媒体循环在
 * rpc_job_tick() 内单点换入，随后广播信号。 */
typedef enum {
    RPC_JOB_NONE = 0,
    RPC_JOB_PLAYLIST_LOAD,     /* path = 目录/文件路径 */
    RPC_JOB_PLAYLIST_APPEND,   /* path = 目录/文件路径 */
    RPC_JOB_REMOTE_LIST,       /* subpath = 远程子路径（连接配置先 set） */
    RPC_JOB_REMOTE_CONNECT     /* subpath = 远程子路径（连接配置先 set） */
} RpcJobKind;

typedef enum {
    RPC_JOB_IDLE = 0,
    RPC_JOB_RUNNING,
    RPC_JOB_FAILED
} RpcJobState;

int  rpc_job_start(RpcJobKind kind, const char *path, const char *subpath, int autoplay);
void rpc_job_set_connection(const RemoteConnectionConfig *connection);
void rpc_job_tick(void);            /* 媒体循环内调用 */
void rpc_job_cancel(void);
int  rpc_job_state(void);
int  rpc_job_kind(void);
int  rpc_job_progress(void);
int  rpc_job_total(void);
const char *rpc_job_error(void);
const char *rpc_job_path(void);
/* 远程列举结果（所有权归 rpc_job.c，消费方只读） */
RemoteDirEntry *rpc_job_entries(int *count, int *error, const char **path);

/* 前端可见的状态/错误广播（Control.StatusMessage / Control.Error） */
void rpc_control_tick(void);       /* 清理超时未心跳的前端登记 */
int rpc_frontend_count(void);      /* 当前在线前端数 */
void rpc_control_emit_status(unsigned long long seq, const char *message);
void rpc_control_emit_error(const char *source, const char *name, const char *message);

/* 播放列表 / 队列的变更广播与状态 */
void rpc_playlist_emit_changed(const char *reason);
const char *rpc_playlist_filter(void);
void rpc_queue_emit_changed(void);
unsigned long long rpc_queue_revision(void);

void rpc_lyrics_reset(void);
const char *rpc_lyrics_sync(void);
void rpc_info_reset(void);
void rpc_info_sync(void);
void rpc_info_emit_progress(const RpcPlaybackSnapshot *snapshot);

#endif /* HAVE_DBUS */

#endif /* MEDIA_RPC_H */
