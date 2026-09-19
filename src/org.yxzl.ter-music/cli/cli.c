/**
 * @file cli.c
 * @brief CLI 子命令分发、参数解析与用法文本
 *
 * 注意：CLI 客户端的 usage / 错误信息沿用 main.c 既有的中文风格
 * （与 `ter-music --help` 现状一致），不引入新的 i18n 键；
 * `ter-music show` 的正文由持有配置的实例经 i18n 渲染。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/cli.h"

#include "config/config.h"
#include "queue/backend_queue.h"
#include "info/info.h"
#include "logger/logger.h"
#include "types.h"
#include "util/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 目标实例总线名（`--bus` 覆盖，默认主实例） */
static char g_cli_bus[128] = "";

static const char *const k_cli_commands[] = {
    "tui", "play", "pause", "resume", "toggle", "stop", "next", "prev",
    "seek", "volume", "speed", "mode", "show", "daemon",
    "version", "--version", "help", "--help", "-h",
    NULL
};

int cli_is_command(const char *arg) {
    if (!arg || arg[0] == '\0') {
        return 0;
    }
    for (int i = 0; k_cli_commands[i] != NULL; i++) {
        if (strcmp(arg, k_cli_commands[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void cli_print_help(const char *program) {
    printf("用法：%s [命令] [选项]\n\n", program ? program : "ter-music");
    printf("不带命令时进入 TUI 界面（与旧版行为一致）：\n");
    printf("  %s [路径]                 打开目录/音频文件并进入 TUI\n", program);
    printf("  %s -o <路径>              同上（-o / --open）\n", program);
    printf("  %s -d, --debug            启用调试日志\n", program);
    printf("  %s tui [路径]             显式进入 TUI\n\n", program);

    printf("播放控制（作用于当前主实例：daemon 或 TUI）：\n");
    printf("  play [路径] [--index N] [--mode 模式] [--no-daemon]\n");
    printf("                            播放指定路径；无实例时自动在后台启动 daemon\n");
    printf("  pause | resume | toggle | stop | next | prev\n");
    printf("  seek <+秒|-秒|mm:ss|N%%>   跳转（相对/绝对/百分比）\n");
    printf("  volume [0-100|+N|-N]      查询或设置音量\n");
    printf("  speed  [0.5-3.0]          查询或设置倍速\n");
    printf("  mode   [名称|0-16]        查询或设置播放模式\n\n");

    printf("信息显示：\n");
    printf("  show [选项]               输出当前播放信息（内容由 TUI「设置 → 信息显示」决定）\n");
    printf("     --json                 输出 JSON 快照（等价于 Info.GetInfo）\n");
    printf("     --watch[=毫秒]         实时刷新（默认 500ms，Ctrl+C 退出）\n");
    printf("     --one-line             单行输出\n");
    printf("     --full | --compact     使用全量 / 精简预设\n");
    printf("     --preset full|compact|custom\n");
    printf("     --fields a,b,c         基本信息字段：state,mode,index,queue,title,artist,\n");
    printf("                            album,format,path,volume,speed\n");
    printf("     --cover | --no-cover   字符封面开关\n");
    printf("     --cover-size WxH       封面尺寸（列 4-40，行 2-20）\n");
    printf("     --charset braille|ascii 封面字符集\n");
    printf("     --progress bar|time|percent|time+percent\n");
    printf("     --lyrics 0|1|2         歌词行数（0 关 / 1 当前句 / 2 当前句+下一句）\n");
    printf("     --width N              输出宽度（默认终端宽度）\n\n");

    printf("后台播放进程：\n");
    printf("  daemon start [--open 路径] [--debug] [--force]\n");
    printf("  daemon foreground [--open 路径] [--debug] [--force]\n");
    printf("  daemon stop [--force] | restart | status [--json] | reload\n\n");

    printf("其他：\n");
    printf("  version | --version        显示版本\n");
    printf("  help | --help              显示本帮助\n");
    printf("  --bus <名称>               指定目标实例的总线名（默认主实例）\n\n");

    printf("退出码：0 成功；1 参数错误；3 无运行实例；4 D-Bus 不可用；5 实例拒绝或操作失败\n");

    if (cli_in_sandbox()) {
        printf("\nLinyaps（如意玲珑）打包环境：请在宿主机使用\n");
        printf("  ll-cli run org.yxzl.ter-music -- ter-music <命令>\n");
        printf("后台播放：systemctl --user enable --now org.yxzl.ter-music\n");
        printf("          （或 ll-cli run org.yxzl.ter-music -- ter-music daemon foreground）\n");
        printf("容器已在运行时读取信息（run 的输出会接到该容器，终端看不到）：\n");
        printf("  ll-cli enter org.yxzl.ter-music -- /opt/apps/org.yxzl.ter-music/files/bin/ter-music show\n");
    }
    printf("\n示例：\n");
    printf("  %s play ~/Music            后台播放整个目录\n", program);
    printf("  %s show                    查看当前曲目信息（含盲文封面、进度、两句歌词）\n", program);
    printf("  %s show --one-line         单行状态，适合放进状态栏\n", program);
    printf("  %s next && %s volume +5    下一首并调高音量\n", program, program);
}

/* 预扫描并移除全局选项 --bus NAME / --bus=NAME */
static void cli_extract_global_options(int *argc, char **argv) {
    int write_index = 1;
    for (int read_index = 1; read_index < *argc; read_index++) {
        const char *arg = argv[read_index];
        if (strcmp(arg, "--bus") == 0 && read_index + 1 < *argc) {
            snprintf(g_cli_bus, sizeof(g_cli_bus), "%s", argv[read_index + 1]);
            read_index++;
            continue;
        }
        if (strncmp(arg, "--bus=", 6) == 0) {
            snprintf(g_cli_bus, sizeof(g_cli_bus), "%s", arg + 6);
            continue;
        }
        argv[write_index++] = argv[read_index];
    }
    *argc = write_index;
    argv[write_index] = NULL;
}

static const char *cli_bus(void) {
    return g_cli_bus[0] ? g_cli_bus : NULL;
}

int cli_in_sandbox(void) {
    /* 显式覆盖优先（测试与特殊部署） */
    const char *override = getenv("TER_MUSIC_SANDBOX");
    if (override && override[0] != '\0') {
        return strcmp(override, "0") != 0;
    }
    /* Linyaps（如意玲珑）容器：/run/linglong/container-init 由 ll-box 注入 */
    return access("/run/linglong/container-init", F_OK) == 0 ? 1 : 0;
}

/* 沙箱内的引导（reason 为具体原因行）
 * 沙箱不允许脱离进程（容器随命令退出而回收），后台播放必须由
 * D-Bus 激活或宿主侧 systemd 用户服务承担。 */
static void cli_print_sandbox_guidance(const char *reason) {
    fprintf(stderr, "错误：%s\n",
            reason ? reason : "Linyaps 打包环境下无法自行分叉后台进程。");
    fprintf(stderr, "请改用下列方式之一（均在宿主机执行）：\n");
    fprintf(stderr, "  · 常驻服务（推荐）：systemctl --user enable --now org.yxzl.ter-music\n");
    fprintf(stderr, "  · 前台运行：ll-cli run org.yxzl.ter-music -- ter-music daemon foreground\n");
    fprintf(stderr, "  · 直接播放：ll-cli run org.yxzl.ter-music -- ter-music play <路径> --foreground\n");
}

static unsigned long long cli_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

/* 等待主实例释放总线名（真实时钟计时；轮询间隔 100ms）
 * @return 1 = 已释放，0 = 超时仍被占用 */
static int cli_wait_until_offline(const char *bus, int timeout_ms) {
    unsigned long long deadline = cli_now_ms() + (unsigned long long)timeout_ms;
    while (cli_now_ms() < deadline) {
        if (!cli_client_primary_available(bus)) {
            return 1;
        }
        usleep(100000);
    }
    return cli_client_primary_available(bus) ? 0 : 1;
}

/* 确保存在可用主实例：
 *  - 已有实例 → OK
 *  - 沙箱内 → 尝试 D-Bus 激活（ll-cli run 由宿主机总线拉起 daemon），成功返回 OK
 *  - 否则返回 CLI_EXIT_NO_INSTANCE（调用方决定是否 auto-daemon） */
/* 确保播放服务在跑：已有则直接用；沙箱内走 D-Bus 激活（不能 fork）；
 * 否则后台拉起 daemon（no_autoplay：核心没有内容可自动播放，队列随后下发）。 */
static int cli_ensure_core_instance(void) {
    if (cli_client_primary_available(cli_bus())) {
        return CLI_EXIT_OK;
    }
    if (cli_in_sandbox()) {
        if (cli_client_activate_instance(cli_bus()) == CLI_EXIT_OK) {
            int pid = 0;
            if (cli_client_wait_for_online(cli_bus(), 10000, &pid) == CLI_EXIT_OK) {
                return CLI_EXIT_OK;
            }
            fprintf(stderr, "错误：已请求启动后台播放，但实例未在 10 秒内就绪。\n");
            return CLI_EXIT_DBUS;
        }
        cli_print_sandbox_guidance(
            "Linyaps 打包环境下无法自行分叉后台进程（脱离的进程会随容器退出而被回收）。");
        return CLI_EXIT_REFUSED;
    }
    {
        char pid_text[32] = "";
        int rc = daemon_start_background(NULL, 0, 0, pid_text, sizeof(pid_text));
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
    }
    return CLI_EXIT_OK;
}

int cli_ensure_core_for_frontend(const char *bus, int attach_only)
{
    if (cli_client_primary_available(bus)) {
        return CLI_EXIT_OK;
    }

    if (attach_only) {
        fprintf(stderr, "错误：没有正在运行的播放服务（已指定 --attach-only，不自动启动）。\n");
        fprintf(stderr, "      先运行 `ter-music daemon start`，或去掉 --attach-only。\n");
        return CLI_EXIT_NO_INSTANCE;
    }

    if (cli_in_sandbox()) {
        if (cli_client_activate_instance(bus) != CLI_EXIT_OK) {
            cli_print_sandbox_guidance("容器内无法自行分叉后台进程。");
            return CLI_EXIT_REFUSED;
        }
        int pid = 0;
        if (cli_client_wait_for_online(bus, 10000, &pid) != CLI_EXIT_OK) {
            fprintf(stderr, "错误：已请求启动播放服务，但未在 10 秒内就绪。\n");
            return CLI_EXIT_DBUS;
        }
        return CLI_EXIT_OK;
    }

    char pid_text[32] = "";
    int rc = daemon_start_background(NULL, 0, 0, pid_text, sizeof(pid_text));
    if (rc == CLI_EXIT_OK) {
        log_info("cli", "Started the playback core in the background (pid %s)",
                 pid_text[0] ? pid_text : "?");
    }
    return rc;
}

static int cli_require_instance(void) {
    if (cli_client_primary_available(cli_bus())) {
        return CLI_EXIT_OK;
    }
    if (cli_in_sandbox()) {
        cli_print_sandbox_guidance("当前没有正在运行的 ter-music 实例。");
        return CLI_EXIT_REFUSED;
    }
    fprintf(stderr, "错误：没有正在运行的 ter-music 实例。\n");
    fprintf(stderr, "      用 `ter-music play <路径>` 启动后台播放，或用 `ter-music` 打开 TUI 界面。\n");
    return CLI_EXIT_NO_INSTANCE;
}

/* ── play ───────────────────────────────────────────────────────── */

static int cli_cmd_play(int argc, char **argv) {
    const char *path = NULL;
    int index = -1;
    int mode = -1;
    int allow_daemon = 1;
    int foreground = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = info_play_mode_from_id(argv[++i]);
            if (mode < 0) {
                fprintf(stderr, "错误：未知播放模式。\n");
                return CLI_EXIT_USAGE;
            }
        } else if (strcmp(argv[i], "--no-daemon") == 0) {
            allow_daemon = 0;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            /* 以前台无界面方式加载并播放（Linyaps 沙箱内自足用法，
             * 也适用于 systemd/tmux 等托管场景） */
            foreground = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            /* 兼容保留：当前实现始终安静执行 */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "错误：未知选项 '%s'。\n", argv[i]);
            return CLI_EXIT_USAGE;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "错误：play 只接受一个路径参数。\n");
            return CLI_EXIT_USAGE;
        }
    }

    if (path && !bq_path_is_local(path)) {
        fprintf(stderr, "错误：核心只播放本地文件；远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）由前端负责：\n");
        fprintf(stderr, "      请在 TUI 中打开远程目录，前端会把曲目下载到本地缓存后再交给核心播放。\n");
        return CLI_EXIT_USAGE;
    }

    /* ── 前端负责内容：先确保播放服务在跑，再扫描并下发队列 ── */
    int rc = cli_ensure_core_instance();
    if (rc != CLI_EXIT_OK) {
        if (foreground && rc == CLI_EXIT_NO_INSTANCE) {
            /* 本进程成为无界面播放进程（不 fork）；此时没有前端，队列由调用方下发 */
            return daemon_run_foreground(NULL, 0, 0, 1);
        }
        return rc;
    }

    if (cli_client_primary_available(cli_bus()) && foreground) {
        fprintf(stderr, "提示：已有 ter-music 实例在运行，改为控制该实例（--foreground 仅在无实例时生效）。\n");
    }

    return cli_client_play(cli_bus(), path, index, mode);
}

/* ── daemon ─────────────────────────────────────────────────────── */

static int cli_cmd_daemon(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "错误：daemon 需要子命令：start | foreground | stop | restart | status | reload\n");
        return CLI_EXIT_USAGE;
    }

    const char *action = argv[2];
    const char *open_path = NULL;
    int debug = 0;
    int force = 0;
    int want_json = 0;
    int no_autoplay = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
            open_path = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--no-autoplay") == 0) {
            no_autoplay = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            want_json = 1;
        } else {
            fprintf(stderr, "错误：未知选项 '%s'。\n", argv[i]);
            return CLI_EXIT_USAGE;
        }
    }

    if (open_path && !bq_path_is_local(open_path)) {
        fprintf(stderr, "错误：核心只播放本地文件；远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）由前端负责：\n");
        fprintf(stderr, "      请在 TUI 中打开远程目录，前端会把曲目下载到本地缓存后再交给核心播放。\n");
        return CLI_EXIT_USAGE;
    }

    if (strcmp(action, "foreground") == 0) {
        /* 核心不扫描目录：`--open <路径>` 在这里没有意义（前端负责扫描），
         * daemon_run_foreground() 会给出明确提示。 */
        return daemon_run_foreground(open_path, debug, force, no_autoplay);
    }

    if (strcmp(action, "start") == 0) {
        /* 沙箱（Linyaps）：不能 fork 脱离进程 → 请求 D-Bus 激活后台播放 */
        if (cli_in_sandbox()) {
            int rc = cli_ensure_core_instance();
            if (rc != CLI_EXIT_OK) {
                return rc;
            }
            char mode[32] = "";
            int pid = 0;
            cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
            printf("后台播放已在运行（%s，pid %d）。\n", mode[0] ? mode : "instance", pid);
            return CLI_EXIT_OK;
        }

        if (open_path && open_path[0]) {
            /* `daemon start --open <路径>`：核心不再扫描目录。
             * 语义是“启动核心 → 前端扫描 → 下发队列”，因此这里先起核心，
             * 再由本进程装载内容并 Queue.Set/PlayAt。 */
            int rc = cli_ensure_core_instance();
            if (rc != CLI_EXIT_OK) {
                return rc;
            }
            return cli_client_play(cli_bus(), open_path, -1, -1);
        }

        char pid_text[32] = "";
        int rc = daemon_start_background(NULL, debug, force, pid_text, sizeof(pid_text));
        if (rc == CLI_EXIT_OK) {
            printf("后台播放已启动（pid %s）。队列由前端下发：\n", pid_text[0] ? pid_text : "?");
            printf("  ter-music play <路径>   扫描目录并交给核心播放\n");
            printf("  ter-music show          查看信息\n");
            printf("  ter-music daemon stop   停止\n");
        }
        return rc;
    }

    if (strcmp(action, "status") == 0) {
        char mode[32] = "";
        int pid = 0;
        int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
        if (rc != CLI_EXIT_OK) {
            if (rc == CLI_EXIT_NO_INSTANCE) {
                fprintf(stderr, "ter-music 未在运行。\n");
            }
            return rc;
        }

        if (want_json) {
            /* 直接输出 Info.GetInfo 的完整 JSON 快照（含 instance/playback/track） */
            return cli_client_show(cli_bus(), NULL, 1, 0);
        }

        printf("实例：%s（pid %d）\n", mode, pid);
        return cli_client_show(cli_bus(), "one_line=1", 0, 0);
    }

    if (strcmp(action, "stop") == 0) {
        char mode[32] = "";
        int pid = 0;
        int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
        if (rc != CLI_EXIT_OK) {
            if (rc == CLI_EXIT_NO_INSTANCE) {
                fprintf(stderr, "ter-music 未在运行。\n");
            }
            return rc;
        }
        if (strcmp(mode, "tui") == 0 && !force) {
            fprintf(stderr, "错误：当前主实例是 TUI（pid %d）。如需一并退出 TUI 请加 --force。\n", pid);
            return CLI_EXIT_REFUSED;
        }

        rc = cli_client_quit(cli_bus());
        if (rc != CLI_EXIT_OK) {
            return rc;
        }

        if (!cli_wait_until_offline(cli_bus(), 5000)) {
            fprintf(stderr, "警告：实例 %d 已收到退出请求，但总线名仍未释放（可能正在等待音频线程收尾）。\n",
                    pid);
            return CLI_EXIT_REFUSED;
        }
        printf("已停止（pid %d）。\n", pid);
        return CLI_EXIT_OK;
    }

    if (strcmp(action, "restart") == 0) {
        char mode[32] = "";
        int pid = 0;

        /* 沙箱（Linyaps）：先优雅停止旧实例，再请求 D-Bus 激活 */
        if (cli_in_sandbox()) {
            int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
            if (rc == CLI_EXIT_OK) {
                if (strcmp(mode, "tui") == 0 && !force) {
                    fprintf(stderr, "错误：当前主实例是 TUI（pid %d）。如需一并退出 TUI 请加 --force。\n", pid);
                    return CLI_EXIT_REFUSED;
                }
                if (cli_client_quit(cli_bus()) != CLI_EXIT_OK) {
                    return CLI_EXIT_REFUSED;
                }
                if (!cli_wait_until_offline(cli_bus(), 10000)) {
                    fprintf(stderr, "错误：旧实例 %d 仍在退出中（等待 10 秒未能释放总线名），请稍后重试。\n", pid);
                    return CLI_EXIT_REFUSED;
                }
            }
            rc = cli_ensure_core_instance();
            if (rc != CLI_EXIT_OK) {
                return rc;
            }
            printf("后台播放已重启。\n");
            return CLI_EXIT_OK;
        }

        if (cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid) == CLI_EXIT_OK) {
            if (strcmp(mode, "tui") == 0 && !force) {
                fprintf(stderr, "错误：当前主实例是 TUI（pid %d）。如需一并退出 TUI 请加 --force。\n", pid);
                return CLI_EXIT_REFUSED;
            }

            int quit_rc = cli_client_quit(cli_bus());
            if (quit_rc != CLI_EXIT_OK) {
                fprintf(stderr, "错误：无法通知现有实例退出（rc=%d）。\n", quit_rc);
                return quit_rc;
            }

            /* 退出是优雅的：停止音频、保存会话可能需要数秒 */
            if (!cli_wait_until_offline(cli_bus(), 10000)) {
                fprintf(stderr, "错误：旧实例 %d 仍在退出中（等待 10 秒未能释放总线名），请稍后重试。\n",
                        pid);
                return CLI_EXIT_REFUSED;
            }
        }

        char pid_text[32] = "";
        int rc = daemon_start_background(open_path, debug, force, pid_text, sizeof(pid_text));
        if (rc == CLI_EXIT_OK) {
            printf("后台播放已重启（pid %s）。\n", pid_text[0] ? pid_text : "?");
        }
        return rc;
    }

    if (strcmp(action, "reload") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        rc = cli_client_reload(cli_bus());
        if (rc == CLI_EXIT_OK) {
            printf("已通知实例重新加载配置。\n");
        }
        return rc;
    }

    fprintf(stderr, "错误：未知 daemon 子命令 '%s'。\n", action);
    return CLI_EXIT_USAGE;
}

