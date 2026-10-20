/**
 * @file cli.c
 * @brief CLI 子命令分发、参数解析与多级帮助
 *
 * 所有用户可见文案经 i18n_get("cli...") 渲染；帮助内容由 cli/help.c
 * 的统一帮助模型输出（概览 / 命令详情 / 子命令详情 / 全量）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/cli.h"
#include "cli/help.h"
#include "cli/lang.h"

#include "config/config.h"
#include "i18n/i18n.h"
#include "playlist/playlist_queue.h"
#include "queue/backend_queue.h"
#include "info/info.h"
#include "logger/logger.h"
#include "types.h"
#include "util/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 目标实例总线名（`--bus` 覆盖，默认主实例） */
static char g_cli_bus[128] = "";

static const char *const k_cli_commands[] = {
    "tui", "play", "pause", "resume", "toggle", "stop", "next", "prev",
    "seek", "volume", "speed", "mode", "show", "daemon",
    "queue", "config", "lyrics", "track", "cover", "keys",
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

/* 用户可见输出的 i18n 包装：格式串来自语言包，末尾统一补换行。
 * 语言包未就绪时 i18n_get() 返回键名本身，不会崩溃。 */
static void cli_out(const char *key, ...)
{
    va_list args;
    va_start(args, key);
    vfprintf(stdout, i18n_get(key), args);
    va_end(args);
    fputc('\n', stdout);
}

static void cli_err(const char *key, ...)
{
    va_list args;
    va_start(args, key);
    vfprintf(stderr, i18n_get(key), args);
    va_end(args);
    fputc('\n', stderr);
}

/* 沙箱引导行：与 cli_diag 一样写入累积缓冲并记入日志。 */
static void cli_diag_key(const char *key)
{
    cli_diag("%s\n", i18n_get(key));
}

/* 子命令参数预扫描：位置 i 是独立的 --help/-h 参数时视为帮助请求。
 * 必须在解析自身参数之前处理，避免未知选项先行报错。 */
static int cli_help_requested(int argc, char **argv, int start_index)
{
    for (int i = start_index; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return 1;
        }
    }
    return 0;
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

/* ── TUI 会命中的诊断输出（语义见 cli.h） ─────────────────────── */
static char g_cli_diag_buf[1024];
static size_t g_cli_diag_len = 0;

void cli_diag(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    fputs(buf, stderr);

    /* 累积缓冲：放不下就不再追加（截断安全，内容已进日志） */
    size_t len = strlen(buf);
    if (len < sizeof(g_cli_diag_buf) - g_cli_diag_len) {
        memcpy(g_cli_diag_buf + g_cli_diag_len, buf, len);
        g_cli_diag_len += len;
        g_cli_diag_buf[g_cli_diag_len] = '\0';
    }

    log_error("cli", "%s", buf);
}

void cli_diag_flush(void) {
    if (g_cli_diag_len == 0) return;
    fputs(g_cli_diag_buf, stderr);
    g_cli_diag_len = 0;
    g_cli_diag_buf[0] = '\0';
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

/* 沙箱内的引导（reason_key 为具体原因对应的 i18n 键）
 * 沙箱不允许脱离进程（容器随命令退出而回收），后台播放必须由
 * D-Bus 激活或宿主侧 systemd 用户服务承担。 */
static void cli_print_sandbox_guidance(const char *reason_key) {
    cli_diag_key(reason_key ? reason_key : "cli.err.sandbox.fork_default");
    cli_diag_key("cli.msg.sandbox.guidance_title");
    cli_diag_key("cli.msg.sandbox.guidance_service");
    cli_diag_key("cli.msg.sandbox.guidance_foreground");
    cli_diag_key("cli.msg.sandbox.guidance_play");
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
            cli_diag_key("cli.err.daemon.not_ready_10s");
            return CLI_EXIT_DBUS;
        }
        cli_print_sandbox_guidance("cli.err.sandbox.fork_detached");
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
        cli_diag_key("cli.err.attach_only.no_service");
        cli_diag_key("cli.err.attach_only.hint");
        return CLI_EXIT_NO_INSTANCE;
    }

    if (cli_in_sandbox()) {
        if (cli_client_activate_instance(bus) != CLI_EXIT_OK) {
            cli_print_sandbox_guidance("cli.err.sandbox.fork_container");
            return CLI_EXIT_REFUSED;
        }
        int pid = 0;
        if (cli_client_wait_for_online(bus, 10000, &pid) != CLI_EXIT_OK) {
            cli_diag_key("cli.err.core.not_ready_10s");
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
        cli_print_sandbox_guidance("cli.err.sandbox.no_instance");
        return CLI_EXIT_REFUSED;
    }
    cli_err("cli.err.no_instance");
    cli_err("cli.err.no_instance.hint");
    return CLI_EXIT_NO_INSTANCE;
}

/* ── play ───────────────────────────────────────────────────────── */

/* `play --foreground` 的分块收集器：内容装配留在前端（本文件），把
 * playlist_queue_foreach_chunk 的每块 {"entries":[…]} 复制进数组，再作为
 * 纯数据交给 daemon_run_foreground_with_queue（daemon 不引用 playlist）。 */
typedef struct {
    char **chunks;
    int count;
    int capacity;
} CliForegroundChunks;

static int cli_foreground_collect_chunk(const char *chunk_json, int chunk_index,
                                        int entries_in_chunk, void *user)
{
    (void)chunk_index;
    (void)entries_in_chunk;
    CliForegroundChunks *c = (CliForegroundChunks *)user;

    if (c->count == c->capacity) {
        int next_cap = c->capacity ? c->capacity * 2 : 8;
        char **grown = realloc(c->chunks, (size_t)next_cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        c->chunks = grown;
        c->capacity = next_cap;
    }
    char *copy = strdup(chunk_json);
    if (!copy) {
        return -1;
    }
    c->chunks[c->count++] = copy;
    return 0;
}

static void cli_foreground_chunks_free(CliForegroundChunks *c)
{
    for (int i = 0; i < c->count; i++) {
        free(c->chunks[i]);
    }
    free(c->chunks);
    c->chunks = NULL;
    c->count = 0;
    c->capacity = 0;
}

static int cli_cmd_play(int argc, char **argv) {
    const char *path = NULL;
    int index = -1;
    int mode = -1;
    int allow_daemon = 1;
    int foreground = 0;

    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "play", NULL);
        return CLI_EXIT_OK;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = info_play_mode_from_id(argv[++i]);
            if (mode < 0) {
                cli_err("cli.err.unknown_play_mode");
                return CLI_EXIT_USAGE;
            }
        } else if (strcmp(argv[i], "--no-daemon") == 0) {
            allow_daemon = 0;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            /* 以前台无界面方式加载并播放（Linyaps 沙箱内自足用法，
             * 也适用于 systemd/tmux 等托管场景） */
            foreground = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            /* 兼容保留：play 的成功路径本就不向 stdout 输出（错误只走 stderr），
             * 因此该参数默认即已满足“安静”，保留仅为脚本兼容。 */
        } else if (argv[i][0] == '-') {
            cli_err("cli.err.unknown_option", argv[i]);
            return CLI_EXIT_USAGE;
        } else if (!path) {
            path = argv[i];
        } else {
            cli_err("cli.err.play.too_many_paths");
            return CLI_EXIT_USAGE;
        }
    }

    if (path && !bq_path_is_local(path)) {
        cli_err("cli.err.remote_not_supported");
        cli_err("cli.err.remote_hint");
        return CLI_EXIT_USAGE;
    }

    /* ── 前台模式：本进程成为核心，不 fork 后台 daemon，也不经 D-Bus 下发 ──
     * 已有主实例时拒绝：两个核心会抢同一个主总线名。否则在前台装载内容并播放。 */
    if (foreground) {
        if (cli_client_primary_available(cli_bus())) {
            cli_err("cli.err.play.foreground_running");
            cli_err("cli.err.play.foreground_hint");
            return CLI_EXIT_REFUSED;
        }

        const char *load_path = path;
        char default_path[MAX_PATH_LEN] = "";
        if (!load_path && index >= 0) {
            /* 无 path 但给了 --index：用配置默认音乐目录（与 cli_client_play 同口径） */
            if (g_app_config.default_startup_path[0]) {
                snprintf(default_path, sizeof(default_path), "%s",
                         g_app_config.default_startup_path);
            } else if (g_app_config.remember_last_path && g_app_config.last_opened_path[0]) {
                snprintf(default_path, sizeof(default_path), "%s",
                         g_app_config.last_opened_path);
            }
            if (!default_path[0]) {
                cli_err("cli.err.play.no_default_path");
                return CLI_EXIT_REFUSED;
            }
            load_path = default_path;
        }

        int start_index = index;
        if (load_path) {
            int selected = -1;
            if (cli_client_load_local_content(load_path, &selected) < 0) {
                cli_err("cli.err.play.cannot_open", load_path);
                return CLI_EXIT_REFUSED;
            }
            if (start_index < 0) {
                start_index = selected;
            }
        }
        /* 内容装配在前端完成后作为纯数据分块交给 daemon（load_path 为空时
         * nchunks=0，由前台核心自己报"没有可播放的曲目"并拒绝起播）。 */
        CliForegroundChunks chunks = {0};
        if (playlist_queue_foreach_chunk(cli_foreground_collect_chunk, &chunks) < 0) {
            cli_err("cli.err.play.dispatch_failed");
            cli_foreground_chunks_free(&chunks);
            return CLI_EXIT_REFUSED;
        }

        int rc = daemon_run_foreground_with_queue((const char *const *)chunks.chunks,
                                                  chunks.count, start_index, mode, 0);
        cli_foreground_chunks_free(&chunks);
        return rc;
    }

    /* ── 前端负责内容：先确保播放服务在跑，再扫描并分块下发队列 ── */
    int rc = cli_ensure_core_instance();
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    return cli_client_play(cli_bus(), path, index, mode);
}

