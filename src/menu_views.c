/**
 * @file menu_views.c
 * @brief 菜单视图模块实现
 * 
 * 实现底部菜单栏功能，支持 F1-F6 功能键切换不同界面
 * 包括：设置、历史记录、播放列表、收藏夹、信息、退出
 * 
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-03-22
 */

#include "../include/defs.h"
#include "../include/search.h"
#include "../include/scrollbar.h"
#include "../include/lyrics.h"
#include "../include/menu_views.h"
#include "../include/remote.h"
#include "../include/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

extern void render_playlist_content(void);
extern void render_controls(void);
extern void create_layout(void);
extern int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                             char *buffer, size_t buffer_size, int trim_whitespace,
                             int password_mode, int prefill);

ViewMode g_current_view = VIEW_MAIN;
int g_menu_selected_idx = 0;
PlayHistory g_play_history = {0};
Favorites g_favorites = {0};
DirHistory g_dir_history = {0};
PlaylistManager g_playlist_manager = {0};
AppConfig g_app_config = {0};

int g_content_selected_idx = 0;
FocusArea g_focus_area = FOCUS_SIDEBAR;

static char config_dir[MAX_PATH_LEN];
static char config_file[MAX_PATH_LEN];
static char history_file[MAX_PATH_LEN];
static char favorites_file[MAX_PATH_LEN];
static char dir_history_file[MAX_PATH_LEN];
static char playlists_dir[MAX_PATH_LEN];
static char temp_playlist_file[MAX_PATH_LEN];

static char g_status_message[256] = "";
static time_t g_status_message_time = 0;

static const char *settings_sidebar_items[] = {
    "颜色主题",
    "默认路径",
    "播放设置",
    "快捷键",
    "远程设备",
    "← 返回"
};
#define SETTINGS_ITEM_COUNT 6

static const char *history_sidebar_items[] = {
    "目录历史",
    "清空历史",
    "← 返回"
};
#define HISTORY_ITEM_COUNT 3

static const char *playlist_sidebar_items[] = {
    "全部歌单",
    "新建歌单",
    "← 返回"
};
#define PLAYLIST_ITEM_COUNT 3

static const char *favorites_sidebar_items[] = {
    "全部收藏",
    "← 返回"
};
#define FAVORITES_ITEM_COUNT 2

static const char *info_sidebar_items[] = {
    "关于",
    "仓库地址",
    "← 返回"
};
#define INFO_ITEM_COUNT 3

static const char *help_sidebar_items[] = {
    "快速上手",
    "← 返回"
};
#define HELP_ITEM_COUNT 2

static const char *settings_sidebar_items_ascii[] = {
    "Theme",
    "Default Path",
    "Playback",
    "Hotkeys",
    "Remote Devices",
    "<- Back"
};

static const char *history_sidebar_items_ascii[] = {
    "Folder History",
    "Clear History",
    "<- Back"
};

static const char *playlist_sidebar_items_ascii[] = {
    "All Playlists",
    "New Playlist",
    "<- Back"
};

static const char *favorites_sidebar_items_ascii[] = {
    "All Favorites",
    "<- Back"
};

static const char *info_sidebar_items_ascii[] = {
    "About",
    "Repository",
    "<- Back"
};

static const char *help_sidebar_items_ascii[] = {
    "Quick Start",
    "<- Back"
};

static const char *color_names[] = {
    "黑色", "红色", "绿色", "黄色",
    "蓝色", "洋红", "青色", "白色"
};

static const char *color_names_ascii[] = {
    "Black", "Red", "Green", "Yellow",
    "Blue", "Magenta", "Cyan", "White"
};

static int ncurses_colors[] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
};

static const char *menu_text(const char *utf8, const char *ascii) {
    return use_english_ui() ? ascii : utf8;
}

static const char *menu_bool_text(int enabled) {
    return enabled ? menu_text("是", "On") : menu_text("否", "Off");
}

static const char *menu_color_name(int color_value) {
    const char **names = use_english_ui() ? color_names_ascii : color_names;
    if (color_value < 0 || color_value >= 8) {
        return menu_text("未知", "Unknown");
    }
    return names[color_value];
}

static const char *menu_language_name(int language) {
    return language == UI_LANG_EN ? "English" : menu_text("中文", "Chinese");
}

static void sanitize_ascii_menu_text(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }

    size_t write = 0;
    int prev_space = 1;
    int saw_non_ascii = 0;

    for (size_t read = 0; src[read] != '\0' && write + 1 < dest_size; read++) {
        unsigned char c = (unsigned char)src[read];

        if (c < 0x80) {
            if (isspace(c)) {
                if (!prev_space) {
                    dest[write++] = ' ';
                    prev_space = 1;
                }
            } else if (isprint(c)) {
                dest[write++] = (char)c;
                prev_space = 0;
            }
        } else {
            saw_non_ascii = 1;
            if (!prev_space && write + 1 < dest_size) {
                dest[write++] = ' ';
                prev_space = 1;
            }
        }
    }

    while (write > 0 && dest[write - 1] == ' ') {
        write--;
    }
    dest[write] = '\0';

    if (write == 0 && saw_non_ascii) {
        snprintf(dest, dest_size, "[status]");
    }
}

static const char **resolve_sidebar_items(const char **items) {
    if (!use_english_ui()) {
        return items;
    }
    if (items == settings_sidebar_items) {
        return settings_sidebar_items_ascii;
    }
    if (items == history_sidebar_items) {
        return history_sidebar_items_ascii;
    }
    if (items == playlist_sidebar_items) {
        return playlist_sidebar_items_ascii;
    }
    if (items == favorites_sidebar_items) {
        return favorites_sidebar_items_ascii;
    }
    if (items == info_sidebar_items) {
        return info_sidebar_items_ascii;
    }
    if (items == help_sidebar_items) {
        return help_sidebar_items_ascii;
    }
    return items;
}

static const char *resolve_menu_title(const char *title) {
    if (!use_english_ui() || !title) {
        return title;
    }
    if (strcmp(title, "设置 [F2]") == 0) return "Settings [F2]";
    if (strcmp(title, "历史 [F3]") == 0) return "History [F3]";
    if (strcmp(title, "歌单 [F4]") == 0) return "Playlists [F4]";
    if (strcmp(title, "收藏 [F5]") == 0) return "Favorites [F5]";
    if (strcmp(title, "信息 [F6]") == 0) return "Info [F6]";
    return title;
}

static char* extract_json_string(const char *json, const char *key, char *output, size_t output_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    
    const char *pos = strstr(json, search_key);
    if (!pos) {
        output[0] = '\0';
        return output;
    }
    
    pos = strchr(pos, ':');
    if (!pos) {
        output[0] = '\0';
        return output;
    }
    
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

static long extract_json_int(const char *json, const char *key) {
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

static double extract_json_float(const char *json, const char *key) {
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

static void escape_json_string(const char *src, char *dest, size_t dest_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dest_size - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = c;
            }
        } else if (c == '\n') {
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = 'n';
            }
        } else if (c == '\t') {
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = 't';
            }
        } else {
            dest[j++] = c;
        }
    }
    dest[j] = '\0';
}

static const char *parse_json_string_value(const char *json, char *output, size_t output_size) {
    if (!json || !output || output_size == 0) {
        return NULL;
    }

    while (*json == ' ' || *json == '\t' || *json == '\n' || *json == '\r') {
        json++;
    }
    if (*json != '"') {
        output[0] = '\0';
        return NULL;
    }

    json++;
    size_t i = 0;
    while (*json && *json != '"' && i < output_size - 1) {
        if (*json == '\\' && *(json + 1)) {
            json++;
            switch (*json) {
                case 'n': output[i++] = '\n'; break;
                case 't': output[i++] = '\t'; break;
                case '"': output[i++] = '"'; break;
                case '\\': output[i++] = '\\'; break;
                default: output[i++] = *json; break;
            }
            json++;
        } else {
            output[i++] = *json++;
        }
    }
    output[i] = '\0';

    if (*json == '"') {
        json++;
    }

    return json;
}

#define TEMP_PLAYLIST_MAGIC "TER_MUSIC_TEMP_PLAYLIST_V2"

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void trim_line_end(char *line) {
    if (!line) {
        return;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

static void escape_temp_playlist_field(const char *src, char *dest, size_t dest_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;

    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!src) {
        return;
    }

    for (size_t i = 0; src[i] && j < dest_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' || c < 0x20) {
            if (j + 3 >= dest_size) {
                break;
            }
            dest[j++] = '%';
            dest[j++] = hex[(c >> 4) & 0x0F];
            dest[j++] = hex[c & 0x0F];
        } else {
            dest[j++] = (char)c;
        }
    }

    dest[j] = '\0';
}

static int unescape_temp_playlist_field(const char *src, char *dest, size_t dest_size) {
    size_t j = 0;

    if (!src || !dest || dest_size == 0) {
        return -1;
    }

    while (*src && j < dest_size - 1) {
        if (*src == '%') {
            int hi = hex_digit_value(*(src + 1));
            int lo = hex_digit_value(*(src + 2));
            if (hi < 0 || lo < 0) {
                return -1;
            }
            dest[j++] = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }

        dest[j++] = *src++;
    }

    dest[j] = '\0';
    return 0;
}

static void extract_parent_directory_for_playlist(const char *path, char *dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!path || path[0] == '\0') {
        return;
    }

    const char *slash = strrchr(path, '/');
    if (!slash) {
        return;
    }

    size_t len = (size_t)(slash - path);
    if (len >= dest_size) {
        len = dest_size - 1;
    }
    memcpy(dest, path, len);
    dest[len] = '\0';
}

void ensure_config_dir_exists(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    uid_t uid = getuid();
    snprintf(config_dir, sizeof(config_dir), "%s/.config/ter-music", home);
    snprintf(config_file, sizeof(config_file), "%s/config.json", config_dir);
    snprintf(history_file, sizeof(history_file), "%s/history.json", config_dir);
    snprintf(favorites_file, sizeof(favorites_file), "%s/favorites.json", config_dir);
    snprintf(dir_history_file, sizeof(dir_history_file), "%s/dir_history.json", config_dir);
    snprintf(playlists_dir, sizeof(playlists_dir), "%s/playlists", config_dir);
    snprintf(temp_playlist_file, sizeof(temp_playlist_file), "/tmp/ter-music-%d-temp-playlist.json", (int)uid);

    mkdir(config_dir, 0755);
    mkdir(playlists_dir, 0755);
}

void init_default_config(void) {
    memset(&g_app_config, 0, sizeof(AppConfig));
    
    const char *xdg_music_home = getenv("XDG_MUSIC_HOME");
    if (xdg_music_home && xdg_music_home[0] != '\0') {
        strncpy(g_app_config.default_startup_path, xdg_music_home, MAX_PATH_LEN - 1);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            struct stat st;
            char candidate[MAX_PATH_LEN];
            
            snprintf(candidate, sizeof(candidate), "%s/Music", home);
            if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
            } else {
                snprintf(candidate, sizeof(candidate), "%s/音乐", home);
                if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                    strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
                } else {
                    snprintf(candidate, sizeof(candidate), "%s/Música", home);
                    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                        strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
                    } else {
                        snprintf(candidate, sizeof(candidate), "%s/Musique", home);
                        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                            strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
                        } else {
                            snprintf(candidate, sizeof(candidate), "%s/Musik", home);
                            if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                                strncpy(g_app_config.default_startup_path, candidate, MAX_PATH_LEN - 1);
                            } else {
                                snprintf(g_app_config.default_startup_path, MAX_PATH_LEN, "%s/Music", home);
                            }
                        }
                    }
                }
            }
        }
    }
    
    g_app_config.theme.playlist_fg = COLOR_WHITE;
    g_app_config.theme.playlist_bg = COLOR_BLACK;
    g_app_config.theme.controls_fg = COLOR_YELLOW;
    g_app_config.theme.controls_bg = COLOR_BLACK;
    g_app_config.theme.lyrics_fg = COLOR_GREEN;
    g_app_config.theme.lyrics_bg = COLOR_BLACK;
    g_app_config.theme.sidebar_fg = COLOR_CYAN;
    g_app_config.theme.sidebar_bg = COLOR_BLACK;
    g_app_config.theme.highlight_fg = COLOR_BLACK;
    g_app_config.theme.highlight_bg = COLOR_WHITE;
    g_app_config.theme.border_fg = COLOR_CYAN;
    g_app_config.theme.border_bg = COLOR_BLACK;
    
    g_app_config.auto_play_on_start = 0;
    g_app_config.remember_last_path = 1;
    g_app_config.clear_history_on_startup = 0;
    g_app_config.resume_last_playback = 0;
    g_app_config.last_played_position = 0;
    g_app_config.last_played_folder_path[0] = '\0';
    g_app_config.last_played_track_path[0] = '\0';
    g_app_config.ui_language = UI_LANG_ZH;
    g_app_config.volume_percent = 100;
    g_app_config.audio_latency_ms = 80;
    g_app_config.show_lyrics_panel = 1;
    g_app_config.default_loop_mode = LOOP_OFF;
    g_app_config.default_playback_speed = 1.0f;
    g_app_config.show_album_cover = 1;
    g_app_config.lyrics_alignment = 0;
    g_app_config.sort_mode = SORT_DEFAULT;
    g_app_config.config_version = 0;
    g_app_config.remote_connection_count = 0;
    memset(g_app_config.remote_connections, 0, sizeof(g_app_config.remote_connections));
}

void apply_color_theme(void) {
    if (!has_colors()) return;
    
    init_pair(COLOR_PAIR_PLAYLIST, g_app_config.theme.playlist_fg, g_app_config.theme.playlist_bg);
    init_pair(COLOR_PAIR_CONTROLS, g_app_config.theme.controls_fg, g_app_config.theme.controls_bg);
    init_pair(COLOR_PAIR_LYRICS, g_app_config.theme.lyrics_fg, g_app_config.theme.lyrics_bg);
    init_pair(COLOR_PAIR_SIDEBAR, g_app_config.theme.sidebar_fg, g_app_config.theme.sidebar_bg);
    init_pair(COLOR_PAIR_HIGHLIGHT, g_app_config.theme.highlight_fg, g_app_config.theme.highlight_bg);
    init_pair(COLOR_PAIR_BORDER, g_app_config.theme.border_fg, g_app_config.theme.border_bg);
}

