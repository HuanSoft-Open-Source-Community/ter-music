/**
 * @file cli.h
 * @brief CLI 模式：子命令分发、D-Bus 客户端与后台播放 daemon
 *
 * 进程模型：
 *  - TUI（无子命令）或 daemon（`daemon foreground`）作为**主实例**持有
 *    org.mpris.MediaPlayer2.ter_music 总线名，并对外提供 Info/Control 接口；
 *  - 其余子命令（play/pause/show/...）是**纯 D-Bus 客户端**，不初始化
 *    ffmpeg / 音频 / ncurses，把命令发给当前主实例。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CLI_H
#define CLI_H

#include <stddef.h>

/* ── 退出码 ─────────────────────────────────────────────────────── */
#define CLI_EXIT_OK          0
#define CLI_EXIT_USAGE       1
#define CLI_EXIT_NO_INSTANCE 3
#define CLI_EXIT_DBUS        4
#define CLI_EXIT_REFUSED     5

/* ── 主实例总线名与对象路径 ─────────────────────────────────────── */
#define CLI_PRIMARY_BUS_NAME   "org.mpris.MediaPlayer2.ter_music"
#define CLI_OBJECT_PATH        "/org/mpris/MediaPlayer2"
#define CLI_INFO_INTERFACE     "org.yxzl.ter_music.Info"
#define CLI_CONTROL_INTERFACE  "org.yxzl.ter_music.Control"
#define CLI_QUEUE_INTERFACE    "org.yxzl.ter_music.Queue"

/* 1 = 当前进程运行在无界面 daemon 模式（供 Info.InstanceInfo / MPRIS 判定） */
extern int g_daemon_mode;

/* 1 = 运行在 Linyaps（如意玲珑）容器内。
 * 依据 /run/linglong/container-init 判定；可用 TER_MUSIC_SANDBOX=1/0 强制覆盖。
 * 沙箱内不能使用 fork+setsid 制造脱离进程（离开的进程会随容器回收），
 * 因此 daemon 的启动策略在此环境下改为 D-Bus 激活 / 前台运行。 */
int cli_in_sandbox(void);

/* 前端（TUI）启动时确保播放服务可用：
 *   有核心 → 直接返回 0；
 *   沙箱   → D-Bus 激活后等待上线；
 *   否则   → 后台拉起 daemon（no_autoplay）。
 * attach_only=1 时**不**自动拉起，没有核心即返回 CLI_EXIT_NO_INSTANCE(3)。 */
int cli_ensure_core_for_frontend(const char *bus, int attach_only);

/* argv[1] 是否为 CLI 子命令（用于 main() 前置分发） */
int cli_is_command(const char *arg);

/* 执行 CLI 子命令，返回进程退出码 */
int cli_run(int argc, char **argv);

/* ── daemon ─────────────────────────────────────────────────────── */

/* 无界面前台运行。force=1 时即使已有主实例也继续（仅作次要实例）。
 * no_autoplay=1 时只装载播放列表、不自动开始播放（供 D-Bus 激活使用）。 */
int daemon_run_foreground(const char *open_path, int debug, int force,
                          int no_autoplay);

/* fork + setsid + exec 自身以启动后台 daemon，并等待总线名就绪。
 * pid_text 输出 daemon 进程 PID。 */
int daemon_start_background(const char *open_path, int debug, int force,
                            char *pid_text, size_t pid_text_size);

/* ── D-Bus 客户端（cli_client.c） ───────────────────────────────── */

/* 主实例是否在线（bus 为 NULL 时用主名） */
int cli_client_primary_available(const char *bus);

/* 等待主实例上线；成功返回 CLI_EXIT_OK 并输出 PID */
int cli_client_wait_for_online(const char *bus, int timeout_ms, int *pid_out);

/* 查询实例模式（"daemon" / "tui"）；bus 为 NULL 时用主名 */
int cli_client_instance_mode(const char *bus, char *mode_out, size_t mode_size,
                             int *pid_out);

/* 传输控制：method ∈ Play/Pause/PlayPause/Stop/Next/Previous */
int cli_client_transport(const char *bus, const char *method);
int cli_client_seek(const char *bus, const char *argument);
int cli_client_volume(const char *bus, const char *argument);
int cli_client_speed(const char *bus, const char *argument);
int cli_client_mode(const char *bus, const char *argument);

/* show：options 为 Info.GetDisplay 的 k=v 覆盖串（可为空串） */
int cli_client_show(const char *bus, const char *options, int want_json,
                    int watch_interval_ms);

/* play：path 可为 NULL；index < 0 表示不指定曲目序号 */
int cli_client_play(const char *bus, const char *path, int index, int mode);

/* ── 前端内容装载（play 子命令用） ───────────────────────────────
 * 扫描/元数据/播放列表都归前端：CLI 进程自己装载内容，再把路径队列下发给核心。 */
/* 装载本地内容；成功时把“应当开始播放的曲目下标”写入 out_track_index。
 * @return 0 成功；-1 路径非法/没有可播放音频 */
int cli_client_load_local_content(const char *path, int *out_track_index);
char *cli_client_build_queue_json(void);               /* 需 free()；NULL = 队列为空 */

/* 请求会话总线按需激活主实例（D-Bus activation，用于 Linyaps 等沙箱环境）：
 * 返回 CLI_EXIT_OK（含“已在运行”）/CLI_EXIT_REFUSED（不可激活）/CLI_EXIT_DBUS */
int cli_client_activate_instance(const char *bus);

int cli_client_quit(const char *bus);
int cli_client_reload(const char *bus);

/* 默认输出宽度：$COLUMNS → 80 */
int cli_default_width(void);

#endif /* CLI_H */