/* ── daemon ─────────────────────────────────────────────────────── */

/* ── daemon：次要实例的提示与收尾 ───────────────────────────────────
 * `daemon start --force` 起的第二个核心持有 `<主名>.instance<pid>`，它**不**
 * 接收普通 CLI 命令。这两个函数只做“让人看得见、收得掉”，不改其存在语义
 * （次要实例是有文档的功能，见 README 的 daemon 注意事项）。 */

static int list_secondary_instances(const char *base, CliInstanceInfo *out, int cap) {
    CliInstanceInfo instances[8];
    int count = cli_client_list_instances(base, instances, 8);
    if (count <= 0) {
        return count;
    }

    int secondaries = 0;
    for (int i = 0; i < count; i++) {
        if (instances[i].primary) {
            continue;
        }
        if (secondaries < cap) {
            out[secondaries] = instances[i];
        }
        secondaries++;
    }
    return secondaries;
}

/* 主实例之外还有实例在跑时，向 stderr 打一段可操作的提示。
 * @param base 枚举用的基名（cli_bus()，NULL = 默认主名）
 * @param primary_running 主实例此刻是否在运行（决定提示措辞，不要用 base 是否为 NULL 推断） */
static void warn_about_secondary_instances(const char *base, int primary_running) {
    CliInstanceInfo secondaries[8];
    int count = list_secondary_instances(base, secondaries, 8);
    if (count <= 0) {
        return;
    }

    if (primary_running) {
        cli_err("cli.warn.secondary.running", count);
    } else {
        cli_err("cli.warn.secondary.not_running", count);
    }
    for (int i = 0; i < count && i < 8; i++) {
        fprintf(stderr, "  pid %d  %s  %s\n", secondaries[i].pid, secondaries[i].mode,
                secondaries[i].bus);
    }
    cli_err("cli.warn.secondary.control_one");
    cli_err("cli.warn.secondary.stop_all");
}