void load_config(void) {
    log_info("menu_views", "Loading config from '%s'", config_file);
    FILE *f = fopen(config_file, "r");
    if (!f) {
        log_debug("menu_views", "Config file not found, using defaults");
        init_default_config();
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        init_default_config();
        return;
    }
    
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);
    
    init_default_config();
    
    extract_json_string(json, "default_startup_path", g_app_config.default_startup_path, MAX_PATH_LEN);
    extract_json_string(json, "last_opened_path", g_app_config.last_opened_path, MAX_PATH_LEN);
    extract_json_string(json, "last_played_folder_path", g_app_config.last_played_folder_path, MAX_PATH_LEN);
    extract_json_string(json, "last_played_track_path", g_app_config.last_played_track_path, MAX_PATH_LEN);
    
    const char *theme_pos = strstr(json, "\"theme\"");
    if (theme_pos) {
        char theme_section[1024];
        const char *theme_start = strchr(theme_pos, '{');
        const char *theme_end = strchr(theme_start ? theme_start : theme_pos, '}');
        if (theme_start && theme_end && theme_end > theme_start) {
            size_t len = theme_end - theme_start + 1;
            if (len < sizeof(theme_section)) {
                strncpy(theme_section, theme_start, len);
                theme_section[len] = '\0';
                
                g_app_config.theme.playlist_fg = (int)extract_json_int(theme_section, "playlist_fg");
                g_app_config.theme.playlist_bg = (int)extract_json_int(theme_section, "playlist_bg");
                g_app_config.theme.controls_fg = (int)extract_json_int(theme_section, "controls_fg");
                g_app_config.theme.controls_bg = (int)extract_json_int(theme_section, "controls_bg");
                g_app_config.theme.lyrics_fg = (int)extract_json_int(theme_section, "lyrics_fg");
                g_app_config.theme.lyrics_bg = (int)extract_json_int(theme_section, "lyrics_bg");
                g_app_config.theme.sidebar_fg = (int)extract_json_int(theme_section, "sidebar_fg");
                g_app_config.theme.sidebar_bg = (int)extract_json_int(theme_section, "sidebar_bg");
                g_app_config.theme.highlight_fg = (int)extract_json_int(theme_section, "highlight_fg");
                g_app_config.theme.highlight_bg = (int)extract_json_int(theme_section, "highlight_bg");
                g_app_config.theme.border_fg = (int)extract_json_int(theme_section, "border_fg");
                g_app_config.theme.border_bg = (int)extract_json_int(theme_section, "border_bg");
            }
        }
    }
    
    g_app_config.auto_play_on_start = (int)extract_json_int(json, "auto_play_on_start");
    g_app_config.remember_last_path = (int)extract_json_int(json, "remember_last_path");
    g_app_config.clear_history_on_startup = (int)extract_json_int(json, "clear_history_on_startup");
    if (strstr(json, "\"resume_last_playback\"")) {
        g_app_config.resume_last_playback = (int)extract_json_int(json, "resume_last_playback");
    }
    if (strstr(json, "\"last_played_position\"")) {
        g_app_config.last_played_position = (int)extract_json_int(json, "last_played_position");
    }
    if (strstr(json, "\"ui_language\"")) {
        g_app_config.ui_language = (int)extract_json_int(json, "ui_language");
    }
    if (strstr(json, "\"volume_percent\"")) {
        g_app_config.volume_percent = (int)extract_json_int(json, "volume_percent");
    }
    if (strstr(json, "\"audio_latency_ms\"")) {
        g_app_config.audio_latency_ms = (int)extract_json_int(json, "audio_latency_ms");
    }
    if (strstr(json, "\"show_lyrics_panel\"")) {
        g_app_config.show_lyrics_panel = (int)extract_json_int(json, "show_lyrics_panel");
    }
    if (strstr(json, "\"default_loop_mode\"")) {
        g_app_config.default_loop_mode = (int)extract_json_int(json, "default_loop_mode");
    }
    if (g_app_config.default_loop_mode < LOOP_OFF || g_app_config.default_loop_mode > LOOP_RANDOM) {
        g_app_config.default_loop_mode = LOOP_OFF;
    }
    if (strstr(json, "\"default_playback_speed\"")) {
        g_app_config.default_playback_speed = (float)extract_json_float(json, "default_playback_speed");
        if (g_app_config.default_playback_speed < 0.5f || g_app_config.default_playback_speed > 3.0f) {
            g_app_config.default_playback_speed = 1.0f;
        }
    } else {
        g_app_config.default_playback_speed = 1.0f;
    }
    if (strstr(json, "\"show_album_cover\"")) {
        g_app_config.show_album_cover = (int)extract_json_int(json, "show_album_cover");
    }
    if (strstr(json, "\"lyrics_alignment\"")) {
        g_app_config.lyrics_alignment = (int)extract_json_int(json, "lyrics_alignment");
        if (g_app_config.lyrics_alignment < 0 || g_app_config.lyrics_alignment > 2) {
            g_app_config.lyrics_alignment = 0;
        }
    }
    if (strstr(json, "\"audio_backend\"")) {
        g_app_config.audio_backend = (int)extract_json_int(json, "audio_backend");
        if (g_app_config.audio_backend < 0 || g_app_config.audio_backend > 2) {
            g_app_config.audio_backend = AUDIO_BACKEND_AUTO;
        }
    }
    if (strstr(json, "\"sort_mode\"")) {
        g_app_config.sort_mode = (int)extract_json_int(json, "sort_mode");
        if (g_app_config.sort_mode < SORT_DEFAULT || g_app_config.sort_mode > SORT_FILENAME) {
            g_app_config.sort_mode = SORT_DEFAULT;
        }
    }
    g_playback_speed = g_app_config.default_playback_speed;

    g_app_config.resume_last_playback = g_app_config.resume_last_playback ? 1 : 0;
    if (g_app_config.last_played_position < 0) {
        g_app_config.last_played_position = 0;
    }
    if (g_app_config.ui_language != UI_LANG_EN) {
        g_app_config.ui_language = UI_LANG_ZH;
    }
    if (g_app_config.volume_percent < 0) {
        g_app_config.volume_percent = 0;
    }
    if (g_app_config.volume_percent > 100) {
        g_app_config.volume_percent = 100;
    }
    if (g_app_config.audio_latency_ms < 20) {
        g_app_config.audio_latency_ms = 20;
    }
    if (g_app_config.audio_latency_ms > 250) {
        g_app_config.audio_latency_ms = 250;
    }

    // Parse remote connections
    g_app_config.remote_connection_count = 0;
    if (strstr(json, "\"remote_connection_count\"")) {
        int count = (int)extract_json_int(json, "remote_connection_count");
        if (count > MAX_REMOTE_CONNECTIONS) count = MAX_REMOTE_CONNECTIONS;
        if (count < 0) count = 0;
        g_app_config.remote_connection_count = count;
        for (int ri = 0; ri < count; ri++) {
            RemoteConnectionConfig *rc = &g_app_config.remote_connections[ri];
            char key[64];
            snprintf(key, sizeof(key), "remote_%d_name", ri);
            extract_json_string(json, key, rc->name, sizeof(rc->name));
            snprintf(key, sizeof(key), "remote_%d_protocol", ri);
            rc->protocol = (int)extract_json_int(json, key);
            snprintf(key, sizeof(key), "remote_%d_host", ri);
            extract_json_string(json, key, rc->host, sizeof(rc->host));
            snprintf(key, sizeof(key), "remote_%d_port", ri);
            rc->port = (int)extract_json_int(json, key);
            snprintf(key, sizeof(key), "remote_%d_username", ri);
            extract_json_string(json, key, rc->username, sizeof(rc->username));
            snprintf(key, sizeof(key), "remote_%d_password", ri);
            extract_json_string(json, key, rc->password, sizeof(rc->password));
            snprintf(key, sizeof(key), "remote_%d_private_key_path", ri);
            extract_json_string(json, key, rc->private_key_path, sizeof(rc->private_key_path));
            snprintf(key, sizeof(key), "remote_%d_base_path", ri);
            extract_json_string(json, key, rc->base_path, sizeof(rc->base_path));
        }
    }

    g_app_config.config_version = (int)extract_json_int(json, "config_version");

    if (g_app_config.config_version < 1) {
        for (int ri = 0; ri < MAX_REMOTE_CONNECTIONS; ri++) {
            g_app_config.remote_connections[ri].password[0] = '\0';
        }
    } else {
        for (int ri = 0; ri < MAX_REMOTE_CONNECTIONS; ri++) {
            if (g_app_config.remote_connections[ri].password[0]) {
                char decrypted[256];
                crypto_decrypt(g_app_config.remote_connections[ri].password, decrypted, sizeof(decrypted));
                strncpy(g_app_config.remote_connections[ri].password, decrypted,
                        sizeof(g_app_config.remote_connections[ri].password) - 1);
            }
        }
    }

    free(json);
}

void save_config(void) {
    log_debug("menu_views", "Saving config to '%s'", config_file);
    FILE *f = fopen(config_file, "w");
    if (!f) return;
    
    char escaped_path[MAX_PATH_LEN * 2];
    
    fprintf(f, "{\n");
    
    escape_json_string(g_app_config.default_startup_path, escaped_path, sizeof(escaped_path));
    fprintf(f, "  \"default_startup_path\": \"%s\",\n", escaped_path);
    
    escape_json_string(g_app_config.last_opened_path, escaped_path, sizeof(escaped_path));
    fprintf(f, "  \"last_opened_path\": \"%s\",\n", escaped_path);

    escape_json_string(g_app_config.last_played_folder_path, escaped_path, sizeof(escaped_path));
    fprintf(f, "  \"last_played_folder_path\": \"%s\",\n", escaped_path);

    escape_json_string(g_app_config.last_played_track_path, escaped_path, sizeof(escaped_path));
    fprintf(f, "  \"last_played_track_path\": \"%s\",\n", escaped_path);
    
    fprintf(f, "  \"theme\": {\n");
    fprintf(f, "    \"playlist_fg\": %d,\n", g_app_config.theme.playlist_fg);
    fprintf(f, "    \"playlist_bg\": %d,\n", g_app_config.theme.playlist_bg);
    fprintf(f, "    \"controls_fg\": %d,\n", g_app_config.theme.controls_fg);
    fprintf(f, "    \"controls_bg\": %d,\n", g_app_config.theme.controls_bg);
    fprintf(f, "    \"lyrics_fg\": %d,\n", g_app_config.theme.lyrics_fg);
    fprintf(f, "    \"lyrics_bg\": %d,\n", g_app_config.theme.lyrics_bg);
    fprintf(f, "    \"sidebar_fg\": %d,\n", g_app_config.theme.sidebar_fg);
    fprintf(f, "    \"sidebar_bg\": %d,\n", g_app_config.theme.sidebar_bg);
    fprintf(f, "    \"highlight_fg\": %d,\n", g_app_config.theme.highlight_fg);
    fprintf(f, "    \"highlight_bg\": %d,\n", g_app_config.theme.highlight_bg);
    fprintf(f, "    \"border_fg\": %d,\n", g_app_config.theme.border_fg);
    fprintf(f, "    \"border_bg\": %d\n", g_app_config.theme.border_bg);
    fprintf(f, "  },\n");
    
    fprintf(f, "  \"auto_play_on_start\": %d,\n", g_app_config.auto_play_on_start);
    fprintf(f, "  \"remember_last_path\": %d,\n", g_app_config.remember_last_path);
    fprintf(f, "  \"clear_history_on_startup\": %d,\n", g_app_config.clear_history_on_startup);
    fprintf(f, "  \"resume_last_playback\": %d,\n", g_app_config.resume_last_playback);
    fprintf(f, "  \"last_played_position\": %d,\n", g_app_config.last_played_position);
    fprintf(f, "  \"ui_language\": %d,\n", g_app_config.ui_language);
    fprintf(f, "  \"volume_percent\": %d,\n", g_app_config.volume_percent);
    fprintf(f, "  \"audio_latency_ms\": %d,\n", g_app_config.audio_latency_ms);
    fprintf(f, "  \"show_lyrics_panel\": %d,\n", g_app_config.show_lyrics_panel);
    fprintf(f, "  \"default_loop_mode\": %d,\n", g_app_config.default_loop_mode);
    fprintf(f, "  \"default_playback_speed\": %.2f,\n", g_app_config.default_playback_speed);
    fprintf(f, "  \"show_album_cover\": %d,\n", g_app_config.show_album_cover);
    fprintf(f, "  \"lyrics_alignment\": %d,\n", g_app_config.lyrics_alignment);
    fprintf(f, "  \"audio_backend\": %d,\n", g_app_config.audio_backend);
    fprintf(f, "  \"sort_mode\": %d,\n", g_app_config.sort_mode);

    fprintf(f, "  \"config_version\": 1,\n");
    fprintf(f, "  \"remote_connection_count\": %d,\n", g_app_config.remote_connection_count);
    for (int ri = 0; ri < g_app_config.remote_connection_count && ri < MAX_REMOTE_CONNECTIONS; ri++) {
        const RemoteConnectionConfig *rc = &g_app_config.remote_connections[ri];
        char escaped[1024];
        escape_json_string(rc->name, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_name\": \"%s\",\n", ri, escaped);
        fprintf(f, "  \"remote_%d_protocol\": %d,\n", ri, rc->protocol);
        escape_json_string(rc->host, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_host\": \"%s\",\n", ri, escaped);
        fprintf(f, "  \"remote_%d_port\": %d,\n", ri, rc->port);
        escape_json_string(rc->username, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_username\": \"%s\",\n", ri, escaped);
        char encrypted[512];
        crypto_encrypt(rc->password, encrypted, sizeof(encrypted));
        escape_json_string(encrypted, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_password\": \"%s\",\n", ri, escaped);
        escape_json_string(rc->private_key_path, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_private_key_path\": \"%s\",\n", ri, escaped);
        escape_json_string(rc->base_path, escaped, sizeof(escaped));
        fprintf(f, "  \"remote_%d_base_path\": \"%s\"%s\n", ri, escaped,
                ri < g_app_config.remote_connection_count - 1 ? "," : "");
    }

    fprintf(f, "}\n");
    
    fclose(f);
}

void load_history(void) {
    FILE *f = fopen(history_file, "r");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return;
    }
    
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);
    
    g_play_history.count = 0;
    
    const char *pos = strstr(json, "\"history\"");
    if (!pos) {
        free(json);
        return;
    }
    
    pos = strchr(pos, '[');
    if (!pos) {
        free(json);
        return;
    }
    
    const char *end = strchr(pos, ']');
    if (!end) {
        free(json);
        return;
    }
    
    while (pos < end && g_play_history.count < MAX_HISTORY_COUNT) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start > end) break;
        
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end > end) break;
        
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        
        strncpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';
        
        extract_json_string(obj, "path", g_play_history.entries[g_play_history.count].path, MAX_PATH_LEN);
        extract_json_string(obj, "title", g_play_history.entries[g_play_history.count].title, MAX_META_LEN);
        extract_json_string(obj, "artist", g_play_history.entries[g_play_history.count].artist, MAX_META_LEN);
        decode_html_entities(g_play_history.entries[g_play_history.count].title);
        decode_html_entities(g_play_history.entries[g_play_history.count].artist);
        g_play_history.entries[g_play_history.count].play_time = (time_t)extract_json_int(obj, "play_time");
        
        g_play_history.count++;
        free(obj);
        
        pos = obj_end + 1;
    }
    
    free(json);
}

void save_history(void) {
    FILE *f = fopen(history_file, "w");
    if (!f) return;
    
    fprintf(f, "{\n  \"history\": [\n");
    
    for (int i = 0; i < g_play_history.count; i++) {
        HistoryEntry *e = &g_play_history.entries[i];
        char escaped_path[MAX_PATH_LEN * 2];
        char escaped_title[MAX_META_LEN * 2];
        char escaped_artist[MAX_META_LEN * 2];
        
        escape_json_string(e->path, escaped_path, sizeof(escaped_path));
        escape_json_string(e->title, escaped_title, sizeof(escaped_title));
        escape_json_string(e->artist, escaped_artist, sizeof(escaped_artist));
        
        fprintf(f, "    {\n");
        fprintf(f, "      \"path\": \"%s\",\n", escaped_path);
        fprintf(f, "      \"title\": \"%s\",\n", escaped_title);
        fprintf(f, "      \"artist\": \"%s\",\n", escaped_artist);
        fprintf(f, "      \"play_time\": %ld\n", (long)e->play_time);
        fprintf(f, "    }%s\n", (i < g_play_history.count - 1) ? "," : "");
    }
    
    fprintf(f, "  ],\n  \"count\": %d\n}\n", g_play_history.count);
    
    fclose(f);
}

void load_favorites(void) {
    FILE *f = fopen(favorites_file, "r");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return;
    }
    
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);
    
    g_favorites.count = 0;
    
    const char *pos = strstr(json, "\"favorites\"");
    if (!pos) {
        free(json);
        return;
    }
    
    pos = strchr(pos, '[');
    if (!pos) {
        free(json);
        return;
    }
    
    const char *end = strchr(pos, ']');
    if (!end) {
        free(json);
        return;
    }
    
    while (pos < end && g_favorites.count < MAX_FAVORITES_COUNT) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start > end) break;
        
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end > end) break;
        
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        
        strncpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';
        
        extract_json_string(obj, "path", g_favorites.tracks[g_favorites.count].path, MAX_PATH_LEN);
        extract_json_string(obj, "title", g_favorites.tracks[g_favorites.count].title, MAX_META_LEN);
        extract_json_string(obj, "artist", g_favorites.tracks[g_favorites.count].artist, MAX_META_LEN);
        extract_json_string(obj, "album", g_favorites.tracks[g_favorites.count].album, MAX_META_LEN);
        decode_html_entities(g_favorites.tracks[g_favorites.count].title);
        decode_html_entities(g_favorites.tracks[g_favorites.count].artist);
        decode_html_entities(g_favorites.tracks[g_favorites.count].album);
        
        g_favorites.count++;
        free(obj);
        
        pos = obj_end + 1;
    }
    
    free(json);
}

void save_favorites(void) {
    FILE *f = fopen(favorites_file, "w");
    if (!f) return;
    
    fprintf(f, "{\n  \"favorites\": [\n");
    
    for (int i = 0; i < g_favorites.count; i++) {
        Track *t = &g_favorites.tracks[i];
        char escaped_path[MAX_PATH_LEN * 2];
        char escaped_title[MAX_META_LEN * 2];
        char escaped_artist[MAX_META_LEN * 2];
        char escaped_album[MAX_META_LEN * 2];
        
        escape_json_string(t->path, escaped_path, sizeof(escaped_path));
        escape_json_string(t->title, escaped_title, sizeof(escaped_title));
        escape_json_string(t->artist, escaped_artist, sizeof(escaped_artist));
        escape_json_string(t->album, escaped_album, sizeof(escaped_album));
        
        fprintf(f, "    {\n");
        fprintf(f, "      \"path\": \"%s\",\n", escaped_path);
        fprintf(f, "      \"title\": \"%s\",\n", escaped_title);
        fprintf(f, "      \"artist\": \"%s\",\n", escaped_artist);
        fprintf(f, "      \"album\": \"%s\"\n", escaped_album);
        fprintf(f, "    }%s\n", (i < g_favorites.count - 1) ? "," : "");
    }
    
    fprintf(f, "  ],\n  \"count\": %d\n}\n", g_favorites.count);
    
    fclose(f);
}

void load_dir_history(void) {
    FILE *f = fopen(dir_history_file, "r");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return;
    }
    
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);
    
    g_dir_history.count = 0;
    
    const char *pos = strstr(json, "\"directories\"");
    if (!pos) {
        free(json);
        return;
    }
    
    pos = strchr(pos, '[');
    if (!pos) {
        free(json);
        return;
    }
    
    const char *end = strchr(pos, ']');
    if (!end) {
        free(json);
        return;
    }
    
    while (pos < end && g_dir_history.count < MAX_DIR_HISTORY_COUNT) {
        const char *obj_start = strchr(pos, '{');
        if (!obj_start || obj_start > end) break;
        
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end > end) break;
        
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        
        strncpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';
        
        extract_json_string(obj, "path", g_dir_history.entries[g_dir_history.count].path, MAX_PATH_LEN);
        g_dir_history.entries[g_dir_history.count].open_time = (time_t)extract_json_int(obj, "open_time");
        
        g_dir_history.count++;
        free(obj);
        
        pos = obj_end + 1;
    }
    
    free(json);
}

void save_dir_history(void) {
    FILE *f = fopen(dir_history_file, "w");
    if (!f) return;
    
    fprintf(f, "{\n  \"directories\": [\n");
    
    for (int i = 0; i < g_dir_history.count; i++) {
        DirHistoryEntry *e = &g_dir_history.entries[i];
        char escaped_path[MAX_PATH_LEN * 2];
        
        escape_json_string(e->path, escaped_path, sizeof(escaped_path));
        
        fprintf(f, "    {\n");
        fprintf(f, "      \"path\": \"%s\",\n", escaped_path);
        fprintf(f, "      \"open_time\": %ld\n", (long)e->open_time);
        fprintf(f, "    }%s\n", (i < g_dir_history.count - 1) ? "," : "");
    }
    
    fprintf(f, "  ],\n  \"count\": %d\n}\n", g_dir_history.count);
    
    fclose(f);
}

