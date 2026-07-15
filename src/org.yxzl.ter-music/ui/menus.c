/**
 * @file menus.c
 * @brief 菜单模块 — 框架层（视图调度、初始化、配置、数据管理）
 *
 * 提供底部菜单栏功能，支持 F1-F8 功能键切换不同界面。
 * 视图具体渲染和输入处理已拆分到各视图模块：
 *   - settings.c   : 设置（主题/路径/播放/远程）
 *   - history.c    : 目录历史
 *   - playlist_view.c : 歌单管理
 *   - favorites.c  : 收藏
 *   - info_view.c  : 关于信息
 *   - help_view.c  : 快速上手帮助
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "ui/dialog.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "config/config.h"
#include "logger/logger.h"
#include "search/search.h"
#include "ui/scrollbar.h"
#include "ui/lyrics.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "remote/remote.h"
#include "config/crypto.h"
#include "config/migration.h"
#include "library/library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

extern void render_playlist_content(void);
extern void render_controls(void);
extern void create_layout(void);
extern int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                             char *buffer, size_t buffer_size, int trim_whitespace,
                             int password_mode, int prefill);

/* ============================================================
 * Config file paths (static to this module)
 * ============================================================ */

static char config_dir[MAX_PATH_LEN];
static char config_file[MAX_PATH_LEN];

/* ============================================================
 * JSON parser helpers
 * Used only by config_migration.c for the v1 (JSON) → v2 (XML) config upgrade.
 * Not used for music data persistence (which uses SQLite exclusively).
 * ============================================================ */

char* extract_json_string(const char *json, const char *key, char *output, size_t output_size)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *pos = strstr(json, search_key);
    if (!pos) { output[0] = '\0'; return output; }

    pos = strchr(pos, ':');
    if (!pos) { output[0] = '\0'; return output; }

    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;

    if (*pos == '"') {
        pos++;
        size_t i = 0;
        while (*pos && *pos != '"' && i < output_size - 1) {
            if (*pos == '\\' && *(pos + 1)) {
                pos++;
                switch (*pos) {
                    case 'n': output[i++] = '\n'; break;
                    case 't': output[i++] = '\t'; break;
                    case '"': output[i++] = '"'; break;
                    case '\\': output[i++] = '\\'; break;
                    default: output[i++] = *pos; break;
                }
                pos++;
            } else {
                output[i++] = *pos++;
            }
        }
        output[i] = '\0';
    } else {
        size_t i = 0;
        while (*pos && *pos != ',' && *pos != '}' && *pos != ']' && i < output_size - 1) {
            if (*pos != ' ' && *pos != '\t' && *pos != '\n' && *pos != '\r') {
                output[i++] = *pos;
            }
            pos++;
        }
        output[i] = '\0';
    }
    return output;
}

long extract_json_int(const char *json, const char *key)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *pos = strstr(json, search_key);
    if (!pos) return 0;
    pos = strchr(pos, ':');
    if (!pos) return 0;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    return atol(pos);
}

double extract_json_float(const char *json, const char *key)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *pos = strstr(json, search_key);
    if (!pos) return 0.0;
    pos = strchr(pos, ':');
    if (!pos) return 0.0;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    return atof(pos);
}

/* ============================================================
 * Config — load / save / init
 * ============================================================ */

void ensure_config_dir_exists(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    snprintf(config_dir, sizeof(config_dir), "%s/.config/ter-music", home);
    snprintf(config_file, sizeof(config_file), "%s/config.xml", config_dir);

    mkdir(config_dir, 0755);
}

const char *get_config_dir(void)
{
    return config_dir[0] ? config_dir : NULL;
}