/* 停掉所有次要实例（主实例已由调用方处理）。
 * @param out_failed 收到“拒绝退出/超时未释放总线名”的个数（可为 NULL）
 * @return 成功停止的个数；-1 = 拿不到总线 */
static int stop_secondary_instances(const char *primary_bus, int *out_failed) {
    if (out_failed) {
        *out_failed = 0;
    }

    CliInstanceInfo secondaries[8];
    int count = list_secondary_instances(primary_bus, secondaries, 8);
    if (count < 0) {
        cli_err("cli.err.list_instances");
        return -1;
    }

    int stopped = 0;
    int failed = 0;
    for (int i = 0; i < count && i < 8; i++) {
        const char *bus = secondaries[i].bus;
        if (cli_client_quit(bus) != CLI_EXIT_OK) {
            cli_err("cli.warn.secondary.refused", bus, secondaries[i].pid);
            failed++;
            continue;
        }
        if (!cli_wait_until_offline(bus, 5000)) {
            cli_err("cli.warn.secondary.bus_not_released", bus, secondaries[i].pid);
            failed++;
            continue;
        }
        cli_out("cli.msg.secondary.stopped", secondaries[i].pid, bus);
        stopped++;
    }

    if (out_failed) {
        *out_failed = failed;
    }
    return stopped;
}

