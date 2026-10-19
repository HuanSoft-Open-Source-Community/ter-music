/**
 * @file cli.h
 * @brief CLI 模式：子命令分发、D-Bus 客户端与后台播放 daemon
 *
 * 进程模型：
 *  - TUI（无子命令）或 daemon（`daemon foreground`）作为**主实例**持有
 *    org.mpris.MediaPlayer2.ter_music 总线名，并对外提供
 *    Info/Control/Lyrics/Queue/Config 接口（api_version=4）；
 *  - 其余子命令（play/pause/show/queue/config/lyrics/track/cover/...）是
 *    **纯 D-Bus 客户端**，不初始化 ffmpeg / 音频 / ncurses，把命令发给当前
 *    主实例；play 例外：它先在本进程扫描/装载内容（内容归前端），再把**本地
 *    路径队列分块下发**（单块 ≤ BQ_SET_MAX=500，首块 Queue.Set、余块
 *    Queue.Append）交给核心执行。`play --foreground` 则不 fork 后台 daemon，
 *    在本进程以前台核心方式装载并播放。
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

/* `keys list|check [--json]`：本地键位审计，不要求运行实例、不走 D-Bus */
int cli_cmd_keys(int argc, char **argv);

/* ── daemon ─────────────────────────────────────────────────────── */

/* 无界面前台运行。force=1 时即使已有主实例也继续（仅作次要实例）。
 * no_autoplay=1：核心本就只就绪等待前端下发队列、不装载任何内容，故此参数
 * 现在只作语义占位（供 D-Bus 激活调用方沿用），不影响实际行为。
 * open_path 不再被核心扫描：非空时本函数会给出“核心不扫描目录”的提示。 */
int daemon_run_foreground(const char *open_path, int debug, int force,
                          int no_autoplay);

/* 前台核心 + 调用方装配好的**纯数据**分块队列：chunks 为
 * playlist_queue_foreach_chunk 产出的 {"entries":[…]} 载荷数组（单块 ≤ BQ_SET_MAX），
 * 本函数只用后端队列 bq_set_json(首块)/bq_append_json(余块) 装载并按
 * 播放模式重排、起播 start_index，然后进入前台主循环。不 fork 后台 daemon、
 * 不经 D-Bus 下发；daemon 侧不引用任何内容模块（core-purity 边界）。
 * 供 `play --foreground` 使用：start_index<0 表示从队首播放，play_mode<0 表示沿用当前模式。
 * @return 进程退出码（CLI_EXIT_*） */
int daemon_run_foreground_with_queue(const char *const *chunks, int nchunks,
                                     int start_index, int play_mode, int debug);

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

/* ── 实例枚举（主实例 + 次要实例） ───────────────────────────────
 * 次要实例（`daemon start --force`，总线名 `<主名>.instance<pid>`）不接收
 * 普通 CLI 命令，因此必须能被列出来，否则用户只能看见“后台有两个 ter-music
 * 在跑”却无从下手。两个函数都只看会话总线，不改变任何实例状态。 */
typedef struct {
    char bus[128];      /* 完整总线名 */
    int pid;            /* 实例进程 pid（取不到为 0） */
    char mode[32];      /* "daemon" / "tui" / "unknown" */
    int primary;        /* 1 = 持有主名 */
} CliInstanceInfo;

/* 枚举 base（NULL → 主名）下的全部实例：主名本身 + `<base>.instance*`。
 * 主实例排在最前，其余按 pid 升序。
 * @return 写入 out 的条数（可能被 out_cap 截断）；-1 = 连不上会话总线 */
int cli_client_list_instances(const char *base, CliInstanceInfo *out, int out_cap);

/* 传输控制：method ∈ Play/Pause/PlayPause/Stop/Next/Previous */
int cli_client_transport(const char *bus, const char *method);
int cli_client_seek(const char *bus, const char *argument);
int cli_client_volume(const char *bus, const char *argument);
int cli_client_speed(const char *bus, const char *argument);
int cli_client_mode(const char *bus, const char *argument);

/* show：options 为 Info.GetDisplay 的 k=v 覆盖串（可为空串） */
int cli_client_show(const char *bus, const char *options, int want_json,
                    int watch_interval_ms);

/* play：path 可为 NULL（无参=继续播放当前队列；仅当显式给 index≥0 时
 * 装载配置默认音乐目录并 PlayAt(index)）。index < 0 表示不指定曲目序号；
 * mode < 0 表示不改播放模式。装载后队列以 ≤BQ_SET_MAX 分块下发（Set+Append）。 */
int cli_client_play(const char *bus, const char *path, int index, int mode);

/* ── 握手（api_version 校验） ─────────────────────────────────────
 * 经 Info.GetInfo 取 core.api_version，< TER_MUSIC_API_VERSION 时打印“核心过旧”
 * 并返回 CLI_EXIT_REFUSED。同进程内按目标总线名缓存，避免重复往返。
 * @return CLI_EXIT_OK（兼容）/CLI_EXIT_REFUSED（过旧）/CLI_EXIT_DBUS/NO_INSTANCE */
int cli_client_check_api(const char *bus);

/* ── queue 子命令（全部复用核心既有 Queue.* 方法，无新增接口） ────── */
int cli_client_queue_list(const char *bus, int offset, int count);
int cli_client_queue_clear(const char *bus);
int cli_client_queue_shuffle(const char *bus);
int cli_client_queue_remove(const char *bus, int position);
int cli_client_queue_append(const char *bus, char **paths, int n);
int cli_client_queue_insert(const char *bus, int position, const char *path);

/* ── config 子命令（复用核心既有 Config.* 方法） ─────────────────── */
int cli_client_config_get(const char *bus, const char *key);
int cli_client_config_set(const char *bus, const char *key, const char *value);
int cli_client_config_reload(const char *bus);
int cli_client_config_reset(const char *bus);

/* ── lyrics / track / cover 子命令 ──────────────────────────────── */
/* lines <= 0：当前歌词行上下文（Info.GetLyricsLines）；
 * lines > 0：经 Lyrics.GetDocument 分页拉全量并带时间戳输出。 */
int cli_client_lyrics(const char *bus, int lines);
int cli_client_track(const char *bus);
/* width/height <= 0 沿用配置；charset 可为 NULL（沿用配置）。 */
int cli_client_cover(const char *bus, int width, int height, const char *charset);

/* ── 前端内容装载（play 子命令用） ───────────────────────────────
 * 扫描/元数据/播放列表都归前端：CLI 进程自己装载内容，再把路径队列下发给核心。 */
/* 装载本地内容；成功时把“应当开始播放的曲目下标”写入 out_track_index。
 * 单文件：用 app_open_path 依**扩展后路径**（~ / 相对路径已展开并定位到所在
 * 目录）算好的物理下标，避免再用原始路径反查落空而误播首曲。
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