void add_dir_history_entry(const char *path) {
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
    g_dir_history.entries[g_dir_history.count].open_time = time(NULL);
    g_dir_history.count++;
    
    save_dir_history();
}

void clear_dir_history(void) {
    log_info("menu_views", "Directory history cleared");
    g_dir_history.count = 0;
    save_dir_history();
}

void load_all_playlists(void) {
    DIR *dir = opendir(playlists_dir);
    if (!dir) return;
    
    g_playlist_manager.count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_playlist_manager.count < MAX_PLAYLISTS_COUNT) {
        if (strstr(entry->d_name, ".json") == NULL) continue;
        
        char filepath[MAX_PATH_LEN];
        snprintf(filepath, sizeof(filepath), "%s/%s", playlists_dir, entry->d_name);
        
        FILE *f = fopen(filepath, "r");
        if (!f) continue;
        
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char *json = malloc(fsize + 1);
        if (!json) {
            fclose(f);
            continue;
        }
        
        fread(json, 1, fsize, f);
        json[fsize] = '\0';
        fclose(f);
        
        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_manager.count];
        memset(pl, 0, sizeof(UserPlaylist));
        
        extract_json_string(json, "name", pl->name, MAX_PLAYLIST_NAME_LEN);
        decode_html_entities(pl->name);
        pl->created_time = (time_t)extract_json_int(json, "created_time");
        pl->modified_time = (time_t)extract_json_int(json, "modified_time");
        
        const char *tracks_pos = strstr(json, "\"tracks\"");
        if (tracks_pos) {
            const char *arr_start = strchr(tracks_pos, '[');
            const char *arr_end = strchr(arr_start ? arr_start : tracks_pos, ']');
            
            if (arr_start && arr_end && arr_end > arr_start) {
                const char *pos = arr_start;
                
                while (pos < arr_end && pl->track_count < MAX_TRACKS) {
                    const char *obj_start = strchr(pos, '{');
                    if (!obj_start || obj_start > arr_end) break;
                    
                    const char *obj_end = strchr(obj_start, '}');
                    if (!obj_end || obj_end > arr_end) break;
                    
                    size_t obj_len = obj_end - obj_start + 1;
                    char *obj = malloc(obj_len + 1);
                    if (!obj) break;
                    
                    strncpy(obj, obj_start, obj_len);
                    obj[obj_len] = '\0';
                    
                    extract_json_string(obj, "path", pl->tracks[pl->track_count].path, MAX_PATH_LEN);
                    extract_json_string(obj, "title", pl->tracks[pl->track_count].title, MAX_META_LEN);
                    extract_json_string(obj, "artist", pl->tracks[pl->track_count].artist, MAX_META_LEN);
                    extract_json_string(obj, "album", pl->tracks[pl->track_count].album, MAX_META_LEN);
                    decode_html_entities(pl->tracks[pl->track_count].title);
                    decode_html_entities(pl->tracks[pl->track_count].artist);
                    decode_html_entities(pl->tracks[pl->track_count].album);
                    
                    pl->track_count++;
                    free(obj);
                    
                    pos = obj_end + 1;
                }
            }
        }
        
        g_playlist_manager.count++;
        free(json);
    }
    
    closedir(dir);
}

void save_all_playlists(void) {
    for (int i = 0; i < g_playlist_manager.count; i++) {
        UserPlaylist *pl = &g_playlist_manager.playlists[i];
        
        char filename[MAX_PLAYLIST_NAME_LEN + 8];
        char safe_name[MAX_PLAYLIST_NAME_LEN];
        
        int j = 0;
        for (const char *p = pl->name; *p && j < MAX_PLAYLIST_NAME_LEN - 1; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || 
                (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
                safe_name[j++] = *p;
            } else if (*p == ' ') {
                safe_name[j++] = '_';
            }
        }
        safe_name[j] = '\0';
        
        if (j == 0) {
            snprintf(safe_name, sizeof(safe_name), "playlist_%d", i);
        }
        
        snprintf(filename, sizeof(filename), "%s.json", safe_name);
        
        char filepath[MAX_PATH_LEN];
        snprintf(filepath, sizeof(filepath), "%s/%s", playlists_dir, filename);
        
        FILE *f = fopen(filepath, "w");
        if (!f) continue;
        
        char escaped_name[MAX_PLAYLIST_NAME_LEN * 2];
        escape_json_string(pl->name, escaped_name, sizeof(escaped_name));
        
        fprintf(f, "{\n");
        fprintf(f, "  \"name\": \"%s\",\n", escaped_name);
        fprintf(f, "  \"created_time\": %ld,\n", (long)pl->created_time);
        fprintf(f, "  \"modified_time\": %ld,\n", (long)pl->modified_time);
        fprintf(f, "  \"tracks\": [\n");
        
        for (int k = 0; k < pl->track_count; k++) {
            Track *t = &pl->tracks[k];
            char escaped_path[MAX_PATH_LEN * 2];
            char escaped_title[MAX_META_LEN * 2];
            char escaped_artist[MAX_META_LEN * 2];
            char escaped_album[MAX_META_LEN * 2];
            
            escape_json_string(t->path, escaped_path, sizeof(escaped_path));
            escape_json_string(t->title, escaped_title, sizeof(escaped_title));
            escape_json_string(t->artist, escaped_artist, sizeof(escaped_artist));
            escape_json_string(t->album, escaped_album, sizeof(escaped_album));
            
            fprintf(f, "    {\n");
            fprintf(f, "      \"path\": \"%s\",\n", escaped_path);
            fprintf(f, "      \"title\": \"%s\",\n", escaped_title);
            fprintf(f, "      \"artist\": \"%s\",\n", escaped_artist);
            fprintf(f, "      \"album\": \"%s\"\n", escaped_album);
            fprintf(f, "    }%s\n", (k < pl->track_count - 1) ? "," : "");
        }
        
        fprintf(f, "  ],\n");
        fprintf(f, "  \"track_count\": %d\n", pl->track_count);
        fprintf(f, "}\n");
        
        fclose(f);
    }
}

void save_temp_playlist(void) {
    int snapshot_count = playlist_count();
    if (!playlist_is_loaded() || snapshot_count == 0) {
        log_debug("menu_views", "save_temp_playlist: nothing to save (loaded=%d count=%d)",
                  playlist_is_loaded(), snapshot_count);
        return;
    }
    log_debug("menu_views", "Saving temp playlist (%d tracks)", snapshot_count);

    char folder_path[MAX_PATH_LEN];
    playlist_copy_folder_path(folder_path, sizeof(folder_path));

    char (*tracks)[MAX_PATH_LEN] = malloc((size_t)snapshot_count * sizeof(*tracks));
    if (!tracks) {
        return;
    }

    int saved_count = 0;
    for (int i = 0; i < snapshot_count; i++) {
        int idx = g_sort_state.active ? g_sort_state.sorted_indices[i] : i;
        if (playlist_get_track_path(idx, tracks[saved_count], MAX_PATH_LEN) == 0) {
            saved_count++;
        }
    }

    if (saved_count == 0) {
        free(tracks);
        return;
    }

    FILE *f = fopen(temp_playlist_file, "w");
    if (!f) {
        free(tracks);
        return;
    }

    char escaped_folder[MAX_PATH_LEN * 3];
    escape_temp_playlist_field(folder_path, escaped_folder, sizeof(escaped_folder));
    fprintf(f, "%s\n", TEMP_PLAYLIST_MAGIC);
    fprintf(f, "folder=%s\n", escaped_folder);

    for (int i = 0; i < saved_count; i++) {
        char escaped_path[MAX_PATH_LEN * 3];
        escape_temp_playlist_field(tracks[i], escaped_path, sizeof(escaped_path));
        fprintf(f, "track=%s\n", escaped_path);
    }

    fclose(f);
    free(tracks);
}

void cleanup_temp_playlist(void) {
    log_debug("menu_views", "Cleaning up temp playlist file");
    if (temp_playlist_file[0] != '\0') {
        unlink(temp_playlist_file);
    }
}

static int apply_restored_playlist(Playlist *restored,
                                   int loaded_count,
                                   int has_multiple_sources,
                                   const char *folder_path,
                                   const char *first_track_dir) {
    if (!restored || loaded_count <= 0) {
        return 0;
    }

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

static int load_temp_playlist_v2(FILE *f) {
    Playlist *restored = calloc(1, sizeof(*restored));
    if (!restored) {
        return 0;
    }

    int loaded_count = 0;
    int has_multiple_sources = 0;
    char folder_path[MAX_PATH_LEN] = "";
    char first_track_dir[MAX_PATH_LEN] = "";
    char line[(MAX_PATH_LEN * 3) + 32];

    while (fgets(line, sizeof(line), f)) {
        trim_line_end(line);

        if (strncmp(line, "folder=", 7) == 0) {
            unescape_temp_playlist_field(line + 7, folder_path, sizeof(folder_path));
            continue;
        }

        if (strncmp(line, "track=", 6) != 0 || loaded_count >= MAX_TRACKS) {
            continue;
        }

        char path_buf[MAX_PATH_LEN];
        if (unescape_temp_playlist_field(line + 6, path_buf, sizeof(path_buf)) != 0) {
            continue;
        }

        if (path_buf[0] == '\0' || access(path_buf, R_OK) != 0) {
            continue;
        }

        strncpy(restored->tracks[loaded_count], path_buf, MAX_PATH_LEN - 1);
        restored->tracks[loaded_count][MAX_PATH_LEN - 1] = '\0';

        char track_dir[MAX_PATH_LEN];
        extract_parent_directory_for_playlist(path_buf, track_dir, sizeof(track_dir));
        if (loaded_count == 0) {
            snprintf(first_track_dir, sizeof(first_track_dir), "%s", track_dir);
        } else if (!has_multiple_sources && strcmp(first_track_dir, track_dir) != 0) {
            has_multiple_sources = 1;
        }

        loaded_count++;
    }

    int result = apply_restored_playlist(restored, loaded_count, has_multiple_sources,
                                         folder_path, first_track_dir);
    free(restored);
    return result;
}

static int load_temp_playlist_legacy_json(void) {
    FILE *f = fopen(temp_playlist_file, "r");
    if (!f) {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        return 0;
    }

    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return 0;
    }

    fseek(f, 0, SEEK_SET);
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);

    int count = (int)extract_json_int(json, "count");
    if (count <= 0 || count > MAX_TRACKS) {
        free(json);
        return 0;
    }

    char folder_path[MAX_PATH_LEN];
    extract_json_string(json, "folder_path", folder_path, sizeof(folder_path));

    Playlist *restored = calloc(1, sizeof(*restored));
    if (!restored) {
        free(json);
        return 0;
    }

    const char *tracks_start = strstr(json, "\"tracks\": [");
    if (!tracks_start) {
        free(restored);
        free(json);
        return 0;
    }

    const char *array_start = strchr(tracks_start, '[');
    const char *array_end = array_start ? strchr(array_start, ']') : NULL;
    if (!array_start || !array_end || array_end <= array_start) {
        free(restored);
        free(json);
        return 0;
    }

    int loaded_count = 0;
    int has_multiple_sources = 0;
    char first_track_dir[MAX_PATH_LEN] = "";
    const char *current_track = array_start + 1;

    while (current_track < array_end && loaded_count < count && loaded_count < MAX_TRACKS) {
        while (current_track < array_end &&
               (*current_track == ' ' || *current_track == '\t' || *current_track == '\n' ||
                *current_track == '\r' || *current_track == ',')) {
            current_track++;
        }

        if (current_track >= array_end) {
            break;
        }

        char path_buf[MAX_PATH_LEN];
        path_buf[0] = '\0';

        if (*current_track == '"') {
            const char *next = parse_json_string_value(current_track, path_buf, sizeof(path_buf));
            if (!next) {
                break;
            }
            current_track = next;
        } else if (*current_track == '{') {
            const char *end_track = strchr(current_track, '}');
            if (!end_track || end_track > array_end) {
                break;
            }

            size_t obj_len = (size_t)(end_track - current_track + 1);
            char *track_obj = malloc(obj_len + 1);
            if (!track_obj) {
                current_track = end_track + 1;
                continue;
            }

            strncpy(track_obj, current_track, obj_len);
            track_obj[obj_len] = '\0';
            extract_json_string(track_obj, "path", path_buf, sizeof(path_buf));
            free(track_obj);
            current_track = end_track + 1;
        } else {
            current_track++;
            continue;
        }

        if (path_buf[0] == '\0' || access(path_buf, R_OK) != 0) {
            continue;
        }

        strncpy(restored->tracks[loaded_count], path_buf, MAX_PATH_LEN - 1);
        restored->tracks[loaded_count][MAX_PATH_LEN - 1] = '\0';

        char track_dir[MAX_PATH_LEN];
        extract_parent_directory_for_playlist(path_buf, track_dir, sizeof(track_dir));
        if (loaded_count == 0) {
            snprintf(first_track_dir, sizeof(first_track_dir), "%s", track_dir);
        } else if (!has_multiple_sources && strcmp(first_track_dir, track_dir) != 0) {
            has_multiple_sources = 1;
        }

        loaded_count++;
    }

    int result = apply_restored_playlist(restored, loaded_count, has_multiple_sources,
                                         folder_path, first_track_dir);

    free(restored);
    free(json);
    return result;
}

int load_temp_playlist(void) {
    log_info("menu_views", "Loading temp playlist from '%s'", temp_playlist_file);
    FILE *f = fopen(temp_playlist_file, "r");
    if (!f) {
        log_debug("menu_views", "No temp playlist file found");
        return 0;
    }

    char header[128];
    if (!fgets(header, sizeof(header), f)) {
        fclose(f);
        return 0;
    }

    trim_line_end(header);
    if (strcmp(header, TEMP_PLAYLIST_MAGIC) == 0) {
        int result = load_temp_playlist_v2(f);
        fclose(f);
        return result;
    }

    fclose(f);
    return load_temp_playlist_legacy_json();
}

void render_menu_hint_bar(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    attron(COLOR_PAIR(COLOR_PAIR_BORDER));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 2, "%s",
             use_english_ui()
                 ? "F1:Home  F2:Settings  F3:History  F4:Playlists  F5:Favorites  F6:Info  F7:Lang  F8:Help  F9:Quit"
                 : "F1:主页  F2:设置  F3:历史  F4:歌单  F5:收藏  F6:信息  F7:中/EN  F8:帮助  F9:退出");
    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

int create_user_playlist(const char *name) {
    if (!name || strlen(name) == 0) return -1;
    if (g_playlist_manager.count >= MAX_PLAYLISTS_COUNT) return -2;
    
    UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_manager.count];
    memset(pl, 0, sizeof(UserPlaylist));
    
    strncpy(pl->name, name, MAX_PLAYLIST_NAME_LEN - 1);
    pl->created_time = time(NULL);
    pl->modified_time = pl->created_time;
    
    g_playlist_manager.count++;
    save_all_playlists();
    
    return 0;
}

int delete_user_playlist(int index) {
    if (index < 0 || index >= g_playlist_manager.count) return -1;
    
    char filename[MAX_PLAYLIST_NAME_LEN + 8];
    char safe_name[MAX_PLAYLIST_NAME_LEN];
    
    int j = 0;
    for (const char *p = g_playlist_manager.playlists[index].name; *p && j < MAX_PLAYLIST_NAME_LEN - 1; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || 
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            safe_name[j++] = *p;
        } else if (*p == ' ') {
            safe_name[j++] = '_';
        }
    }
    safe_name[j] = '\0';
    
    if (j > 0) {
        snprintf(filename, sizeof(filename), "%s.json", safe_name);
        char filepath[MAX_PATH_LEN];
        snprintf(filepath, sizeof(filepath), "%s/%s", playlists_dir, filename);
        unlink(filepath);
    }
    
    for (int i = index; i < g_playlist_manager.count - 1; i++) {
        g_playlist_manager.playlists[i] = g_playlist_manager.playlists[i + 1];
    }
    g_playlist_manager.count--;
    
    save_all_playlists();
    return 0;
}

int rename_user_playlist(int index, const char *new_name) {
    if (index < 0 || index >= g_playlist_manager.count) return -1;
    if (!new_name || strlen(new_name) == 0) return -2;

    UserPlaylist *pl = &g_playlist_manager.playlists[index];

    char old_filename[MAX_PLAYLIST_NAME_LEN + 8];
    char safe_old_name[MAX_PLAYLIST_NAME_LEN];
    int j = 0;
    for (const char *p = pl->name; *p && j < MAX_PLAYLIST_NAME_LEN - 1; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            safe_old_name[j++] = *p;
        } else if (*p == ' ') {
            safe_old_name[j++] = '_';
        }
    }
    safe_old_name[j] = '\0';

    if (j > 0) {
        snprintf(old_filename, sizeof(old_filename), "%s.json", safe_old_name);
        char old_filepath[MAX_PATH_LEN];
        snprintf(old_filepath, sizeof(old_filepath), "%s/%s", playlists_dir, old_filename);
        unlink(old_filepath);
    }

    strncpy(pl->name, new_name, MAX_PLAYLIST_NAME_LEN - 1);
    pl->name[MAX_PLAYLIST_NAME_LEN - 1] = '\0';
    pl->modified_time = time(NULL);

    save_all_playlists();
    return 0;
}