static int cli_cmd_daemon(int argc, char **argv) {
    /* `daemon <sub> --help` → 二级详情；其余 `--help` → 一级详情。
     * 未知子命令的二级帮助失败时退回一级，不吞掉帮助请求。 */
    if (argc >= 4 && argv[2][0] != '-' && cli_help_requested(argc, argv, 3) &&
        cli_help_print_command(argv[0], "daemon", argv[2]) == 0) {
        return CLI_EXIT_OK;
    }
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "daemon", NULL);
        return CLI_EXIT_OK;
    }

    if (argc < 3) {
        cli_err("cli.err.daemon.missing_subcommand");
        return CLI_EXIT_USAGE;
    }

    const char *action = argv[2];
    const char *open_path = NULL;
    int debug = 0;
    int force = 0;
    int want_json = 0;
    int no_autoplay = 0;
    int all = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
            open_path = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            all = 1;
        } else if (strcmp(argv[i], "--no-autoplay") == 0) {
            no_autoplay = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            want_json = 1;
        } else {
            cli_err("cli.err.unknown_option", argv[i]);
            return CLI_EXIT_USAGE;
        }
    }

    if (open_path && !bq_path_is_local(open_path)) {
        cli_err("cli.err.remote_not_supported");
        cli_err("cli.err.remote_hint");
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
            int mrc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
            if (mrc == CLI_EXIT_OK) {
                cli_out("cli.msg.daemon.already_running", mode[0] ? mode : "instance", pid);
            } else {
                /* 名字虽已上线，但 InstanceInfo 没拿到 mode/pid：不打印误导文本 */
                cli_out("cli.msg.daemon.requested");
            }
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
            cli_out("cli.msg.daemon.started", pid_text[0] ? pid_text : "?");
            cli_out("cli.msg.daemon.hint_play");
            cli_out("cli.msg.daemon.hint_show");
            cli_out("cli.msg.daemon.hint_stop");
        }
        return rc;
    }

    if (strcmp(action, "status") == 0) {
        char mode[32] = "";
        int pid = 0;
        int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);

        /* 名字是否在线：instance_mode 只在“主名不在”时回 NO_INSTANCE；其余
         * （含 DBUS）都表示名字已被持有、只是 InstanceInfo 查询失败。次要实例
         * 提示据此选措辞，绝不能把“名字在线但 InstanceInfo 失败”说成“主实例未运行”。 */
        int primary_online = (rc != CLI_EXIT_NO_INSTANCE);
        warn_about_secondary_instances(cli_bus(), primary_online);

        if (rc == CLI_EXIT_NO_INSTANCE) {
            cli_err("cli.err.not_running");
            return rc;
        }
        if (rc != CLI_EXIT_OK) {
            /* 名字在线但 InstanceInfo 失败：如实说明，不要谎报“未运行” */
            cli_err("cli.warn.instance_info_failed", rc);
            return rc;
        }

        if (want_json) {
            /* 直接输出 Info.GetInfo 的完整 JSON 快照（含 instance/playback/track） */
            return cli_client_show(cli_bus(), NULL, 1, 0);
        }

        cli_out("cli.msg.daemon.instance", mode, pid);
        return cli_client_show(cli_bus(), "one_line=1", 0, 0);
    }

    if (strcmp(action, "stop") == 0) {
        char mode[32] = "";
        int pid = 0;
        int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);

        if (rc == CLI_EXIT_OK) {
            if (strcmp(mode, "tui") == 0 && !force) {
                cli_err("cli.err.daemon.tui_requires_force", pid);
                return CLI_EXIT_REFUSED;
            }

            int quit_rc = cli_client_quit(cli_bus());
            if (quit_rc != CLI_EXIT_OK) {
                return quit_rc;
            }

            if (!cli_wait_until_offline(cli_bus(), 5000)) {
                cli_err("cli.warn.bus_not_released", pid);
                return CLI_EXIT_REFUSED;
            }
            cli_out("cli.msg.daemon.stopped", pid);
        } else if (!all) {
            if (rc == CLI_EXIT_NO_INSTANCE) {
                cli_err("cli.err.not_running");
            }
            return rc;
        }

        if (!all) {
            return CLI_EXIT_OK;
        }

        /* --all：把次要实例一并收掉。主实例没了也照做——“主实例已死、只剩
         * 次要实例”正是最需要收尾的情形。 */
        int failed_secondaries = 0;
        int stopped_secondaries = stop_secondary_instances(cli_bus(), &failed_secondaries);
        if (stopped_secondaries < 0) {
            return CLI_EXIT_DBUS;
        }
        if (rc != CLI_EXIT_OK && stopped_secondaries == 0 && failed_secondaries == 0) {
            cli_err("cli.err.not_running");
            return CLI_EXIT_NO_INSTANCE;
        }
        if (stopped_secondaries > 0) {
            cli_out("cli.msg.secondary.stopped_count", stopped_secondaries);
        }
        return failed_secondaries > 0 ? CLI_EXIT_REFUSED : CLI_EXIT_OK;
    }

    if (strcmp(action, "restart") == 0) {
        char mode[32] = "";
        int pid = 0;

        /* 沙箱（Linyaps）：先优雅停止旧实例，再请求 D-Bus 激活 */
        if (cli_in_sandbox()) {
            int rc = cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid);
            if (rc == CLI_EXIT_OK) {
                if (strcmp(mode, "tui") == 0 && !force) {
                    cli_err("cli.err.daemon.tui_requires_force", pid);
                    return CLI_EXIT_REFUSED;
                }
                if (cli_client_quit(cli_bus()) != CLI_EXIT_OK) {
                    return CLI_EXIT_REFUSED;
                }
                if (!cli_wait_until_offline(cli_bus(), 10000)) {
                    cli_err("cli.err.daemon.old_instance_exiting", pid);
                    return CLI_EXIT_REFUSED;
                }
            }
            rc = cli_ensure_core_instance();
            if (rc != CLI_EXIT_OK) {
                return rc;
            }
            cli_out("cli.msg.daemon.restarted");
            return CLI_EXIT_OK;
        }

        if (cli_client_instance_mode(cli_bus(), mode, sizeof(mode), &pid) == CLI_EXIT_OK) {
            if (strcmp(mode, "tui") == 0 && !force) {
                cli_err("cli.err.daemon.tui_requires_force", pid);
                return CLI_EXIT_REFUSED;
            }

            int quit_rc = cli_client_quit(cli_bus());
            if (quit_rc != CLI_EXIT_OK) {
                cli_err("cli.err.daemon.quit_failed", quit_rc);
                return quit_rc;
            }

            /* 退出是优雅的：停止音频、保存会话可能需要数秒 */
            if (!cli_wait_until_offline(cli_bus(), 10000)) {
                cli_err("cli.err.daemon.old_instance_exiting", pid);
                return CLI_EXIT_REFUSED;
            }
        }

        /* restart --open 与 start --open 同路径：核心不扫描目录，把 open_path
         * 交给 daemon_start_background 只会让子进程命中“核心不扫描目录”的拒绝
         * 分支静默空转。改为：先起空核心，再由本进程装载内容并分块下发播放。 */
        if (open_path && open_path[0]) {
            int rc = cli_ensure_core_instance();
            if (rc != CLI_EXIT_OK) {
                return rc;
            }
            cli_out("cli.msg.daemon.restarted_loading", open_path);
            return cli_client_play(cli_bus(), open_path, -1, -1);
        }

        char pid_text[32] = "";
        int rc = daemon_start_background(NULL, debug, force, pid_text, sizeof(pid_text));
        if (rc == CLI_EXIT_OK) {
            cli_out("cli.msg.daemon.restarted_pid", pid_text[0] ? pid_text : "?");
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
            cli_out("cli.msg.daemon.reloaded");
        }
        return rc;
    }

    cli_err("cli.err.daemon.unknown_subcommand", action);
    return CLI_EXIT_USAGE;
}

