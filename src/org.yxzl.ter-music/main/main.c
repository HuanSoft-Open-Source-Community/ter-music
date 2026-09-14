#include "types.h"
#include "playlist/playlist.h"
#include "ui/dialog.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "config/config.h"
#include "logger/logger.h"
#include "media/session.h"
#include "ui/menus.h"
#include "ui/lyrics.h"
#include "remote/remote.h"
#include "app/open.h"
#include "cli/cli.h"
#include "core/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>

extern void init_ncurses();
extern void create_layout();
extern void run_event_loop();
extern void cleanup();
extern int load_playlist(const char *path);
extern void prompt_open_folder();

int g_debug_enabled = 0;
/* g_config_reload_requested / g_should_exit 定义在 core/core.c（声明见 core/core.h） */

/* 崩溃处理用的独立信号栈（sigaltstack）。
 * 若崩溃原因是栈溢出，信号处理器会在已耗尽的栈上运行、立即二次触发，
 * 结果只剩内核的 "Segmentation fault"，看不到任何 backtrace——
 * Linyaps 沙箱内的启动崩溃就是这样被掩盖的。改用独立栈后处理器才能跑完。 */
static void *g_crash_stack = NULL;

void crash_handler(int sig);

static void crash_install_alt_stack(void) {
    const size_t stack_size = SIGSTKSZ < 65536 ? 65536 : (size_t)SIGSTKSZ;
    g_crash_stack = malloc(stack_size);
    if (!g_crash_stack) {
        return;
    }

    stack_t alt_stack;
    memset(&alt_stack, 0, sizeof(alt_stack));
    alt_stack.ss_sp = g_crash_stack;
    alt_stack.ss_size = stack_size;
    alt_stack.ss_flags = 0;
    if (sigaltstack(&alt_stack, NULL) != 0) {
        free(g_crash_stack);
        g_crash_stack = NULL;
        return;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = crash_handler;
    action.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}

void crash_handler(int sig) {
    log_error("main", "Fatal signal %d received! Performing emergency shutdown", sig);
    logger_shutdown();
    endwin();
    cleanup_temp_playlist();
    fprintf(stderr, "致命错误：收到信号 %d\n", sig);
    fprintf(stderr, "Backtrace:\n");
    void *bt[48];
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    fprintf(stderr, "\n");
    exit(1);
}

/* 收到 SIGHUP（终端关闭/挂断）时同时设置退出标志和配置重载标志，
 * 确保程序走安全退出路径而不是继续在后台播放。
 * 保留 g_config_reload_requested 兼容事件循环中的既有检查路径。 */
void sighup_handler(int sig) {
    (void)sig;
    g_config_reload_requested = 1;
    g_should_exit = 1;
}

/* SIGTERM/SIGINT 触发安全退出 */
void sigterm_handler(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static int find_audio_directory_recursive(const char *path, char *found_path, size_t found_path_size, int depth) {
    if (!path || !found_path || found_path_size == 0 || depth > 8) {
        return 0;
    }

    if (app_dir_has_audio_files(path)) {
        snprintf(found_path, found_path_size, "%s", path);
        return 1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char child_path[MAX_PATH_LEN];
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name) >= (int)sizeof(child_path)) {
            continue;
        }

        struct stat st;
        if (lstat(child_path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            continue;
        }

        if (find_audio_directory_recursive(child_path, found_path, found_path_size, depth + 1)) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static void print_usage(const char *prog_name) {
    printf("用法：%s [选项]            进入 TUI 界面\n", prog_name);
    printf("      %s <命令> [选项]      使用 CLI 模式（播放控制 / 信息显示 / 后台播放）\n\n", prog_name);
    printf("TUI 选项：\n");
    printf("  -o, --open <path>    启动时打开指定音乐目录、音频文件或远程URL\n");
    printf("  -d, --debug          启用调试日志（输出到 ter-music-debug.log）\n");
    printf("  -h, --help           显示帮助信息\n");
    printf("\nCLI 命令（用 `%s help` 查看完整列表）：\n", prog_name);
    printf("  play/pause/resume/toggle/stop/next/prev   基础播放控制\n");
    printf("  seek/volume/speed/mode                    跳转、音量、倍速、播放模式\n");
    printf("  show [--json|--watch|--one-line|...]      输出当前播放信息\n");
    printf("  daemon start|stop|restart|status|reload   后台播放进程管理\n");
    printf("\n");
    printf("远程 URL 示例：\n");
    printf("  %s ftp://user:pass@host/path/to/music\n", prog_name);
    printf("  %s sftp://host/path\n", prog_name);
    printf("  %s --open http://webdav-server/music\n", prog_name);
    printf("\n");
    printf("本地示例：\n");
    printf("  %s -o ~/Music\n", prog_name);
    printf("  %s --open /path/to/music\n", prog_name);
    printf("  %s -o /path/to/song.mp3\n", prog_name);
}

int main(int argc, char *argv[]) {
    /* 崩溃处理器必须装在独立信号栈上：栈溢出时才能打印 backtrace */
    crash_install_alt_stack();
    signal(SIGHUP, sighup_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    /* CLI 子命令前置分发：必须早于 isatty 检查、logger_init 与 ncurses 初始化，
     * 因为 CLI 客户端不需要终端，daemon 运行时也没有终端。 */
    if (argc > 1 && strcmp(argv[1], "tui") == 0) {
        argc--;
        argv++;
    } else if (argc > 1 && cli_is_command(argv[1])) {
        return cli_run(argc, argv);
    }

    /* 友好提示：首个参数既不是已知子命令，也不像路径/URL 且并不存在时，
     * 多半是命令拼写错误（不改变既有行为，仅追加一行 stderr 提示）。 */
    if (argc > 1 && argv[1][0] != '-' && argv[1][0] != '\0' &&
        strchr(argv[1], '/') == NULL && strstr(argv[1], "://") == NULL) {
        struct stat probe;
        if (stat(argv[1], &probe) != 0) {
            fprintf(stderr, "提示：'%s' 既不是已知命令，也不是存在的路径（用 `%s help` 查看命令列表）。\n",
                    argv[1], argv[0]);
        }
    }

    char *open_path = NULL;
    int opt;
    struct option long_options[] = {
        {"open", required_argument, 0, 'o'},
        {"help", no_argument, 0, 'h'},
        {"debug", no_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "o:hdv", long_options, NULL)) != -1) {
        switch (opt) {
            case 'o':
                open_path = optarg;
                break;
            case 'd':
                g_debug_enabled = 1;
                break;
            case 'v':
                printf("%s %s\n", APP_NAME, APP_VERSION);
                return 0;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!open_path && optind < argc) {
        open_path = argv[optind];
    }

    if (g_debug_enabled) {
        logger_init();
        log_info("main", "=== ter-music %s debug session started ===", APP_VERSION);
        log_info("main", "Debug logging enabled via --debug flag");
    }

    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "错误：ter-music 需要在终端里直接运行。\n");
        fprintf(stderr, "请不要把它通过管道重定向到其他命令。\n");
        return 1;
    }
    
    init_ncurses();
    log_info("main", "ncurses initialized, terminal size: %dx%d", COLS, LINES);

    init_menu_views();
    i18n_init(g_app_config.ui_language);
    set_volume_percent(g_app_config.volume_percent);
    
    if (g_app_config.clear_history_on_startup) {
        clear_dir_history();
    }
    
    init_ffmpeg();
    remote_init();
    log_info("main", "Subsystems initialized");

    g_active_backend = g_app_config.audio_backend;
    init_audio_device();
    media_session_init();
    
    create_layout();
    
    reset_playlist_state();
    
    int loaded = 0;
    int used_fallback = 0;
    int attempted_resume_load = 0;
    int resumed_playback = 0;
    int opened_single_file = 0;
    int opened_track_index = -1;
    char final_path[MAX_PATH_LEN] = "";

    int temp_loaded = load_temp_playlist();
    if (temp_loaded > 0) {
        log_info("main", "Restored temp playlist from previous session");
        loaded = 1;
        /* temp 恢复属于“恢复上次会话”路径，允许后续恢复播放 */
        attempted_resume_load = 1;
        playlist_copy_folder_path(final_path, sizeof(final_path));

        /* 树根保持用户上次浏览的根目录（last_opened_path），
         * 而不是曲目所在的子目录；根目录失效时回退 temp 目录。 */
        if (g_app_config.remember_last_path && g_app_config.last_opened_path[0] != '\0' &&
            strcmp(final_path, g_app_config.last_opened_path) != 0 &&
            load_playlist(g_app_config.last_opened_path) > 0) {
            log_info("main", "Tree root restored to last opened path: '%s'",
                     g_app_config.last_opened_path);
            snprintf(final_path, sizeof(final_path), "%s", g_app_config.last_opened_path);
        } else {
            // 重新扫描目录以更新曲库（检测新增或删除的歌曲）
            if (final_path[0] != '\0') {
                struct stat st;
                if (stat(final_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    load_playlist(final_path);
                }
            }
        }
    }

    if (open_path && strlen(open_path) > 0) {
        log_info("main", "Processing --open path: %s", open_path);
        if (remote_is_remote_path(open_path)) {
            RemoteConnectionConfig rconn;
            if (remote_parse_url(open_path, &rconn) == 0) {
                log_info("main", "Remote URL parsed: protocol=%d host=%s port=%d",
                         rconn.protocol, rconn.host, rconn.port);
                int count = load_remote_playlist(&rconn, rconn.base_path);
                if (count > 0) {
                    log_info("main", "Remote playlist loaded: %d tracks from %s", count, open_path);
                    loaded = 1;
                    snprintf(final_path, sizeof(final_path), "%s", open_path);
                } else {
                    const char *err = remote_strerror();
                    if (err && err[0]) {
                        mvprintw(2, 2, "%s",
                                 i18n_get("main.warn.prefix"));
                        mvprintw(2, 12, "%s", err);
                    } else {
                        mvprintw(2, 2, "%s",
                                 i18n_get("main.warn.no_audio_files"));
                    }
                    mvprintw(3, 2, "%s",
                             i18n_get("main.continue_default"));
                    refresh();
                    used_fallback = 1;
                }
            } else {
                log_warn("main", "Failed to parse remote URL: %s", open_path);
                mvprintw(2, 2, "%s",
                         i18n_get("main.warn.invalid_remote"));
                mvprintw(3, 2, "%s", i18n_get("main.continue_default"));
                refresh();
                used_fallback = 1;
            }
        } else {
        log_info("main", "Loading local path from --open: %s", open_path);

        AppOpenResult open_result = app_open_path(open_path, final_path, sizeof(final_path),
                                                  &opened_track_index, &opened_single_file);
        if (open_result == APP_OPEN_OK) {
            log_info("main", "Local path loaded: '%s', final_path='%s'", open_path, final_path);
            loaded = 1;
        } else {
            log_warn("main", "Failed to load local path '%s' (result=%d)", open_path,
                     (int)open_result);
            const char *error_msg;
            if (open_result == APP_OPEN_ERR_FILE_LOAD) {
                error_msg = i18n_get("main.warn.cannot_open");
                mvprintw(2, 2, "%s", error_msg);
            } else if (open_result == APP_OPEN_ERR_NO_AUDIO) {
                error_msg = i18n_get("main.warn.no_playable");
                mvprintw(2, 2, "%s", error_msg);
            } else {
                mvprintw(2, 2, i18n_get("main.warn.invalid_path"), open_path);
            }
            mvprintw(3, 2, "%s", i18n_get("main.continue_default"));
            refresh();
            used_fallback = 1;
        }
        }
    }

    if (!loaded && !open_path && g_app_config.resume_last_playback &&
        g_app_config.last_played_folder_path[0] != '\0') {
        /* 恢复播放时，树根应保持用户上次浏览的根目录（last_opened_path），
         * 而不是当前曲目所在的子目录（last_played_folder_path）。
         * 仅当未开启“记住上次路径”或根目录不可用时，才回退到曲目目录。 */
        const char *resume_dir = g_app_config.last_played_folder_path;
        if (g_app_config.remember_last_path && g_app_config.last_opened_path[0] != '\0') {
            resume_dir = g_app_config.last_opened_path;
        }
        attempted_resume_load = 1;
        log_info("main", "Attempting resume load from: '%s'", resume_dir);
        /* app_open_path() 会先过 app_dir_has_audio_files()（仅检查直接子文件），
         * 而树根的音频可能全部位于子目录中（正是需要恢复的场景），
         * 因此这里直接使用递归扫描的 load_playlist()。 */
        if (load_playlist(resume_dir) > 0) {
            g_selected_index = 0;
            snprintf(final_path, sizeof(final_path), "%s", resume_dir);
            loaded = 1;
            log_info("main", "Resume path loaded: '%s'", resume_dir);
        } else {
            log_warn("main", "Resume path not found: '%s'", resume_dir);
        }
    }

    if (!loaded) {
        char current_dir[MAX_PATH_LEN];
        char auto_found_path[MAX_PATH_LEN];
        log_info("main", "No playlist loaded, trying auto-detect in current directory");

        if (getcwd(current_dir, sizeof(current_dir)) &&
            find_audio_directory_recursive(current_dir, auto_found_path, sizeof(auto_found_path), 0) &&
            app_open_path(auto_found_path, final_path, sizeof(final_path), NULL, NULL) == APP_OPEN_OK) {
            log_info("main", "Auto-detected music folder: '%s'", auto_found_path);
            loaded = 1;

            if (used_fallback) {
                mvprintw(4, 2,
                         i18n_get("main.auto_detect"),
                         auto_found_path);
                refresh();
            }
        }
    }
    
    if (!loaded && g_app_config.default_startup_path[0] != '\0') {
        log_info("main", "Trying default startup path: '%s'", g_app_config.default_startup_path);
        if (app_open_path(g_app_config.default_startup_path, final_path, sizeof(final_path),
                          NULL, NULL) == APP_OPEN_OK) {
            log_info("main", "Loaded from default path: '%s'", g_app_config.default_startup_path);
            loaded = 1;

            if (used_fallback) {
                mvprintw(4, 2,
                         i18n_get("main.loaded_default"),
                         g_app_config.default_startup_path);
                refresh();
            }
        }
    }
    
    if (!loaded && g_app_config.remember_last_path && g_app_config.last_opened_path[0] != '\0') {
        log_info("main", "Trying last opened path: '%s'", g_app_config.last_opened_path);
        if (app_open_path(g_app_config.last_opened_path, final_path, sizeof(final_path),
                          NULL, NULL) == APP_OPEN_OK) {
            log_info("main", "Loaded from last opened path");
            loaded = 1;
        }
    }
    
    if (loaded && final_path[0] != '\0') {
        add_dir_history_entry(final_path);
        
        if (g_app_config.remember_last_path) {
            snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", final_path);
            save_config();
        }

        if (g_app_config.resume_last_playback && !opened_single_file &&
            ((attempted_resume_load && !open_path) ||
             strcmp(final_path, g_app_config.last_played_folder_path) == 0)) {
            log_info("main", "Attempting to resume playback session");
            resumed_playback = app_resume_saved_playback();
            if (resumed_playback) {
                log_info("main", "Playback session restored: track idx=%d pos=%d",
                         g_current_play_index, g_app_config.last_played_position);
            } else {
                log_info("main", "Resume playback failed, clearing saved session");
                app_clear_saved_session();
            }
        }

        /* Restore queue from disk */
        play_queue_load(&g_play_queue);

        if (!resumed_playback && playlist_count() > 0 &&
            (g_app_config.auto_play_on_start || opened_single_file)) {
            int auto_idx = -1;
            if (opened_single_file) {
                auto_idx = opened_track_index;
                if (auto_idx < 0) {
                    auto_idx = g_sort_state.active ? g_sort_state.sorted_indices[0] : 0;
                }
            } else if (g_app_config.auto_play_on_start) {
                auto_idx = g_sort_state.active ? g_sort_state.sorted_indices[0] : 0;
            }
            if (auto_idx >= 0) {
                log_info("main", "Auto-playing track idx=%d", auto_idx);
                play_audio(auto_idx);
                app_set_selection_for_track(auto_idx);
            }
        }
        if (attempted_resume_load && !open_path && !resumed_playback) {
            app_clear_saved_session();
        }
    } else if (attempted_resume_load) {
        app_clear_saved_session();
    }
    
    /* 歌词状态由 ui/lyrics.c 持有，核心经钩子推进（注册在 run_event_loop 内亦可，
     * 这里显式注册，便于 grep 到前端与核心的边界） */
    core_set_lyrics_tick(update_lyrics_display);

    log_info("main", "Starting event loop");
    run_event_loop();

    log_info("main", "Event loop exited, beginning shutdown");
    play_queue_save(&g_play_queue);
    save_temp_playlist();
    cleanup();
    cleanup_temp_playlist();

    log_info("main", "ter-music exited cleanly");
    logger_shutdown();

    printf("%s\n", i18n_get("app.exit_clean"));
    return 0;
}
