#include "../include/defs.h"
#include "../include/media_session.h"
#include "../include/menu_views.h"
#include "../include/remote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <ncursesw/ncurses.h>
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
volatile sig_atomic_t g_config_reload_requested = 0;

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

void sighup_handler(int sig) {
    (void)sig;
    g_config_reload_requested = 1;
}

static void expand_user_path(const char *input, char *output, size_t output_size) {
    if (!output || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (!input || input[0] == '\0') {
        return;
    }

    if (input[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(output, output_size, "%s%s", home, input + 1);
            return;
        }
    }

    snprintf(output, output_size, "%s", input);
}

static int has_audio_files(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    
    const char *audio_extensions[] = {
        ".mp3", ".MP3", ".wav", ".WAV", ".flac", ".FLAC",
        ".ogg", ".OGG", ".m4a", ".M4A", ".aac", ".AAC",
        ".wma", ".WMA", ".ape", ".APE", ".opus", ".OPUS", NULL
    };
    
    struct dirent *entry;
    int found = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        const char *ext = strrchr(entry->d_name, '.');
        if (ext) {
            for (int i = 0; audio_extensions[i] != NULL; i++) {
                if (strcmp(ext, audio_extensions[i]) == 0) {
                    found = 1;
                    break;
                }
            }
        }
        if (found) break;
    }
    
    closedir(dir);
    return found;
}

static int load_startup_playlist(const char *path, char *final_path, size_t final_path_size) {
    struct stat s;
    if (!path || path[0] == '\0') {
        return 0;
    }

    if (stat(path, &s) != 0) {
        return 0;
    }

    if (S_ISREG(s.st_mode)) {
        // 提取父目录路径
        char dir_buf[MAX_PATH_LEN];
        const char *slash = strrchr(path, '/');
        if (slash && slash > path) {
            size_t len = (size_t)(slash - path);
            memcpy(dir_buf, path, len);
            dir_buf[len] = '\0';
        } else {
            snprintf(dir_buf, sizeof(dir_buf), ".");
        }

        // 加载整个目录以显示同级音乐
        if (load_playlist(dir_buf) <= 0) {
            return 0;
        }

        // 定位被打开的文件
        g_selected_index = playlist_find_track_index_by_path(path);
        if (g_selected_index < 0) {
            g_selected_index = 0;
        }
        // 排序激活时，将物理索引转换为视觉位置
        if (g_sort_state.active) {
            int visual = 0;
            for (int i = 0; i < g_playlist.count; i++) {
                if (g_sort_state.sorted_indices[i] == g_selected_index) {
                    visual = i;
                    break;
                }
            }
            g_selected_index = visual;
        }

        snprintf(final_path, final_path_size, "%s", dir_buf);
        return 1;
    }

    if (!S_ISDIR(s.st_mode)) {
        return 0;
    }

    if (!has_audio_files(path)) {
        return 0;
    }

    if (load_playlist(path) <= 0) {
        return 0;
    }

    g_selected_index = 0;
    snprintf(final_path, final_path_size, "%s", path);
    return 1;
}

static void clear_saved_playback_session(void) {
    g_app_config.resume_last_playback = 0;
    g_app_config.last_played_position = 0;
    g_app_config.last_played_folder_path[0] = '\0';
    g_app_config.last_played_track_path[0] = '\0';
    save_config();
}

static int find_track_index_by_path(const char *track_path) {
    return playlist_find_track_index_by_path(track_path);
}

static int restore_saved_playback_session(void) {
    if (!g_app_config.resume_last_playback || g_app_config.last_played_track_path[0] == '\0') {
        return 0;
    }

    int track_index = find_track_index_by_path(g_app_config.last_played_track_path);
    if (track_index < 0) {
        return 0;
    }

    g_selected_index = track_index;
    g_initial_seek_position = g_app_config.last_played_position;
    play_audio(track_index);
    // 排序激活时，将物理索引转换为视觉位置
    if (g_sort_state.active) {
        int visual = 0;
        for (int i = 0; i < g_playlist.count; i++) {
            if (g_sort_state.sorted_indices[i] == track_index) {
                visual = i;
                break;
            }
        }
        g_selected_index = visual;
    }
    return 1;
}