/* ── show ───────────────────────────────────────────────────────── */

static int cli_cmd_show(int argc, char **argv) {
    char options[512] = "";
    int want_json = 0;
    int watch_ms = 0;
    int width_set = 0;

    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "show", NULL);
        return CLI_EXIT_OK;
    }

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
                cli_err("cli.err.show.cover_size");
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
            cli_err("cli.err.show.unknown_option", arg);
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

/* ── queue ──────────────────────────────────────────────────────── */

/* 把逗号分隔的若干路径拆成指针数组（调用方负责 free 每个 strdup 串与数组）。
 * @return 路径个数；NULL 出参经 *out 返回堆数组；内存不足或超上限返回 -1 */
static int cli_split_comma_paths(char **argv, int from, int to,
                                 char ***out, int *out_n) {
    static const int k_max = 512;
    char **list = NULL;
    int n = 0;
    int cap = 0;

    for (int i = from; i < to; i++) {
        char *token = argv[i];
        char *cursor = token;
        while (cursor && *cursor) {
            char *comma = strchr(cursor, ',');
            size_t len = comma ? (size_t)(comma - cursor) : strlen(cursor);
            if (len > 0) {
                if (n >= k_max) {
                    for (int j = 0; j < n; j++) free(list[j]);
                    free(list);
                    return -1;
                }
                if (n >= cap) {
                    int new_cap = cap ? cap * 2 : 16;
                    char **grown = realloc(list, (size_t)new_cap * sizeof(*grown));
                    if (!grown) {
                        for (int j = 0; j < n; j++) free(list[j]);
                        free(list);
                        return -1;
                    }
                    list = grown;
                    cap = new_cap;
                }
                char *piece = strndup(cursor, len);
                if (!piece) {
                    for (int j = 0; j < n; j++) free(list[j]);
                    free(list);
                    return -1;
                }
                list[n++] = piece;
            }
            if (!comma) break;
            cursor = comma + 1;
        }
    }

    *out = list;
    *out_n = n;
    return 0;
}