void init_default_config(void)
{
    memset(&g_app_config, 0, sizeof(AppConfig));

    const char *xdg_music_home = getenv("XDG_MUSIC_HOME");
    if (xdg_music_home && xdg_music_home[0] != '\0') {
        strncpy(g_app_config.default_startup_path, xdg_music_home, MAX_PATH_LEN - 1);
        g_app_config.default_startup_path[MAX_PATH_LEN - 1] = '\0';
    } else {
        const char *home = getenv("HOME");
        if (home) {
            struct stat st;
            char candidate[MAX_PATH_LEN];

            static const char *music_dirs[] = {
                "/Music", "/音乐", "/Música", "/Musique", "/Musik"
            };
            int found = 0;
            for (size_t i = 0; i < sizeof(music_dirs) / sizeof(music_dirs[0]); i++) {
                snprintf(candidate, sizeof(candidate), "%s%s", home, music_dirs[i]);
                if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                    strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
                    g_app_config.default_startup_path[MAX_PATH_LEN - 1] = '\0';
                    found = 1;
                    break;
                }
            }
            if (!found) {
                snprintf(g_app_config.default_startup_path, MAX_PATH_LEN, "%s/Music", home);
            }
        }
    }

    g_app_config.theme.playlist_fg   = COLOR_WHITE;
    g_app_config.theme.playlist_bg   = -1;  /* transparent */
    g_app_config.theme.controls_fg   = COLOR_YELLOW;
    g_app_config.theme.controls_bg   = -1;  /* transparent */
    g_app_config.theme.lyrics_fg     = COLOR_GREEN;
    g_app_config.theme.lyrics_bg     = -1;  /* transparent */
    g_app_config.theme.sidebar_fg    = COLOR_CYAN;
    g_app_config.theme.sidebar_bg    = -1;  /* transparent */
    g_app_config.theme.highlight_fg  = COLOR_BLACK;
    g_app_config.theme.highlight_bg  = COLOR_WHITE;
    g_app_config.theme.border_fg     = COLOR_CYAN;
    g_app_config.theme.border_bg     = -1;  /* transparent */

    g_app_config.auto_play_on_start    = 0;
    g_app_config.remember_last_path    = 1;
    g_app_config.clear_history_on_startup = 0;
    g_app_config.resume_last_playback  = 0;
    g_app_config.last_played_position  = 0;
    g_app_config.last_played_folder_path[0] = '\0';
    g_app_config.last_played_track_path[0]  = '\0';
    strcpy(g_app_config.ui_language, "zh_CN");
    g_app_config.volume_percent        = 100;
    g_app_config.audio_latency_ms      = 80;
    g_app_config.show_lyrics_panel     = 1;
    g_app_config.default_play_mode     = PLAY_MODE_SEQUENTIAL;
    g_app_config.advanced_play_modes_enabled = 0;
    g_app_config.default_playback_speed = 1.0f;
    g_app_config.show_album_cover      = 1;
    g_app_config.lyrics_alignment      = 0;
    g_app_config.sort_mode             = SORT_DEFAULT;
    g_app_config.config_version        = 0;
    g_app_config.remote_connection_count = 0;
    memset(g_app_config.remote_connections, 0, sizeof(g_app_config.remote_connections));
}

void apply_color_theme(void)
{
    if (!has_colors()) return;

    int max_c = COLORS - 1;
    if (max_c < 0) max_c = 7;
#define CLR(v) ((v) == -1 ? -1 : (v) < 0 ? 0 : (v) > max_c ? max_c : (v))

    init_pair(COLOR_PAIR_PLAYLIST,  CLR(g_app_config.theme.playlist_fg),  CLR(g_app_config.theme.playlist_bg));
    init_pair(COLOR_PAIR_CONTROLS,  CLR(g_app_config.theme.controls_fg),  CLR(g_app_config.theme.controls_bg));
    init_pair(COLOR_PAIR_LYRICS,    CLR(g_app_config.theme.lyrics_fg),    CLR(g_app_config.theme.lyrics_bg));
    init_pair(COLOR_PAIR_SIDEBAR,   CLR(g_app_config.theme.sidebar_fg),   CLR(g_app_config.theme.sidebar_bg));
    init_pair(COLOR_PAIR_HIGHLIGHT, CLR(g_app_config.theme.highlight_fg), CLR(g_app_config.theme.highlight_bg));
    init_pair(COLOR_PAIR_BORDER,    CLR(g_app_config.theme.border_fg),    CLR(g_app_config.theme.border_bg));

#undef CLR
}

void load_config(void)
{
    log_info("menu_views", "Loading config from '%s'", config_file);

    /* Try native XML format first */
    init_default_config();
    int loaded = 0;
    if (config_load_from_xml(config_file, &g_app_config) == 0) {
        loaded = 1;
    }

    /* XML not found — check for old JSON config needing migration */
    if (!loaded && config_needs_migration()) {
        log_info("menu_views", "Performing v1 (JSON) → v2 (XML) migration");
        if (config_migrate_v1_to_v2() == 0) {
            if (config_load_from_xml(config_file, &g_app_config) == 0) {
                log_info("menu_views", "Migration successful, config loaded");
                loaded = 1;
            }
        }
        if (!loaded)
            log_warn("menu_views", "Migration attempted but failed to load migrated config");
    }

    if (!loaded) {
        /* Nothing worked — stick with defaults already set by init_default_config */
        log_debug("menu_views", "No valid config found, using defaults");
    }

    /* Migrate old configs (version < 3): change bg=0 (old COLOR_BLACK default)
     * to -1 (COLOR_DEFAULT / transparent) for all background color fields. */
    if (g_app_config.config_version < 4) {
        log_info("menu_views", "Migrating config v%d → v4: bg=0 → -1 (transparent)",
                 g_app_config.config_version);
        #define MIGRATE_BG(field) if ((field) == 0) (field) = -1
        MIGRATE_BG(g_app_config.theme.playlist_bg);
        MIGRATE_BG(g_app_config.theme.controls_bg);
        MIGRATE_BG(g_app_config.theme.lyrics_bg);
        MIGRATE_BG(g_app_config.theme.sidebar_bg);
        MIGRATE_BG(g_app_config.theme.border_bg);
        #undef MIGRATE_BG
        g_app_config.config_version = CONFIG_CURRENT_VERSION;
        save_config();
    }

    g_playback_speed = g_app_config.default_playback_speed;
}