int add_track_to_playlist(int playlist_idx, Track *track) {
    if (playlist_idx < 0 || playlist_idx >= g_playlist_manager.count) return -1;
    if (!track) return -2;
    
    UserPlaylist *pl = &g_playlist_manager.playlists[playlist_idx];
    if (pl->track_count >= MAX_TRACKS) return -3;
    
    for (int i = 0; i < pl->track_count; i++) {
        if (strcmp(pl->tracks[i].path, track->path) == 0) {
            return 0;
        }
    }
    
    pl->tracks[pl->track_count] = *track;
    pl->track_count++;
    pl->modified_time = time(NULL);
    
    save_all_playlists();
    return 0;
}

int remove_track_from_playlist(int playlist_idx, int track_idx) {
    if (playlist_idx < 0 || playlist_idx >= g_playlist_manager.count) return -1;
    
    UserPlaylist *pl = &g_playlist_manager.playlists[playlist_idx];
    if (track_idx < 0 || track_idx >= pl->track_count) return -2;
    
    for (int i = track_idx; i < pl->track_count - 1; i++) {
        pl->tracks[i] = pl->tracks[i + 1];
    }
    pl->track_count--;
    pl->modified_time = time(NULL);
    
    save_all_playlists();
    return 0;
}

void init_all_persistent_data(void) {
    ensure_config_dir_exists();
    load_config();
    apply_color_theme();
    g_loop_mode = g_app_config.default_loop_mode;
    load_history();
    load_favorites();
    load_dir_history();
    load_all_playlists();
}

void show_status_message(const char *msg) {
    if (use_ascii_fallback_ui()) {
        sanitize_ascii_menu_text(g_status_message, sizeof(g_status_message), msg);
    } else {
        strncpy(g_status_message, msg, sizeof(g_status_message) - 1);
        g_status_message[sizeof(g_status_message) - 1] = '\0';
    }
    g_status_message_time = time(NULL);
}

void init_menu_views(void) {
    log_info("menu_views", "Initializing menu views");
    g_current_view = VIEW_MAIN;
    g_menu_selected_idx = 0;
    g_content_selected_idx = 0;
    g_focus_area = FOCUS_SIDEBAR;

    init_all_persistent_data();
}

void render_menu_frame(const char *title) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    clear();
    
    attron(COLOR_PAIR(COLOR_PAIR_BORDER));
    box(stdscr, 0, 0);
    mvprintw(0, 2, " %s ", resolve_menu_title(title));
    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    
    int menu_width = max_x / 4;
    mvvline(1, menu_width, ACS_VLINE, max_y - 3);
    mvaddch(1, menu_width, ACS_TTEE);
    mvaddch(max_y - 3, menu_width, ACS_BTEE);
    
    if (strlen(g_status_message) > 0 && (time(NULL) - g_status_message_time) < 3) {
        attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        mvprintw(max_y - 2, 2, "%s", g_status_message);
        attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    }
    
    refresh();
}

void render_menu_sidebar(int selected_idx, const char **items, int item_count) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int menu_width = max_x / 4;
    int start_y = 2;
    items = resolve_sidebar_items(items);
    
    attron(COLOR_PAIR(COLOR_PAIR_SIDEBAR));
    
    for (int y = start_y; y < max_y - 2; y++) {
        mvhline(y, 1, ' ', menu_width - 1);
    }
    
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

void add_history_entry(Track *track) {
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
    
    save_history();
}

int add_to_favorites(Track *track) {
    if (!track || g_favorites.count >= MAX_FAVORITES_COUNT) {
        return -1;
    }
    
    for (int i = 0; i < g_favorites.count; i++) {
        if (strcmp(g_favorites.tracks[i].path, track->path) == 0) {
            return 0;
        }
    }
    
    strncpy(g_favorites.tracks[g_favorites.count].path, track->path, MAX_PATH_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].title, track->title, MAX_META_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].artist, track->artist, MAX_META_LEN - 1);
    strncpy(g_favorites.tracks[g_favorites.count].album, track->album, MAX_META_LEN - 1);
    
    g_favorites.count++;
    save_favorites();
    
    return 0;
}

int remove_from_favorites(int index) {
    if (index < 0 || index >= g_favorites.count) {
        return -1;
    }
    
    if (index < g_favorites.count - 1) {
        memmove(&g_favorites.tracks[index], &g_favorites.tracks[index + 1],
                sizeof(Track) * (g_favorites.count - index - 1));
    }
    
    g_favorites.count--;
    save_favorites();
    
    return 0;
}

static int g_settings_current_option = 0;
static int g_settings_color_editing = 0;
static int g_settings_color_which = 0;

/* ---------- Remote devices state ---------- */

static int g_remote_mode = 0;           // 0=list, 1=actions, 2=form, 3=browse
static int g_remote_selected = 0;       // selected item in current mode
static int g_remote_selected_conn = -1; // connection index for actions/form/browse
static RemoteDirEntry *g_remote_entries = NULL;
static int g_remote_entry_count = 0;
static int g_remote_entry_offset = 0;
static char g_remote_current_path[1024] = "";
static RemoteConnectionConfig g_remote_form_config;
static int g_remote_form_editing_idx = -1;  // -1 = adding new, >=0 = editing existing

static void render_remote_content(void);
static void handle_remote_content_input(int ch);
static void remote_enter_list_mode(void);
static void remote_start_add(void);
static void remote_start_edit(int conn_idx);
static void remote_delete_connection(int conn_idx);
static void remote_start_browse(int conn_idx);
static void remote_refresh_entries(void);

static int clamp_latency_ms(int latency_ms) {
    if (latency_ms < 20) {
        return 20;
    }
    if (latency_ms > 250) {
        return 250;
    }
    return latency_ms;
}

static const char *settings_options[] = {
    "播放列表前景色",
    "播放列表背景色",
    "控制区前景色",
    "控制区背景色",
    "歌词前景色",
    "歌词背景色",
    "侧边栏前景色",
    "侧边栏背景色",
    "高亮前景色",
    "高亮背景色",
    "边框前景色",
    "边框背景色",
    "默认启动路径",
    "启动后自动播放",
    "记住上次路径",
    "启动时清空历史",
    "界面语言",
    "默认音量",
    "输出时延",
    "显示歌词面板",
    "默认循环模式",
    "默认倍速",
    "显示专辑图片",
    "歌词对齐方式",
    "音频后端",
    "排序方式"
};
static const char *settings_options_ascii[] = {
    "Playlist Foreground",
    "Playlist Background",
    "Controls Foreground",
    "Controls Background",
    "Lyrics Foreground",
    "Lyrics Background",
    "Sidebar Foreground",
    "Sidebar Background",
    "Highlight Foreground",
    "Highlight Background",
    "Border Foreground",
    "Border Background",
    "Default Startup Path",
    "Auto Play On Start",
    "Remember Last Path",
    "Clear History On Start",
    "Language",
    "Default Volume",
    "Output Latency",
    "Show Lyrics Panel",
    "Default Loop Mode",
    "Default Speed",
    "Show Album Cover",
    "Lyrics Alignment",
    "Audio Backend",
    "Sort Mode"
};
#define SETTINGS_OPTION_COUNT 26

enum {
    SETTINGS_IDX_THEME_COLOR_PAIR_0 = 0,
    SETTINGS_IDX_THEME_COLOR_PAIR_1 = 1,
    SETTINGS_IDX_THEME_COLOR_PAIR_2 = 2,
    SETTINGS_IDX_THEME_COLOR_PAIR_3 = 3,
    SETTINGS_IDX_THEME_COLOR_PAIR_4 = 4,
    SETTINGS_IDX_THEME_COLOR_PAIR_5 = 5,
    SETTINGS_IDX_THEME_COLOR_PAIR_6 = 6,
    SETTINGS_IDX_THEME_COLOR_PAIR_7 = 7,
    SETTINGS_IDX_THEME_COLOR_PAIR_8 = 8,
    SETTINGS_IDX_THEME_COLOR_PAIR_9 = 9,
    SETTINGS_IDX_THEME_COLOR_PAIR_10 = 10,
    SETTINGS_IDX_THEME_COLOR_PAIR_11 = 11,
    SETTINGS_IDX_DEFAULT_PATH = 12,
    SETTINGS_IDX_AUTO_PLAY = 13,
    SETTINGS_IDX_REMEMBER_PATH = 14,
    SETTINGS_IDX_CLEAR_HISTORY = 15,
    SETTINGS_IDX_LANGUAGE = 16,
    SETTINGS_IDX_VOLUME = 17,
    SETTINGS_IDX_LATENCY = 18,
    SETTINGS_IDX_SHOW_LYRICS = 19,
    SETTINGS_IDX_DEFAULT_LOOP = 20,
    SETTINGS_IDX_DEFAULT_SPEED = 21,
    SETTINGS_IDX_SHOW_ALBUM_COVER = 22,
    SETTINGS_IDX_LYRICS_ALIGNMENT = 23,
    SETTINGS_IDX_AUDIO_BACKEND = 24,
    SETTINGS_IDX_SORT_MODE = 25
};

typedef struct {
    const int *indices;
    int count;
} SettingsSectionSpec;

static const int settings_theme_option_indices[] = {
    SETTINGS_IDX_THEME_COLOR_PAIR_0,
    SETTINGS_IDX_THEME_COLOR_PAIR_1,
    SETTINGS_IDX_THEME_COLOR_PAIR_2,
    SETTINGS_IDX_THEME_COLOR_PAIR_3,
    SETTINGS_IDX_THEME_COLOR_PAIR_4,
    SETTINGS_IDX_THEME_COLOR_PAIR_5,
    SETTINGS_IDX_THEME_COLOR_PAIR_6,
    SETTINGS_IDX_THEME_COLOR_PAIR_7,
    SETTINGS_IDX_THEME_COLOR_PAIR_8,
    SETTINGS_IDX_THEME_COLOR_PAIR_9,
    SETTINGS_IDX_THEME_COLOR_PAIR_10,
    SETTINGS_IDX_THEME_COLOR_PAIR_11
};

static const int settings_path_option_indices[] = {
    SETTINGS_IDX_DEFAULT_PATH
};

static const int settings_playback_option_indices[] = {
    SETTINGS_IDX_AUTO_PLAY,
    SETTINGS_IDX_REMEMBER_PATH,
    SETTINGS_IDX_CLEAR_HISTORY,
    SETTINGS_IDX_LANGUAGE,
    SETTINGS_IDX_VOLUME,
    SETTINGS_IDX_LATENCY,
    SETTINGS_IDX_SHOW_LYRICS,
    SETTINGS_IDX_SHOW_ALBUM_COVER,
    SETTINGS_IDX_LYRICS_ALIGNMENT,
    SETTINGS_IDX_DEFAULT_LOOP,
    SETTINGS_IDX_DEFAULT_SPEED,
    SETTINGS_IDX_AUDIO_BACKEND,
    SETTINGS_IDX_SORT_MODE
};

static SettingsSectionSpec get_settings_section_spec_for_sidebar(int sidebar_idx) {
    switch (sidebar_idx) {
        case 0:
            return (SettingsSectionSpec){settings_theme_option_indices, (int)(sizeof(settings_theme_option_indices) / sizeof(settings_theme_option_indices[0]))};
        case 1:
            return (SettingsSectionSpec){settings_path_option_indices, (int)(sizeof(settings_path_option_indices) / sizeof(settings_path_option_indices[0]))};
        case 2:
            return (SettingsSectionSpec){settings_playback_option_indices, (int)(sizeof(settings_playback_option_indices) / sizeof(settings_playback_option_indices[0]))};
        default:
            return (SettingsSectionSpec){NULL, 0};
    }
}

static SettingsSectionSpec get_active_settings_section_spec(void) {
    return get_settings_section_spec_for_sidebar(g_menu_selected_idx);
}

static int get_settings_section_position(SettingsSectionSpec spec, int option_index) {
    for (int i = 0; i < spec.count; i++) {
        if (spec.indices[i] == option_index) {
            return i;
        }
    }
    return -1;
}

static void sync_settings_selection_to_sidebar(void) {
    SettingsSectionSpec spec = get_active_settings_section_spec();
    if (spec.count <= 0) {
        g_settings_current_option = -1;
        return;
    }

    if (get_settings_section_position(spec, g_settings_current_option) < 0) {
        g_settings_current_option = spec.indices[0];
    }
}

static void rerender_settings_view(void) {
    render_menu_frame("设置 [F2]");
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

static void format_settings_option_line(int option_index, char *line, size_t line_size) {
    const char **current_settings_options = use_english_ui() ? settings_options_ascii : settings_options;
    const char *separator = use_english_ui() ? ": " : "：";
    const char *unset_label = use_english_ui() ? "(Not set)" : "(未设置)";

    int *color_values[] = {
        &g_app_config.theme.playlist_fg,
        &g_app_config.theme.playlist_bg,
        &g_app_config.theme.controls_fg,
        &g_app_config.theme.controls_bg,
        &g_app_config.theme.lyrics_fg,
        &g_app_config.theme.lyrics_bg,
        &g_app_config.theme.sidebar_fg,
        &g_app_config.theme.sidebar_bg,
        &g_app_config.theme.highlight_fg,
        &g_app_config.theme.highlight_bg,
        &g_app_config.theme.border_fg,
        &g_app_config.theme.border_bg
    };

    if (!line || line_size == 0 || option_index < 0 || option_index >= SETTINGS_OPTION_COUNT) {
        return;
    }

    if (option_index < 12) {
        int color_val = *color_values[option_index];
        snprintf(line, line_size, "%s%s%s (%d)",
                 current_settings_options[option_index], separator,
                 menu_color_name(color_val), color_val);
    } else if (option_index == SETTINGS_IDX_DEFAULT_PATH) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 g_app_config.default_startup_path[0] ? g_app_config.default_startup_path : unset_label);
    } else if (option_index == SETTINGS_IDX_AUTO_PLAY) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.auto_play_on_start));
    } else if (option_index == SETTINGS_IDX_REMEMBER_PATH) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.remember_last_path));
    } else if (option_index == SETTINGS_IDX_CLEAR_HISTORY) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.clear_history_on_startup));
    } else if (option_index == SETTINGS_IDX_LANGUAGE) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_language_name(g_app_config.ui_language));
    } else if (option_index == SETTINGS_IDX_VOLUME) {
        snprintf(line, line_size, "%s%s%d%%",
                 current_settings_options[option_index], separator,
                 g_app_config.volume_percent);
    } else if (option_index == SETTINGS_IDX_LATENCY) {
        snprintf(line, line_size, "%s%s%d ms",
                 current_settings_options[option_index], separator,
                 g_app_config.audio_latency_ms);
    } else if (option_index == SETTINGS_IDX_SHOW_LYRICS) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.show_lyrics_panel));
    } else if (option_index == SETTINGS_IDX_SHOW_ALBUM_COVER) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.show_album_cover));
    } else if (option_index == SETTINGS_IDX_LYRICS_ALIGNMENT) {
        const char *align_str;
        switch (g_app_config.lyrics_alignment) {
            case 1: align_str = use_english_ui() ? "Center" : "居中"; break;
            case 2: align_str = use_english_ui() ? "Right" : "居右"; break;
            default: align_str = use_english_ui() ? "Left" : "居左";
        }
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator, align_str);
    } else if (option_index == SETTINGS_IDX_DEFAULT_LOOP) {
        const char *loop_mode_str;
        switch (g_app_config.default_loop_mode) {
            case LOOP_OFF:
                loop_mode_str = use_english_ui() ? "Off" : "关闭";
                break;
            case LOOP_SINGLE:
                loop_mode_str = use_english_ui() ? "Single" : "单曲";
                break;
            case LOOP_LIST:
                loop_mode_str = use_english_ui() ? "List" : "列表";
                break;
            case LOOP_RANDOM:
                loop_mode_str = use_english_ui() ? "Random" : "随机";
                break;
            default:
                loop_mode_str = use_english_ui() ? "Off" : "关闭";
        }
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 loop_mode_str);
    } else if (option_index == SETTINGS_IDX_DEFAULT_SPEED) {
        snprintf(line, line_size, "%s%s%.2fx",
                 current_settings_options[option_index], separator,
                 g_app_config.default_playback_speed);
    } else if (option_index == SETTINGS_IDX_AUDIO_BACKEND) {
        const char *backend_str;
        switch (g_app_config.audio_backend) {
            case AUDIO_BACKEND_AUTO:  backend_str = use_english_ui() ? "Auto" : "自动"; break;
            case AUDIO_BACKEND_PULSE: backend_str = "PulseAudio"; break;
            case AUDIO_BACKEND_ALSA:  backend_str = "ALSA"; break;
            default:                  backend_str = use_english_ui() ? "Auto" : "自动";
        }
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator, backend_str);
    } else if (option_index == SETTINGS_IDX_SORT_MODE) {
        const char *sort_str;
        switch (g_app_config.sort_mode) {
            case SORT_DEFAULT:  sort_str = menu_text("默认", "Default"); break;
            case SORT_TITLE:    sort_str = menu_text("标题", "Title"); break;
            case SORT_ARTIST:   sort_str = menu_text("艺术家", "Artist"); break;
            case SORT_ALBUM:    sort_str = menu_text("专辑", "Album"); break;
            case SORT_FILENAME: sort_str = menu_text("文件名", "Filename"); break;
            default:            sort_str = menu_text("默认", "Default");
        }
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator, sort_str);
    } else {
        snprintf(line, line_size, "%s", current_settings_options[option_index]);
    }
}