static int cli_cmd_queue(int argc, char **argv) {
    if (argc >= 4 && argv[2][0] != '-' && cli_help_requested(argc, argv, 3) &&
        cli_help_print_command(argv[0], "queue", argv[2]) == 0) {
        return CLI_EXIT_OK;
    }
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "queue", NULL);
        return CLI_EXIT_OK;
    }

    if (argc < 3) {
        cli_err("cli.err.queue.missing_subcommand");
        return CLI_EXIT_USAGE;
    }
    const char *action = argv[2];

    if (strcmp(action, "list") == 0) {
        int offset = 0;
        int count = 0;   /* 0 = 核心默认页 */
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
                offset = atoi(argv[++i]);
            } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
                count = atoi(argv[++i]);
            } else {
                cli_err("cli.err.queue.unknown_list_option", argv[i]);
                return CLI_EXIT_USAGE;
            }
        }
        if (offset < 0) {
            cli_err("cli.err.queue.negative_offset");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_queue_list(cli_bus(), offset, count);
    }

    if (strcmp(action, "clear") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_queue_clear(cli_bus());
    }
    if (strcmp(action, "shuffle") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_queue_shuffle(cli_bus());
    }

    if (strcmp(action, "remove") == 0) {
        if (argc < 4) {
            cli_err("cli.err.queue.remove_needs_pos");
            return CLI_EXIT_USAGE;
        }
        int pos = atoi(argv[3]);
        if (pos < 0) {
            cli_err("cli.err.queue.remove_negative");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_queue_remove(cli_bus(), pos);
    }

    if (strcmp(action, "append") == 0) {
        if (argc < 4) {
            cli_err("cli.err.queue.append_needs_path");
            return CLI_EXIT_USAGE;
        }
        char **paths = NULL;
        int n = 0;
        if (cli_split_comma_paths(argv, 3, argc, &paths, &n) != 0 || n <= 0) {
            cli_err("cli.err.queue.append_parse");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc == CLI_EXIT_OK) {
            rc = cli_client_queue_append(cli_bus(), paths, n);
        }
        for (int i = 0; i < n; i++) free(paths[i]);
        free(paths);
        return rc;
    }

    if (strcmp(action, "insert") == 0) {
        if (argc < 5) {
            cli_err("cli.err.queue.insert_needs_args");
            return CLI_EXIT_USAGE;
        }
        int pos = atoi(argv[3]);
        const char *path = argv[4];
        if (pos < 0) {
            cli_err("cli.err.queue.insert_negative");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_queue_insert(cli_bus(), pos, path);
    }

    cli_err("cli.err.queue.unknown_subcommand", action);
    return CLI_EXIT_USAGE;
}

/* ── config ─────────────────────────────────────────────────────── */

static int cli_cmd_config(int argc, char **argv) {
    if (argc >= 4 && argv[2][0] != '-' && cli_help_requested(argc, argv, 3) &&
        cli_help_print_command(argv[0], "config", argv[2]) == 0) {
        return CLI_EXIT_OK;
    }
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "config", NULL);
        return CLI_EXIT_OK;
    }

    if (argc < 3) {
        cli_err("cli.err.config.missing_subcommand");
        return CLI_EXIT_USAGE;
    }
    const char *action = argv[2];

    if (strcmp(action, "get") == 0) {
        const char *key = (argc >= 4) ? argv[3] : NULL;
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_config_get(cli_bus(), key);
    }

    if (strcmp(action, "set") == 0) {
        if (argc < 5) {
            cli_err("cli.err.config.set_needs_args");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        return cli_client_config_set(cli_bus(), argv[3], argv[4]);
    }

    if (strcmp(action, "reload") == 0) {
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        rc = cli_client_config_reload(cli_bus());
        if (rc == CLI_EXIT_OK) {
            cli_out("cli.msg.config.reloaded");
        }
        return rc;
    }

    if (strcmp(action, "reset") == 0) {
        /* 防误触：重置会覆盖全部配置，必须显式 --yes 确认 */
        int confirmed = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--yes") == 0) confirmed = 1;
            else {
                cli_err("cli.err.config.unknown_reset_option", argv[i]);
                return CLI_EXIT_USAGE;
            }
        }
        if (!confirmed) {
            cli_err("cli.err.config.reset_confirm");
            cli_err("cli.err.config.reset_confirm_hint");
            return CLI_EXIT_USAGE;
        }
        int rc = cli_require_instance();
        if (rc != CLI_EXIT_OK) return rc;
        rc = cli_client_config_reset(cli_bus());
        if (rc == CLI_EXIT_OK) {
            cli_out("cli.msg.config.reset");
        }
        return rc;
    }

    cli_err("cli.err.config.unknown_subcommand", action);
    return CLI_EXIT_USAGE;
}