void save_config(void)
{
    log_debug("menu_views", "Saving config to '%s'", config_file);
    g_app_config.config_version = CONFIG_CURRENT_VERSION;
    /* Atomic write: write to temp file first, then rename */
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config_file);
    if (config_save_to_xml(tmp_path, &g_app_config) == 0) {
        rename(tmp_path, config_file);
    }
}

void reload_config(void)
{
    log_info("menu_views", "Reloading config on SIGHUP");
    init_default_config();
    load_config();
    apply_color_theme();
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    g_play_mode = (PlayMode)g_app_config.default_play_mode;
    show_status_message("配置已重新加载 / Config reloaded");
}

/* ============================================================
 * Data management — history / favorites / dir history / playlists
 * ============================================================ */

void load_history(void)
{
    g_play_history.count = 0;
    if (library_is_available()) {
        g_play_history.count = library_history_get_all(g_play_history.entries, MAX_HISTORY_COUNT);
    }
}

void load_favorites(void)
{
    g_favorites.count = 0;
    if (library_is_available()) {
        g_favorites.count = library_favorites_get_all(g_favorites.tracks, MAX_FAVORITES_COUNT);
    }
}

void load_dir_history(void)
{
    g_dir_history.count = 0;
    if (library_is_available()) {
        g_dir_history.count = library_dir_history_get_all(g_dir_history.entries, MAX_DIR_HISTORY_COUNT);
    }
}

void add_dir_history_entry(const char *path)
{
    if (!path || strlen(path) == 0) return;
    log_debug("menu_views", "add_dir_history_entry('%s')", path);

    for (int i = 0; i < g_dir_history.count; i++) {
        if (strcmp(g_dir_history.entries[i].path, path) == 0) {
            for (int j = i; j < g_dir_history.count - 1; j++) {
                g_dir_history.entries[j] = g_dir_history.entries[j + 1];
            }
            g_dir_history.count--;
            break;
        }
    }

    if (g_dir_history.count >= MAX_DIR_HISTORY_COUNT) {
        memmove(&g_dir_history.entries[0], &g_dir_history.entries[1],
                sizeof(DirHistoryEntry) * (MAX_DIR_HISTORY_COUNT - 1));
        g_dir_history.count = MAX_DIR_HISTORY_COUNT - 1;
    }

    strncpy(g_dir_history.entries[g_dir_history.count].path, path, MAX_PATH_LEN - 1);
    g_dir_history.entries[g_dir_history.count].path[MAX_PATH_LEN - 1] = '\0';
    g_dir_history.entries[g_dir_history.count].open_time = time(NULL);
    g_dir_history.count++;

    if (library_is_available())
        library_dir_history_add(path);
}

void clear_dir_history(void)
{
    log_info("menu_views", "Directory history cleared");
    g_dir_history.count = 0;
    if (library_is_available())
        library_dir_history_clear();
}

void load_all_playlists(void)
{
    g_playlist_manager.count = 0;
    if (!library_is_available()) return;

    int ids[MAX_PLAYLISTS_COUNT];
    char names[MAX_PLAYLISTS_COUNT][MAX_PLAYLIST_NAME_LEN];
    int pl_count = library_playlist_get_all(ids, names, MAX_PLAYLISTS_COUNT);
    for (int i = 0; i < pl_count && i < MAX_PLAYLISTS_COUNT; i++) {
        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_manager.count];
        pl->db_id = ids[i];
        strncpy(pl->name, names[i], MAX_PLAYLIST_NAME_LEN - 1);
        pl->name[MAX_PLAYLIST_NAME_LEN - 1] = '\0';
        pl->track_count = library_playlist_get_tracks(ids[i], pl->tracks, MAX_TRACKS);
        pl->created_time  = 0;
        pl->modified_time = 0;
        g_playlist_manager.count++;
    }
}

void save_temp_playlist(void)
{
    int snapshot_count = playlist_count();
    if (!playlist_is_loaded() || snapshot_count == 0) {
        log_debug("menu_views", "save_temp_playlist: nothing to save (loaded=%d count=%d)",
                  playlist_is_loaded(), snapshot_count);
        return;
    }
    log_debug("menu_views", "Saving temp playlist (%d tracks)", snapshot_count);

    char folder_path[MAX_PATH_LEN] = "";
    playlist_copy_folder_path(folder_path, sizeof(folder_path));

    char (*tracks)[MAX_PATH_LEN] = malloc((size_t)snapshot_count * sizeof(*tracks));
    if (!tracks) return;

    int saved_count = 0;
    for (int i = 0; i < snapshot_count; i++) {
        int idx = g_sort_state.active ? g_sort_state.sorted_indices[i] : i;
        if (playlist_get_track_path(idx, tracks[saved_count], MAX_PATH_LEN) == 0) {
            saved_count++;
        }
    }

    if (saved_count > 0 && library_is_available()) {
        library_temp_playlist_save(folder_path, (const char (*)[MAX_PATH_LEN])tracks, saved_count);
    }

    free(tracks);
}

