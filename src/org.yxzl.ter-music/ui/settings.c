/**
 * @file settings.c
 * @brief 设置视图 — 主题、路径、播放设置、快捷键、远程设备
 *
 * 从 menus.c 拆分而来，负责设定页面下的所有渲染和输入处理。
 * 包括颜色主题调整、默认路径编辑、播放参数切换、快捷键说明
 * 和远程设备管理（列表 / 操作 / 表单 / 浏览）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "ui/dialog.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "config/config.h"
#include "logger/logger.h"
#include "remote/remote.h"
#include "config/crypto.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>

/* ============================================================
 * Settings internal state
 * ============================================================ */

static int g_settings_current_option = 0;
static int g_settings_color_editing = 0;
static int g_settings_color_which = 0;

/* ============================================================
 * Settings option arrays
 * ============================================================ */

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
    "默认播放模式",
    "默认倍速",
    "显示专辑图片",
    "歌词对齐方式",
    "音频后端",
    "排序方式",
    "高级播放模式",
    "默认播放模式"
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
    "Default Play Mode",
    "Default Speed",
    "Show Album Cover",
    "Lyrics Alignment",
    "Audio Backend",
    "Sort Mode",
    "Advanced Play Modes",
    "Default Play Mode"
};
#define SETTINGS_OPTION_COUNT 28

enum {
    SETTINGS_IDX_THEME_COLOR_PAIR_0  = 0,
    SETTINGS_IDX_THEME_COLOR_PAIR_1  = 1,
    SETTINGS_IDX_THEME_COLOR_PAIR_2  = 2,
    SETTINGS_IDX_THEME_COLOR_PAIR_3  = 3,
    SETTINGS_IDX_THEME_COLOR_PAIR_4  = 4,
    SETTINGS_IDX_THEME_COLOR_PAIR_5  = 5,
    SETTINGS_IDX_THEME_COLOR_PAIR_6  = 6,
    SETTINGS_IDX_THEME_COLOR_PAIR_7  = 7,
    SETTINGS_IDX_THEME_COLOR_PAIR_8  = 8,
    SETTINGS_IDX_THEME_COLOR_PAIR_9  = 9,
    SETTINGS_IDX_THEME_COLOR_PAIR_10 = 10,
    SETTINGS_IDX_THEME_COLOR_PAIR_11 = 11,
    SETTINGS_IDX_DEFAULT_PATH        = 12,
    SETTINGS_IDX_AUTO_PLAY           = 13,
    SETTINGS_IDX_REMEMBER_PATH       = 14,
    SETTINGS_IDX_CLEAR_HISTORY       = 15,
    SETTINGS_IDX_LANGUAGE            = 16,
    SETTINGS_IDX_VOLUME              = 17,
    SETTINGS_IDX_LATENCY             = 18,
    SETTINGS_IDX_SHOW_LYRICS         = 19,
    SETTINGS_IDX_DEFAULT_PLAY_MODE   = 20,
    SETTINGS_IDX_DEFAULT_SPEED       = 21,
    SETTINGS_IDX_SHOW_ALBUM_COVER    = 22,
    SETTINGS_IDX_LYRICS_ALIGNMENT    = 23,
    SETTINGS_IDX_AUDIO_BACKEND       = 24,
    SETTINGS_IDX_SORT_MODE           = 25,
    SETTINGS_IDX_ADVANCED_PLAY_MODES = 26,
    SETTINGS_IDX_DEFAULT_PLAY_MODE2  = 27
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
    SETTINGS_IDX_DEFAULT_SPEED,
    SETTINGS_IDX_AUDIO_BACKEND,
    SETTINGS_IDX_SORT_MODE
};

static const int settings_playmode_option_indices[] = {
    SETTINGS_IDX_DEFAULT_PLAY_MODE,
    SETTINGS_IDX_ADVANCED_PLAY_MODES
};

/* Reset settings view state (called from menus.c on view switch) */
void reset_settings_view(void)
{
    g_settings_current_option = 0;
}

/* ============================================================
 * Settings section helpers
 * ============================================================ */