/* ── lyrics / track / cover ─────────────────────────────────────── */

static int cli_cmd_lyrics(int argc, char **argv) {
    int lines = 0;   /* <=0 = 当前歌词行上下文 */
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "lyrics", NULL);
        return CLI_EXIT_OK;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--lines") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--lines=", 8) == 0) {
            lines = atoi(argv[i] + 8);
        } else {
            cli_err("cli.err.lyrics.unknown_option", argv[i]);
            return CLI_EXIT_USAGE;
        }
    }
    int rc = cli_require_instance();
    if (rc != CLI_EXIT_OK) return rc;
    return cli_client_lyrics(cli_bus(), lines);
}

static int cli_cmd_track(int argc, char **argv) {
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "track", NULL);
        return CLI_EXIT_OK;
    }
    for (int i = 2; i < argc; i++) {
        cli_err("cli.err.track.no_options", argv[i]);
        return CLI_EXIT_USAGE;
    }
    int rc = cli_require_instance();
    if (rc != CLI_EXIT_OK) return rc;
    return cli_client_track(cli_bus());
}

static int cli_cmd_cover(int argc, char **argv) {
    int width = 0;   /* <=0 = 沿用配置 */
    int height = 0;
    const char *charset = NULL;
    if (cli_help_requested(argc, argv, 2)) {
        cli_help_print_command(argv[0], "cover", NULL);
        return CLI_EXIT_OK;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--charset") == 0 && i + 1 < argc) {
            charset = argv[++i];
        } else {
            cli_err("cli.err.cover.unknown_option", argv[i]);
            return CLI_EXIT_USAGE;
        }
    }
    int rc = cli_require_instance();
    if (rc != CLI_EXIT_OK) return rc;
    return cli_client_cover(cli_bus(), width, height, charset);
}

