/**
 * @file daemon.c
 * @brief 无界面后台播放进程（daemon）与后台启动逻辑
 *
 * daemon 与 TUI 共用同一套子系统（ffmpeg / 音频后端 / 播放队列 / MPRIS+
 * Info+Control D-Bus 接口），区别仅在于：
 *   - 不调用 init_ncurses()，因此不依赖终端；
 *   - 主循环只保留“状态推进 + D-Bus 派发”，没有输入与渲染；
 *   - SIGHUP 只触发配置重载，不退出进程。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/cli.h"
#include "core/core.h"

#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "info/info.h"
#include "media/rpc.h"
#include "media/session.h"
#include "queue/backend_queue.h"
#include "logger/logger.h"
#include "ui/braille/braille_art.h"
#include "lyrics/lyrics.h"
#include "util/utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 1 = 当前进程为无界面 daemon（Info.InstanceInfo 与 MPRIS 判定使用） */
int g_daemon_mode = 0;

#define DAEMON_TICK_US 20000        /* 20ms：兼顾控制响应与 CPU 占用 */
#define DAEMON_START_TIMEOUT_MS 5000
#define DAEMON_LOG_NAME "daemon.log"

extern volatile sig_atomic_t g_should_exit;

static uint64_t daemon_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ── 看门狗：最后一个前端离开后是否随核心退出 ─────────────────────
 * 默认**否**（关掉 TUI 音乐继续）。开启后要求同时满足：
 *   1) 本进程持有主总线名（不是次要实例）；
 *   2) 曾经有前端 Attach 过（刚起步还没人来连时不自杀）；
 *   3) 前端数已归零；
 *   4) 归零后等待 CORE_EXIT_GRACE_MS 宽限，期间没有前端回来。
 * 宽限从“最后一次见到前端”起算，所以短暂重连不会被误杀。 */
#define CORE_EXIT_GRACE_MS 10000

static int frontend_watchdog_should_exit(int have_primary_name, int frontend_count)
{
    static uint64_t last_seen_ms = 0;
    static int announced = 0;

    if (!g_app_config.core_exit_when_no_frontend) {
        return 0;
    }
    if (!have_primary_name || !rpc_frontend_ever_attached()) {
        return 0;
    }

    uint64_t now = daemon_now_ms();
    if (frontend_count > 0) {
        last_seen_ms = now;
        announced = 0;
        return 0;
    }

    if (last_seen_ms == 0) {
        last_seen_ms = now;
        return 0;
    }
    if (now - last_seen_ms < CORE_EXIT_GRACE_MS) {
        return 0;
    }
    if (!announced) {
        log_info("daemon", "core_exit_when_no_frontend: no front end for %d ms, exiting",
                 (int)(now - last_seen_ms));
        announced = 1;
    }
    return 1;
}

/* daemon 模式下 SIGHUP 仅重载配置，不退出（终端挂断无关） */
static void daemon_sighup_handler(int sig) {
    (void)sig;
    g_config_reload_requested = 1;
}

static int daemon_prepare_logging(int debug) {
    /* TER_MUSIC_FORCE_LOG=1：即使没传 --debug 也把日志落到配置目录，
     * 方便脚本/CI 诊断后台进程（默认关闭，行为不变）。 */
    const char *force = getenv("TER_MUSIC_FORCE_LOG");
    if (!debug && !(force && force[0] == '1')) {
        return 0;
    }

    const char *config_dir = app_config_dir(1);
    if (config_dir && config_dir[0]) {
        /* debug 日志落盘到配置目录（容器内 CWD 可能只读，不能依赖相对路径） */
        setenv("TER_MUSIC_LOG_DIR", config_dir, 1);
    }
    logger_set_enabled(1);
    logger_init();
    log_info("daemon", "=== ter-music %s daemon session started ===", APP_VERSION);
    return 1;
}