static int find_audio_directory_recursive(const char *path, char *found_path, size_t found_path_size, int depth) {
    if (!path || !found_path || found_path_size == 0 || depth > 8) {
        return 0;
    }

    if (has_audio_files(path)) {
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
    printf("用法：%s [选项]\n\n", prog_name);
    printf("选项：\n");
    printf("  -o, --open <path>    启动时打开指定音乐目录、音频文件或远程URL\n");
    printf("  -d, --debug          启用调试日志（输出到 ter-music-debug.log）\n");
    printf("  -h, --help           显示帮助信息\n");
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
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGHUP, sighup_handler);

    char *open_path = NULL;
    int opt;
    struct option long_options[] = {
        {"open", required_argument, 0, 'o'},
        {"help", no_argument, 0, 'h'},
        {"debug", no_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "o:hd", long_options, NULL)) != -1) {
        switch (opt) {
            case 'o':
                open_path = optarg;
                break;
            case 'd':
                g_debug_enabled = 1;
                break;
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
    char final_path[MAX_PATH_LEN] = "";

    int temp_loaded = load_temp_playlist();
    if (temp_loaded > 0) {
        log_info("main", "Restored temp playlist from previous session");
        loaded = 1;
        playlist_copy_folder_path(final_path, sizeof(final_path));

        // 重新扫描目录以更新曲库（检测新增或删除的歌曲）
        if (final_path[0] != '\0') {
            struct stat st;
            if (stat(final_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                load_playlist(final_path);
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
                                 use_english_ui()
                                     ? "Warning:" : "警告：");
                        mvprintw(2, 12, "%s", err);
                    } else {
                        mvprintw(2, 2, "%s",
                                 use_english_ui()
                                     ? "Warning: no audio files found"
                                     : "警告：没有找到音频文件");
                    }
                    mvprintw(3, 2, "%s",
                             use_english_ui()
                                 ? "Continuing with current directory and default paths..."
                                 : "将继续尝试当前目录和默认启动路径...");
                    refresh();
                    used_fallback = 1;
                }
            } else {
                log_warn("main", "Failed to parse remote URL: %s", open_path);
                mvprintw(2, 2, "%s",
                         use_english_ui()
                             ? "Warning: invalid remote URL"
                             : "警告：远程 URL 格式无效");
                mvprintw(3, 2, "%s",
                         use_english_ui()
                             ? "Continuing with current directory and default paths..."
                             : "将继续尝试当前目录和默认启动路径...");
                refresh();
                used_fallback = 1;
            }
        } else {
        log_info("main", "Loading local path from --open: %s", open_path);
        char expanded_path[MAX_PATH_LEN];
        expand_user_path(open_path, expanded_path, sizeof(expanded_path));

        struct stat s;
        if (stat(expanded_path, &s) == 0) {
            if (S_ISREG(s.st_mode) || S_ISDIR(s.st_mode)) {
                if (load_startup_playlist(expanded_path, final_path, sizeof(final_path))) {
                    log_info("main", "Local path loaded: '%s', final_path='%s'", expanded_path, final_path);
                    loaded = 1;
                    if (S_ISREG(s.st_mode)) {
                        opened_single_file = 1;
                    }
                } else {
                    log_warn("main", "Failed to load local path: '%s'", expanded_path);
                    const char *error_msg = S_ISREG(s.st_mode)
                        ? (use_english_ui() ? "Warning: cannot open audio file" : "警告：无法打开音频文件")
                        : (use_english_ui() ? "Warning: the selected folder has no playable audio files" : "警告：指定目录中没有可播放的音频文件");
                    
                    mvprintw(2, 2, "%s", error_msg);
                    mvprintw(3, 2, "%s",
                             use_english_ui()
                                 ? "Continuing with current directory and default paths..."
                                 : "将继续尝试当前目录和默认启动路径...");
                    refresh();
                    used_fallback = 1;
                }
            } else {
                mvprintw(2, 2, use_english_ui() ? "Warning: invalid path: %s" : "警告：指定路径无效：%s", open_path);
                mvprintw(3, 2, "%s",
                         use_english_ui()
                             ? "Continuing with current directory and default paths..."
                             : "将继续尝试当前目录和默认启动路径...");
                refresh();
                used_fallback = 1;
            }
        } else {
            mvprintw(2, 2, use_english_ui() ? "Warning: invalid path: %s" : "警告：指定路径无效：%s", open_path);
            mvprintw(3, 2, "%s",
                     use_english_ui()
                         ? "Continuing with current directory and default paths..."
                         : "将继续尝试当前目录和默认启动路径...");
            refresh();
            used_fallback = 1;
        }
        }
    }

    if (!loaded && !open_path && g_app_config.resume_last_playback &&
        g_app_config.last_played_folder_path[0] != '\0') {
        attempted_resume_load = 1;
        log_info("main", "Attempting resume load from: '%s'", g_app_config.last_played_folder_path);
        if (load_startup_playlist(g_app_config.last_played_folder_path, final_path, sizeof(final_path))) {
            log_info("main", "Resume path loaded: '%s'", g_app_config.last_played_folder_path);
            loaded = 1;
        } else {
            log_warn("main", "Resume path not found: '%s'", g_app_config.last_played_folder_path);
        }
    }

    if (!loaded) {
        char current_dir[MAX_PATH_LEN];
        char auto_found_path[MAX_PATH_LEN];
        log_info("main", "No playlist loaded, trying auto-detect in current directory");

        if (getcwd(current_dir, sizeof(current_dir)) &&
            find_audio_directory_recursive(current_dir, auto_found_path, sizeof(auto_found_path), 0) &&
            load_startup_playlist(auto_found_path, final_path, sizeof(final_path))) {
            log_info("main", "Auto-detected music folder: '%s'", auto_found_path);
            loaded = 1;

            if (used_fallback) {
                mvprintw(4, 2,
                         use_english_ui() ? "Auto-detected music folder in current directory: %s"
                                          : "已从当前目录自动找到音乐目录：%s",
                         auto_found_path);
                refresh();
            }
        }
    }
    
    if (!loaded && g_app_config.default_startup_path[0] != '\0') {
        log_info("main", "Trying default startup path: '%s'", g_app_config.default_startup_path);
        if (load_startup_playlist(g_app_config.default_startup_path, final_path, sizeof(final_path))) {
            log_info("main", "Loaded from default path: '%s'", g_app_config.default_startup_path);
            loaded = 1;

            if (used_fallback) {
                mvprintw(4, 2,
                         use_english_ui() ? "Loaded from default path: %s" : "已从默认路径加载：%s",
                         g_app_config.default_startup_path);
                refresh();
            }
        }
    }
    
    if (!loaded && g_app_config.remember_last_path && g_app_config.last_opened_path[0] != '\0') {
        log_info("main", "Trying last opened path: '%s'", g_app_config.last_opened_path);
        if (load_startup_playlist(g_app_config.last_opened_path, final_path, sizeof(final_path))) {
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

        if (g_app_config.resume_last_playback &&
            !opened_single_file &&
            strcmp(final_path, g_app_config.last_played_folder_path) == 0) {
            log_info("main", "Attempting to resume playback session");
            resumed_playback = restore_saved_playback_session();
            if (resumed_playback) {
                log_info("main", "Playback session restored: track idx=%d pos=%d",
                         g_current_play_index, g_app_config.last_played_position);
            } else {
                log_info("main", "Resume playback failed, clearing saved session");
                clear_saved_playback_session();
            }
        }

        if (!resumed_playback && playlist_count() > 0 &&
            (g_app_config.auto_play_on_start || opened_single_file)) {
            log_info("main", "Auto-playing first track");
            int first_idx = g_sort_state.active ? g_sort_state.sorted_indices[0] : 0;
            play_audio(first_idx);
        }
        if (attempted_resume_load &&
            !resumed_playback &&
            strcmp(final_path, g_app_config.last_played_folder_path) != 0) {
            clear_saved_playback_session();
        }
    } else if (attempted_resume_load) {
        clear_saved_playback_session();
    }
    
    log_info("main", "Starting event loop");
    run_event_loop();

    log_info("main", "Event loop exited, beginning shutdown");
    save_temp_playlist();
    cleanup();
    cleanup_temp_playlist();

    log_info("main", "ter-music exited cleanly");
    logger_shutdown();

    printf("%s\n", use_english_ui() ? "ter-music exited cleanly." : "ter-music 已正常退出。");
    return 0;
}