/* ── show ───────────────────────────────────────────────────────── */

static int cli_cmd_show(int argc, char **argv) {
    char options[512] = "";
    int want_json = 0;
    int watch_ms = 0;
    int width_set = 0;

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--json") == 0) {
            want_json = 1;
        } else if (strncmp(arg, "--watch", 7) == 0) {
            watch_ms = 500;
            if (arg[7] == '=') {
                watch_ms = atoi(arg + 8);
                if (watch_ms < 100) {
                    watch_ms = 100;
                }
            }
        } else if (strcmp(arg, "--one-line") == 0) {
            strncat(options, "one_line=1;", sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--full") == 0) {
            strncat(options, "preset=full;", sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--compact") == 0) {
            strncat(options, "preset=compact;", sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--preset") == 0 && i + 1 < argc) {
            char pair[64];
            snprintf(pair, sizeof(pair), "preset=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--fields") == 0 && i + 1 < argc) {
            char pair[256];
            snprintf(pair, sizeof(pair), "fields=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--cover") == 0) {
            strncat(options, "cover=1;", sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--no-cover") == 0) {
            strncat(options, "cover=0;", sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--cover-size") == 0 && i + 1 < argc) {
            int cols = 0;
            int rows = 0;
            if (sscanf(argv[++i], "%dx%d", &cols, &rows) != 2) {
                fprintf(stderr, "错误：--cover-size 需要 WxH 形式，例如 16x8。\n");
                return CLI_EXIT_USAGE;
            }
            char pair[64];
            snprintf(pair, sizeof(pair), "cover_cols=%d;cover_rows=%d;", cols, rows);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--charset") == 0 && i + 1 < argc) {
            char pair[64];
            snprintf(pair, sizeof(pair), "cover_charset=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--progress") == 0 && i + 1 < argc) {
            char pair[64];
            snprintf(pair, sizeof(pair), "progress_style=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--lyrics") == 0 && i + 1 < argc) {
            char pair[64];
            snprintf(pair, sizeof(pair), "lyrics=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            char pair[64];
            snprintf(pair, sizeof(pair), "width=%s;", argv[++i]);
            strncat(options, pair, sizeof(options) - strlen(options) - 1);
            width_set = 1;
        } else if (strcmp(arg, "--no-progress") == 0) {
            strncat(options, "progress=0;", sizeof(options) - strlen(options) - 1);
        } else {
            fprintf(stderr, "错误：未知的 show 选项 '%s'（用 `ter-music help` 查看用法）。\n", arg);
            return CLI_EXIT_USAGE;
        }
    }

    if (!width_set) {
        char pair[64];
        snprintf(pair, sizeof(pair), "width=%d;", cli_default_width());
        strncat(options, pair, sizeof(options) - strlen(options) - 1);
    }

    int rc = cli_require_instance();
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    return cli_client_show(cli_bus(), options, want_json, watch_ms);
}

/* ── 入口 ───────────────────────────────────────────────────────── */

int cli_run(int argc, char **argv) {
    ensure_utf8_locale();
    cli_extract_global_options(&argc, argv);

    if (argc < 2) {
        cli_print_help(argv[0]);
        return CLI_EXIT_OK;
    }

    const char *command = argv[1];

    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        cli_print_help(argv[0]);
        return CLI_EXIT_OK;
    }
    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) {
        printf("%s %s\n", APP_NAME, APP_VERSION);
        return CLI_EXIT_OK;
    }
    if (strcmp(command, "play") == 0) {
        return cli_cmd_play(argc, argv);
    }
    if (strcmp(command, "show") == 0) {
        return cli_cmd_show(argc, argv);
    }
    if (strcmp(command, "daemon") == 0) {
        return cli_cmd_daemon(argc, argv);
    }

    if (strcmp(command, "pause") == 0 || strcmp(command, "resume") == 0 ||
        strcmp(command, "toggle") == 0 || strcmp(command, "stop") == 0 ||
        strcmp(command, "next") == 0 || strcmp(command, "prev") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        const char *method = "PlayPause";
        if (strcmp(command, "pause") == 0) method = "Pause";
        else if (strcmp(command, "resume") == 0) method = "Play";
        else if (strcmp(command, "stop") == 0) method = "Stop";
        else if (strcmp(command, "next") == 0) method = "Next";
        else if (strcmp(command, "prev") == 0) method = "Previous";
        return cli_client_transport(cli_bus(), method);
    }

    if (strcmp(command, "seek") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        return cli_client_seek(cli_bus(), argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(command, "volume") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        return cli_client_volume(cli_bus(), argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(command, "speed") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        return cli_client_speed(cli_bus(), argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(command, "mode") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) {
            return rc;
        }
        return cli_client_mode(cli_bus(), argc > 2 ? argv[2] : NULL);
    }

    /* "tui" 由 main.c 在分发前剥离，不应到达这里 */
    fprintf(stderr, "错误：未知命令 '%s'。\n", command);
    cli_print_help(argv[0]);
    return CLI_EXIT_USAGE;
}