static void daemon_shutdown(void) {
    log_info("daemon", "Shutting down daemon");

    /* 先持久化会话并释放 D-Bus 名字，再收尾音频：
     * 播放线程/音频设备的收尾可能较慢（极端情况下阻塞在设备写入，
     * 见 audio.c 的 wait_for_playback_thread_shutdown），
     * 但“实例是否在运行”必须立刻对其他进程可见，否则
     * `ter-music daemon stop/restart` 会一直等到超时。 */
    persist_playback_session_state();
    media_session_shutdown();

    stop_audio();
    wait_for_playback_thread_shutdown();
    /* 后端只做播放：不保存内容列表、不碰临时播放列表（那些归前端）。
     * 队列本身是前端下发的，退出时不再落盘——前端持有内容真相。 */
    bq_shutdown();
    audio_backend_shutdown();
    reset_album_cover_cache();
    info_release_cover_cache();

    log_info("daemon", "daemon exited cleanly");
    logger_shutdown();
}

int daemon_run_foreground(const char *open_path, int debug, int force,
                          int no_autoplay) {
    /* no_autoplay 只影响前端（D-Bus 激活时不自动播放）；后端没有内容可
     * “自动播放”，队列完全由前端下发。 */
    (void)no_autoplay;
    g_daemon_mode = 1;

    /* 无界面进程同样需要 UTF-8 locale：信息块渲染的宽度计算与截断
     * 依赖 mbrtowc/wcwidth，否则会切断多字节字符（D-Bus 会因此 abort）。 */
    ensure_utf8_locale();

    daemon_prepare_logging(debug);
    log_info("daemon", "Starting headless playback daemon (open='%s', force=%d, no_autoplay=%d)",
             open_path ? open_path : "", force, no_autoplay);

    signal(SIGHUP, daemon_sighup_handler);

    /* 后端只做播放：读取配置并应用运行时字段，**不**初始化曲库/收藏/
     * 历史/用户歌单——这些内容归前端（见 check-core-purity.sh 的 B 轴）。 */
    ensure_config_dir_exists();
    core_config_apply();
    i18n_init(g_app_config.ui_language);
    set_volume_percent(g_app_config.volume_percent);

    init_ffmpeg();
    g_active_backend = g_app_config.audio_backend;
    init_audio_device();
    media_session_init();
    bq_init();

    if (!media_session_has_primary_name()) {
        const char *bus = media_session_bus_name();
        if (!force) {
            fprintf(stderr, "错误：已有 ter-music 主实例在运行，无法作为后台播放进程启动。\n");
            fprintf(stderr, "      用 `ter-music play <路径>` 控制现有实例，或先 `ter-music daemon stop`。\n");
            log_warn("daemon", "Primary bus name unavailable (using '%s'), refusing to start", bus);
            daemon_shutdown();
            return CLI_EXIT_REFUSED;
        }
        log_warn("daemon", "Running as secondary instance on bus '%s' (--force)", bus);
        fprintf(stderr, "警告：以次要实例身份运行（总线名 %s），CLI 命令仍会发送给主实例。\n", bus);
    }

    if (open_path && open_path[0]) {
        /* 后端不再扫描目录：`--open <dir>` 是“前端负责扫描”的旧语义，
         * 继续接受它只会让人以为核心会自己加载曲目。 */
        fprintf(stderr, "错误：核心不再扫描目录。\n");
        fprintf(stderr, "      请在前端（TUI 或 `ter-music play <路径>`）扫描后，经 D-Bus Queue.Set 下发队列。\n");
        log_warn("daemon", "Refusing --open '%s': the core does not scan directories any more", open_path);
    } else {
        /* 队列一律由前端下发（Queue.Set/Append）：核心不扫描目录，也不自行
         * 从 queue.txt 恢复内容——那份文件归前端所有。 */
        log_info("daemon", "Waiting for a front end to deliver a queue");
    }

    log_info("daemon", "Entering main loop (queue=%d)", bq_count());

    while (!g_should_exit) {
        /* 与 TUI 共用同一套核心工作（回收线程/挂起动作/歌词推进/D-Bus tick/配置重载） */
        core_tick();
        /* 会话总线没了 = 再也没有前端能联系到这个核心：继续播放只会变成一个
         * 谁也停不掉、谁也控制不了的孤儿播放进程（占着音频设备）。退出是唯一
         * 可控的选择；前端重连时会自行拉起新核心并补推队列。
         * 从未拿到总线的进程（无会话总线的托管场景）不会被置位，行为不变。 */
        if (media_session_lost()) {
            log_warn("daemon", "Session bus is gone; no front end can reach this core, exiting");
            g_should_exit = 1;
        }
        if (frontend_watchdog_should_exit(media_session_has_primary_name(),
                                          rpc_frontend_count())) {
            g_should_exit = 1;
        }
        usleep(DAEMON_TICK_US);
    }

    daemon_shutdown();
    return CLI_EXIT_OK;
}