void cleanup_temp_playlist(void)
{
    log_debug("menu_views", "Cleaning up temp playlist");
    if (library_is_available())
        library_temp_playlist_cleanup();
}

static void extract_parent_directory_for_playlist(const char *path, char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) { dest[0] = '\0'; return; }
    dest[0] = '\0';
    if (!path || path[0] == '\0') return;
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    size_t len = (size_t)(slash - path);
    if (len >= dest_size) len = dest_size - 1;
    memcpy(dest, path, len);
    dest[len] = '\0';
}

static int apply_restored_playlist(Playlist *restored, int loaded_count,
                                   int has_multiple_sources, const char *folder_path,
                                   const char *first_track_dir)
{
    if (!restored || loaded_count <= 0) return 0;

    restored->count = loaded_count;
    restored->is_loaded = 1;
    restored->has_multiple_sources = has_multiple_sources;

    struct stat st;
    if (folder_path && folder_path[0] != '\0' &&
        stat(folder_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(restored->folder_path, folder_path, sizeof(restored->folder_path) - 1);
        restored->folder_path[sizeof(restored->folder_path) - 1] = '\0';
    } else if (first_track_dir && first_track_dir[0] != '\0') {
        strncpy(restored->folder_path, first_track_dir, sizeof(restored->folder_path) - 1);
        restored->folder_path[sizeof(restored->folder_path) - 1] = '\0';
    }

    playlist_lock();
    g_playlist = *restored;
    playlist_unlock();
    search_clear();
    recompute_sort_order();
    return loaded_count;
}

int load_temp_playlist(void)
{
    if (!library_is_available()) {
        log_debug("menu_views", "Library not available, cannot load temp playlist");
        return 0;
    }

    char folder_path[MAX_PATH_LEN] = "";
    char (*tracks)[MAX_PATH_LEN] = malloc((size_t)MAX_TRACKS * sizeof(*tracks));
    if (!tracks) return 0;

    int loaded_count = library_temp_playlist_load(folder_path, sizeof(folder_path),
                                                  tracks, MAX_TRACKS);

    if (loaded_count <= 0) {
        log_debug("menu_views", "No temp playlist found in SQLite");
        free(tracks);
        return 0;
    }

    log_info("menu_views", "Loading temp playlist from SQLite (%d tracks)", loaded_count);

    Playlist *restored = calloc(1, sizeof(*restored));
    if (!restored) { free(tracks); return 0; }

    int has_multiple_sources = 0;
    char first_track_dir[MAX_PATH_LEN] = "";

    for (int i = 0; i < loaded_count; i++) {
        if (tracks[i][0] == '\0' || access(tracks[i], R_OK) != 0) continue;

        strncpy(restored->tracks[i], tracks[i], MAX_PATH_LEN - 1);
        restored->tracks[i][MAX_PATH_LEN - 1] = '\0';

        char track_dir[MAX_PATH_LEN];
        extract_parent_directory_for_playlist(tracks[i], track_dir, sizeof(track_dir));
        if (i == 0) {
            snprintf(first_track_dir, sizeof(first_track_dir), "%s", track_dir);
        } else if (!has_multiple_sources && strcmp(first_track_dir, track_dir) != 0) {
            has_multiple_sources = 1;
        }

        if (restored->count < MAX_TRACKS - 1) restored->count++;
    }

    int result = apply_restored_playlist(restored, restored->count, has_multiple_sources,
                                         folder_path, first_track_dir);

    free(tracks);
    free(restored);
    return result;
}

void add_history_entry(Track *track)
{
    if (!track || g_play_history.count >= MAX_HISTORY_COUNT) return;
    log_debug("menu_views", "add_history_entry: '%s' - '%s'", track->title, track->artist);

    if (g_play_history.count > 0) {
        memmove(&g_play_history.entries[1], &g_play_history.entries[0],
                sizeof(HistoryEntry) * (g_play_history.count));
    }

    strncpy(g_play_history.entries[0].path, track->path, MAX_PATH_LEN - 1);
    strncpy(g_play_history.entries[0].title, track->title, MAX_META_LEN - 1);
    strncpy(g_play_history.entries[0].artist, track->artist, MAX_META_LEN - 1);
    g_play_history.entries[0].play_time = time(NULL);

    g_play_history.count++;
    if (g_play_history.count > MAX_HISTORY_COUNT) {
        g_play_history.count = MAX_HISTORY_COUNT;
    }

    if (library_is_available()) {
        library_scan_file(track->path);
        library_history_add(track->path, 0);
    }
}

int add_to_favorites(Track *track)
{
    if (!track || g_favorites.count >= MAX_FAVORITES_COUNT) return -1;

    for (int i = 0; i < g_favorites.count; i++) {
        if (strcmp(g_favorites.tracks[i].path, track->path) == 0) return 0;
    }

    strncpy(g_favorites.tracks[g_favorites.count].path, track->path, MAX_PATH_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].title, track->title, MAX_META_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].artist, track->artist, MAX_META_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].album, track->album, MAX_META_LEN - 1);

    g_favorites.count++;

    if (library_is_available()) {
        library_scan_file(track->path);
        library_favorites_add(track->path);
    }
    return 0;
}