static SettingsSectionSpec get_settings_section_spec_for_sidebar(int sidebar_idx)
{
    switch (sidebar_idx) {
        case 0:
            return (SettingsSectionSpec){settings_theme_option_indices,
                (int)(sizeof(settings_theme_option_indices) / sizeof(settings_theme_option_indices[0]))};
        case 1:
            return (SettingsSectionSpec){settings_path_option_indices,
                (int)(sizeof(settings_path_option_indices) / sizeof(settings_path_option_indices[0]))};
        case 2:
            return (SettingsSectionSpec){settings_playback_option_indices,
                (int)(sizeof(settings_playback_option_indices) / sizeof(settings_playback_option_indices[0]))};
        case 3:
            return (SettingsSectionSpec){settings_playmode_option_indices,
                (int)(sizeof(settings_playmode_option_indices) / sizeof(settings_playmode_option_indices[0]))};
        default:
            return (SettingsSectionSpec){NULL, 0};
    }
}

static SettingsSectionSpec get_active_settings_section_spec(void)
{
    return get_settings_section_spec_for_sidebar(g_menu_selected_idx);
}

static int get_settings_section_position(SettingsSectionSpec spec, int option_index)
{
    for (int i = 0; i < spec.count; i++) {
        if (spec.indices[i] == option_index) {
            return i;
        }
    }
    return -1;
}

static void sync_settings_selection_to_sidebar(void)
{
    SettingsSectionSpec spec = get_active_settings_section_spec();
    if (spec.count <= 0) {
        g_settings_current_option = -1;
        return;
    }

    if (get_settings_section_position(spec, g_settings_current_option) < 0) {
        g_settings_current_option = spec.indices[0];
    }
}

/* ============================================================
 * Formatting helpers
 * ============================================================ */

static int clamp_latency_ms(int latency_ms)
{
    if (latency_ms < 20)  return 20;
    if (latency_ms > 250) return 250;
    return latency_ms;
}

static int is_valid_path(const char *path)
{
    if (!path || strlen(path) == 0) return 0;
    for (size_t i = 0; i < strlen(path); i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 && c != '\0') return 0;
    }
    if (path[0] == '~' && strlen(path) > 1 && path[1] != '/') return 0;
    return 1;
}