/* ── 后台启动 ───────────────────────────────────────────────────── */

static void daemon_log_tail(const char *log_path) {
    FILE *file = fopen(log_path, "r");
    if (!file) {
        return;
    }

    /* 读取末尾最多 8 行作为失败诊断 */
    char lines[8][256];
    int count = 0;
    while (fgets(lines[count % 8], sizeof(lines[0]), file)) {
        count++;
    }
    fclose(file);

    int available = count < 8 ? count : 8;
    if (available <= 0) {
        return;
    }

    fprintf(stderr, "最近日志（%s）：\n", log_path);
    for (int i = 0; i < available; i++) {
        const char *line = lines[(count - available + i) % 8];
        fprintf(stderr, "  %s", line);
    }
}

int daemon_start_background(const char *open_path, int debug, int force,
                            char *pid_text, size_t pid_text_size) {
    if (pid_text && pid_text_size) {
        pid_text[0] = '\0';
    }

    if (cli_client_primary_available(NULL)) {
        char mode[32] = "";
        int pid = 0;
        cli_client_instance_mode(NULL, mode, sizeof(mode), &pid);
        if (!force) {
            fprintf(stderr, "错误：已有 ter-music 实例在运行（%s，pid %d）。\n",
                    mode[0] ? mode : "unknown", pid);
            fprintf(stderr, "      用 `ter-music play <路径>` 直接控制它，或先 `ter-music daemon stop`。\n");
            return CLI_EXIT_REFUSED;
        }
        fprintf(stderr, "警告：已有实例在运行（pid %d），将以次要实例身份启动（--force）。\n", pid);
    }

    const char *config_dir = app_config_dir(1);
    char log_path[MAX_PATH_LEN];
    snprintf(log_path, sizeof(log_path), "%s/%s",
             (config_dir && config_dir[0]) ? config_dir : ".", DAEMON_LOG_NAME);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "错误：fork 失败：%s\n", strerror(errno));
        return CLI_EXIT_DBUS;
    }

    if (pid == 0) {
        /* 子进程：脱离控制终端并 exec 自身（避免在 fork 后带状态继续运行） */
        if (setsid() < 0) {
            _exit(127);
        }
        if (fork() != 0) {
            _exit(0);
        }

        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        char self[PATH_MAX];
        ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (length <= 0) {
            snprintf(self, sizeof(self), "ter-music");
        } else {
            self[length] = '\0';
        }

        char *child_argv[10];
        int argc = 0;
        child_argv[argc++] = self;
        child_argv[argc++] = "daemon";
        child_argv[argc++] = "foreground";
        if (open_path && open_path[0]) {
            child_argv[argc++] = "--open";
            child_argv[argc++] = (char *)open_path;
        }
        if (debug) {
            child_argv[argc++] = "--debug";
        }
        if (force) {
            child_argv[argc++] = "--force";
        }
        child_argv[argc] = NULL;

        execv(self, child_argv);
        _exit(127);
    }

    /* 父进程：回收第一层子进程，再等待总线名就绪 */
    int status = 0;
    waitpid(pid, &status, 0);

    int daemon_pid = 0;
    if (cli_client_wait_for_online(NULL, DAEMON_START_TIMEOUT_MS, &daemon_pid) != CLI_EXIT_OK) {
        fprintf(stderr, "错误：后台播放进程未在 %d 秒内就绪。\n", DAEMON_START_TIMEOUT_MS / 1000);
        daemon_log_tail(log_path);
        return CLI_EXIT_DBUS;
    }

    if (pid_text && pid_text_size) {
        snprintf(pid_text, pid_text_size, "%d", daemon_pid);
    }
    return CLI_EXIT_OK;
}