static void render_settings_option_group(int start_y, int content_start_x, int max_y, SettingsSectionSpec spec) {
    for (int i = 0; i < spec.count && start_y + i < max_y - 2; i++) {
        int option_index = spec.indices[i];
        char line[256];

        format_settings_option_line(option_index, line, sizeof(line));
        move(start_y + i, content_start_x);
        if (option_index == g_settings_current_option && g_focus_area == FOCUS_CONTENT) {
            attron(A_REVERSE);
            printw("%s", line);
            clrtoeol();
            attroff(A_REVERSE);
        } else {
            printw("%s", line);
            clrtoeol();
        }
    }
}

static void move_settings_content_selection(int delta) {
    SettingsSectionSpec spec = get_active_settings_section_spec();
    if (spec.count <= 0) {
        return;
    }

    int position = get_settings_section_position(spec, g_settings_current_option);
    if (position < 0) {
        position = 0;
    } else {
        position += delta;
        if (position < 0) {
            position = spec.count - 1;
        } else if (position >= spec.count) {
            position = 0;
        }
    }

    g_settings_current_option = spec.indices[position];
}

static void adjust_settings_theme_option(int option_index, int delta) {
    int *color_values[] = {
        &g_app_config.theme.playlist_fg,
        &g_app_config.theme.playlist_bg,
        &g_app_config.theme.controls_fg,
        &g_app_config.theme.controls_bg,
        &g_app_config.theme.lyrics_fg,
        &g_app_config.theme.lyrics_bg,
        &g_app_config.theme.sidebar_fg,
        &g_app_config.theme.sidebar_bg,
        &g_app_config.theme.highlight_fg,
        &g_app_config.theme.highlight_bg,
        &g_app_config.theme.border_fg,
        &g_app_config.theme.border_bg
    };

    if (option_index < 0 || option_index >= 12) {
        return;
    }

    if (delta == 0) {
        delta = 1;
    }

    int paired_idx = (option_index % 2 == 0) ? option_index + 1 : option_index - 1;
    int paired_color = *color_values[paired_idx];
    int next = *color_values[option_index];

    do {
        next += delta;
        if (next < 0) {
            next = 7;
        } else if (next > 7) {
            next = 0;
        }
    } while (next == paired_color);

    *color_values[option_index] = next;
    apply_color_theme();
    save_config();
}

static int is_valid_path(const char *path) {
    if (!path || strlen(path) == 0) {
        return 0;
    }

    for (size_t i = 0; i < strlen(path); i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 && c != '\0') {
            return 0;
        }
    }

    if (path[0] == '~') {
        if (strlen(path) > 1 && path[1] != '/') {
            return 0;
        }
    }

    return 1;
}

static void edit_default_startup_path(void) {
    noecho();
    curs_set(1);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int menu_width = max_x / 4;

    const char *path_prompt = menu_text("输入路径：", "Enter path: ");
    char input_path[MAX_PATH_LEN];
    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                      path_prompt, input_path, sizeof(input_path), 1, 0, 0);

    noecho();
    curs_set(0);

    if (strlen(input_path) > 0) {
        if (!is_valid_path(input_path)) {
            show_status_message(menu_text("路径格式无效", "Invalid path format"));
            return;
        }

        if (input_path[0] == '~') {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(g_app_config.default_startup_path, MAX_PATH_LEN, "%s%s", home, input_path + 1);
            }
        } else {
            strncpy(g_app_config.default_startup_path, input_path, MAX_PATH_LEN - 1);
        }
        save_config();
        show_status_message(menu_text("默认启动路径已保存", "Default startup path saved"));
    }
}

static void adjust_or_toggle_settings_option(int option_index, int delta) {
    if (option_index < 0) {
        return;
    }

    if (option_index < 12) {
        adjust_settings_theme_option(option_index, delta);
        return;
    }

    switch (option_index) {
        case SETTINGS_IDX_AUTO_PLAY:
            g_app_config.auto_play_on_start = !g_app_config.auto_play_on_start;
            save_config();
            break;
        case SETTINGS_IDX_REMEMBER_PATH:
            g_app_config.remember_last_path = !g_app_config.remember_last_path;
            save_config();
            break;
        case SETTINGS_IDX_CLEAR_HISTORY:
            g_app_config.clear_history_on_startup = !g_app_config.clear_history_on_startup;
            save_config();
            break;
        case SETTINGS_IDX_LANGUAGE:
            if (delta < 0) {
                g_app_config.ui_language = UI_LANG_ZH;
            } else if (delta > 0) {
                g_app_config.ui_language = UI_LANG_EN;
            } else {
                g_app_config.ui_language = (g_app_config.ui_language == UI_LANG_EN) ? UI_LANG_ZH : UI_LANG_EN;
            }
            save_config();
            break;
        case SETTINGS_IDX_VOLUME:
            if (delta == 0) {
                delta = 1;
            }
            set_volume_percent(g_app_config.volume_percent + delta * 5);
            g_app_config.volume_percent = get_volume_percent();
            break;
        case SETTINGS_IDX_LATENCY:
            if (delta == 0) {
                delta = 1;
            }
            g_app_config.audio_latency_ms = clamp_latency_ms(g_app_config.audio_latency_ms + delta * 10);
            save_config();
            show_status_message(menu_text("输出时延将在下次播放生效",
                                          "Output latency applies on next playback"));
            break;
        case SETTINGS_IDX_SHOW_LYRICS:
            g_app_config.show_lyrics_panel = !g_app_config.show_lyrics_panel;
            save_config();
            break;
        case SETTINGS_IDX_SHOW_ALBUM_COVER:
            g_app_config.show_album_cover = !g_app_config.show_album_cover;
            save_config();
            break;
        case SETTINGS_IDX_LYRICS_ALIGNMENT:
            if (delta < 0) {
                g_app_config.lyrics_alignment = (g_app_config.lyrics_alignment - 1 + 3) % 3;
            } else {
                g_app_config.lyrics_alignment = (g_app_config.lyrics_alignment + 1) % 3;
            }
            save_config();
            break;
        case SETTINGS_IDX_DEFAULT_LOOP:
            if (delta < 0) {
                g_app_config.default_loop_mode = (g_app_config.default_loop_mode - 1 + 4) % 4;
            } else if (delta > 0) {
                g_app_config.default_loop_mode = (g_app_config.default_loop_mode + 1) % 4;
            } else {
                g_app_config.default_loop_mode = (g_app_config.default_loop_mode + 1) % 4;
            }
            save_config();
            g_loop_mode = g_app_config.default_loop_mode;
            show_status_message(menu_text("默认循环模式已更新，当前会话已应用",
                                          "Default loop mode updated, applied to current session"));
            break;
        case SETTINGS_IDX_DEFAULT_SPEED: {
            static float speed_ratios[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
            static int speed_count = sizeof(speed_ratios) / sizeof(speed_ratios[0]);
            int current_idx = 1;
            for (int i = 0; i < speed_count; i++) {
                if (fabs(g_app_config.default_playback_speed - speed_ratios[i]) < 0.01f) {
                    current_idx = i;
                    break;
                }
            }
            if (delta < 0) {
                current_idx = (current_idx - 1 + speed_count) % speed_count;
            } else {
                current_idx = (current_idx + 1) % speed_count;
            }
            g_app_config.default_playback_speed = speed_ratios[current_idx];
            g_playback_speed = g_app_config.default_playback_speed;
            save_config();
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: %.2fx",
                     menu_text("默认倍速已更新", "Default speed updated"),
                     g_app_config.default_playback_speed);
            show_status_message(msg);
            break;
        }
        case SETTINGS_IDX_AUDIO_BACKEND: {
            int options[] = {AUDIO_BACKEND_AUTO, AUDIO_BACKEND_PULSE, AUDIO_BACKEND_ALSA};
            int count = 3;
            int has_pulse = audio_backend_is_available(AUDIO_BACKEND_PULSE);
            int has_alsa = audio_backend_is_available(AUDIO_BACKEND_ALSA);
            int current = 0;
            for (int i = 0; i < count; i++) {
                if (g_app_config.audio_backend == options[i]) {
                    current = i;
                    break;
                }
            }
            int direction = (delta >= 0) ? 1 : -1;
            int next = current;
            int attempts = 0;
            do {
                next = (next + direction + count) % count;
                attempts++;
                if ((options[next] == AUDIO_BACKEND_PULSE && !has_pulse) ||
                    (options[next] == AUDIO_BACKEND_ALSA && !has_alsa)) {
                    continue;
                }
                break;
            } while (attempts < count);
            g_app_config.audio_backend = options[next];
            save_config();
            show_status_message(menu_text("音频后端将在下次启动时生效",
                                          "Audio backend will take effect on next restart"));
            break;
        }
        case SETTINGS_IDX_SORT_MODE:
            if (delta < 0) {
                g_app_config.sort_mode = (g_app_config.sort_mode - 1 + 5) % 5;
            } else {
                g_app_config.sort_mode = (g_app_config.sort_mode + 1) % 5;
            }
            save_config();
            recompute_sort_order();
            show_status_message(menu_text("排序方式已生效", "Sort mode applied"));
            break;
        default:
            break;
    }
}

static void activate_settings_current_option(void) {
    if (g_settings_current_option == SETTINGS_IDX_DEFAULT_PATH) {
        edit_default_startup_path();
        return;
    }

    if (g_settings_current_option == SETTINGS_IDX_AUTO_PLAY ||
        g_settings_current_option == SETTINGS_IDX_REMEMBER_PATH ||
        g_settings_current_option == SETTINGS_IDX_CLEAR_HISTORY ||
        g_settings_current_option == SETTINGS_IDX_LANGUAGE ||
        g_settings_current_option == SETTINGS_IDX_SHOW_LYRICS ||
        g_settings_current_option == SETTINGS_IDX_SHOW_ALBUM_COVER ||
        g_settings_current_option == SETTINGS_IDX_LYRICS_ALIGNMENT ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_LOOP ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_SPEED ||
        g_settings_current_option == SETTINGS_IDX_SORT_MODE) {
        adjust_or_toggle_settings_option(g_settings_current_option, 0);
        return;
    }

    if (g_settings_current_option >= 0) {
        adjust_or_toggle_settings_option(g_settings_current_option, 1);
    }
}

/* ---------- Remote devices content ---------- */

void remote_enter_list_mode(void) {
    g_remote_mode = 0;
    g_remote_selected = 0;
    g_remote_selected_conn = -1;
    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;
    g_remote_current_path[0] = '\0';
}

static void remote_go_back(void) {
    switch (g_remote_mode) {
        case 0: // list -> back to sidebar
            g_focus_area = FOCUS_SIDEBAR;
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            break;
        case 1: // actions -> list
            g_remote_mode = 0;
            g_remote_selected = 0;
            render_settings_content();
            break;
        case 3: // browse
            if (g_remote_current_path[0] && strcmp(g_remote_current_path, "/") != 0) {
                char *last_slash = strrchr(g_remote_current_path, '/');
                if (last_slash && last_slash != g_remote_current_path) {
                    *last_slash = '\0';
                } else if (last_slash == g_remote_current_path) {
                    g_remote_current_path[1] = '\0';
                } else {
                    g_remote_current_path[0] = '\0';
                }
                g_remote_selected = 0;
                remote_refresh_entries();
            } else {
                g_remote_mode = 1;
                g_remote_selected = 0;
                if (g_remote_entries) {
                    remote_free_entries(g_remote_entries, g_remote_entry_count);
                    g_remote_entries = NULL;
                }
                g_remote_entry_count = 0;
                render_settings_content();
            }
            break;
    }
}

static void rerender_remote_view(void) {
    render_menu_frame(menu_text("设置 [F2]", "Settings [F2]"));
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

static void remote_refresh_entries(void) {
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= g_app_config.remote_connection_count) {
        return;
    }
    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;

    show_status_message(menu_text("正在连接...", "Connecting..."));
    refresh();

    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[g_remote_selected_conn];
    int ret = remote_list_directory(conn, g_remote_current_path, &g_remote_entries, &g_remote_entry_count);
    if (ret < 0) {
        g_remote_entries = NULL;
        g_remote_entry_count = 0;
        const char *err = remote_strerror();
        if (err && err[0]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: %s",
                     menu_text("无法列出远程目录", "Cannot list remote directory"), err);
            show_status_message(buf);
        } else {
            show_status_message(menu_text("无法列出远程目录", "Cannot list remote directory"));
        }
    }
}

static void remote_start_browse(int conn_idx) {
    g_remote_selected_conn = conn_idx;
    g_remote_mode = 3;
    g_remote_selected = 0;

    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[conn_idx];
    strncpy(g_remote_current_path, conn->base_path, sizeof(g_remote_current_path) - 1);

    remote_refresh_entries();
    rerender_remote_view();
}

static void remote_refresh_connection(void) {
    if (g_remote_selected_conn < 0 || g_remote_selected_conn >= g_app_config.remote_connection_count) return;
    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[g_remote_selected_conn];

    if (g_remote_entries) {
        remote_free_entries(g_remote_entries, g_remote_entry_count);
        g_remote_entries = NULL;
    }
    g_remote_entry_count = 0;
    g_remote_entry_offset = 0;

    show_status_message(menu_text("正在连接...", "Connecting..."));
    render_settings_content();
    refresh();

    RemoteDirEntry *entries = NULL;
    int count = 0;
    int ret = remote_list_directory(conn, conn->base_path, &entries, &count);
    if (entries) remote_free_entries(entries, count);

    if (ret < 0) {
        const char *err = remote_strerror();
        if (err && err[0]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s%s%s",
                     menu_text("刷新失败：", "Refresh failed: "), err,
                     menu_text("  ← 按任意键继续", "  ← Press any key"));
            show_status_message(buf);
        } else {
            show_status_message(menu_text("刷新失败  ← 按任意键继续", "Refresh failed  ← Press any key"));
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %d %s",
                 menu_text("连接成功，共", "Connection OK, found"),
                 count,
                 menu_text("个条目", "entries"));
        show_status_message(buf);
    }
    render_settings_content();
}

static void remote_load_playlist(void) {
    if (g_remote_selected_conn < 0) return;
    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[g_remote_selected_conn];

    stop_audio();
    int count = load_remote_playlist(conn, g_remote_current_path);
    if (count > 0) {
        g_selected_index = 0;
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: %s (%d %s)",
                 menu_text("已加载", "Loaded"), conn->name, count,
                 menu_text("首歌曲", "tracks"));
        show_status_message(msg);
        exit_current_view();
    } else {
        show_status_message(menu_text("该目录没有音频文件", "No audio files in this directory"));
    }
}

/* ---------- Remote connection form (multi-field) ---------- */

static int remote_form_field_count(void) {
    return (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) ? 8 : 7;
}

static void remote_form_field_label(int field_idx, char *buf, size_t size) {
    switch (field_idx) {
        case 0: snprintf(buf, size, "%s:", menu_text("名称", "Name")); break;
        case 1: snprintf(buf, size, "%s:", menu_text("协议", "Protocol")); break;
        case 2: snprintf(buf, size, "%s:", menu_text("主机", "Host")); break;
        case 3: snprintf(buf, size, "%s:", menu_text("端口", "Port")); break;
        case 4: snprintf(buf, size, "%s:", menu_text("用户名", "Username")); break;
        case 5: snprintf(buf, size, "%s:", menu_text("密码", "Password")); break;
        case 6:
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP)
                snprintf(buf, size, "%s:", menu_text("私钥", "Private Key"));
            else
                snprintf(buf, size, "%s:", menu_text("基础路径", "Base Path"));
            break;
        case 7: snprintf(buf, size, "%s:", menu_text("基础路径", "Base Path")); break;
    }
}

static void remote_form_value_text(int field_idx, char *buf, size_t size) {
    const RemoteConnectionConfig *rc = &g_remote_form_config;
    buf[0] = '\0';
    switch (field_idx) {
        case 0: snprintf(buf, size, "%s", rc->name); break;
        case 1: snprintf(buf, size, "%s", remote_protocol_name(rc->protocol)); break;
        case 2: snprintf(buf, size, "%s", rc->host); break;
        case 3: if (rc->port > 0) snprintf(buf, size, "%d", rc->port); break;
        case 4: snprintf(buf, size, "%s", rc->username); break;
        case 5:
            if (rc->password[0]) {
                int n = strlen(rc->password);
                if (n > 50) n = 50;
                memset(buf, '*', n);
                buf[n] = '\0';
            }
            break;
        case 6:
            if (rc->protocol == REMOTE_PROTOCOL_SFTP)
                snprintf(buf, size, "%s", rc->private_key_path);
            else
                snprintf(buf, size, "%s", rc->base_path);
            break;
        case 7: snprintf(buf, size, "%s", rc->base_path); break;
    }
}

