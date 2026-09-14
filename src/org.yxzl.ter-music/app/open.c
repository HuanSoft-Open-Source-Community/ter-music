/**
 * @file open.c
 * @brief 路径打开与播放会话恢复的共用原语（自 main/main.c 抽取）
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "app/open.h"

#include "audio/audio.h"
#include "audio/play_queue.h"
#include "config/config.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "ui/menus.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const k_audio_extensions[] = {
    ".mp3", ".MP3", ".wav", ".WAV", ".flac", ".FLAC",
    ".ogg", ".OGG", ".m4a", ".M4A", ".aac", ".AAC",
    ".wma", ".WMA", ".ape", ".APE", ".opus", ".OPUS",
    ".wv", ".WV", NULL
};

static void expand_user_path(const char *input, char *output, size_t output_size)
{
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

int app_dir_has_audio_files(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return 0;
    }

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) {
            continue;
        }
        for (int i = 0; k_audio_extensions[i] != NULL; i++) {
            if (strcmp(ext, k_audio_extensions[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    closedir(dir);
    return found;
}

void app_set_selection_for_track(int physical_idx)
{
    if (physical_idx < 0) {
        g_selected_index = 0;
        return;
    }

    if (playlist_tree_is_active()) {
        g_selected_index = playlist_reveal_track(physical_idx);
        if (g_selected_index < 0) {
            g_selected_index = 0;
        }
        return;
    }

    if (g_sort_state.active) {
        g_selected_index = 0;
        for (int i = 0; i < g_playlist.count; i++) {
            if (g_sort_state.sorted_indices[i] == physical_idx) {
                g_selected_index = i;
                break;
            }
        }
        return;
    }

    g_selected_index = physical_idx;
}

AppOpenResult app_open_path(const char *path,
                            char *final_path,
                            size_t final_path_size,
                            int *track_index_out,
                            int *is_single_file_out)
{
    if (final_path && final_path_size > 0) {
        final_path[0] = '\0';
    }
    if (track_index_out) {
        *track_index_out = -1;
    }
    if (is_single_file_out) {
        *is_single_file_out = 0;
    }

    if (!path || path[0] == '\0') {
        return APP_OPEN_ERR_EMPTY;
    }

    char expanded[MAX_PATH_LEN];
    expand_user_path(path, expanded, sizeof(expanded));
    if (expanded[0] == '\0') {
        return APP_OPEN_ERR_EMPTY;
    }

    struct stat st;
    if (stat(expanded, &st) != 0) {
        return APP_OPEN_ERR_INVALID_PATH;
    }

    if (S_ISREG(st.st_mode)) {
        /* 提取父目录路径，加载整个目录以显示同级音乐 */
        char dir_buf[MAX_PATH_LEN];
        const char *slash = strrchr(expanded, '/');
        if (slash && slash > expanded) {
            size_t len = (size_t)(slash - expanded);
            memcpy(dir_buf, expanded, len);
            dir_buf[len] = '\0';
        } else {
            snprintf(dir_buf, sizeof(dir_buf), ".");
        }

        if (load_playlist(dir_buf) <= 0) {
            return APP_OPEN_ERR_FILE_LOAD;
        }

        int physical_idx = playlist_find_track_index_by_path(expanded);
        if (physical_idx < 0) {
            physical_idx = 0;
        }

        app_set_selection_for_track(physical_idx);

        if (final_path && final_path_size > 0) {
            snprintf(final_path, final_path_size, "%s", dir_buf);
        }
        if (track_index_out) {
            *track_index_out = physical_idx;
        }
        if (is_single_file_out) {
            *is_single_file_out = 1;
        }
        return APP_OPEN_OK;
    }

    if (!S_ISDIR(st.st_mode)) {
        return APP_OPEN_ERR_INVALID_PATH;
    }

    if (!app_dir_has_audio_files(expanded)) {
        return APP_OPEN_ERR_NO_AUDIO;
    }

    if (load_playlist(expanded) <= 0) {
        return APP_OPEN_ERR_NO_AUDIO;
    }

    g_selected_index = 0;
    if (final_path && final_path_size > 0) {
        snprintf(final_path, final_path_size, "%s", expanded);
    }
    if (track_index_out) {
        *track_index_out = 0;
    }
    return APP_OPEN_OK;
}

int app_resume_saved_playback(void)
{
    if (!g_app_config.resume_last_playback ||
        g_app_config.last_played_track_path[0] == '\0') {
        return 0;
    }

    int track_index = playlist_find_track_index_by_path(g_app_config.last_played_track_path);
    if (track_index < 0) {
        return 0;
    }

    audio_set_initial_seek_position(g_app_config.last_played_position);
    app_set_selection_for_track(track_index);
    play_audio(track_index);
    return 1;
}

void app_clear_saved_session(void)
{
    g_app_config.resume_last_playback = 0;
    g_app_config.last_played_position = 0;
    g_app_config.last_played_folder_path[0] = '\0';
    g_app_config.last_played_track_path[0] = '\0';
    save_config();
}

int app_restore_session(int honor_autoplay,
                        char *final_path,
                        size_t final_path_size)
{
    if (final_path && final_path_size > 0) {
        final_path[0] = '\0';
    }

    int loaded = 0;
    char path[MAX_PATH_LEN] = "";

    int temp_loaded = load_temp_playlist();
    if (temp_loaded > 0) {
        loaded = 1;
        playlist_copy_folder_path(path, sizeof(path));

        /* 树根保持用户上次浏览的根目录，而不是曲目所在子目录 */
        if (g_app_config.remember_last_path &&
            g_app_config.last_opened_path[0] != '\0' &&
            strcmp(path, g_app_config.last_opened_path) != 0 &&
            load_playlist(g_app_config.last_opened_path) > 0) {
            snprintf(path, sizeof(path), "%s", g_app_config.last_opened_path);
        } else if (path[0] != '\0') {
            /* 重新扫描目录以更新曲库（检测新增或删除的歌曲） */
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                load_playlist(path);
            }
        }
    }

    if (!loaded && g_app_config.default_startup_path[0] != '\0') {
        if (app_open_path(g_app_config.default_startup_path, path, sizeof(path),
                          NULL, NULL) == APP_OPEN_OK) {
            loaded = 1;
        }
    }

    if (!loaded && g_app_config.remember_last_path &&
        g_app_config.last_opened_path[0] != '\0') {
        if (app_open_path(g_app_config.last_opened_path, path, sizeof(path),
                          NULL, NULL) == APP_OPEN_OK) {
            loaded = 1;
        }
    }

    if (!loaded) {
        log_warn("app_open", "No playlist could be restored (no temp playlist, "
                             "no default path, no last opened path)");
        return 0;
    }

    if (final_path && final_path_size > 0) {
        snprintf(final_path, final_path_size, "%s", path);
    }

    if (g_app_config.remember_last_path && path[0] != '\0') {
        snprintf(g_app_config.last_opened_path,
                 sizeof(g_app_config.last_opened_path), "%s", path);
        save_config();
    }

    play_queue_load(&g_play_queue);

    if (honor_autoplay) {
        int resumed = app_resume_saved_playback();
        if (!resumed && g_app_config.auto_play_on_start && playlist_count() > 0) {
            int idx = g_sort_state.active ? g_sort_state.sorted_indices[0] : 0;
            if (idx >= 0 && idx < playlist_count()) {
                play_audio(idx);
                app_set_selection_for_track(idx);
            }
        }
    }

    log_info("app_open", "Session restored: %d tracks, path='%s'",
             playlist_count(), path);
    return playlist_count();
}