int remove_from_favorites(int index)
{
    char removed_path[MAX_PATH_LEN];
    if (index < 0 || index >= g_favorites.count) return -1;

    strncpy(removed_path, g_favorites.tracks[index].path, MAX_PATH_LEN - 1);
    removed_path[MAX_PATH_LEN - 1] = '\0';

    if (index < g_favorites.count - 1) {
        memmove(&g_favorites.tracks[index], &g_favorites.tracks[index + 1],
                sizeof(Track) * (size_t)(g_favorites.count - index - 1));
    }
    g_favorites.count--;

    if (library_is_available())
        library_favorites_remove(removed_path);

    return 0;
}

int create_user_playlist(const char *name)
{
    if (!name || strlen(name) == 0) return -1;
    if (g_playlist_manager.count >= MAX_PLAYLISTS_COUNT) return -2;

    UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_manager.count];
    memset(pl, 0, sizeof(UserPlaylist));

    strncpy(pl->name, name, MAX_PLAYLIST_NAME_LEN - 1);
    pl->created_time  = time(NULL);
    pl->modified_time = pl->created_time;

    if (library_is_available())
        pl->db_id = library_playlist_create(name);

    g_playlist_manager.count++;
    return 0;
}

int delete_user_playlist(int index)
{
    if (index < 0 || index >= g_playlist_manager.count) return -1;

    if (library_is_available() && g_playlist_manager.playlists[index].db_id > 0)
        library_playlist_delete(g_playlist_manager.playlists[index].db_id);

    for (int i = index; i < g_playlist_manager.count - 1; i++) {
        g_playlist_manager.playlists[i] = g_playlist_manager.playlists[i + 1];
    }
    g_playlist_manager.count--;
    return 0;
}

int rename_user_playlist(int index, const char *new_name)
{
    if (index < 0 || index >= g_playlist_manager.count) return -1;
    if (!new_name || strlen(new_name) == 0) return -2;

    UserPlaylist *pl = &g_playlist_manager.playlists[index];

    if (library_is_available() && pl->db_id > 0)
        library_playlist_rename(pl->db_id, new_name);

    strncpy(pl->name, new_name, MAX_PLAYLIST_NAME_LEN - 1);
    pl->name[MAX_PLAYLIST_NAME_LEN - 1] = '\0';
    pl->modified_time = time(NULL);
    return 0;
}

int add_track_to_playlist(int playlist_idx, Track *track)
{
    if (playlist_idx < 0 || playlist_idx >= g_playlist_manager.count) return -1;
    if (!track) return -2;

    UserPlaylist *pl = &g_playlist_manager.playlists[playlist_idx];
    if (pl->track_count >= MAX_TRACKS) return -3;

    for (int i = 0; i < pl->track_count; i++) {
        if (strcmp(pl->tracks[i].path, track->path) == 0) return 0;
    }

    pl->tracks[pl->track_count] = *track;
    pl->track_count++;
    pl->modified_time = time(NULL);

    if (library_is_available() && pl->db_id > 0) {
        library_scan_file(track->path);
        library_playlist_add_track(pl->db_id, track->path);
    }
    return 0;
}

int remove_track_from_playlist(int playlist_idx, int track_idx)
{
    if (playlist_idx < 0 || playlist_idx >= g_playlist_manager.count) return -1;

    UserPlaylist *pl = &g_playlist_manager.playlists[playlist_idx];
    if (track_idx < 0 || track_idx >= pl->track_count) return -2;

    char removed_path[MAX_PATH_LEN] = "";
    if (library_is_available() && pl->db_id > 0) {
        strncpy(removed_path, pl->tracks[track_idx].path, MAX_PATH_LEN - 1);
        removed_path[MAX_PATH_LEN - 1] = '\0';
    }

    for (int i = track_idx; i < pl->track_count - 1; i++) {
        pl->tracks[i] = pl->tracks[i + 1];
    }
    pl->track_count--;
    pl->modified_time = time(NULL);

    if (library_is_available() && pl->db_id > 0 && removed_path[0])
        library_playlist_remove_track_by_path(pl->db_id, removed_path);

    return 0;
}