static void remote_form_edit_field(int field_idx) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int content_start_x = (max_x / 4) + 2;
    int form_start_y = 4;

    curs_set(1);
    noecho();

    char label[32];
    remote_form_field_label(field_idx, label, sizeof(label));

    switch (field_idx) {
        case 0:  // Name
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.name,
                              sizeof(g_remote_form_config.name), 1, 0, 1);
            break;
        case 1:  // Protocol (also cyclable via +/-)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", g_remote_form_config.protocol);
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, buf, sizeof(buf), 1, 0, 1);
                int val = atoi(buf);
                if (val >= 0 && val <= 4) g_remote_form_config.protocol = val;
            }
            break;
        case 2:  // Host
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.host,
                              sizeof(g_remote_form_config.host), 1, 0, 1);
            break;
        case 3:  // Port
            {
                char buf[16];
                if (g_remote_form_config.port > 0)
                    snprintf(buf, sizeof(buf), "%d", g_remote_form_config.port);
                else
                    buf[0] = '\0';
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, buf, sizeof(buf), 1, 0, 1);
                g_remote_form_config.port = buf[0] ? (int)strtol(buf, NULL, 10) : 0;
            }
            break;
        case 4:  // Username
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.username,
                              sizeof(g_remote_form_config.username), 1, 0, 1);
            break;
        case 5:  // Password (masked)
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.password,
                              sizeof(g_remote_form_config.password), 1, 1, 1);
            break;
        case 6:  // Private Key (SFTP) or Base Path
            if (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) {
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, g_remote_form_config.private_key_path,
                                  sizeof(g_remote_form_config.private_key_path), 1, 0, 1);
            } else {
                prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                                  label, g_remote_form_config.base_path,
                                  sizeof(g_remote_form_config.base_path), 1, 0, 1);
                if (!g_remote_form_config.base_path[0])
                    strncpy(g_remote_form_config.base_path, "/",
                            sizeof(g_remote_form_config.base_path) - 1);
            }
            break;
        case 7:  // Base Path
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.base_path,
                              sizeof(g_remote_form_config.base_path), 1, 0, 1);
            if (!g_remote_form_config.base_path[0])
                strncpy(g_remote_form_config.base_path, "/",
                        sizeof(g_remote_form_config.base_path) - 1);
            break;
    }

    curs_set(0);
    noecho();
}

static void remote_form_save(void) {
    if (!g_remote_form_config.name[0]) {
        show_status_message(menu_text("名称为必填项", "Name is required"));
        rerender_remote_view();
        return;
    }
    if (!g_remote_form_config.host[0]) {
        show_status_message(menu_text("主机地址为必填项", "Host is required"));
        rerender_remote_view();
        return;
    }
    if (!g_remote_form_config.base_path[0]) {
        strncpy(g_remote_form_config.base_path, "/",
                sizeof(g_remote_form_config.base_path) - 1);
    }

    if (g_remote_form_editing_idx >= 0) {
        g_app_config.remote_connections[g_remote_form_editing_idx] = g_remote_form_config;
    } else {
        if (g_app_config.remote_connection_count >= MAX_REMOTE_CONNECTIONS) {
            show_status_message(menu_text("远程连接已满", "Remote connections full"));
            rerender_remote_view();
            return;
        }
        g_app_config.remote_connections[g_app_config.remote_connection_count++] = g_remote_form_config;
    }

    save_config();
    show_status_message(menu_text("连接已保存", "Connection saved"));
    g_remote_mode = 0;
    g_remote_selected = g_remote_form_editing_idx >= 0
        ? g_remote_form_editing_idx
        : (g_app_config.remote_connection_count - 1);
    g_remote_form_editing_idx = -1;
    rerender_remote_view();
}

static void remote_form_cancel(void) {
    g_remote_mode = 0;
    g_remote_selected = g_remote_form_editing_idx >= 0
        ? g_remote_form_editing_idx
        : g_app_config.remote_connection_count;
    g_remote_form_editing_idx = -1;
    rerender_remote_view();
}

static void remote_start_add(void) {
    memset(&g_remote_form_config, 0, sizeof(g_remote_form_config));
    g_remote_form_config.protocol = REMOTE_PROTOCOL_FTP;
    g_remote_form_editing_idx = -1;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_start_edit(int conn_idx) {
    if (conn_idx < 0 || conn_idx >= g_app_config.remote_connection_count) return;
    g_remote_form_config = g_app_config.remote_connections[conn_idx];
    g_remote_form_editing_idx = conn_idx;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_delete_connection(int conn_idx) {
    if (conn_idx < 0 || conn_idx >= g_app_config.remote_connection_count) return;
    if (conn_idx < g_app_config.remote_connection_count - 1) {
        memmove(&g_app_config.remote_connections[conn_idx],
                &g_app_config.remote_connections[conn_idx + 1],
                sizeof(RemoteConnectionConfig) * (g_app_config.remote_connection_count - conn_idx - 1));
    }
    g_app_config.remote_connection_count--;
    save_config();
    show_status_message(menu_text("连接已删除", "Connection deleted"));

    g_remote_mode = 0;
    if (g_remote_selected >= g_app_config.remote_connection_count) {
        g_remote_selected = g_app_config.remote_connection_count > 0 ? g_app_config.remote_connection_count - 1 : 0;
    }
    rerender_remote_view();
}

static void render_remote_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int content_start_x = (max_x / 4) + 2;
    int y = 2;

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    for (int row = 2; row < max_y - 2; row++) {
        move(row, content_start_x);
        clrtoeol();
    }

    if (g_remote_mode == 0) {
        mvprintw(y++, content_start_x, "%s",
                 menu_text("远程设备：↑/↓ 选择 ENTER 管理  ESC 返回",
                           "Remote: Up/Down select Enter manage Esc back"));
        y++;

        int count = g_app_config.remote_connection_count;
        char header[64];
        snprintf(header, sizeof(header), "%s (%d)",
                 menu_text("已保存的连接", "Saved Connections"), count);
        mvprintw(y++, content_start_x, "%s", header);
        y++;

        for (int i = 0; i < count && y < max_y - 3; i++) {
            const RemoteConnectionConfig *rc = &g_app_config.remote_connections[i];
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %-20s [%s] %s",
                     rc->name, remote_protocol_name(rc->protocol), rc->host);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
        }

        y++;
        if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attron(A_REVERSE);
        mvprintw(y++, content_start_x, "  %s", menu_text("↓ 添加新连接", "↓ Add New Connection"));
        if (g_focus_area == FOCUS_CONTENT && g_remote_selected == count) attroff(A_REVERSE);

    } else if (g_remote_mode == 1) {
        int conn_idx = g_remote_selected_conn;
        if (conn_idx < 0 || conn_idx >= g_app_config.remote_connection_count) {
            mvprintw(y++, content_start_x, "%s", menu_text("(无连接)", "(No connection)"));
            attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
            return;
        }
        const RemoteConnectionConfig *rc = &g_app_config.remote_connections[conn_idx];

        mvprintw(y++, content_start_x, "%s: %s [%s]",
                 menu_text("连接", "Connection"), rc->name, remote_protocol_name(rc->protocol));
        y++;

        const char *actions[] = {
            menu_text("浏览目录", "Browse Folder"),
            menu_text("加载此目录到播放列表", "Load to Playlist"),
            menu_text("刷新连接", "Refresh Connection"),
            menu_text("编辑连接", "Edit Connection"),
            menu_text("删除连接", "Delete Connection")
        };
        int action_count = 5;

        for (int i = 0; i < action_count; i++) {
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %s", actions[i]);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
        }

    } else if (g_remote_mode == 2) {
        int field_count = remote_form_field_count();
        const char *title_fmt = g_remote_form_editing_idx >= 0
            ? menu_text("编辑连接 (↑/↓ 选择 ENTER 编辑 +/- 切换协议 S 保存 ESC 取消)",
                       "Edit Connection (Up/Down select Enter edit +/- Protocol S Save Esc Cancel)")
            : menu_text("添加连接 (↑/↓ 选择 ENTER 编辑 +/- 切换协议 S 保存 ESC 取消)",
                       "Add Connection (Up/Down select Enter edit +/- Protocol S Save Esc Cancel)");
        mvprintw(y++, content_start_x, "%s", title_fmt);
        y++;

        char label[32], value[256];
        for (int i = 0; i < field_count && y < max_y - 2; i++) {
            remote_form_field_label(i, label, sizeof(label));
            remote_form_value_text(i, value, sizeof(value));
            move(y, content_start_x);
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attron(A_REVERSE);
            printw("  %-14s %s", label, value);
            clrtoeol();
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == i) attroff(A_REVERSE);
            y++;
        }

        if (y < max_y - 2) {
            mvprintw(y, content_start_x, "%s",
                     menu_text("↑/↓:选择  ENTER:编辑  +/-:协议  S:保存  ESC:取消",
                               "Up/Down:navigate  Enter:edit  +/-:protocol  S:save  Esc:cancel"));
        }

    } else if (g_remote_mode == 3) {
        if (g_remote_selected_conn < 0) {
            mvprintw(y++, content_start_x, "%s", menu_text("(无连接)", "(No connection)"));
            attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
            return;
        }
        const RemoteConnectionConfig *conn = &g_app_config.remote_connections[g_remote_selected_conn];

        char header[256];
        snprintf(header, sizeof(header), "%s: %s > %s",
                 menu_text("浏览", "Browse"), conn->name,
                 g_remote_current_path[0] ? g_remote_current_path : "/");
        mvprintw(y++, content_start_x, "%s", header);
        y++;

        if (!g_remote_entries) {
            mvprintw(y++, content_start_x, "%s", menu_text("正在加载...", "Loading..."));
        } else if (g_remote_entry_count == 0) {
            mvprintw(y++, content_start_x, "%s", menu_text("(空目录)", "(Empty directory)"));
        } else {
            int display_count = g_remote_entry_count;
            int max_display = max_y - y - 3;
            if (display_count > max_display) display_count = max_display;
            if (g_remote_entry_offset > g_remote_entry_count - display_count)
                g_remote_entry_offset = g_remote_entry_count - display_count;
            if (g_remote_entry_offset < 0) g_remote_entry_offset = 0;

            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attron(A_REVERSE);
            mvprintw(y++, content_start_x, "  %s",
                     menu_text("↓ 加载此目录到播放列表", "↓ Load into Playlist"));
            if (g_focus_area == FOCUS_CONTENT && g_remote_selected == 0) attroff(A_REVERSE);

            for (int i = g_remote_entry_offset; i < g_remote_entry_count && y < max_y - 3; i++) {
                const RemoteDirEntry *e = &g_remote_entries[i];
                int is_sel = (g_focus_area == FOCUS_CONTENT && g_remote_selected == i + 1);
                if (is_sel) attron(A_REVERSE);
                if (e->is_dir) {
                    mvprintw(y++, content_start_x, "  [%s] %s",
                             menu_text("目录", "dir"), e->name);
                } else {
                    mvprintw(y++, content_start_x, "  %s", e->name);
                }
                if (is_sel) attroff(A_REVERSE);
            }
        }

        if (y < max_y - 2) {
            mvprintw(y, content_start_x, "%s",
                     menu_text("↑/↓ 选择  ENTER 加载  RIGHT 进入目录  ESC 返回",
                               "Up/Down select  Enter load  Right enter dir  Esc back"));
        }
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

static void handle_remote_content_input(int ch) {
    int conn_count = g_app_config.remote_connection_count;

    if (g_remote_mode == 0) {
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = conn_count;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected > conn_count) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                if (g_remote_selected >= 0 && g_remote_selected < conn_count) {
                    g_remote_selected_conn = g_remote_selected;
                    g_remote_mode = 1;
                    g_remote_selected = 0;
                    rerender_remote_view();
                } else {
                    remote_start_add();
                }
                break;
            case KEY_LEFT:
            case 27:
                remote_go_back();
                break;
        }
    } else if (g_remote_mode == 1) {
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = 4;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected > 4) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                if (g_remote_selected == 0) {
                    remote_start_browse(g_remote_selected_conn);
                } else if (g_remote_selected == 1) {
                    if (g_remote_selected_conn >= 0 && g_remote_selected_conn < g_app_config.remote_connection_count) {
                        const RemoteConnectionConfig *c = &g_app_config.remote_connections[g_remote_selected_conn];
                        strncpy(g_remote_current_path, c->base_path, sizeof(g_remote_current_path) - 1);
                        remote_load_playlist();
                    }
                } else if (g_remote_selected == 2) {
                    remote_refresh_connection();
                } else if (g_remote_selected == 3) {
                    remote_start_edit(g_remote_selected_conn);
                } else if (g_remote_selected == 4) {
                    remote_delete_connection(g_remote_selected_conn);
                }
                break;
            case KEY_LEFT:
            case 27:
                remote_go_back();
                break;
        }
    } else if (g_remote_mode == 2) {
        int field_count = remote_form_field_count();
        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = field_count - 1;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= field_count) g_remote_selected = 0;
                render_settings_content();
                break;
            case 10:
            case ' ':
                remote_form_edit_field(g_remote_selected);
                render_settings_content();
                break;
            case '+':
            case '=':
                if (g_remote_selected == 1) {
                    g_remote_form_config.protocol = (g_remote_form_config.protocol + 1) % 5;
                    if (g_remote_selected >= remote_form_field_count())
                        g_remote_selected = remote_form_field_count() - 1;
                    render_settings_content();
                }
                break;
            case '-':
            case '_':
                if (g_remote_selected == 1) {
                    g_remote_form_config.protocol = (g_remote_form_config.protocol + 4) % 5;
                    if (g_remote_selected >= remote_form_field_count())
                        g_remote_selected = remote_form_field_count() - 1;
                    render_settings_content();
                }
                break;
            case 's':
            case 'S':
                remote_form_save();
                break;
            case KEY_LEFT:
            case 27:
                remote_form_cancel();
                break;
        }
    } else if (g_remote_mode == 3) {
        int total_items = 1 + g_remote_entry_count;

        switch (ch) {
            case KEY_UP:
                g_remote_selected--;
                if (g_remote_selected < 0) g_remote_selected = total_items - 1;
                render_settings_content();
                break;
            case KEY_DOWN:
                g_remote_selected++;
                if (g_remote_selected >= total_items) g_remote_selected = 0;
                render_settings_content();
                break;
            case KEY_RIGHT:
                if (g_remote_selected > 0) {
                    int entry_idx = g_remote_selected - 1;
                    if (entry_idx >= 0 && entry_idx < g_remote_entry_count &&
                        g_remote_entries[entry_idx].is_dir) {
                        size_t cur_len = strlen(g_remote_current_path);
                        if (cur_len > 0 && g_remote_current_path[cur_len - 1] != '/') {
                            strncat(g_remote_current_path, "/", sizeof(g_remote_current_path) - cur_len - 1);
                        }
                        strncat(g_remote_current_path, g_remote_entries[entry_idx].name,
                                sizeof(g_remote_current_path) - strlen(g_remote_current_path) - 1);
                        g_remote_selected = 0;
                        g_remote_entry_offset = 0;
                        remote_refresh_entries();
                        rerender_remote_view();
                    }
                }
                break;
            case 10:
            case ' ':
                if (g_remote_selected == 0) {
                    remote_load_playlist();
                } else {
                    int entry_idx = g_remote_selected - 1;
                    if (entry_idx >= 0 && entry_idx < g_remote_entry_count) {
                        if (g_remote_entries[entry_idx].is_dir) {
                            size_t cur_len = strlen(g_remote_current_path);
                            if (cur_len > 0 && g_remote_current_path[cur_len - 1] != '/') {
                                strncat(g_remote_current_path, "/", sizeof(g_remote_current_path) - cur_len - 1);
                            }
                            strncat(g_remote_current_path, g_remote_entries[entry_idx].name,
                                    sizeof(g_remote_current_path) - strlen(g_remote_current_path) - 1);
                            g_remote_selected = 0;
                            g_remote_entry_offset = 0;
                            remote_refresh_entries();
                            rerender_remote_view();
                        } else {
                            remote_load_playlist();
                        }
                    }
                }
                break;
            case KEY_LEFT:
                remote_go_back();
                break;
            case 27:
                remote_go_back();
                break;
        }
    }
}

void render_settings_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;
    SettingsSectionSpec spec = get_active_settings_section_spec();

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    for (int y = 2; y < max_y - 2; y++) {
        move(y, content_start_x);
        clrtoeol();
    }

    if (g_menu_selected_idx == 0) {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("颜色主题：↑/↓ 选择，+/ - 或 → 调整，←/TAB 返回菜单",
                           "Theme: Up/Down select, +/- or Right adjust, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("前景色和背景色会自动避免使用相同颜色",
                           "Foreground/background pairs avoid identical colors"));
    } else if (g_menu_selected_idx == 1) {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("默认路径：ENTER 编辑，←/TAB 返回菜单",
                           "Default path: Enter edits, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("支持使用 ~ 开头的用户目录路径",
                           "Paths starting with ~ are expanded to your home directory"));
    } else if (g_menu_selected_idx == 2) {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("播放设置：↑/↓ 选择，ENTER 切换，+/ - 调整数值，←/TAB 返回菜单",
                           "Playback: Up/Down select, Enter toggles, +/- adjusts, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("语言可用 ENTER 切换，时延会在下次播放时生效",
                           "Press Enter to toggle language; latency applies on next playback"));
    } else if (g_menu_selected_idx == 3) {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("快捷键说明：←/TAB 返回菜单",
                           "Hotkeys: Left/Tab back"));
        start_y += 2;
        mvprintw(start_y++, content_start_x, "%s", menu_text("F1-F8：切换页面 / 语言 / 退出", "F1-F8: Views / language / quit"));
        mvprintw(start_y++, content_start_x, "%s", menu_text("O / I：打开目录 / 追加目录", "O / I: Open folder / append folder"));
        mvprintw(start_y++, content_start_x, "%s", menu_text("C / L：切换控件焦点与列表焦点", "C / L: Controls focus / list focus"));
        mvprintw(start_y++, content_start_x, "%s", menu_text("D：进入或退出歌词定位模式", "D: Toggle lyric seek mode"));
        mvprintw(start_y++, content_start_x, "%s", menu_text("空格 / Enter：执行当前动作", "Space / Enter: Activate current action"));
        mvprintw(start_y++, content_start_x, "%s", menu_text("+ / -：调整音量", "+ / -: Adjust volume"));
        mvprintw(start_y++, content_start_x, "%s", menu_text(", / .：快退 / 快进", ", / .: Seek backward / forward"));
    } else if (g_menu_selected_idx == 4) {
        render_remote_content();
    } else {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("按 ENTER 返回主界面", "Press Enter to return to the main view"));
    }
    
    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    
    refresh();
}