static void format_settings_option_line(int option_index, char *line, size_t line_size)
{
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

    if (!line || line_size == 0 || option_index < 0 || option_index >= SETTINGS_OPTION_COUNT) return;

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
    } else if (option_index == SETTINGS_IDX_DEFAULT_PLAY_MODE) {
        const char *mode_str = play_mode_display_name(
            (PlayMode)g_app_config.default_play_mode, use_english_ui());
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator, mode_str);
    } else if (option_index == SETTINGS_IDX_ADVANCED_PLAY_MODES) {
        snprintf(line, line_size, "%s%s%s",
                 current_settings_options[option_index], separator,
                 menu_bool_text(g_app_config.advanced_play_modes_enabled));
    } else if (option_index == SETTINGS_IDX_DEFAULT_SPEED) {
        snprintf(line, line_size, "%s%s%.2fx",
                 current_settings_options[option_index], separator,
                 (double)g_app_config.default_playback_speed);
    } else if (option_index == SETTINGS_IDX_AUDIO_BACKEND) {
        const char *backend_str;
        switch (g_app_config.audio_backend) {
            case AUDIO_BACKEND_AUTO:     backend_str = menu_text("自动", "Auto"); break;
            case AUDIO_BACKEND_PULSE:    backend_str = "PulseAudio"; break;
            case AUDIO_BACKEND_ALSA:     backend_str = "ALSA"; break;
            case AUDIO_BACKEND_PIPEWIRE: backend_str = "PipeWire"; break;
            default:                     backend_str = menu_text("自动", "Auto");
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

/* ============================================================
 * Settings option group rendering
 * ============================================================ */

static void render_settings_option_group(int start_y, int content_start_x, int max_y, SettingsSectionSpec spec)
{
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

/* ============================================================
 * Settings input helpers
 * ============================================================ */

static void move_settings_content_selection(int delta)
{
    SettingsSectionSpec spec = get_active_settings_section_spec();
    if (spec.count <= 0) return;

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

static void adjust_settings_theme_option(int option_index, int delta)
{
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

    if (option_index < 0 || option_index >= 12) return;
    if (delta == 0) delta = 1;

    int paired_idx = (option_index % 2 == 0) ? option_index + 1 : option_index - 1;
    int paired_color = *color_values[paired_idx];
    int next = *color_values[option_index];

    do {
        next += delta;
        if (next < 0)  next = 7;
        else if (next > 7) next = 0;
    } while (next == paired_color);

    *color_values[option_index] = next;
    apply_color_theme();
    save_config();
}

static void edit_default_startup_path(void)
{
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
            g_app_config.default_startup_path[MAX_PATH_LEN - 1] = '\0';
        }
        save_config();
        show_status_message(menu_text("默认启动路径已保存", "Default startup path saved"));
    }
}

static void adjust_or_toggle_settings_option(int option_index, int delta)
{
    if (option_index < 0) return;

    if (option_index < 12) {
        adjust_settings_theme_option(option_index, delta);
        return;
    }

    extern float g_playback_speed;

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
            if (delta == 0) delta = 1;
            set_volume_percent(g_app_config.volume_percent + delta * 5);
            g_app_config.volume_percent = get_volume_percent();
            break;
        case SETTINGS_IDX_LATENCY:
            if (delta == 0) delta = 1;
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
        case SETTINGS_IDX_DEFAULT_PLAY_MODE:
            if (delta < 0) {
                g_app_config.default_play_mode = (g_app_config.default_play_mode - 1 + PLAY_MODE_COUNT) % PLAY_MODE_COUNT;
            } else if (delta > 0) {
                g_app_config.default_play_mode = (g_app_config.default_play_mode + 1) % PLAY_MODE_COUNT;
            } else {
                g_app_config.default_play_mode = (g_app_config.default_play_mode + 1) % PLAY_MODE_COUNT;
            }
            save_config();
            g_play_mode = (PlayMode)g_app_config.default_play_mode;
            show_status_message(menu_text("默认播放模式已更新，当前会话已应用",
                                          "Default play mode updated, applied to current session"));
            break;
        case SETTINGS_IDX_ADVANCED_PLAY_MODES:
            g_app_config.advanced_play_modes_enabled = !g_app_config.advanced_play_modes_enabled;
            save_config();
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
                     (double)g_app_config.default_playback_speed);
            show_status_message(msg);
            break;
        }
        case SETTINGS_IDX_AUDIO_BACKEND: {
            int options[] = {AUDIO_BACKEND_AUTO, AUDIO_BACKEND_PIPEWIRE,
                             AUDIO_BACKEND_PULSE, AUDIO_BACKEND_ALSA};
            int count = 4;
            int has_pw    = audio_backend_is_available(AUDIO_BACKEND_PIPEWIRE);
            int has_pulse = audio_backend_is_available(AUDIO_BACKEND_PULSE);
            int has_alsa  = audio_backend_is_available(AUDIO_BACKEND_ALSA);
            int current = 0;
            for (int i = 0; i < count; i++) {
                if (g_app_config.audio_backend == options[i]) { current = i; break; }
            }
            int direction = (delta >= 0) ? 1 : -1;
            int next = current;
            int attempts = 0;
            do {
                next = (next + direction + count) % count;
                attempts++;
                if ((options[next] == AUDIO_BACKEND_PIPEWIRE && !has_pw) ||
                    (options[next] == AUDIO_BACKEND_PULSE && !has_pulse) ||
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

static void activate_settings_current_option(void)
{
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
        g_settings_current_option == SETTINGS_IDX_DEFAULT_PLAY_MODE ||
        g_settings_current_option == SETTINGS_IDX_ADVANCED_PLAY_MODES ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_SPEED ||
        g_settings_current_option == SETTINGS_IDX_SORT_MODE) {
        adjust_or_toggle_settings_option(g_settings_current_option, 0);
        return;
    }

    if (g_settings_current_option >= 0) {
        adjust_or_toggle_settings_option(g_settings_current_option, 1);
    }
}

/* ============================================================
 * Remote device UI state
 * ============================================================ */

static int g_remote_mode           = 0;   // 0=list, 1=actions, 2=form, 3=browse
static int g_remote_selected       = 0;
static int g_remote_selected_conn  = -1;
static RemoteDirEntry *g_remote_entries = NULL;
static int g_remote_entry_count    = 0;
static int g_remote_entry_offset   = 0;
static char g_remote_current_path[1024] = "";
static RemoteConnectionConfig g_remote_form_config;
static int g_remote_form_editing_idx = -1;

/* Forward declarations for remote helpers */
static void render_remote_content(void);
static void handle_remote_content_input(int ch);
void remote_enter_list_mode(void);  /* non-static: declared in menu_internal.h */
static void remote_start_add(void);
static void remote_start_edit(int conn_idx);
static void remote_delete_connection(int conn_idx);
static void remote_start_browse(int conn_idx);
static void remote_refresh_entries(void);

/* ============================================================
 * Remote device — navigation / lifecycle
 * ============================================================ */

void remote_enter_list_mode(void)
{
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

static void remote_go_back(void)
{
    switch (g_remote_mode) {
        case 0:
            g_focus_area = FOCUS_SIDEBAR;
            render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
            render_settings_content();
            break;
        case 1:
            g_remote_mode = 0;
            g_remote_selected = 0;
            render_settings_content();
            break;
        case 3:
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

static void rerender_remote_view(void)
{
    render_menu_frame(menu_text("设置 [F2]", "Settings [F2]"));
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

static void remote_refresh_entries(void)
{
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

static void remote_start_browse(int conn_idx)
{
    g_remote_selected_conn = conn_idx;
    g_remote_mode = 3;
    g_remote_selected = 0;

    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[conn_idx];
    strncpy(g_remote_current_path, conn->base_path, sizeof(g_remote_current_path) - 1);
    g_remote_current_path[sizeof(g_remote_current_path) - 1] = '\0';

    remote_refresh_entries();
    rerender_remote_view();
}

static void remote_refresh_connection(void)
{
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

static void remote_load_playlist(void)
{
    if (g_remote_selected_conn < 0) return;
    const RemoteConnectionConfig *conn = &g_app_config.remote_connections[g_remote_selected_conn];

    extern void stop_audio(void);
    stop_audio();
    int count = load_remote_playlist(conn, g_remote_current_path);
    if (count > 0) {
        extern int g_selected_index;
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

/* ============================================================
 * Remote device — form (add / edit)
 * ============================================================ */

static int remote_form_field_count(void)
{
    return (g_remote_form_config.protocol == REMOTE_PROTOCOL_SFTP) ? 8 : 7;
}

static void remote_form_field_label(int field_idx, char *buf, size_t size)
{
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

static void remote_form_value_text(int field_idx, char *buf, size_t size)
{
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
                int n = (int)strlen(rc->password);
                if (n > 50) n = 50;
                memset(buf, '*', (size_t)n);
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

static void remote_form_edit_field(int field_idx)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int content_start_x = (max_x / 4) + 2;
    int form_start_y = 4;

    curs_set(1);
    noecho();

    char label[32];
    remote_form_field_label(field_idx, label, sizeof(label));

    switch (field_idx) {
        case 0:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.name,
                              sizeof(g_remote_form_config.name), 1, 0, 1);
            break;
        case 1: {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", g_remote_form_config.protocol);
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, buf, sizeof(buf), 1, 0, 1);
            int val = atoi(buf);
            if (val >= 0 && val <= 4) g_remote_form_config.protocol = val;
            break;
        }
        case 2:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.host,
                              sizeof(g_remote_form_config.host), 1, 0, 1);
            break;
        case 3: {
            char buf[16];
            if (g_remote_form_config.port > 0)
                snprintf(buf, sizeof(buf), "%d", g_remote_form_config.port);
            else
                buf[0] = '\0';
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, buf, sizeof(buf), 1, 0, 1);
            g_remote_form_config.port = buf[0] ? (int)strtol(buf, NULL, 10) : 0;
            break;
        }
        case 4:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.username,
                              sizeof(g_remote_form_config.username), 1, 0, 1);
            break;
        case 5:
            prompt_text_input(stdscr, form_start_y + field_idx, content_start_x,
                              label, g_remote_form_config.password,
                              sizeof(g_remote_form_config.password), 1, 1, 1);
            break;
        case 6:
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
        case 7:
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

static void remote_form_save(void)
{
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

static void remote_form_cancel(void)
{
    g_remote_mode = 0;
    g_remote_selected = g_remote_form_editing_idx >= 0
        ? g_remote_form_editing_idx
        : g_app_config.remote_connection_count;
    g_remote_form_editing_idx = -1;
    rerender_remote_view();
}

static void remote_start_add(void)
{
    memset(&g_remote_form_config, 0, sizeof(g_remote_form_config));
    g_remote_form_config.protocol = REMOTE_PROTOCOL_FTP;
    g_remote_form_editing_idx = -1;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_start_edit(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= g_app_config.remote_connection_count) return;
    g_remote_form_config = g_app_config.remote_connections[conn_idx];
    g_remote_form_editing_idx = conn_idx;
    g_remote_mode = 2;
    g_remote_selected = 0;
    rerender_remote_view();
}

static void remote_delete_connection(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= g_app_config.remote_connection_count) return;
    if (conn_idx < g_app_config.remote_connection_count - 1) {
        memmove(&g_app_config.remote_connections[conn_idx],
                &g_app_config.remote_connections[conn_idx + 1],
                sizeof(RemoteConnectionConfig) * (size_t)(g_app_config.remote_connection_count - conn_idx - 1));
    }
    g_app_config.remote_connection_count--;
    save_config();
    show_status_message(menu_text("连接已删除", "Connection deleted"));

    g_remote_mode = 0;
    if (g_remote_selected >= g_app_config.remote_connection_count) {
        g_remote_selected = g_app_config.remote_connection_count > 0
            ? g_app_config.remote_connection_count - 1 : 0;
    }
    rerender_remote_view();
}

/* ============================================================
 * Remote device — rendering
 * ============================================================ */

static void render_remote_content(void)
{
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
            ? menu_text("编辑连接 (↑/↓ 选择 ENTER 编辑 +/- 协议 S 保存 ESC 取消)",
                       "Edit Connection (Up/Down select Enter edit +/- Protocol S Save Esc Cancel)")
            : menu_text("添加连接 (↑/↓ 选择 ENTER 编辑 +/- 协议 S 保存 ESC 取消)",
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

/* ============================================================
 * Remote device — input handling
 * ============================================================ */

static void handle_remote_content_input(int ch)
{
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
                        g_remote_current_path[sizeof(g_remote_current_path) - 1] = '\0';
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
                            strncat(g_remote_current_path, "/",
                                    sizeof(g_remote_current_path) - cur_len - 1);
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
                                strncat(g_remote_current_path, "/",
                                        sizeof(g_remote_current_path) - cur_len - 1);
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

/* ============================================================
 * Settings content rendering (public API)
 * ============================================================ */

void render_settings_content(void)
{
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

    if (g_menu_selected_idx == 0) {  /* 颜色主题 */
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("颜色主题：↑/↓ 选择，+/ - 或 → 调整，←/TAB 返回菜单",
                           "Theme: Up/Down select, +/- or Right adjust, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("前景色和背景色会自动避免使用相同颜色",
                           "Foreground/background pairs avoid identical colors"));
    } else if (g_menu_selected_idx == 1) {  /* 默认路径 */
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("默认路径：ENTER 编辑，←/TAB 返回菜单",
                           "Default path: Enter edits, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("支持使用 ~ 开头的用户目录路径",
                           "Paths starting with ~ are expanded to your home directory"));
    } else if (g_menu_selected_idx == 2) {  /* 播放设置 */
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("播放设置：↑/↓ 选择，ENTER 切换，+/ - 调整数值，←/TAB 返回菜单",
                           "Playback: Up/Down select, Enter toggles, +/- adjusts, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
        start_y += spec.count + 1;
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("语言可用 ENTER 切换，时延会在下次播放时生效",
                           "Press Enter to toggle language; latency applies on next playback"));
    } else if (g_menu_selected_idx == 3) {  /* 播放模式 (NEW) */
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("播放模式：↑/↓ 选择，+/ - 调整，ENTER 切换，←/TAB 返回菜单",
                           "Play Mode: Up/Down select, +/- adjust, Enter toggle, Left/Tab back"));
        start_y += 2;
        render_settings_option_group(start_y, content_start_x, max_y, spec);
    } else if (g_menu_selected_idx == 4) {  /* 快捷键 */
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("快捷键说明：←/TAB 返回菜单",
                           "Hotkeys: Left/Tab back"));
        start_y += 2;
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("F1-F8：切换页面 / 语言 / 退出", "F1-F8: Views / language / quit"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("O / I：打开目录 / 追加目录", "O / I: Open folder / append folder"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("C / L：切换控件焦点与列表焦点", "C / L: Controls focus / list focus"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("D：进入或退出歌词定位模式", "D: Toggle lyric seek mode"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("空格 / Enter：执行当前动作", "Space / Enter: Activate current action"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text("+ / -：调整音量", "+ / -: Adjust volume"));
        mvprintw(start_y++, content_start_x, "%s",
                 menu_text(", / .：快退 / 快进", ", / .: Seek backward / forward"));
    } else if (g_menu_selected_idx == 5) {  /* 远程设备 */
        render_remote_content();
    } else {
        mvprintw(start_y, content_start_x, "%s",
                 menu_text("按 ENTER 返回主界面", "Press Enter to return to the main view"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

/* ============================================================
 * Settings view re-render helper
 * ============================================================ */

static void rerender_settings_view(void)
{
    render_menu_frame("设置 [F2]");
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

/* ============================================================
 * Settings input handling (declared in menu_internal.h)
 * ============================================================ */

void handle_settings_input(int ch)
{
    /* Remote section handles its own input when content-focused */
    if (g_menu_selected_idx == 5 && g_focus_area == FOCUS_CONTENT) {
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

        case 9:  /* TAB */
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
