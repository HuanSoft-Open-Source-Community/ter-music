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

#endif /* MEDIA_RPC_H */