static int g_history_content_offset = 0;

void render_history_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;
    
    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    
    mvprintw(start_y, content_start_x, use_english_ui() ? "Folder History (%d)" : "目录历史（%d 个目录）", g_dir_history.count);
    mvprintw(start_y + 1, content_start_x, "----------------------------------------");
    start_y += 3;
    
    if (g_dir_history.count == 0) {
        mvprintw(start_y, content_start_x, "%s", menu_text("还没有目录历史。", "No folder history yet."));
        mvprintw(start_y + 1, content_start_x, "%s", menu_text("打开音乐目录后会自动记录。", "Opened music folders will appear here."));
    } else {
        int visible_lines = max_y - start_y - 2;
        
        if (g_content_selected_idx >= g_dir_history.count) {
            g_content_selected_idx = g_dir_history.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;
        
        if (g_content_selected_idx < g_history_content_offset) {
            g_history_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_history_content_offset + visible_lines) {
            g_history_content_offset = g_content_selected_idx - visible_lines + 1;
        }
        
        for (int i = 0; i < visible_lines && (g_history_content_offset + i) < g_dir_history.count; i++) {
            int idx = g_history_content_offset + i;
            DirHistoryEntry *entry = &g_dir_history.entries[idx];
            
            char time_str[64];
            struct tm *tm_info = localtime(&entry->open_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);
            
            char display_path[MAX_PATH_LEN];
            int path_width = max_x - menu_width - 25;
            if (path_width > MAX_PATH_LEN - 1) path_width = MAX_PATH_LEN - 1;
            utf8_str_truncate(display_path, entry->path, path_width);
            
            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                mvprintw(start_y + i, content_start_x, " %s [%s]", display_path, time_str);
                attroff(A_REVERSE);
            } else {
                mvprintw(start_y + i, content_start_x, " %s [%s]", display_path, time_str);
            }
        }
        
        int bottom_y = max_y - 3;
        mvprintw(bottom_y, content_start_x, "%s",
                 menu_text("按 ENTER 替换播放列表，按 'A' 追加到队列",
                           "Press Enter to replace, 'A' to append"));
        mvprintw(bottom_y + 1, content_start_x, "%s",
                 menu_text("按 'D' 删除当前项，按 'C' 清空全部",
                           "Press 'D' to delete, 'C' to clear all"));
    }
    
    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    
    refresh();
}

static int g_playlist_content_offset = 0;
static int g_playlist_view_mode = 0;
static int g_playlist_selected_playlist = -1;
static int g_playlist_track_offset = 0;

void render_playlist_manager_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;
    
    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));
    
    if (g_playlist_view_mode == 0) {
        mvprintw(start_y, content_start_x, use_english_ui() ? "Playlists (%d)" : "用户歌单（%d 个）", g_playlist_manager.count);
        mvprintw(start_y + 1, content_start_x, "----------------------------------------");
        start_y += 3;
        
        if (g_playlist_manager.count == 0) {
            mvprintw(start_y, content_start_x, "%s", menu_text("还没有创建歌单。", "No playlists created yet."));
            mvprintw(start_y + 1, content_start_x, "%s", menu_text("请从左侧选择“新建歌单”。", "Choose 'New Playlist' on the left."));
        } else {
            int visible_lines = max_y - start_y - 2;
            
            for (int i = 0; i < visible_lines && i < g_playlist_manager.count; i++) {
                UserPlaylist *pl = &g_playlist_manager.playlists[i];
                char display_name[MAX_PLAYLIST_NAME_LEN + 8];
                utf8_str_pad(display_name, sizeof(display_name), pl->name, 30);
                
                if (i == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                    attron(A_REVERSE);
                    mvprintw(start_y + i, content_start_x,
                             use_english_ui() ? " %s (%d tracks)" : " %s (%d 首)",
                             display_name, pl->track_count);
                    attroff(A_REVERSE);
                } else {
                    mvprintw(start_y + i, content_start_x,
                             use_english_ui() ? " %s (%d tracks)" : " %s (%d 首)",
                             display_name, pl->track_count);
                }
            }
            
            int bottom_y = max_y - 3;
            mvprintw(bottom_y, content_start_x, "%s",
                     menu_text("ENTER: 查看歌曲 | D: 删除歌单 | R: 重命名",
                               "Enter: View tracks | D: Delete | R: Rename"));
        }
    } else {
        if (g_playlist_selected_playlist >= 0 && g_playlist_selected_playlist < g_playlist_manager.count) {
            UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
            
            mvprintw(start_y, content_start_x,
                     use_english_ui() ? "Playlist: %s (%d tracks)" : "歌单：%s（%d 首）",
                     pl->name, pl->track_count);
            mvprintw(start_y + 1, content_start_x, "----------------------------------------");
            start_y += 3;
            
            if (pl->track_count == 0) {
                mvprintw(start_y, content_start_x, "%s", menu_text("这个歌单还是空的。", "This playlist is empty."));
                mvprintw(start_y + 1, content_start_x, "%s", menu_text("请从主界面把歌曲加入歌单。", "Add tracks from the main view."));
            } else {
                int visible_lines = max_y - start_y - 2;
                
                if (g_content_selected_idx >= pl->track_count) {
                    g_content_selected_idx = pl->track_count - 1;
                }
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                
                if (g_content_selected_idx < g_playlist_track_offset) {
                    g_playlist_track_offset = g_content_selected_idx;
                } else if (g_content_selected_idx >= g_playlist_track_offset + visible_lines) {
                    g_playlist_track_offset = g_content_selected_idx - visible_lines + 1;
                }
                
                for (int i = 0; i < visible_lines && (g_playlist_track_offset + i) < pl->track_count; i++) {
                    int idx = g_playlist_track_offset + i;
                    Track *t = &pl->tracks[idx];
                    
                    char truncated_title[MAX_META_LEN];
                    char display_title[MAX_META_LEN + 32];
                    int title_width = max_x - menu_width - 30;
                    utf8_str_truncate(truncated_title, t->title, title_width > 0 ? title_width : 30);
                    utf8_str_pad(display_title, sizeof(display_title), truncated_title, title_width > 0 ? title_width : 30);
                    
                    if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                        attron(A_REVERSE);
                        mvprintw(start_y + i, content_start_x, " %s - %s", display_title, t->artist);
                        attroff(A_REVERSE);
                    } else {
                        mvprintw(start_y + i, content_start_x, " %s - %s", display_title, t->artist);
                    }
                }
                
                int bottom_y = max_y - 3;
                mvprintw(bottom_y, content_start_x, "%s",
                         menu_text("ENTER: 播放 | D: 从歌单移除 | ESC: 返回",
                                   "Enter: Play | D: Remove | Esc: Back"));
            }
        }
    }
    
    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    
    refresh();
}

static int g_favorites_content_offset = 0;

void render_favorites_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;
    
    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));
    
    mvprintw(start_y, content_start_x, use_english_ui() ? "Favorites (%d)" : "收藏夹（%d 首）", g_favorites.count);
    mvprintw(start_y + 1, content_start_x, "----------------------------------------");
    start_y += 3;
    
    if (g_favorites.count == 0) {
        mvprintw(start_y, content_start_x, "%s", menu_text("还没有收藏。", "No favorites yet."));
        mvprintw(start_y + 1, content_start_x, "%s", menu_text("在主界面按 'F' 可加入收藏。", "Press 'F' in the main view to favorite."));
    } else {
        int visible_lines = max_y - start_y - 2;
        
        if (g_content_selected_idx >= g_favorites.count) {
            g_content_selected_idx = g_favorites.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;
        
        if (g_content_selected_idx < g_favorites_content_offset) {
            g_favorites_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_favorites_content_offset + visible_lines) {
            g_favorites_content_offset = g_content_selected_idx - visible_lines + 1;
        }
        
        for (int i = 0; i < visible_lines && (g_favorites_content_offset + i) < g_favorites.count; i++) {
            int idx = g_favorites_content_offset + i;
            Track *t = &g_favorites.tracks[idx];
            
            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            char display_title[MAX_META_LEN + 32];
            char display_artist[MAX_META_LEN + 32];
            int title_width = (max_x - menu_width - 10) * 3 / 5;
            int artist_width = (max_x - menu_width - 10) * 2 / 5;
            
            utf8_str_truncate(truncated_title, t->title, title_width > 0 ? title_width : 30);
            utf8_str_truncate(truncated_artist, t->artist, artist_width > 0 ? artist_width : 20);
            utf8_str_pad(display_title, sizeof(display_title), truncated_title, title_width > 0 ? title_width : 30);
            utf8_str_pad(display_artist, sizeof(display_artist), truncated_artist, artist_width > 0 ? artist_width : 20);
            
            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                mvprintw(start_y + i, content_start_x, " %s - %s", display_title, display_artist);
                attroff(A_REVERSE);
            } else {
                mvprintw(start_y + i, content_start_x, " %s - %s", display_title, display_artist);
            }
        }
        
        int bottom_y = max_y - 3;
        mvprintw(bottom_y, content_start_x, "%s",
                 menu_text("ENTER: 播放 | D: 移出收藏",
                           "Enter: Play | D: Remove"));
    }
    
    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    
    refresh();
}

void render_info_content(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;
    
    attron(COLOR_PAIR(COLOR_PAIR_BORDER));
    
    mvprintw(start_y, content_start_x, "%s %s", menu_text("关于", "About"), APP_NAME);
    mvprintw(start_y + 1, content_start_x, "========================================");
    start_y += 3;
    
    mvprintw(start_y, content_start_x, use_english_ui() ? "Name: %s" : "名称：%s", APP_NAME);
    mvprintw(start_y + 1, content_start_x, use_english_ui() ? "Version: %s" : "版本：%s", APP_VERSION);
    mvprintw(start_y + 2, content_start_x, use_english_ui() ? "Authors: %s" : "巨献：%s", APP_AUTHORS);
    mvprintw(start_y + 3, content_start_x, use_english_ui() ? "Email: %s" : "邮箱：%s", APP_EMAIL);
    start_y += 5;
    
    mvprintw(start_y, content_start_x, "%s", menu_text("简介：", "Summary:"));
    mvprintw(start_y + 1, content_start_x, "%s", menu_text("  基于 ncurses 的终端音乐播放器。", "  A terminal music player built on ncurses."));
    mvprintw(start_y + 2, content_start_x, "%s", menu_text("  通过 FFmpeg 支持多种音频格式。", "  Supports multiple audio formats via FFmpeg."));
    mvprintw(start_y + 3, content_start_x, "%s", menu_text("  提供歌单、收藏和歌词显示。", "  Includes playlists, favorites, and lyrics."));
    start_y += 5;
    
    mvprintw(start_y, content_start_x, "%s", menu_text("仓库地址：", "Repository:"));
    mvprintw(start_y + 1, content_start_x, "  %s", APP_REPO);
    start_y += 3;
    
    mvprintw(start_y, content_start_x, "%s", menu_text("许可证：GPL v3", "License: GPL v3"));
    start_y += 2;
    
    mvprintw(start_y, content_start_x, "%s", menu_text("这里的信息为只读。", "This page is read-only."));
    
    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));

    refresh();
}

/* ──── 帮助页面 ──── */

#define HELP_MAX_LINES 2000
#define HELP_SEARCH_RESULTS 200

static char *g_help_lines[HELP_MAX_LINES];
static int g_help_line_count = 0;
static int g_help_scroll_offset = 0;
static int g_help_loaded = 0;

static int g_help_search_results[HELP_SEARCH_RESULTS];
static int g_help_search_count = 0;
static int g_help_search_active = 0;
static int g_help_search_selected = -1;

void help_free_lines(void) {
    for (int i = 0; i < g_help_line_count; i++) {
        free(g_help_lines[i]);
        g_help_lines[i] = NULL;
    }
    g_help_line_count = 0;
    g_help_loaded = 0;
}

static void help_load_file(void) {
    if (g_help_loaded) return;

    help_free_lines();

    const char *suffix = use_english_ui() ? "en" : "zh";
    char path[MAX_PATH_LEN];
    FILE *f = NULL;

    snprintf(path, sizeof(path), "/usr/local/share/ter-music/help-quickstart-%s.txt", suffix);
    f = fopen(path, "r");

    if (!f) {
        snprintf(path, sizeof(path), "/usr/share/ter-music/help-quickstart-%s.txt", suffix);
        f = fopen(path, "r");
    }

    if (!f) {
        snprintf(path, sizeof(path), "data/help-quickstart-%s.txt", suffix);
        f = fopen(path, "r");
    }

    if (!f) return;

    char buf[1024];
    while (fgets(buf, sizeof(buf), f) && g_help_line_count < HELP_MAX_LINES) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        g_help_lines[g_help_line_count] = strdup(buf);
        g_help_line_count++;
    }
    fclose(f);

    g_help_loaded = 1;
}

void render_help_content(void) {
    help_load_file();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    int visible_lines = max_y - start_y - 5;
    if (visible_lines < 1) visible_lines = 1;

    int total_lines = g_help_line_count;

    /* 若搜索结果激活且选中项存在，滚动到选中位置 */
    if (g_help_search_active && g_help_search_count > 0 && g_help_search_selected >= 0) {
        int target = g_help_search_results[g_help_search_selected];
        g_help_scroll_offset = target - visible_lines / 2;
        if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
    }

    /* 3 行底部缓冲 */
    int max_offset = total_lines - visible_lines - 3;
    if (max_offset < 0) max_offset = 0;
    if (g_help_scroll_offset > max_offset) g_help_scroll_offset = max_offset;

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    mvprintw(start_y, content_start_x, "%s",
             use_english_ui() ? "Help [F8]" : "帮助 [F8]");
    mvprintw(start_y + 1, content_start_x, "========================================");

    for (int i = 0; i < visible_lines && (g_help_scroll_offset + i) < total_lines; i++) {
        int line_idx = g_help_scroll_offset + i;
        int row = start_y + 3 + i;

        int is_match = 0;
        if (g_help_search_active && g_help_search_count > 0) {
            for (int j = 0; j < g_help_search_count; j++) {
                if (g_help_search_results[j] == line_idx) {
                    is_match = 1;
                    break;
                }
            }
        }

        mvhline(row, content_start_x, ' ', max_x - content_start_x - 1);
        if (is_match) attron(A_BOLD);
        mvprintw(row, content_start_x, "%s", g_help_lines[line_idx]);
        if (is_match) attroff(A_BOLD);
    }

    /* 底部提示行 */
    int hint_row = max_y - 3;
    mvhline(hint_row, content_start_x, ' ', max_x - content_start_x - 1);

    char hint[256];
    if (g_help_search_active && g_help_search_count > 0) {
        snprintf(hint, sizeof(hint),
                 use_english_ui()
                     ? "Search: %d matches | n/N: next | Esc: clear search"
                     : "搜索: %d 个匹配 | n/N: 下一个 | Esc: 清除搜索",
                 g_help_search_count);
    } else {
        snprintf(hint, sizeof(hint),
                 use_english_ui()
                     ? "Up/Down: scroll 3 lines  |  /: search  |  Esc: back"
                     : "上/下: 滚动3行  |  /: 搜索  |  Esc: 返回");
    }

    int display_end = g_help_scroll_offset + visible_lines;
    if (display_end > total_lines) display_end = total_lines;

    char pos[64];
    snprintf(pos, sizeof(pos), "%s %d-%d / %d",
             use_english_ui() ? "Line" : "行",
             g_help_scroll_offset + 1, display_end, total_lines);

    mvprintw(hint_row, content_start_x, "%s", hint);
    mvprintw(hint_row, max_x - (int)strlen(pos) - 3, "%s", pos);

    // 绘制滚动条
    scrollbar_draw(stdscr, start_y + 3, visible_lines,
                   total_lines, visible_lines, g_help_scroll_offset, max_x - 2);

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));

    refresh();
}

static void help_search_prompt(void) {
    echo();
    curs_set(1);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int search_row = max_y - 3;

    mvprintw(search_row, content_start_x, "%s",
             use_english_ui() ? "Search: " : "搜索: ");
    clrtoeol();
    refresh();

    char input[MAX_META_LEN];
    memset(input, 0, sizeof(input));
    int pos = 0;
    int ch;

    flushinp();

    while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != 27 && pos < MAX_META_LEN - 1) {
        if (ch == ERR) continue;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                mvaddch(search_row, content_start_x + 8 + pos, ' ');
                move(search_row, content_start_x + 8 + pos);
                refresh();
            }
        } else if (ch >= 32 && ch < 127) {
            input[pos++] = (char)ch;
            addch(ch);
            refresh();
        }
    }

    input[pos] = '\0';
    flushinp();
    noecho();
    curs_set(0);

    if (ch == 27) {
        g_help_search_active = 0;
        g_help_search_count = 0;
        render_help_content();
        return;
    }

    if (strlen(input) > 0) {
        g_help_search_count = search_lines(
            (const char **)g_help_lines, g_help_line_count,
            input, g_help_search_results, HELP_SEARCH_RESULTS);
        g_help_search_active = 1;
        g_help_search_selected = 0;

        if (g_help_search_count > 0) {
            g_help_scroll_offset = g_help_search_results[0] - 2;
            if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
        }
    }

    render_help_content();
}