/* ============================================================
 * JSON migration — one-time v1 → v2
 * ============================================================ */

void try_migrate_from_json(void)
{
    if (!library_is_available()) return;
    const char *home = getenv("HOME");
    if (!home) return;

    char cfg[MAX_PATH_LEN];
    snprintf(cfg, sizeof(cfg), "%s/.config/ter-music", home);

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/history.json", cfg);
    bool has_history   = (access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/favorites.json", cfg);
    bool has_favs      = (access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/dir_history.json", cfg);
    bool has_dirhist   = (access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/playlists", cfg);
    bool has_playlists = (access(path, F_OK) == 0);

    if (!has_history && !has_favs && !has_dirhist && !has_playlists)
        return;

    if (library_history_get_count() > 0 || library_favorites_get_count() > 0)
        return;

    log_info("menu_views", "Migrating legacy v1 JSON data to SQLite...");

#define READ_FILE(p, buf, len) do { \
        FILE *f_ = fopen(p, "r"); \
        if (!f_) { buf = NULL; len = 0; } \
        else { \
            fseek(f_, 0, SEEK_END); len = ftell(f_); fseek(f_, 0, SEEK_SET); \
            buf = malloc((size_t)(len) + 1); \
            if (buf) { fread(buf, 1, (size_t)len, f_); buf[len] = '\0'; } \
            fclose(f_); \
        } \
    } while (0)

#define FOR_EACH_PATH(buf, body) do { \
        char *pos_ = (buf); \
        while ((pos_ = strstr(pos_, "\"path\":\"")) != NULL) { \
            pos_ += 8; \
            char p_[MAX_PATH_LEN]; int i_ = 0; \
            while (*pos_ && *pos_ != '"' && i_ < MAX_PATH_LEN - 1) { \
                if (*pos_ == '\\' && *(pos_ + 1)) pos_++; \
                p_[i_++] = *pos_++; \
            } \
            p_[i_] = '\0'; \
            if (p_[0]) { body } \
            if (pos_) pos_++; \
        } \
    } while (0)

    /* history.json → play_history */
    if (has_history) {
        snprintf(path, sizeof(path), "%s/history.json", cfg);
        char *buf = NULL; long len = 0;
        READ_FILE(path, buf, len);
        if (buf) {
            FOR_EACH_PATH(buf, {
                library_scan_file(p_);
                library_history_add(p_, 0);
            });
            free(buf);
        }
    }

    /* favorites.json → favorites */
    if (has_favs) {
        snprintf(path, sizeof(path), "%s/favorites.json", cfg);
        char *buf = NULL; long len = 0;
        READ_FILE(path, buf, len);
        if (buf) {
            FOR_EACH_PATH(buf, {
                library_scan_file(p_);
                library_favorites_add(p_);
            });
            free(buf);
        }
    }

    /* dir_history.json → dir_history */
    if (has_dirhist) {
        snprintf(path, sizeof(path), "%s/dir_history.json", cfg);
        char *buf = NULL; long len = 0;
        READ_FILE(path, buf, len);
        if (buf) {
            FOR_EACH_PATH(buf, {
                library_dir_history_add(p_);
            });
            free(buf);
        }
    }

    /* playlists/*.json → playlists */
    if (has_playlists) {
        snprintf(path, sizeof(path), "%s/playlists", cfg);
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char fp[MAX_PATH_LEN];
                snprintf(fp, sizeof(fp), "%s/playlists/%s", cfg, entry->d_name);

                char *buf = NULL; long len = 0;
                READ_FILE(fp, buf, len);
                if (!buf) continue;

                char *name_val = strstr(buf, "\"name\":\"");
                if (name_val) {
                    name_val += 8;
                    char pl_name[MAX_PLAYLIST_NAME_LEN];
                    int ni = 0;
                    while (*name_val && *name_val != '"' && ni < MAX_PLAYLIST_NAME_LEN - 1) {
                        if (*name_val == '\\' && *(name_val + 1)) name_val++;
                        pl_name[ni++] = *name_val++;
                    }
                    pl_name[ni] = '\0';

                    if (pl_name[0]) {
                        int pl_id = library_playlist_create(pl_name);
                        if (pl_id > 0) {
                            FOR_EACH_PATH(buf, {
                                library_scan_file(p_);
                                library_playlist_add_track(pl_id, p_);
                            });
                        }
                    }
                }
                free(buf);
            }
            closedir(dir);
        }
    }

#undef READ_FILE
#undef FOR_EACH_PATH

    log_info("menu_views", "JSON migration to SQLite complete");

    load_history();
    load_favorites();
    load_dir_history();
    load_all_playlists();
}

