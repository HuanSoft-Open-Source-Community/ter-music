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
 *  - 方法必须在媒体循环内非阻塞有界：目录扫描走 media/rpc_job.c 的
 *    后台任务，磁盘结果经信号通知前端；
 *  - 核心只接受**本地文件路径**：远程音乐源由前端负责（前端下载到本地
 *    缓存后再把路径交给核心），故核心不认识任何远程概念。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef MEDIA_RPC_H
#define MEDIA_RPC_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "info/info.h"

#ifdef HAVE_DBUS
#include <dbus/dbus.h>
#endif

/* ── 版本与握手 ─────────────────────────────────────────────────────
 * 1 = M2 之前的接口面（Info/Control/Lyrics 第一版，Info JSON 里 schema=1）；
 * 2 = M2 定义的接口面：Info 增加 core 对象，新增 Playlist/Queue/
 *     Library/Favorites/History/DirHistory/Config 与增量信号。
 * 3 = 移除 Remote 接口（远程音乐源改为前端功能），Playlist.Load/Append、
 *     Control.OpenPath 与 MPRIS OpenUri 只接受本地路径，Info 快照不再带
 *     远程来源标记字段；Queue 从“曲目下标”改为**本地路径**语义。
 * 4 = 内容归前端：Playlist/Library/Favorites/History/DirHistory 与后台扫描
 *     任务整体撤下，Control.OpenPath/PlayIndex 与 Queue.Rebuild 一并撤下，
 *     队列内容只能经 Queue.Set/Append/InsertAfter 下发。
 * 前端在接入时校验 core.api_version >= 4，不兼容则提示“核心过旧”并退出。 */
#define TER_MUSIC_API_VERSION 4

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
#define RPC_IFACE_QUEUE       "org.yxzl.ter_music.Queue"
#define RPC_IFACE_CONFIG      "org.yxzl.ter_music.Config"

/* 已撤下的接口（api_version 4）
 * ---------------------------------
 * 曲库 / 收藏 / 历史 / 目录历史 / 播放列表**内容**归前端：前端自己扫描、
 * 自己持有 SQLite、自己渲染；核心只接受前端下发的本地路径队列。因此这些
 * 接口不再发布，旧前端调用会拿到 org.freedesktop.DBus.Error.UnknownMethod。 */

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
int rpc_action_play_position(int position);

/* ── 共享取值 ───────────────────────────────────────────────────── */
const char *rpc_playback_status_name(PlayState state);      /* MPRIS 状态名 */
void rpc_capture_snapshot(RpcPlaybackSnapshot *snapshot);
InfoInstance rpc_instance_info(void);

/* ── 接口处理器与同步钩子 ───────────────────────────────────────── */
DBusMessage *rpc_lyrics_handle(DBusMessage *message);
DBusMessage *rpc_queue_handle(DBusMessage *message);
/* Library/Favorites/History/DirHistory 共用一个入口（按接口名分发） */
DBusMessage *rpc_config_handle(DBusMessage *message);
DBusMessage *rpc_info_handle(DBusMessage *message);
DBusMessage *rpc_control_handle(DBusMessage *message);

/* 自省片段：各接口提供自己的 <interface> 段，session.c 负责拼装 */
const char *rpc_lyrics_introspection(void);
const char *rpc_queue_introspection(void);
const char *rpc_config_introspection(void);
const char *rpc_info_introspection(void);
const char *rpc_control_introspection(void);

/* ── 后台任务 ─────────────────────────────────────────────────────
 * 旧实现把“目录扫描”作为后台任务发布给前端（media/rpc_job.c）：现在扫描属
 * 前端职责，核心没有阻塞 IO 需要搬到后台，故整套任务状态机一并撤下。 */

/* 前端可见的状态/错误广播（Control.StatusMessage / Control.Error） */
void rpc_control_tick(void);       /* 清理超时未心跳的前端登记 */
int rpc_frontend_count(void);      /* 当前在线前端数 */
int rpc_frontend_ever_attached(void);   /* 是否曾有前端接入（看门狗用） */
void rpc_frontend_reset_registry(void); /* 关闭 D-Bus 时清空注册表 */
void rpc_control_emit_status(unsigned long long seq, const char *message);
void rpc_control_emit_error(const char *source, const char *name, const char *message);

/* 队列变更广播与状态 */
void rpc_queue_emit_changed(void);
unsigned long long rpc_queue_revision(void);

/* 配置变更广播（内容侧已撤下，配置仍属后端） */
void rpc_config_emit_changed(const char *patch_json);

/* 歌词 / 信息快照的同步与广播 */
void rpc_lyrics_reset(void);
const char *rpc_lyrics_sync(void);
void rpc_info_reset(void);
void rpc_info_sync(void);
void rpc_info_emit_progress(const RpcPlaybackSnapshot *snapshot);

#endif /* HAVE_DBUS */

#endif /* MEDIA_RPC_H */