static void handle_help_input(int ch) {
    switch (ch) {
        case KEY_UP:
            g_help_scroll_offset -= 3;
            if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
            render_help_content();
            break;

        case KEY_DOWN:
            g_help_scroll_offset += 3;
            render_help_content();
            break;

        case '/':
            help_search_prompt();
            break;

        case 'n':
        case 'N':
            if (g_help_search_active && g_help_search_count > 0) {
                g_help_search_selected++;
                if (g_help_search_selected >= g_help_search_count) {
                    g_help_search_selected = 0;
                }
                render_help_content();
            }
            break;

        case 27:
            if (g_help_search_active) {
                g_help_search_active = 0;
                g_help_search_count = 0;
                render_help_content();
            } else {
                exit_current_view();
            }
            break;
    }
}

void switch_to_view(ViewMode view) {
    log_info("menu_views", "View switched: %d -> %d", g_current_view, view);
    g_current_view = view;
    g_menu_selected_idx = 0;
    g_content_selected_idx = 0;
    g_focus_area = FOCUS_SIDEBAR;
    g_settings_current_option = 0;
    g_history_content_offset = 0;
    g_playlist_view_mode = 0;
    g_playlist_selected_playlist = -1;
    g_playlist_track_offset = 0;
    g_favorites_content_offset = 0;
    g_help_scroll_offset = 0;
    g_help_search_active = 0;
    g_help_search_count = 0;
    
    switch (view) {
        case VIEW_SETTINGS:
            render_menu_frame("设置 [F2]");
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            render_menu_hint_bar();
            break;
        case VIEW_HISTORY:
            render_menu_frame("历史 [F3]");
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            render_history_content();
            render_menu_hint_bar();
            break;
        case VIEW_PLAYLIST:
            render_menu_frame("歌单 [F4]");
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            render_playlist_manager_content();
            render_menu_hint_bar();
            break;
        case VIEW_FAVORITES:
            render_menu_frame("收藏 [F5]");
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            render_favorites_content();
            render_menu_hint_bar();
            break;
        case VIEW_INFO:
            render_menu_frame("信息 [F6]");
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            render_info_content();
            render_menu_hint_bar();
            break;
        case VIEW_HELP:
            render_menu_frame(use_english_ui() ? "Help [F8]" : "帮助 [F8]");
            render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            render_help_content();
            render_menu_hint_bar();
            break;
        default:
            break;
    }
}

void exit_current_view(void) {
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

static void rerender_active_view(void) {
    if (g_current_view == VIEW_MAIN) {
        render_playlist_content();
        render_controls();
        render_lyrics();
        render_menu_hint_bar();
        return;
    }

    switch (g_current_view) {
        case VIEW_SETTINGS:
            render_menu_frame("设置 [F2]");
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            render_menu_hint_bar();
            break;
        case VIEW_HISTORY:
            render_menu_frame("历史 [F3]");
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            render_history_content();
            render_menu_hint_bar();
            break;
        case VIEW_PLAYLIST:
            render_menu_frame("歌单 [F4]");
            render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            render_playlist_manager_content();
            render_menu_hint_bar();
            break;
        case VIEW_FAVORITES:
            render_menu_frame("收藏 [F5]");
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            render_favorites_content();
            render_menu_hint_bar();
            break;
        case VIEW_INFO:
            render_menu_frame("信息 [F6]");
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            render_info_content();
            render_menu_hint_bar();
            break;
        case VIEW_HELP:
            render_menu_frame(use_english_ui() ? "Help [F8]" : "帮助 [F8]");
            render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            render_help_content();
            render_menu_hint_bar();
            break;
        default:
            break;
    }
}

void toggle_ui_language(void) {
    g_app_config.ui_language = (g_app_config.ui_language == UI_LANG_EN) ? UI_LANG_ZH : UI_LANG_EN;
    save_config();
    help_free_lines();
    rerender_active_view();
}

void handle_function_keys(int fkey) {
    log_debug("menu_views", "Function key F%d pressed", fkey - KEY_F(0));
    switch(fkey) {
        case KEY_F(1):
            exit_current_view();
            break;
        case KEY_F(2):
            switch_to_view(VIEW_SETTINGS);
            break;
        case KEY_F(3):
            switch_to_view(VIEW_HISTORY);
            break;
        case KEY_F(4):
            switch_to_view(VIEW_PLAYLIST);
            break;
        case KEY_F(5):
            switch_to_view(VIEW_FAVORITES);
            break;
        case KEY_F(6):
            switch_to_view(VIEW_INFO);
            break;
        case KEY_F(7):
            toggle_ui_language();
            break;
        case KEY_F(8):
            switch_to_view(VIEW_HELP);
            break;
        case KEY_F(9):
            cleanup();
            printf("%s\n", menu_text("ter-music 已正常退出。", "ter-music exited cleanly."));
            exit(0);
            break;
        default:
            break;
    }
}

static void handle_settings_input(int ch) {
    // Remote section handles its own input when content-focused
    if (g_menu_selected_idx == 4 && g_focus_area == FOCUS_CONTENT) {
        handle_remote_content_input(ch);
        return;
    }
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = SETTINGS_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            } else {
                move_settings_content_selection(-1);
                render_settings_content();
            }
            break;
            
        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= SETTINGS_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            } else {
                move_settings_content_selection(1);
                render_settings_content();
            }
            break;
            
        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            }
            break;
            
        case KEY_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == SETTINGS_ITEM_COUNT - 1) {
                    exit_current_view();
                } else {
                    g_focus_area = FOCUS_CONTENT;
                    sync_settings_selection_to_sidebar();
                    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                    render_settings_content();
                }
            } else {
                adjust_or_toggle_settings_option(g_settings_current_option, 1);
                rerender_settings_view();
            }
            break;

        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == SETTINGS_ITEM_COUNT - 1) {
                    exit_current_view();
                } else {
                    g_focus_area = FOCUS_CONTENT;
                    sync_settings_selection_to_sidebar();
                    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                    render_settings_content();
                }
            } else {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            }
            break;
            
        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == SETTINGS_ITEM_COUNT - 1) {
                    exit_current_view();
                } else {
                    g_focus_area = FOCUS_CONTENT;
                    sync_settings_selection_to_sidebar();
                    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                    render_settings_content();
                }
            } else {
                activate_settings_current_option();
                rerender_settings_view();
            }
            break;

        case '+':
        case '=':
            if (g_focus_area == FOCUS_CONTENT) {
                adjust_or_toggle_settings_option(g_settings_current_option, 1);
                rerender_settings_view();
            }
            break;

        case '-':
        case '_':
            if (g_focus_area == FOCUS_CONTENT) {
                adjust_or_toggle_settings_option(g_settings_current_option, -1);
                rerender_settings_view();
            }
            break;
            
        case 's':
        case 'S':
            save_config();
            show_status_message(menu_text("设置已保存", "Settings saved"));
            rerender_settings_view();
            break;
    }
}

static void handle_history_input(int ch) {
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = HISTORY_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_dir_history.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_history_content();
            }
            break;
            
        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= HISTORY_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_dir_history.count) g_content_selected_idx = 0;
                render_history_content();
            }
            break;
            
        case KEY_RIGHT:
        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            break;
            
        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            break;
            
        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                } else if (g_menu_selected_idx == 1) {
                    clear_dir_history();
                    g_content_selected_idx = 0;
                    show_status_message(menu_text("历史记录已清空", "History cleared"));
                    render_menu_frame("历史 [F3]");
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                } else if (g_menu_selected_idx == HISTORY_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_dir_history.count > 0 && g_content_selected_idx >= 0 && 
                    g_content_selected_idx < g_dir_history.count) {
                    
                    const char *path = g_dir_history.entries[g_content_selected_idx].path;
                    stop_audio();
                    int count = load_playlist(path);
                    
                    if (count > 0) {
                        g_selected_index = 0;
                        add_dir_history_entry(path);
                        exit_current_view();
                        show_status_message(menu_text("目录加载成功", "Folder loaded"));
                    } else {
                        show_status_message(menu_text("目录中没有音频文件", "No audio files in this folder"));
                        render_history_content();
                    }
                }
            }
            break;

        case 'a':
        case 'A':
            if (g_focus_area == FOCUS_CONTENT &&
                g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 &&
                g_content_selected_idx < g_dir_history.count) {

                const char *path = g_dir_history.entries[g_content_selected_idx].path;
                int count = append_playlist(path);

                if (count > 0) {
                    add_dir_history_entry(path);
                    exit_current_view();
                    if (g_app_config.remember_last_path) {
                        snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", path);
                        save_config();
                    }

                    char msg[96];
                    snprintf(msg, sizeof(msg), "%s %d %s",
                             menu_text("已追加", "Appended"),
                             count,
                             menu_text("首歌曲到队列", "tracks to queue"));
                    show_status_message(msg);
                } else {
                    show_status_message(menu_text("目录中没有新的音频文件", "No new audio files to append"));
                    render_history_content();
                }
            }
            break;
            
        case 'd':
        case 'D':
            if (g_focus_area == FOCUS_CONTENT && g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_dir_history.count) {
                
                for (int i = g_content_selected_idx; i < g_dir_history.count - 1; i++) {
                    g_dir_history.entries[i] = g_dir_history.entries[i + 1];
                }
                g_dir_history.count--;
                save_dir_history();
                
                if (g_content_selected_idx >= g_dir_history.count) {
                    g_content_selected_idx = g_dir_history.count - 1;
                }
                
                show_status_message(menu_text("已删除历史项", "History entry removed"));
                render_history_content();
            }
            break;
            
        case 'c':
        case 'C':
            if (g_focus_area == FOCUS_CONTENT) {
                clear_dir_history();
                g_content_selected_idx = 0;
                show_status_message(menu_text("历史记录已清空", "History cleared"));
                render_history_content();
            }
            break;

        case 'r':
        case 'R':
            if (g_focus_area == FOCUS_CONTENT && g_playlist_view_mode == 0) {
                if (g_content_selected_idx >= 0 && g_content_selected_idx < g_playlist_manager.count) {
                    noecho();
                    curs_set(1);

                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    int menu_width = max_x / 4;

                    const char *rename_prompt = menu_text("输入新名称：", "New name: ");
                    char new_name[MAX_PLAYLIST_NAME_LEN];
                    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                                      rename_prompt, new_name, sizeof(new_name), 1, 0, 0);

                    noecho();
                    curs_set(0);

                    if (strlen(new_name) > 0) {
                        int result = rename_user_playlist(g_content_selected_idx, new_name);
                        if (result == 0) {
                            show_status_message(menu_text("歌单已重命名", "Playlist renamed"));
                        } else {
                            show_status_message(menu_text("歌单重命名失败", "Failed to rename playlist"));
                        }
                    }

                    render_menu_frame("歌单 [F4]");
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                }
            }
            break;
    }
}

static void handle_playlist_input(int ch) {
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = PLAYLIST_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            } else {
                if (g_playlist_view_mode == 0) {
                    g_content_selected_idx--;
                    if (g_content_selected_idx < 0) g_content_selected_idx = g_playlist_manager.count - 1;
                    if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                } else {
                    g_content_selected_idx--;
                    if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                    if (g_playlist_selected_playlist >= 0) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                    }
                }
                render_playlist_manager_content();
            }
            break;
            
        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= PLAYLIST_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
            } else {
                if (g_playlist_view_mode == 0) {
                    g_content_selected_idx++;
                    if (g_content_selected_idx >= g_playlist_manager.count) g_content_selected_idx = 0;
                } else {
                    g_content_selected_idx++;
                    if (g_playlist_selected_playlist >= 0) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                    }
                }
                render_playlist_manager_content();
            }
            break;
            
        case KEY_RIGHT:
        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                render_playlist_manager_content();
            }
            break;
            
        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_playlist_view_mode == 1) {
                    g_playlist_view_mode = 0;
                    g_content_selected_idx = 0;
                    render_playlist_manager_content();
                } else {
                    g_focus_area = FOCUS_SIDEBAR;
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                }
            }
            break;
            
        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                } else if (g_menu_selected_idx == 1) {
                    noecho();
                    curs_set(1);
                    
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    int menu_width = max_x / 4;
                    
                    const char *create_prompt = menu_text("输入歌单名称：", "Playlist name: ");
                    char name[MAX_PLAYLIST_NAME_LEN];
                    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                                      create_prompt, name, sizeof(name), 1, 0, 0);

                    noecho();
                    curs_set(0);
                    
                    if (strlen(name) > 0) {
                        int result = create_user_playlist(name);
                        if (result == 0) {
                            show_status_message(menu_text("歌单已创建", "Playlist created"));
                        } else if (result == -2) {
                            show_status_message(menu_text("歌单数量已达上限", "Playlist limit reached"));
                        } else {
                            show_status_message(menu_text("创建歌单失败", "Failed to create playlist"));
                        }
                    }
                    
                    render_menu_frame("歌单 [F4]");
                    render_menu_sidebar(g_menu_selected_idx, playlist_sidebar_items, PLAYLIST_ITEM_COUNT);
                    render_playlist_manager_content();
                } else if (g_menu_selected_idx == PLAYLIST_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_playlist_view_mode == 0) {
                    if (g_content_selected_idx >= 0 && g_content_selected_idx < g_playlist_manager.count) {
                        g_playlist_selected_playlist = g_content_selected_idx;
                        g_playlist_view_mode = 1;
                        g_content_selected_idx = 0;
                        g_playlist_track_offset = 0;
                        render_playlist_manager_content();
                    }
                } else {
                    if (g_playlist_selected_playlist >= 0 && g_playlist_selected_playlist < g_playlist_manager.count) {
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= 0 && g_content_selected_idx < pl->track_count) {
                            Track *t = &pl->tracks[g_content_selected_idx];
                            
                            int found = playlist_find_track_index_by_path(t->path);
                            
                            if (found >= 0) {
                                play_audio(found);
                                exit_current_view();
                            } else {
                                show_status_message(menu_text("当前播放列表中没有这首歌", "Track not found in current playlist"));
                            }
                        }
                    }
                }
            }
            break;
            
        case 'd':
        case 'D':
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_playlist_view_mode == 0) {
                    if (g_content_selected_idx >= 0 && g_content_selected_idx < g_playlist_manager.count) {
                        delete_user_playlist(g_content_selected_idx);
                        if (g_content_selected_idx >= g_playlist_manager.count) {
                            g_content_selected_idx = g_playlist_manager.count - 1;
                        }
                        show_status_message(menu_text("歌单已删除", "Playlist deleted"));
                        render_playlist_manager_content();
                    }
                } else {
                    if (g_playlist_selected_playlist >= 0 && g_playlist_selected_playlist < g_playlist_manager.count) {
                        remove_track_from_playlist(g_playlist_selected_playlist, g_content_selected_idx);
                        UserPlaylist *pl = &g_playlist_manager.playlists[g_playlist_selected_playlist];
                        if (g_content_selected_idx >= pl->track_count) {
                            g_content_selected_idx = pl->track_count - 1;
                        }
                        show_status_message(menu_text("歌曲已移除", "Track removed"));
                        render_playlist_manager_content();
                    }
                }
            }
            break;
    }
}

static void handle_favorites_input(int ch) {
    switch (ch) {
        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = FAVORITES_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_favorites.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_favorites_content();
            }
            break;
            
        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= FAVORITES_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_favorites.count) g_content_selected_idx = 0;
                render_favorites_content();
            }
            break;
            
        case KEY_RIGHT:
        case 9:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            break;
            
        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            break;
            
        case 10:
        case ' ':
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                    render_favorites_content();
                } else if (g_menu_selected_idx == FAVORITES_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_favorites.count > 0 && g_content_selected_idx >= 0 && 
                    g_content_selected_idx < g_favorites.count) {
                    
                    Track *t = &g_favorites.tracks[g_content_selected_idx];
                    
                    int found = playlist_find_track_index_by_path(t->path);
                    
                    if (found >= 0) {
                        play_audio(found);
                        exit_current_view();
                    } else {
                        show_status_message(menu_text("当前播放列表中没有这首歌", "Track not found in current playlist"));
                    }
                }
            }
            break;
            
        case 'd':
        case 'D':
            if (g_focus_area == FOCUS_CONTENT && g_favorites.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_favorites.count) {
                
                remove_from_favorites(g_content_selected_idx);
                if (g_content_selected_idx >= g_favorites.count) {
                    g_content_selected_idx = g_favorites.count - 1;
                }
                show_status_message(menu_text("已从收藏移除", "Removed from favorites"));
                render_favorites_content();
            }
            break;
    }
}

static void handle_info_input(int ch) {
    switch (ch) {
        case KEY_UP:
            g_menu_selected_idx--;
            if (g_menu_selected_idx < 0) g_menu_selected_idx = INFO_ITEM_COUNT - 1;
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            break;
            
        case KEY_DOWN:
            g_menu_selected_idx++;
            if (g_menu_selected_idx >= INFO_ITEM_COUNT) g_menu_selected_idx = 0;
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            break;
            
        case 10:
        case ' ':
            if (g_menu_selected_idx == INFO_ITEM_COUNT - 1) {
                exit_current_view();
            }
            break;
    }
}

void handle_menu_input(int ch) {
    if (g_current_view == VIEW_INFO) {
        check_konami_input(ch);
    }

    if (ch == 27) {
        if (g_current_view == VIEW_PLAYLIST && g_playlist_view_mode == 1) {
            g_playlist_view_mode = 0;
            g_content_selected_idx = g_playlist_selected_playlist;
            render_playlist_manager_content();
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
        case VIEW_SETTINGS:
            handle_settings_input(ch);
            break;
        case VIEW_HISTORY:
            handle_history_input(ch);
            break;
        case VIEW_PLAYLIST:
            handle_playlist_input(ch);
            break;
        case VIEW_FAVORITES:
            handle_favorites_input(ch);
            break;
        case VIEW_INFO:
            handle_info_input(ch);
            break;
        case VIEW_HELP:
            handle_help_input(ch);
            break;
        default:
            break;
    }
}