void init_all_persistent_data(void)
{
    ensure_config_dir_exists();
    load_config();
    apply_color_theme();
    g_play_mode = (PlayMode)g_app_config.default_play_mode;

    library_init();

    load_history();
    load_favorites();
    load_dir_history();
    load_all_playlists();

    try_migrate_from_json();
}

/* ============================================================
 * Initialisation
 * ============================================================ */

void init_menu_views(void)
{
    log_info("menu_views", "Initializing menu views");
    g_current_view = VIEW_MAIN;
    g_menu_selected_idx = 0;
    g_content_selected_idx = 0;
    g_focus_area = FOCUS_SIDEBAR;

    init_all_persistent_data();
}

/* ============================================================
 * Menu frame / sidebar rendering
 * ============================================================ */

void render_menu_frame(const char *title)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

        bkgdset(' ' | COLOR_PAIR(0));  /* transparent bg */
    clear();

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));
    rounded_box(stdscr);
    mvprintw(0, 2, " %s ", resolve_menu_title(title));

    int menu_width = max_x / 4;
    int vline_len = max_y - 3;
    if (vline_len < 1) vline_len = 1;
    for (int i = 0; i < vline_len; i++)
        mvaddstr(1 + i, menu_width, "\xe2\x94\x82"); /* │ */
    mvaddstr(1, menu_width, "\xe2\x94\xac");          /* ┬ U+252C */
    mvaddstr(max_y - 3, menu_width, "\xe2\x94\xb4");  /* ┴ U+2534 */

    if (strlen(get_status_message()) > 0 && (time(NULL) - get_status_message_time()) < 3) {
        attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        mvprintw(max_y - 2, 2, "%s", get_status_message());
        attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    }

    refresh();
}

void render_menu_sidebar(int selected_idx, const char **items, int item_count)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int start_y = 2;
    items = resolve_sidebar_items(items);

    /* clear sidebar area with A_NORMAL (no color pair → transparent) */
    for (int y = start_y; y < max_y - 2; y++) {
        mvhline(y, 1, ' ', menu_width - 1);
    }

    /* draw sidebar text items with color pair */
    attron(COLOR_PAIR(COLOR_PAIR_SIDEBAR));
    for (int i = 0; i < item_count && (start_y + i) < max_y - 2; i++) {
        if (i == selected_idx && g_focus_area == FOCUS_SIDEBAR) {
            attron(A_REVERSE);
            mvprintw(start_y + i, 2, "%s", items[i]);
            attroff(A_REVERSE);
        } else {
            mvprintw(start_y + i, 2, "%s", items[i]);
        }
    }
    attroff(COLOR_PAIR(COLOR_PAIR_SIDEBAR));
    refresh();
}

void render_menu_hint_bar(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    short fg, bg;
    pair_content(COLOR_PAIR_BORDER, &fg, &bg);
    log_info("hint_bar", "called: max_y=%d max_x=%d pair=%d fg=%d bg=%d theme_border_fg=%d theme_border_bg=%d",
             max_y, max_x, COLOR_PAIR_BORDER, (int)fg, (int)bg,
             (int)g_app_config.theme.border_fg, (int)g_app_config.theme.border_bg);

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));
    for (int x = 0; x < max_x; x++)
        mvaddch(max_y - 1, x, ' ');
    mvprintw(max_y - 1, 2, "%s",
             i18n_get("menu.hint_bar"));
    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();

    log_info("hint_bar", "rendered: row=%d len=%d", max_y - 1, max_x);
}

/* ============================================================
 * View switching
 * ============================================================ */

void switch_to_view(ViewMode view)
{
    log_info("menu_views", "View switched: %d -> %d", g_current_view, view);
    g_current_view = view;
    g_menu_selected_idx = 0;
    g_content_selected_idx = 0;
    g_focus_area = FOCUS_SIDEBAR;

    /* Reset per-view state */
    reset_settings_view();
    reset_playlist_view();
    reset_help_view();
    reset_language_view();
    g_history_content_offset  = 0;
    g_playlist_content_offset = 0;
    g_favorites_content_offset = 0;

    switch (view) {
        case VIEW_SETTINGS:
            render_menu_frame(i18n_get("menu.settings"));
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            render_menu_hint_bar();
            break;
        case VIEW_HISTORY:
            render_menu_frame(i18n_get("menu.history"));
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            render_history_content();
            render_menu_hint_bar();
            break;
        case VIEW_PLAYLIST:
            render_menu_frame(i18n_get("menu.playlists"));
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            render_playlist_manager_content();
            render_menu_hint_bar();
            break;
        case VIEW_FAVORITES:
            render_menu_frame(i18n_get("menu.favorites"));
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            render_favorites_content();
            render_menu_hint_bar();
            break;
        case VIEW_INFO:
            render_menu_frame(i18n_get("menu.info"));
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            render_info_content();
            render_menu_hint_bar();
            break;
        case VIEW_HELP:
            render_menu_frame(i18n_get("menu.help"));
            render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            render_help_content();
            render_menu_hint_bar();
            break;
        case VIEW_LANGUAGE:
            render_menu_frame(i18n_get("menu.language"));
            render_menu_sidebar(g_menu_selected_idx, language_sidebar_items, LANGUAGE_ITEM_COUNT);
            render_language_content();
            render_menu_hint_bar();
            break;
        default:
            break;
    }
}