/* ── 入口 ───────────────────────────────────────────────────────── */

int cli_run(int argc, char **argv) {
    ensure_utf8_locale();
    /* main() 已调用时以下两步幂等：--lang 已在任意位置被移除，语言已初始化 */
    cli_lang_extract_option(&argc, argv);
    cli_lang_init();
    cli_extract_global_options(&argc, argv);

    if (argc < 2) {
        cli_help_print_overview(argv[0]);
        return CLI_EXIT_OK;
    }

    const char *command = argv[1];

    if (strcmp(command, "help") == 0) {
        if (argc == 2) {
            cli_help_print_overview(argv[0]);
            return CLI_EXIT_OK;
        }
        if (strcmp(argv[2], "all") == 0) {
            cli_help_print_all(argv[0]);
            return CLI_EXIT_OK;
        }
        int rc = (argc >= 4)
                     ? cli_help_print_command(argv[0], argv[2], argv[3])
                     : cli_help_print_command(argv[0], argv[2], NULL);
        if (rc != 0) {
            cli_err("cli.err.help.unknown_target", argv[2]);
            cli_err("cli.err.help.hint", argv[0]);
            return CLI_EXIT_USAGE;
        }
        return CLI_EXIT_OK;
    }
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        /* 带多余参数时同样只输出概览 */
        cli_help_print_overview(argv[0]);
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
    if (strcmp(command, "queue") == 0) {
        return cli_cmd_queue(argc, argv);
    }
    if (strcmp(command, "config") == 0) {
        return cli_cmd_config(argc, argv);
    }
    if (strcmp(command, "lyrics") == 0) {
        return cli_cmd_lyrics(argc, argv);
    }
    if (strcmp(command, "track") == 0) {
        return cli_cmd_track(argc, argv);
    }
    if (strcmp(command, "cover") == 0) {
        return cli_cmd_cover(argc, argv);
    }
    if (strcmp(command, "keys") == 0) {
        return cli_cmd_keys(argc, argv);
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
    cli_err("cli.err.unknown_command", command);
    cli_help_print_overview(argv[0]);
    return CLI_EXIT_USAGE;
}