void exit_current_view(void)
{
    g_current_view = VIEW_MAIN;
    g_menu_selected_idx = 0;
    g_content_selected_idx = 0;
    g_focus_area = FOCUS_SIDEBAR;

    clear();
    create_layout();
    render_playlist_content();
    render_controls();
    render_lyrics();
}

void rerender_active_view(void)
{
    if (g_current_view == VIEW_MAIN) {
        render_playlist_content();
        render_controls();
        render_lyrics();
        render_menu_hint_bar();
        return;
    }

    switch (g_current_view) {
        case VIEW_SETTINGS:
            render_menu_frame(i18n_get("menu.settings"));
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            render_menu_hint_bar();
            break;
        case VIEW_HISTORY:
            render_menu_frame(i18n_get("menu.history"));
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            render_history_content();
            render_menu_hint_bar();
            break;
        case VIEW_PLAYLIST:
            render_menu_frame(i18n_get("menu.playlists"));
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            render_playlist_manager_content();
            render_menu_hint_bar();
            break;
        case VIEW_FAVORITES:
            render_menu_frame(i18n_get("menu.favorites"));
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            render_favorites_content();
            render_menu_hint_bar();
            break;
        case VIEW_INFO:
            render_menu_frame(i18n_get("menu.info"));
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            render_info_content();
            render_menu_hint_bar();
            break;
        case VIEW_HELP:
            render_menu_frame(i18n_get("menu.help"));
            render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            render_help_content();
            render_menu_hint_bar();
            break;
        case VIEW_LANGUAGE:
            render_menu_frame(i18n_get("menu.language"));
            render_menu_sidebar(g_menu_selected_idx, language_sidebar_items, LANGUAGE_ITEM_COUNT);
            render_language_content();
            render_menu_hint_bar();
            break;
        default:
            break;
    }
}

void toggle_ui_language(void)
{
    if (strcmp(g_app_config.ui_language, "zh_CN") == 0)
        strcpy(g_app_config.ui_language, "en_US");
    else
        strcpy(g_app_config.ui_language, "zh_CN");
    i18n_reload(g_app_config.ui_language);
    save_config();
    help_free_lines();
    rerender_active_view();
}

/* ============================================================
 * Function keys
 * ============================================================ */

void handle_function_keys(int fkey)
{
    log_debug("menu_views", "Function key F%d pressed", fkey - KEY_F(0));
    switch (fkey) {
        case KEY_F(1): exit_current_view(); break;
        case KEY_F(2): switch_to_view(VIEW_SETTINGS); break;
        case KEY_F(3): switch_to_view(VIEW_HISTORY); break;
        case KEY_F(4): switch_to_view(VIEW_PLAYLIST); break;
        case KEY_F(5): switch_to_view(VIEW_FAVORITES); break;
        case KEY_F(6): switch_to_view(VIEW_INFO); break;
        case KEY_F(7): switch_to_view(VIEW_LANGUAGE); break;
        case KEY_F(8): switch_to_view(VIEW_HELP); break;
        case KEY_F(9):
            cleanup();
            printf("%s\n", i18n_get("app.exit_clean"));
            exit(0);
            break;
        default: break;
    }
}

/* ============================================================
 * Main menu input dispatcher
 * ============================================================ */

void handle_menu_input(int ch)
{
    if (g_current_view == VIEW_INFO) {
        extern void check_konami_input(int);
        check_konami_input(ch);
    }

    if (ch == 27) {
        if (g_current_view == VIEW_PLAYLIST) {
            handle_playlist_input(ch);
            return;
        }
        exit_current_view();
        return;
    }

    if (ch >= KEY_F(1) && ch <= KEY_F(9)) {
        handle_function_keys(ch);
        return;
    }

    switch (g_current_view) {
        case VIEW_SETTINGS:  handle_settings_input(ch); break;
        case VIEW_HISTORY:   handle_history_input(ch); break;
        case VIEW_PLAYLIST:  handle_playlist_input(ch); break;
        case VIEW_FAVORITES: handle_favorites_input(ch); break;
        case VIEW_INFO:      handle_info_input(ch); break;
        case VIEW_HELP:      handle_help_input(ch); break;
        case VIEW_LANGUAGE:  handle_language_input(ch); break;
        default: break;
    }
}
