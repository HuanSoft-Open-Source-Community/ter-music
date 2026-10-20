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
#include "player/player.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "ui/remote_view.h"
#include "config/config.h"
#include "logger/logger.h"
#include "info/info.h"
#include "playlist/playlist.h"
#include "playlist/encoding.h"
#include "i18n/i18n.h"
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

/* 1/未知(-1) 都视为可选；只有明确不可用(0)才过滤。 */
static int backend_option_selectable(int backend)
{
    return player_audio_backend_available(backend) != 0;
}

/* ============================================================
 * Settings internal state
 * ============================================================ */

static int g_settings_current_option = 0;
static int g_settings_color_editing = 0;
static int g_settings_color_which = 0;

/* 快捷键编辑器（Task 5）：选中行、滚动偏移、捕获模式与提示消息。 */
static int g_hotkey_selected = 0;      /* 0 .. actions_count()-1 */
static int g_hotkey_offset = 0;
static int g_hotkey_capturing = 0;
static char g_hotkey_message[256];

/* ============================================================
 * Selection menu state (uses sub-window + box())
 * ============================================================ */

static int g_sel_active = 0;    /* selection menu open? */
static int g_sel_src   = -1;    /* SETTINGS_IDX_* that triggered it */
static int g_sel_idx   = 0;     /* currently highlighted option index */
static int g_sel_count = 0;     /* total option count */
static WINDOW *g_sel_win = NULL; /* popup sub-window */
static int g_sel_multiselect = 0; /* 1 = 多选模式（信息显示字段） */
static int g_sel_work_mask = 0;   /* 多选模式下的工作掩码 */

/* ============================================================
 * Settings option arrays
 * ============================================================ */

static const char *settings_options[] = {
    "settings.opt.playlist_fg",
    "settings.opt.playlist_bg",
    "settings.opt.controls_fg",
    "settings.opt.controls_bg",
    "settings.opt.lyrics_fg",
    "settings.opt.lyrics_bg",
    "settings.opt.sidebar_fg",
    "settings.opt.sidebar_bg",
    "settings.opt.highlight_fg",
    "settings.opt.highlight_bg",
    "settings.opt.border_fg",
    "settings.opt.border_bg",
    "settings.opt.default_path",
    "settings.opt.auto_play",
    "settings.opt.remember_path",
    "settings.opt.clear_history",
    "settings.opt.latency",
    "settings.opt.show_lyrics",
    "settings.opt.play_mode",
    "settings.opt.default_speed",
    "settings.opt.cover_art",
    "settings.opt.lyrics_align",
    "settings.opt.audio_backend",
    "settings.opt.sort_mode",
    "settings.opt.advanced_modes",
    "settings.opt.seamless_preload",
    "settings.opt.cue_encoding",
    "settings.opt.enable_eq",
    "settings.opt.preampl",
    "settings.opt.eq_31hz",
    "settings.opt.eq_62hz",
    "settings.opt.eq_125hz",
    "settings.opt.eq_250hz",
    "settings.opt.eq_500hz",
    "settings.opt.eq_1khz",
    "settings.opt.eq_2khz",
    "settings.opt.eq_4khz",
    "settings.opt.eq_8khz",
    "settings.opt.eq_16khz",
    "settings.opt.eq_preset",
    "settings.opt.info_preset",
    "settings.opt.info_fields",
    "settings.opt.info_cover",
    "settings.opt.info_cover_size",
    "settings.opt.info_cover_charset",
    "settings.opt.info_progress",
    "settings.opt.info_progress_style",
    "settings.opt.info_lyrics",
    "settings.opt.reload_config",
    "settings.opt.reset_config",
    "settings.opt.stop_core_on_exit"
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
    "Output Latency",
    "Show Lyrics Panel",
    "Default Play Mode",
    "Default Speed",
    "Show Album Cover",
    "Lyrics Alignment",
    "Audio Backend",
    "Sort Mode",
    "Advanced Play Modes",
    "Seamless Preload",
    "CUE Encoding",
    "Enable Equalizer",
    "Pre-amp",
    "31 Hz",
    "62 Hz",
    "125 Hz",
    "250 Hz",
    "500 Hz",
    "1 kHz",
    "2 kHz",
    "4 kHz",
    "8 kHz",
    "16 kHz",
    "EQ Preset",
    "Info Display Preset",
    "Basic Info Fields",
    "Show Text Cover",
    "Cover Size",
    "Cover Charset",
    "Show Progress",
    "Progress Style",
    "Lyric Lines",
    "Reload Config",
    "Restore Defaults",
    "Stop Playback Process On Exit"
};
/* 与 SETTINGS_IDX_* 一一对应的选项总数（含 eq_preset、reload/reset；Task 5.4/5.5，
 * 另含尾部追加的 stop_core_on_exit）。 */
#define SETTINGS_OPTION_COUNT 51

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
    SETTINGS_IDX_LATENCY             = 16,
    SETTINGS_IDX_SHOW_LYRICS         = 17,
    SETTINGS_IDX_DEFAULT_PLAY_MODE   = 18,
    SETTINGS_IDX_DEFAULT_SPEED       = 19,
    SETTINGS_IDX_SHOW_ALBUM_COVER    = 20,
    SETTINGS_IDX_LYRICS_ALIGNMENT    = 21,
    SETTINGS_IDX_AUDIO_BACKEND       = 22,
    SETTINGS_IDX_SORT_MODE           = 23,
    SETTINGS_IDX_ADVANCED_PLAY_MODES = 24,
    /* 原先这里有一个 DEFAULT_PLAY_MODE2=25 的死项（重复 play_mode，不进任何
     * section、无处理器），Task 5.4 去重后其后的下标整体前移一格。 */
    SETTINGS_IDX_SEAMLESS_PRELOAD    = 25,
    SETTINGS_IDX_CUE_ENCODING        = 26,
    SETTINGS_IDX_EQ_ENABLED          = 27,
    SETTINGS_IDX_EQ_PREAMP           = 28,
    SETTINGS_IDX_EQ_BAND_0           = 29,  /* 31 Hz */
    SETTINGS_IDX_EQ_BAND_1           = 30,  /* 62 Hz */
    SETTINGS_IDX_EQ_BAND_2           = 31,  /* 125 Hz */
    SETTINGS_IDX_EQ_BAND_3           = 32,  /* 250 Hz */
    SETTINGS_IDX_EQ_BAND_4           = 33,  /* 500 Hz */
    SETTINGS_IDX_EQ_BAND_5           = 34,  /* 1 kHz */
    SETTINGS_IDX_EQ_BAND_6           = 35,  /* 2 kHz */
    SETTINGS_IDX_EQ_BAND_7           = 36,  /* 4 kHz */
    SETTINGS_IDX_EQ_BAND_8           = 37,  /* 8 kHz */
    SETTINGS_IDX_EQ_BAND_9           = 38,  /* 16 kHz */
    SETTINGS_IDX_EQ_PRESET           = 39,
    /* ── 信息显示（CLI `ter-music show` / D-Bus，见 info/info.h） ── */
    SETTINGS_IDX_INFO_PRESET         = 40,
    SETTINGS_IDX_INFO_FIELDS         = 41,
    SETTINGS_IDX_INFO_COVER          = 42,
    SETTINGS_IDX_INFO_COVER_SIZE     = 43,
    SETTINGS_IDX_INFO_COVER_CHARSET  = 44,
    SETTINGS_IDX_INFO_PROGRESS       = 45,
    SETTINGS_IDX_INFO_PROGRESS_STYLE = 46,
    SETTINGS_IDX_INFO_LYRICS         = 47,
    /* ── 配置管理（Task 5.5：重新加载 / 恢复默认，动作项） ── */
    SETTINGS_IDX_RELOAD_CONFIG       = 48,
    SETTINGS_IDX_RESET_CONFIG        = 49,
    /* ── 生命周期：退出时停止播放进程（尾部追加，不重排既有下标，
     * 规避 196-197 行注释所述"删除死项整体前移"约束） ── */
    SETTINGS_IDX_STOP_CORE_ON_EXIT   = 50
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
    SETTINGS_IDX_DEFAULT_PATH,
    /* 配置管理动作项归入"默认路径"分区（无独立 general 分区，且受限于 util.c
     * 侧栏项不可增删）：与配置文件操作同组，Task 5.5。 */
    SETTINGS_IDX_RELOAD_CONFIG,
    SETTINGS_IDX_RESET_CONFIG
};

static const int settings_playback_option_indices[] = {
    SETTINGS_IDX_AUTO_PLAY,
    SETTINGS_IDX_REMEMBER_PATH,
    SETTINGS_IDX_CLEAR_HISTORY,
    /* 退出时停止播放进程：与 auto_play 等同属播放生命周期开关（resume_last_playback
     * 仅有配置字段、未在此页暴露，故紧随启动/退出开关组之后）。 */
    SETTINGS_IDX_STOP_CORE_ON_EXIT,
    SETTINGS_IDX_LATENCY,
    SETTINGS_IDX_SHOW_LYRICS,
    SETTINGS_IDX_SHOW_ALBUM_COVER,
    SETTINGS_IDX_SEAMLESS_PRELOAD,
    SETTINGS_IDX_LYRICS_ALIGNMENT,
    SETTINGS_IDX_DEFAULT_SPEED,
    SETTINGS_IDX_AUDIO_BACKEND,
    SETTINGS_IDX_SORT_MODE,
    SETTINGS_IDX_CUE_ENCODING
};

static const int settings_playmode_option_indices[] = {
    SETTINGS_IDX_DEFAULT_PLAY_MODE,
    SETTINGS_IDX_ADVANCED_PLAY_MODES
};

static const int settings_eq_option_indices[] = {
    SETTINGS_IDX_EQ_ENABLED,
    SETTINGS_IDX_EQ_PREAMP,
    SETTINGS_IDX_EQ_BAND_0,
    SETTINGS_IDX_EQ_BAND_1,
    SETTINGS_IDX_EQ_BAND_2,
    SETTINGS_IDX_EQ_BAND_3,
    SETTINGS_IDX_EQ_BAND_4,
    SETTINGS_IDX_EQ_BAND_5,
    SETTINGS_IDX_EQ_BAND_6,
    SETTINGS_IDX_EQ_BAND_7,
    SETTINGS_IDX_EQ_BAND_8,
    SETTINGS_IDX_EQ_BAND_9
};

static const int settings_info_option_indices[] = {
    SETTINGS_IDX_INFO_PRESET,
    SETTINGS_IDX_INFO_FIELDS,
    SETTINGS_IDX_INFO_COVER,
    SETTINGS_IDX_INFO_COVER_SIZE,
    SETTINGS_IDX_INFO_COVER_CHARSET,
    SETTINGS_IDX_INFO_PROGRESS,
    SETTINGS_IDX_INFO_PROGRESS_STYLE,
    SETTINGS_IDX_INFO_LYRICS
};

/* Forward declarations for sel menu / re-render helpers */
static void create_sel_window(void);
static void close_sel_menu(int apply);
static void rerender_settings_view(void);

/* Forward declarations for EQ visual UI */
static void render_eq_visual(void);
static int handle_eq_action(ActionId action);

/* Reset settings view state (called from menus.c on view switch) */
void reset_settings_view(void)
{
    if (g_sel_active)
        close_sel_menu(0);
    g_settings_current_option = 0;
    g_hotkey_selected = 0;
    g_hotkey_offset = 0;
    g_hotkey_capturing = 0;
    g_hotkey_message[0] = '\0';
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
        case 6:
            return (SettingsSectionSpec){settings_eq_option_indices,
                (int)(sizeof(settings_eq_option_indices) / sizeof(settings_eq_option_indices[0]))};
        case 7:  /* 信息显示（CLI / D-Bus） */
            return (SettingsSectionSpec){settings_info_option_indices,
                (int)(sizeof(settings_info_option_indices) / sizeof(settings_info_option_indices[0]))};
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

/* ============================================================
 * 信息显示（CLI `ter-music show` / D-Bus）辅助
 * ============================================================ */

/* 封面尺寸候选（列 x 行）；受 INFO_COVER_COLS/ROWS 约束 */
static const int k_info_cover_sizes[][2] = {
    { 4,  2}, { 8,  4}, {12,  6}, {16,  8},
    {20, 10}, {24, 12}, {30, 15}, {40, 20}
};
static const int k_info_cover_size_count =
    (int)(sizeof(k_info_cover_sizes) / sizeof(k_info_cover_sizes[0]));

static const char *info_preset_label(int preset)
{
    switch (preset) {
        case INFO_PRESET_FULL:    return i18n_get("settings.info.preset.full");
        case INFO_PRESET_COMPACT: return i18n_get("settings.info.preset.compact");
        default:                  return i18n_get("settings.info.preset.custom");
    }
}

static const char *info_progress_style_label(int style)
{
    switch (style) {
        case INFO_PROGRESS_BAR_TIME:     return i18n_get("settings.info.progress.bar");
        case INFO_PROGRESS_TIME:         return i18n_get("settings.info.progress.time");
        case INFO_PROGRESS_PERCENT:      return i18n_get("settings.info.progress.percent");
        default:                         return i18n_get("settings.info.progress.time_percent");
    }
}

static const char *info_charset_label(int charset)
{
    return (charset == INFO_COVER_ASCII)
        ? i18n_get("settings.info.charset.ascii")
        : i18n_get("settings.info.charset.braille");
}

static const char *info_lyrics_label(int lines)
{
    switch (lines) {
        case INFO_LYRICS_OFF:     return i18n_get("settings.info.lyrics.none");
        case INFO_LYRICS_CURRENT: return i18n_get("settings.info.lyrics.current");
        default:                  return i18n_get("settings.info.lyrics.current_next");
    }
}

/* 已启用字段的名称列表（用于设置页摘要显示） */
static void info_fields_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    size_t pos = 0;
    for (int i = 0; i < info_field_count(); i++) {
        const InfoFieldDef *field = info_field_at(i);
        if (!field || (g_app_config.info_fields_mask & field->bit) == 0) {
            continue;
        }
        int written = snprintf(out + pos, out_size - pos, "%s%s",
                               pos == 0 ? "" : ",", i18n_get(field->label_key));
        if (written < 0 || (size_t)written >= out_size - pos) {
            break;
        }
        pos += (size_t)written;
    }

    if (pos == 0) {
        snprintf(out, out_size, "%s", i18n_get("general.none"));
    }
}

/* 应用预设：只决定“显示哪些内容”，封面尺寸/字符集属于用户偏好 */
static void info_apply_preset_to_config(int preset)
{
    g_app_config.info_preset = preset;
    if (preset == INFO_PRESET_FULL) {
        g_app_config.info_fields_mask = INFO_FIELD_ALL;
        g_app_config.info_show_cover = 1;
        g_app_config.info_show_progress = 1;
        g_app_config.info_progress_style = INFO_PROGRESS_BAR_TIME;
        g_app_config.info_lyrics_lines = INFO_LYRICS_BOTH;
    } else if (preset == INFO_PRESET_COMPACT) {
        g_app_config.info_fields_mask = INFO_FIELD_COMPACT;
        g_app_config.info_show_cover = 0;
        g_app_config.info_show_progress = 1;
        g_app_config.info_progress_style = INFO_PROGRESS_TIME_PERCENT;
        g_app_config.info_lyrics_lines = INFO_LYRICS_CURRENT;
    }
}

/* 分项被修改后预设不再是全量/精简，切到自定义 */
static void info_mark_custom(void)
{
    if (g_app_config.info_preset != INFO_PRESET_CUSTOM) {
        g_app_config.info_preset = INFO_PRESET_CUSTOM;
    }
}

/* 选择菜单中“信息显示”相关选项的显示文本（create_sel_window 与
 * draw_sel_menu 共用，避免两份重复的 opts[] 构建代码） */
static void info_sel_option_text(int src, int index, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    switch (src) {
        case SETTINGS_IDX_INFO_PRESET:
            if (index >= 0 && index < INFO_PRESET_COUNT)
                snprintf(out, out_size, "%s", info_preset_label(index));
            break;
        case SETTINGS_IDX_INFO_FIELDS: {
            const InfoFieldDef *field = info_field_at(index);
            if (field)
                snprintf(out, out_size, "[%c] %s",
                         (g_sel_work_mask & field->bit) ? 'x' : ' ',
                         i18n_get(field->label_key));
            break;
        }
        case SETTINGS_IDX_INFO_COVER_SIZE:
            if (index >= 0 && index < k_info_cover_size_count)
                snprintf(out, out_size, "%dx%d",
                         k_info_cover_sizes[index][0], k_info_cover_sizes[index][1]);
            break;
        case SETTINGS_IDX_INFO_COVER_CHARSET:
            if (index >= 0 && index < INFO_COVER_CHARSET_COUNT)
                snprintf(out, out_size, "%s", index == INFO_COVER_ASCII
                         ? i18n_get("settings.info.charset.ascii")
                         : i18n_get("settings.info.charset.braille"));
            break;
        case SETTINGS_IDX_INFO_PROGRESS_STYLE:
            if (index >= 0 && index < INFO_PROGRESS_STYLE_COUNT)
                snprintf(out, out_size, "%s", info_progress_style_label(index));
            break;
        case SETTINGS_IDX_INFO_LYRICS:
            if (index >= 0 && index <= INFO_LYRICS_MAX)
                snprintf(out, out_size, "%s", info_lyrics_label(index));
            break;
        default:
            break;
    }
}

/* 当前封面尺寸在候选表中的索引（不匹配时取最接近的一档） */
static int info_cover_size_index(void)
{
    int best = 0;
    int best_distance = 1 << 30;
    for (int i = 0; i < k_info_cover_size_count; i++) {
        int distance = abs(g_app_config.info_cover_cols - k_info_cover_sizes[i][0]) +
                       abs(g_app_config.info_cover_rows - k_info_cover_sizes[i][1]);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static void format_settings_option_line(int option_index, char *line, size_t line_size)
{
    const char **current_settings_options = settings_options;
    const char *separator = i18n_get("settings.separator");
    const char *unset_label = i18n_get("general.not_set");

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
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_color_name(color_val), color_val);
    } else if (option_index == SETTINGS_IDX_DEFAULT_PATH) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 g_app_config.default_startup_path[0] ? g_app_config.default_startup_path : unset_label);
    } else if (option_index == SETTINGS_IDX_AUTO_PLAY) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.auto_play_on_start));
    } else if (option_index == SETTINGS_IDX_REMEMBER_PATH) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.remember_last_path));
    } else if (option_index == SETTINGS_IDX_CLEAR_HISTORY) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.clear_history_on_startup));
    } else if (option_index == SETTINGS_IDX_STOP_CORE_ON_EXIT) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.stop_core_on_exit));
    } else if (option_index == SETTINGS_IDX_LATENCY) {
        snprintf(line, line_size, "%s%s%d ms",
                 i18n_get(current_settings_options[option_index]), separator,
                 g_app_config.audio_latency_ms);
    } else if (option_index == SETTINGS_IDX_SHOW_LYRICS) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.show_lyrics_panel));
    } else if (option_index == SETTINGS_IDX_SHOW_ALBUM_COVER) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.show_album_cover));
    } else if (option_index == SETTINGS_IDX_SEAMLESS_PRELOAD) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.seamless_preload));
    } else if (option_index == SETTINGS_IDX_LYRICS_ALIGNMENT) {
        const char *align_str;
        switch (g_app_config.lyrics_alignment) {
            case 1: align_str = i18n_get("align.center"); break;
            case 2: align_str = i18n_get("align.right"); break;
            default: align_str = i18n_get("align.left");
        }
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, align_str);
    } else if (option_index == SETTINGS_IDX_DEFAULT_PLAY_MODE) {
        const char *mode_str = player_play_mode_name_of(
            (PlayMode)g_app_config.default_play_mode, 0);
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, mode_str);
    } else if (option_index == SETTINGS_IDX_ADVANCED_PLAY_MODES) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.advanced_play_modes_enabled));
    } else if (option_index == SETTINGS_IDX_DEFAULT_SPEED) {
        snprintf(line, line_size, "%s%s%.2fx",
                 i18n_get(current_settings_options[option_index]), separator,
                 (double)g_app_config.default_playback_speed);
    } else if (option_index == SETTINGS_IDX_AUDIO_BACKEND) {
        const char *backend_str;
        switch (g_app_config.audio_backend) {
            case AUDIO_BACKEND_AUTO:     backend_str = i18n_get("general.auto"); break;
            case AUDIO_BACKEND_PULSE:    backend_str = "PulseAudio"; break;
            case AUDIO_BACKEND_ALSA:     backend_str = "ALSA"; break;
            case AUDIO_BACKEND_PIPEWIRE: backend_str = "PipeWire"; break;
            default:                     backend_str = i18n_get("general.auto");
        }
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, backend_str);
    } else if (option_index == SETTINGS_IDX_SORT_MODE) {
        const char *sort_str;
        switch (g_app_config.sort_mode) {
            case SORT_DEFAULT:  sort_str = i18n_get("sort.default"); break;
            case SORT_TITLE:    sort_str = i18n_get("sort.title"); break;
            case SORT_ARTIST:   sort_str = i18n_get("sort.artist"); break;
            case SORT_ALBUM:    sort_str = i18n_get("sort.album"); break;
            case SORT_FILENAME: sort_str = i18n_get("sort.filename"); break;
            default:            sort_str = i18n_get("sort.default");
        }
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, sort_str);
    } else if (option_index == SETTINGS_IDX_CUE_ENCODING) {
        const char *enc_str;
        switch (g_app_config.cue_encoding) {
            case CUE_ENCODING_AUTO:      enc_str = i18n_get("general.auto"); break;
            case CUE_ENCODING_UTF8:      enc_str = "UTF-8"; break;
            case CUE_ENCODING_GB18030:   enc_str = "GB18030"; break;
            case CUE_ENCODING_GBK:       enc_str = "GBK"; break;
            case CUE_ENCODING_BIG5:      enc_str = "BIG5"; break;
            case CUE_ENCODING_SHIFT_JIS: enc_str = "Shift-JIS"; break;
            default:                     enc_str = i18n_get("general.auto"); break;
        }
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, enc_str);
    } else if (option_index >= SETTINGS_IDX_EQ_ENABLED &&
               option_index <= SETTINGS_IDX_EQ_BAND_9) {
        if (option_index == SETTINGS_IDX_EQ_ENABLED) {
            snprintf(line, line_size, "%s%s%s",
                     i18n_get(current_settings_options[option_index]), separator,
                     menu_bool_text(g_app_config.eq_enabled));
        } else if (option_index == SETTINGS_IDX_EQ_PREAMP) {
            snprintf(line, line_size, "%s%s%d dB",
                     i18n_get(current_settings_options[option_index]), separator,
                     g_app_config.eq_preamp);
        } else {
            int band = option_index - SETTINGS_IDX_EQ_BAND_0;
            int gain = g_app_config.eq_band_gains[band];
            if (gain >= 0)
                snprintf(line, line_size, "%s%s+%d dB",
                         i18n_get(current_settings_options[option_index]), separator, gain);
            else
                snprintf(line, line_size, "%s%s%d dB",
                         i18n_get(current_settings_options[option_index]), separator, gain);
        }
    } else if (option_index == SETTINGS_IDX_INFO_PRESET) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 info_preset_label(g_app_config.info_preset));
    } else if (option_index == SETTINGS_IDX_INFO_FIELDS) {
        char summary[192];
        info_fields_summary(summary, sizeof(summary));
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator, summary);
    } else if (option_index == SETTINGS_IDX_INFO_COVER) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.info_show_cover));
    } else if (option_index == SETTINGS_IDX_INFO_COVER_SIZE) {
        snprintf(line, line_size, "%s%s%dx%d",
                 i18n_get(current_settings_options[option_index]), separator,
                 g_app_config.info_cover_cols, g_app_config.info_cover_rows);
    } else if (option_index == SETTINGS_IDX_INFO_COVER_CHARSET) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 info_charset_label(g_app_config.info_cover_charset));
    } else if (option_index == SETTINGS_IDX_INFO_PROGRESS) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_bool_text(g_app_config.info_show_progress));
    } else if (option_index == SETTINGS_IDX_INFO_PROGRESS_STYLE) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 info_progress_style_label(g_app_config.info_progress_style));
    } else if (option_index == SETTINGS_IDX_INFO_LYRICS) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 info_lyrics_label(g_app_config.info_lyrics_lines));
    } else {
        snprintf(line, line_size, "%s", i18n_get(current_settings_options[option_index]));
    }
}

/* ============================================================
 * Settings option group rendering
 * ============================================================ */

static void render_settings_option_group(int start_y, int max_y, SettingsSectionSpec spec)
{
    MenuContentArea area;
    menu_content_area(&area);

    for (int i = 0; i < spec.count && start_y + i < max_y - 2; i++) {
        int option_index = spec.indices[i];
        char line[256];
        format_settings_option_line(option_index, line, sizeof(line));
        menu_content_clear_row(&area, start_y + i);
        if (option_index == g_settings_current_option && g_focus_area == FOCUS_CONTENT) {
            attron(A_REVERSE);
            menu_content_puts(&area, start_y + i, line);
            attroff(A_REVERSE);
        } else {
            menu_content_puts(&area, start_y + i, line);
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
    player_config_persist();
}

static void edit_default_startup_path(void)
{
    noecho();
    curs_set(1);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int menu_width = max_x / 4;

    const char *path_prompt = i18n_get("settings.path.prompt");
    char input_path[MAX_PATH_LEN];
    prompt_text_input(stdscr, max_y - 2, menu_width + 2,
                      path_prompt, input_path, sizeof(input_path), 1, 0, 0);

    noecho();
    curs_set(0);

    if (strlen(input_path) > 0) {
        if (!is_valid_path(input_path)) {
            show_status_message(i18n_get("settings.path.invalid"));
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
        player_config_persist();
        show_status_message(i18n_get("settings.path.saved"));
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
            player_config_persist();
            break;
        case SETTINGS_IDX_REMEMBER_PATH:
            g_app_config.remember_last_path = !g_app_config.remember_last_path;
            player_config_persist();
            break;
        case SETTINGS_IDX_CLEAR_HISTORY:
            g_app_config.clear_history_on_startup = !g_app_config.clear_history_on_startup;
            player_config_persist();
            break;
        case SETTINGS_IDX_STOP_CORE_ON_EXIT:
            g_app_config.stop_core_on_exit = !g_app_config.stop_core_on_exit;
            player_config_persist();
            break;
        case SETTINGS_IDX_LATENCY:
            if (delta == 0) delta = 1;
            g_app_config.audio_latency_ms = clamp_latency_ms(g_app_config.audio_latency_ms + delta * 10);
            player_config_persist();
            show_status_message(i18n_get("settings.opt.latency_hint"));
            break;
        case SETTINGS_IDX_SHOW_LYRICS:
            g_app_config.show_lyrics_panel = !g_app_config.show_lyrics_panel;
            player_config_persist();
            break;
        case SETTINGS_IDX_SHOW_ALBUM_COVER:
            g_app_config.show_album_cover = !g_app_config.show_album_cover;
            player_config_persist();
            break;
        case SETTINGS_IDX_SEAMLESS_PRELOAD:
            g_app_config.seamless_preload = !g_app_config.seamless_preload;
            player_config_persist();
            break;
        case SETTINGS_IDX_DEFAULT_PLAY_MODE:
            if (delta < 0) {
                g_app_config.default_play_mode = (g_app_config.default_play_mode - 1 + PLAY_MODE_COUNT) % PLAY_MODE_COUNT;
            } else if (delta > 0) {
                g_app_config.default_play_mode = (g_app_config.default_play_mode + 1) % PLAY_MODE_COUNT;
            } else {
                g_app_config.default_play_mode = (g_app_config.default_play_mode + 1) % PLAY_MODE_COUNT;
            }
            player_config_persist();
            player_set_play_mode((PlayMode)g_app_config.default_play_mode);
            show_status_message(i18n_get("settings.play_mode.updated"));
            break;
        case SETTINGS_IDX_LYRICS_ALIGNMENT:
            if (delta < 0) {
                g_app_config.lyrics_alignment = (g_app_config.lyrics_alignment - 1 + 3) % 3;
            } else {
                g_app_config.lyrics_alignment = (g_app_config.lyrics_alignment + 1) % 3;
            }
            player_config_persist();
            break;
        case SETTINGS_IDX_ADVANCED_PLAY_MODES:
            g_app_config.advanced_play_modes_enabled = !g_app_config.advanced_play_modes_enabled;
            player_config_persist();
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
            player_set_speed(g_app_config.default_playback_speed);
            player_config_persist();
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: %.2fx",
                     i18n_get("settings.speed.updated"),
                     (double)g_app_config.default_playback_speed);
            show_status_message(msg);
            break;
        }
        case SETTINGS_IDX_INFO_PRESET:
            /* 左右键在 全量/精简/自定义 间循环 */
            if (delta < 0) {
                g_app_config.info_preset =
                    (g_app_config.info_preset - 1 + INFO_PRESET_COUNT) % INFO_PRESET_COUNT;
            } else {
                g_app_config.info_preset =
                    (g_app_config.info_preset + 1) % INFO_PRESET_COUNT;
            }
            info_apply_preset_to_config(g_app_config.info_preset);
            player_config_persist();
            show_status_message(i18n_get("settings.info.saved"));
            break;
        case SETTINGS_IDX_INFO_COVER:
            g_app_config.info_show_cover = !g_app_config.info_show_cover;
            info_mark_custom();
            player_config_persist();
            break;
        case SETTINGS_IDX_INFO_PROGRESS:
            g_app_config.info_show_progress = !g_app_config.info_show_progress;
            info_mark_custom();
            player_config_persist();
            break;
        case SETTINGS_IDX_INFO_COVER_SIZE: {
            int index = info_cover_size_index();
            if (delta <= 0) {
                index = (index + k_info_cover_size_count - 1) % k_info_cover_size_count;
            } else {
                index = (index + 1) % k_info_cover_size_count;
            }
            g_app_config.info_cover_cols = k_info_cover_sizes[index][0];
            g_app_config.info_cover_rows = k_info_cover_sizes[index][1];
            info_mark_custom();
            player_config_persist();
            break;
        }
        case SETTINGS_IDX_INFO_COVER_CHARSET:
            if (delta < 0) {
                g_app_config.info_cover_charset =
                    (g_app_config.info_cover_charset - 1 + INFO_COVER_CHARSET_COUNT) % INFO_COVER_CHARSET_COUNT;
            } else {
                g_app_config.info_cover_charset =
                    (g_app_config.info_cover_charset + 1) % INFO_COVER_CHARSET_COUNT;
            }
            info_mark_custom();
            player_config_persist();
            break;
        case SETTINGS_IDX_INFO_PROGRESS_STYLE:
            if (delta < 0) {
                g_app_config.info_progress_style =
                    (g_app_config.info_progress_style - 1 + INFO_PROGRESS_STYLE_COUNT) % INFO_PROGRESS_STYLE_COUNT;
            } else {
                g_app_config.info_progress_style =
                    (g_app_config.info_progress_style + 1) % INFO_PROGRESS_STYLE_COUNT;
            }
            info_mark_custom();
            player_config_persist();
            break;
        case SETTINGS_IDX_INFO_LYRICS:
            if (delta < 0) {
                g_app_config.info_lyrics_lines =
                    (g_app_config.info_lyrics_lines - 1 + (INFO_LYRICS_MAX + 1)) % (INFO_LYRICS_MAX + 1);
            } else {
                g_app_config.info_lyrics_lines =
                    (g_app_config.info_lyrics_lines + 1) % (INFO_LYRICS_MAX + 1);
            }
            info_mark_custom();
            player_config_persist();
            break;
        case SETTINGS_IDX_AUDIO_BACKEND: {
            int options[] = {AUDIO_BACKEND_AUTO, AUDIO_BACKEND_PIPEWIRE,
                             AUDIO_BACKEND_PULSE, AUDIO_BACKEND_ALSA};
            int count = 4;
            int has_pw    = backend_option_selectable(AUDIO_BACKEND_PIPEWIRE);
            int has_pulse = backend_option_selectable(AUDIO_BACKEND_PULSE);
            int has_alsa  = backend_option_selectable(AUDIO_BACKEND_ALSA);
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
            player_config_persist();
            show_status_message(i18n_get("settings.backend.hint"));
            break;
        }
        case SETTINGS_IDX_SORT_MODE:
            if (delta < 0) {
                g_app_config.sort_mode = (g_app_config.sort_mode - 1 + 5) % 5;
            } else {
                g_app_config.sort_mode = (g_app_config.sort_mode + 1) % 5;
            }
            player_config_persist();
            recompute_sort_order();
            show_status_message(i18n_get("settings.sort.applied"));
            break;
        case SETTINGS_IDX_CUE_ENCODING:
            if (delta < 0) {
                g_app_config.cue_encoding = (g_app_config.cue_encoding - 1 + CUE_ENCODING_COUNT) % CUE_ENCODING_COUNT;
            } else {
                g_app_config.cue_encoding = (g_app_config.cue_encoding + 1) % CUE_ENCODING_COUNT;
            }
            player_config_persist();
            show_status_message(i18n_get("settings.cue.saved"));
            break;
        case SETTINGS_IDX_EQ_ENABLED:
            g_app_config.eq_enabled = !g_app_config.eq_enabled;
            player_eq_set_enabled(g_app_config.eq_enabled);
            player_config_persist();
            show_status_message(g_app_config.eq_enabled
                ? i18n_get("eq.enabled")
                : i18n_get("eq.disabled"));
            break;
        case SETTINGS_IDX_EQ_PREAMP:
            if (delta == 0) delta = 1;
            g_app_config.eq_preamp += delta;
            if (g_app_config.eq_preamp < EQ_PREAMP_MIN) g_app_config.eq_preamp = EQ_PREAMP_MIN;
            if (g_app_config.eq_preamp > EQ_PREAMP_MAX) g_app_config.eq_preamp = EQ_PREAMP_MAX;
            player_eq_set_preamp(g_app_config.eq_preamp);
            player_config_persist();
            break;
        default:
            if (option_index >= SETTINGS_IDX_EQ_BAND_0 &&
                option_index <= SETTINGS_IDX_EQ_BAND_9) {
                int band = option_index - SETTINGS_IDX_EQ_BAND_0;
                if (delta == 0) delta = 1;
                g_app_config.eq_band_gains[band] += delta;
                if (g_app_config.eq_band_gains[band] < EQ_GAIN_MIN)
                    g_app_config.eq_band_gains[band] = EQ_GAIN_MIN;
                if (g_app_config.eq_band_gains[band] > EQ_GAIN_MAX)
                    g_app_config.eq_band_gains[band] = EQ_GAIN_MAX;
                player_eq_set_band_gain(band, g_app_config.eq_band_gains[band]);
                player_config_persist();
            }
            break;
    }
}

/* ============================================================
 * Theme color palette helpers (dynamic count based on COLORS)
 * ============================================================ */

static int sel_color_count(void)
{
    if (!has_colors() || COLORS <= 0) return 0;
    int base;
    if (COLORS >= 256) base = 24;
    else if (COLORS >= 16)  base = 16;
    else if (COLORS >= 8)   base = 8;
    else base = COLORS;
    return base + 1;  /* +1 for transparent (-1) */
}

static int sel_color_val(int i)
{
    if (i == 0) return -1;  /* transparent */
    i--;  /* shift: remaining colors map to old indices */
    if (i < 8) return i;
    if (i < 16) return 8 + (i - 8);
    static const int cube[] = {208,130,198,93,37,75,203,118};
    if ((i - 16) < 8) return cube[i - 16];
    return -1;
}

/* ============================================================
 * Selection menu — close / apply
 * ============================================================ */

static void close_sel_menu(int apply)
{
    if (!g_sel_active) return;

    if (apply) {
        extern float g_playback_speed;

        switch (g_sel_src) {
            case SETTINGS_IDX_DEFAULT_PLAY_MODE:
                g_app_config.default_play_mode = g_sel_idx;
                player_config_persist();
                player_set_play_mode((PlayMode)g_app_config.default_play_mode);
                show_status_message(i18n_get("settings.play_mode.updated"));
                break;

            case SETTINGS_IDX_DEFAULT_SPEED:
                g_app_config.default_playback_speed = player_speed_steps(NULL)[g_sel_idx];
                player_set_speed_index(g_sel_idx);
                player_config_persist();
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%s: %.2fx",
                             i18n_get("settings.speed.updated"),
                             (double)g_app_config.default_playback_speed);
                    show_status_message(msg);
                }
                break;

            case SETTINGS_IDX_AUDIO_BACKEND: {
                int backend_opts[] = {AUDIO_BACKEND_AUTO, AUDIO_BACKEND_PIPEWIRE,
                                      AUDIO_BACKEND_PULSE, AUDIO_BACKEND_ALSA};
                int idx = 0;
                for (int j = 0; j < 4; j++) {
                    if (j > 0 && backend_opts[j] != AUDIO_BACKEND_AUTO
                        && !backend_option_selectable(backend_opts[j]))
                        continue;
                    if (idx == g_sel_idx) {
                        g_app_config.audio_backend = backend_opts[j];
                        break;
                    }
                    idx++;
                }
                player_config_persist();
                show_status_message(i18n_get("settings.backend.hint"));
                break;
            }

            case SETTINGS_IDX_SORT_MODE:
                g_app_config.sort_mode = g_sel_idx;
                player_config_persist();
                recompute_sort_order();
                show_status_message(i18n_get("settings.sort.applied"));
                break;

            case SETTINGS_IDX_CUE_ENCODING:
                g_app_config.cue_encoding = g_sel_idx;
                player_config_persist();
                show_status_message(i18n_get("settings.cue.saved"));
                break;

            case SETTINGS_IDX_LYRICS_ALIGNMENT:
                g_app_config.lyrics_alignment = g_sel_idx;
                player_config_persist();
                break;

            case SETTINGS_IDX_LATENCY: {
                int latency_opts[] = {20, 40, 60, 80, 100, 120, 150, 200, 250};
                g_app_config.audio_latency_ms = latency_opts[g_sel_idx];
                player_config_persist();
                show_status_message(i18n_get("settings.opt.latency_hint"));
                break;
            }

            case SETTINGS_IDX_INFO_PRESET:
                info_apply_preset_to_config(g_sel_idx);
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_INFO_FIELDS:
                g_app_config.info_fields_mask = g_sel_work_mask & INFO_FIELD_ALL;
                info_mark_custom();
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_INFO_COVER_SIZE:
                if (g_sel_idx >= 0 && g_sel_idx < k_info_cover_size_count) {
                    g_app_config.info_cover_cols = k_info_cover_sizes[g_sel_idx][0];
                    g_app_config.info_cover_rows = k_info_cover_sizes[g_sel_idx][1];
                }
                info_mark_custom();
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_INFO_COVER_CHARSET:
                g_app_config.info_cover_charset = g_sel_idx;
                info_mark_custom();
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_INFO_PROGRESS_STYLE:
                g_app_config.info_progress_style = g_sel_idx;
                info_mark_custom();
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_INFO_LYRICS:
                g_app_config.info_lyrics_lines = g_sel_idx;
                info_mark_custom();
                player_config_persist();
                show_status_message(i18n_get("settings.info.saved"));
                break;

            case SETTINGS_IDX_EQ_PRESET:
                player_eq_apply_preset(g_sel_idx);
                /* Sync config with new EQ state */
                g_app_config.eq_enabled = player_eq_enabled();
                for (int b = 0; b < EQ_BAND_COUNT; b++)
                    g_app_config.eq_band_gains[b] = player_eq_get_band_gain(b);
                player_config_persist();
                /* Pre-amp is preserved, not reset by preset */
                break;

            default:
                if (g_sel_src >= 0 && g_sel_src < 12) {
                    /* Theme color: find selected color value, skip paired */
                    int *cv[] = {&g_app_config.theme.playlist_fg,&g_app_config.theme.playlist_bg,
                                 &g_app_config.theme.controls_fg,&g_app_config.theme.controls_bg,
                                 &g_app_config.theme.lyrics_fg,&g_app_config.theme.lyrics_bg,
                                 &g_app_config.theme.sidebar_fg,&g_app_config.theme.sidebar_bg,
                                 &g_app_config.theme.highlight_fg,&g_app_config.theme.highlight_bg,
                                 &g_app_config.theme.border_fg,&g_app_config.theme.border_bg};
                    int paired = *cv[(g_sel_src % 2 == 0) ? g_sel_src + 1 : g_sel_src - 1];
                    int idx = 0;
                    int n = sel_color_count();
                    for (int j = 0; j < n; j++) {
                        int v = sel_color_val(j);
                        if (v == paired && v != -1) continue;
                        if (idx == g_sel_idx) {
                            *cv[g_sel_src] = v;
                            break;
                        }
                        idx++;
                    }
                    apply_color_theme();
                    player_config_persist();
                }
                break;
        }
    }

    /* Destroy popup sub-window */
    if (g_sel_win) {
        delwin(g_sel_win);
        g_sel_win = NULL;
    }

    g_sel_active = 0;
    g_sel_src = -1;
    g_sel_multiselect = 0;
}

/* ============================================================
 * Selection menu — open
 * ============================================================ */

static void open_sel_menu(int option_index)
{
    if (g_sel_active) close_sel_menu(0);

    int count = 0, cur = 0;

    g_sel_multiselect = 0;
    g_sel_work_mask = g_app_config.info_fields_mask;

    switch (option_index) {
        case SETTINGS_IDX_DEFAULT_PLAY_MODE:
            count = PLAY_MODE_COUNT;
            cur   = g_app_config.default_play_mode;
            break;
        case SETTINGS_IDX_DEFAULT_SPEED:
            cur   = player_speed_index();
            {
                int step_count = 0;
                const float *steps = player_speed_steps(&step_count);
                count = step_count;
                (void)steps;
            }
            break;
        case SETTINGS_IDX_AUDIO_BACKEND: {
            int backend_opts[] = {AUDIO_BACKEND_AUTO, AUDIO_BACKEND_PIPEWIRE,
                                  AUDIO_BACKEND_PULSE, AUDIO_BACKEND_ALSA};
            count = 0;
            for (int j = 0; j < 4; j++) {
                if (j > 0 && backend_opts[j] != AUDIO_BACKEND_AUTO
                    && !backend_option_selectable(backend_opts[j]))
                    continue;
                if (g_app_config.audio_backend == backend_opts[j])
                    cur = count;
                count++;
            }
            break;
        }
        case SETTINGS_IDX_SORT_MODE:
            count = 5;
            cur   = g_app_config.sort_mode;
            break;
        case SETTINGS_IDX_CUE_ENCODING:
            count = CUE_ENCODING_COUNT;
            cur   = g_app_config.cue_encoding;
            break;
        case SETTINGS_IDX_EQ_PRESET:
            count = EQ_PRESET_COUNT;
            cur   = 0;
            break;
        case SETTINGS_IDX_LYRICS_ALIGNMENT:
            count = 3;
            cur   = g_app_config.lyrics_alignment;
            break;
        case SETTINGS_IDX_INFO_PRESET:
            count = INFO_PRESET_COUNT;
            cur   = g_app_config.info_preset;
            break;
        case SETTINGS_IDX_INFO_FIELDS:
            count = info_field_count();
            cur   = 0;
            g_sel_multiselect = 1;
            break;
        case SETTINGS_IDX_INFO_COVER_SIZE:
            count = k_info_cover_size_count;
            cur   = info_cover_size_index();
            break;
        case SETTINGS_IDX_INFO_COVER_CHARSET:
            count = INFO_COVER_CHARSET_COUNT;
            cur   = g_app_config.info_cover_charset;
            break;
        case SETTINGS_IDX_INFO_PROGRESS_STYLE:
            count = INFO_PROGRESS_STYLE_COUNT;
            cur   = g_app_config.info_progress_style;
            break;
        case SETTINGS_IDX_INFO_LYRICS:
            count = INFO_LYRICS_MAX + 1;
            cur   = g_app_config.info_lyrics_lines;
            break;
        case SETTINGS_IDX_LATENCY: {
            int lat[] = {20,40,60,80,100,120,150,200,250};
            count = 9;
            cur   = 0;
            int best = abs(g_app_config.audio_latency_ms - lat[0]);
            for (int i = 1; i < count; i++) {
                int d = abs(g_app_config.audio_latency_ms - lat[i]);
                if (d < best) { best = d; cur = i; }
            }
            break;
        }
        default:
            if (option_index >= 0 && option_index < 12) {
                /* Theme color: show 8 colors, skip paired fg/bg */
                int *cv[] = {&g_app_config.theme.playlist_fg,&g_app_config.theme.playlist_bg,
                             &g_app_config.theme.controls_fg,&g_app_config.theme.controls_bg,
                             &g_app_config.theme.lyrics_fg,&g_app_config.theme.lyrics_bg,
                             &g_app_config.theme.sidebar_fg,&g_app_config.theme.sidebar_bg,
                             &g_app_config.theme.highlight_fg,&g_app_config.theme.highlight_bg,
                             &g_app_config.theme.border_fg,&g_app_config.theme.border_bg};
                int paired = *cv[(option_index % 2 == 0) ? option_index + 1 : option_index - 1];
                int current = *cv[option_index];
                count = 0;
                int n = sel_color_count();
                for (int i = 0; i < n; i++) {
                    int v = sel_color_val(i);
                    if (v == paired && v != -1) continue;
                    if (v == current) cur = count;
                    count++;
                }
                /* Custom slot: auto-select if current not in presets */
                if (n > 0) {
                    int found = 0;
                    for (int i = 0; i < n; i++) {
                        if (sel_color_val(i) == current) { found = 1; break; }
                    }
                    if (!found && current >= 0 && current < COLORS)
                        cur = count;
                    count++; /* +1 for custom */
                }
            }
            break;
    }

    if (count <= 0) return;

    g_sel_active = 1;
    g_sel_src    = option_index;
    g_sel_idx    = cur;
    g_sel_count  = count;

    create_sel_window();
}

/* ============================================================
 * Selection menu — render on sub-window with box()
 * ============================================================ */

static void create_sel_window(void)
{
    /* Destroy any existing window first */
    if (g_sel_win) { delwin(g_sel_win); g_sel_win = NULL; }

    if (!g_sel_active) return;

    const int src = g_sel_src;
    const int cnt = g_sel_count;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    /* Build option-text array to gauge width */
    char opts[32][48];
    int n = cnt > 32 ? 32 : cnt;
    int max_w = 14;

    for (int i = 0; i < n; i++) {
        opts[i][0] = '\0';
        switch (src) {
            case SETTINGS_IDX_DEFAULT_PLAY_MODE:
                snprintf(opts[i], 48, "%s",
                         player_play_mode_name_of((PlayMode)i, 0)); break;
            case SETTINGS_IDX_DEFAULT_SPEED:
                if (i < player_speed_step_count())
                    snprintf(opts[i], 48, "%.2fx", (double)player_speed_steps(NULL)[i]);
                break;
            case SETTINGS_IDX_AUDIO_BACKEND: {
                int be[] = {AUDIO_BACKEND_AUTO,AUDIO_BACKEND_PIPEWIRE,
                            AUDIO_BACKEND_PULSE,AUDIO_BACKEND_ALSA};
                int idx = 0;
                for (int j = 0; j < 4; j++) {
                    if (j>0 && be[j]!=AUDIO_BACKEND_AUTO && !backend_option_selectable(be[j]))
                        continue;
                    if (idx == i) {
                        switch (be[j]) {
                            case AUDIO_BACKEND_AUTO: snprintf(opts[i],48,"%s",i18n_get("general.auto"));break;
                            case AUDIO_BACKEND_PIPEWIRE: snprintf(opts[i],48,"PipeWire");break;
                            case AUDIO_BACKEND_PULSE:    snprintf(opts[i],48,"PulseAudio");break;
                            case AUDIO_BACKEND_ALSA:     snprintf(opts[i],48,"ALSA");break;
                        }
                        break;
                    }
                    idx++;
                }
                break;
            }
            case SETTINGS_IDX_SORT_MODE:{
                const char *a[]={i18n_get("sort.default"),i18n_get("sort.title"),
                                 i18n_get("sort.artist"),i18n_get("sort.album"),
                                 i18n_get("sort.filename")};
                if (i<5) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_CUE_ENCODING:{
                const char *a[]={i18n_get("general.auto"),"UTF-8","GB18030","GBK",
                                 "BIG5","Shift-JIS"};
                if (i<CUE_ENCODING_COUNT) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_LYRICS_ALIGNMENT:{
                const char *a[]={i18n_get("align.left"),i18n_get("align.center"),
                                 i18n_get("align.right")};
                if (i<3) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_LATENCY:{
                /* 输出延迟弹窗：按 "%d ms" 表渲染（Task 5.3，独立完整的 case）。 */
                int lat[]={20,40,60,80,100,120,150,200,250};
                if (i < 9) snprintf(opts[i], 48, "%d ms", lat[i]);
                break;
            }
            case SETTINGS_IDX_EQ_PRESET:{
                /* EQ 预设弹窗：显示 i18n 预设名（Task 5.3，与延迟 case 彼此独立）。 */
                const char *preset_keys[] = {
                    "eq.preset.rock", "eq.preset.pop", "eq.preset.classical",
                    "eq.preset.jazz", "eq.preset.electronic", "eq.preset.vocal",
                    "eq.preset.bass_boost", "eq.preset.live",
                    "eq.preset.balanced", "eq.preset.flat"
                };
                if (i < EQ_PRESET_COUNT)
                    snprintf(opts[i], 48, "%s", i18n_get(preset_keys[i]));
                break;
            }
            case SETTINGS_IDX_INFO_PRESET:
            case SETTINGS_IDX_INFO_FIELDS:
            case SETTINGS_IDX_INFO_COVER_SIZE:
            case SETTINGS_IDX_INFO_COVER_CHARSET:
            case SETTINGS_IDX_INFO_PROGRESS_STYLE:
            case SETTINGS_IDX_INFO_LYRICS:
                info_sel_option_text(src, i, opts[i], sizeof(opts[i]));
                break;
            default:
                if (src >= 0 && src < 12) {
                    int cv[] = {g_app_config.theme.playlist_fg,g_app_config.theme.playlist_bg,
                                g_app_config.theme.controls_fg,g_app_config.theme.controls_bg,
                                g_app_config.theme.lyrics_fg,g_app_config.theme.lyrics_bg,
                                g_app_config.theme.sidebar_fg,g_app_config.theme.sidebar_bg,
                                g_app_config.theme.highlight_fg,g_app_config.theme.highlight_bg,
                                g_app_config.theme.border_fg,g_app_config.theme.border_bg};
                    /* Custom slot (last item) */
                    if (i == g_sel_count - 1) {
                        int val = cv[src];
                        int in_pre = 0;
                        for (int k = 0; k < sel_color_count(); k++) {
                            if (sel_color_val(k) == val) { in_pre = 1; break; }
                        }
                        if (in_pre || val < 0 || val >= COLORS)
                            snprintf(opts[i], 48, "[%s...]", i18n_get("general.custom"));
                        else
                            snprintf(opts[i], 48, "%s: %d", i18n_get("general.custom"), val);
                        break;
                    }
                    /* Preset colors */
                    int paired = cv[(src % 2 == 0) ? src + 1 : src - 1];
                    int idx = 0;
                    int n = sel_color_count();
                    for (int j = 0; j < n; j++) {
                        int v = sel_color_val(j);
                        if (v == paired && v != -1) continue;
                        if (idx == i) {
                            snprintf(opts[i], 48, "%s (%d)", menu_color_name(v), v);
                            break;
                        }
                        idx++;
                    }
                }
                break;
        }
        int l = utf8_str_width(opts[i]);
        if (l > max_w) max_w = l;
    }

    int box_w = max_w + 4;   /* 1 space + text + 1 space + border */
    if (box_w < 20) box_w = 20;
    if (box_w > 50) box_w = 50;
    int max_box_w = max_x - (max_x / 4 + 2) - 2;
    if (max_box_w < 20) max_box_w = 20;
    if (box_w > max_box_w) box_w = max_box_w;
    int box_h = n + 2;
    if (box_h > 20) box_h = 20;

    /* Position: right-anchor, row-aligned with option */
    int menu_x = max_x / 4;
    int col    = max_x - box_w - 2;
    if (col < menu_x + 2) col = menu_x + 2;

    int row = 4; /* default first-option row */
    SettingsSectionSpec spec = get_active_settings_section_spec();
    int pos = get_settings_section_position(spec, src);
    if (pos >= 0) row = 4 + pos;
    if (row + box_h >= max_y - 2) row = max_y - box_h - 3;
    if (row < 2) row = 2;

    g_sel_win = newwin(box_h, box_w, row, col);
    if (!g_sel_win) {
        g_sel_win = NULL;
        g_sel_active = 0;
    }
}

static void draw_sel_menu(void)
{
    if (!g_sel_active || !g_sel_win) return;

    int box_h, box_w;
    getmaxyx(g_sel_win, box_h, box_w);

    const int src = g_sel_src;
    const int cnt = g_sel_count;

    /* Build option-text array */
    char opts[32][48];
    int n = cnt > 32 ? 32 : cnt;

    for (int i = 0; i < n; i++) {
        opts[i][0] = '\0';
        switch (src) {
            case SETTINGS_IDX_DEFAULT_PLAY_MODE:
                snprintf(opts[i], 48, "%s",
                         player_play_mode_name_of((PlayMode)i, 0)); break;
            case SETTINGS_IDX_DEFAULT_SPEED:
                if (i < player_speed_step_count())
                    snprintf(opts[i], 48, "%.2fx", (double)player_speed_steps(NULL)[i]);
                break;
            case SETTINGS_IDX_AUDIO_BACKEND: {
                int be[] = {AUDIO_BACKEND_AUTO,AUDIO_BACKEND_PIPEWIRE,
                            AUDIO_BACKEND_PULSE,AUDIO_BACKEND_ALSA};
                int idx = 0;
                for (int j = 0; j < 4; j++) {
                    if (j>0 && be[j]!=AUDIO_BACKEND_AUTO && !backend_option_selectable(be[j]))
                        continue;
                    if (idx == i) {
                        switch (be[j]) {
                            case AUDIO_BACKEND_AUTO: snprintf(opts[i],48,"%s",i18n_get("general.auto"));break;
                            case AUDIO_BACKEND_PIPEWIRE: snprintf(opts[i],48,"PipeWire");break;
                            case AUDIO_BACKEND_PULSE:    snprintf(opts[i],48,"PulseAudio");break;
                            case AUDIO_BACKEND_ALSA:     snprintf(opts[i],48,"ALSA");break;
                        }
                        break;
                    }
                    idx++;
                }
                break;
            }
            case SETTINGS_IDX_SORT_MODE:{
                const char *a[]={i18n_get("sort.default"),i18n_get("sort.title"),
                                 i18n_get("sort.artist"),i18n_get("sort.album"),
                                 i18n_get("sort.filename")};
                if (i<5) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_CUE_ENCODING:{
                const char *a[]={i18n_get("general.auto"),"UTF-8","GB18030","GBK",
                                 "BIG5","Shift-JIS"};
                if (i<CUE_ENCODING_COUNT) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_LYRICS_ALIGNMENT:{
                const char *a[]={i18n_get("align.left"),i18n_get("align.center"),
                                 i18n_get("align.right")};
                if (i<3) snprintf(opts[i],48,"%s",a[i]); break;
            }
            case SETTINGS_IDX_LATENCY:{
                /* 输出延迟弹窗：按 "%d ms" 表渲染（Task 5.3，独立完整的 case，
                 * 修复原先直落 EQ 预设、毫秒行不可达的结构错乱）。 */
                int lat[]={20,40,60,80,100,120,150,200,250};
                if (i < 9) snprintf(opts[i], 48, "%d ms", lat[i]);
                break;
            }
            case SETTINGS_IDX_EQ_PRESET:{
                /* EQ 预设弹窗：显示 i18n 预设名（Task 5.3，与延迟 case 彼此独立）。 */
                const char *preset_keys[] = {
                    "eq.preset.rock", "eq.preset.pop", "eq.preset.classical",
                    "eq.preset.jazz", "eq.preset.electronic", "eq.preset.vocal",
                    "eq.preset.bass_boost", "eq.preset.live",
                    "eq.preset.balanced", "eq.preset.flat"
                };
                if (i < EQ_PRESET_COUNT)
                    snprintf(opts[i], 48, "%s", i18n_get(preset_keys[i]));
                break;
            }
            case SETTINGS_IDX_INFO_PRESET:
            case SETTINGS_IDX_INFO_FIELDS:
            case SETTINGS_IDX_INFO_COVER_SIZE:
            case SETTINGS_IDX_INFO_COVER_CHARSET:
            case SETTINGS_IDX_INFO_PROGRESS_STYLE:
            case SETTINGS_IDX_INFO_LYRICS:
                info_sel_option_text(src, i, opts[i], sizeof(opts[i]));
                break;
            default:
                if (src >= 0 && src < 12) {
                    int cv[] = {g_app_config.theme.playlist_fg,g_app_config.theme.playlist_bg,
                                g_app_config.theme.controls_fg,g_app_config.theme.controls_bg,
                                g_app_config.theme.lyrics_fg,g_app_config.theme.lyrics_bg,
                                g_app_config.theme.sidebar_fg,g_app_config.theme.sidebar_bg,
                                g_app_config.theme.highlight_fg,g_app_config.theme.highlight_bg,
                                g_app_config.theme.border_fg,g_app_config.theme.border_bg};
                    if (i == g_sel_count - 1) {
                        int val = cv[src];
                        int in_pre = 0;
                        for (int k = 0; k < sel_color_count(); k++) {
                            if (sel_color_val(k) == val) { in_pre = 1; break; }
                        }
                        if (in_pre || val < 0 || val >= COLORS)
                            snprintf(opts[i], 48, "[%s...]", i18n_get("general.custom"));
                        else
                            snprintf(opts[i], 48, "%s: %d", i18n_get("general.custom"), val);
                        break;
                    }
                    int paired = cv[(src % 2 == 0) ? src + 1 : src - 1];
                    int idx = 0;
                    int nc = sel_color_count();
                    for (int j = 0; j < nc; j++) {
                        int v = sel_color_val(j);
                        if (v == paired && v != -1) continue;
                        if (idx == i) {
                            snprintf(opts[i], 48, "%s (%d)", menu_color_name(v), v);
                            break;
                        }
                        idx++;
                    }
                }
                break;
        }
    }

    /* ── Draw on sub-window ── */
    werase(g_sel_win);

    /* Border and title: use highlight color pair */
    wattron(g_sel_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    rounded_box(g_sel_win);

    /* Title */
    const char *title = "";
    switch (src) {
        case SETTINGS_IDX_DEFAULT_PLAY_MODE: title = i18n_get("popup.play_mode"); break;
        case SETTINGS_IDX_DEFAULT_SPEED:     title = i18n_get("popup.speed"); break;
        case SETTINGS_IDX_AUDIO_BACKEND:     title = i18n_get("popup.backend"); break;
        case SETTINGS_IDX_SORT_MODE:         title = i18n_get("popup.sort"); break;
        case SETTINGS_IDX_LYRICS_ALIGNMENT:  title = i18n_get("popup.align"); break;
        case SETTINGS_IDX_LATENCY:           title = i18n_get("popup.latency"); break;
        case SETTINGS_IDX_CUE_ENCODING:      title = i18n_get("popup.cue_enc"); break;
        case SETTINGS_IDX_EQ_PRESET:        title = i18n_get("popup.eq_preset"); break;
        case SETTINGS_IDX_INFO_PRESET:
        case SETTINGS_IDX_INFO_FIELDS:
        case SETTINGS_IDX_INFO_COVER_SIZE:
        case SETTINGS_IDX_INFO_COVER_CHARSET:
        case SETTINGS_IDX_INFO_PROGRESS_STYLE:
        case SETTINGS_IDX_INFO_LYRICS:
            if (src >= 0 && src < SETTINGS_OPTION_COUNT)
                title = i18n_get(settings_options[src]);
            break;
        default:
            if (src >= 0 && src < 12)
                title = i18n_get("popup.color");
            break;
    }
    if (title[0])
        mvwprintw(g_sel_win, 0, 2, "%s", title);

    /* Pre-fill all option rows with highlight background color so there are
       no blank spots — the COLOR_PAIR fills the background, then we overlay
       option text in terminal defaults (A_NORMAL / A_REVERSE) on top. */
    int visible_opts = box_h - 2;
    int scroll_offset = 0;
    if (g_sel_idx >= visible_opts)
        scroll_offset = g_sel_idx - visible_opts + 1;

    for (int i = 0; i < visible_opts; i++)
        mvwprintw(g_sel_win, i + 1, 1, " %-*s ", box_w - 4, "");

    /* Options — HIGHLIGHT color pair, selected item uses A_REVERSE
       for differentiation. wattrset per iteration ensures clean slate
       with no attribute residual. */
    for (int i = 0; i < visible_opts; i++) {
        int opt_idx = scroll_offset + i;
        if (opt_idx >= n) continue;
        if (opt_idx == g_sel_idx)
            wattrset(g_sel_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_REVERSE);
        else
            wattrset(g_sel_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        char clipped[256];
        utf8_str_truncate(clipped, opts[opt_idx], box_w - 4);
        mvwprintw(g_sel_win, i + 1, 1, " %s ", clipped);
    }

    /* Scroll indicators (use wattrset to avoid any A_REVERSE residual) */
    wattrset(g_sel_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    if (scroll_offset > 0)
        mvwaddch(g_sel_win, 1, box_w - 2, '^');
    if (scroll_offset + visible_opts < g_sel_count)
        mvwaddch(g_sel_win, box_h - 2, box_w - 2, 'v');

    wnoutrefresh(g_sel_win);
}

/* ============================================================
 * Selection menu — input handling
 * ============================================================ */

static int handle_sel_input(int ch)
{
    if (!g_sel_active) return 0;

    /* Esc 是系统保留键；其余键位由 select_menu 上下文解析。 */
    ActionId action = (ch == 27) ? ACTION_SELECT_CANCEL
                                 : keymap_resolve(CTX_SELECT_MENU, ch);

    switch (action) {
        case ACTION_SELECT_UP:
            if (g_sel_idx > 0) { g_sel_idx--; draw_sel_menu(); refresh(); }
            return 1;
        case ACTION_SELECT_DOWN:
            if (g_sel_idx < g_sel_count - 1) { g_sel_idx++; draw_sel_menu(); refresh(); }
            return 1;
        case ACTION_SELECT_ALL:
            if (g_sel_multiselect) {
                g_sel_work_mask = INFO_FIELD_ALL;
                draw_sel_menu();
                refresh();
                return 1;
            }
            return 0;
        case ACTION_SELECT_NONE:
            if (g_sel_multiselect) {
                g_sel_work_mask = 0;
                draw_sel_menu();
                refresh();
                return 1;
            }
            return 0;
        case ACTION_SELECT_TOGGLE:
            if (g_sel_multiselect) {
                const InfoFieldDef *field = info_field_at(g_sel_idx);
                if (field) {
                    g_sel_work_mask ^= field->bit;
                    draw_sel_menu();
                    refresh();
                }
                return 1;
            }
            /* FALLTHROUGH：单选菜单中空格等同回车 */
            /* fall through */
        case ACTION_SELECT_ACCEPT:
            if (g_sel_multiselect) {
                close_sel_menu(1);
                rerender_settings_view();
                return 1;
            }
            /* Custom color slot: prompt for a numeric value */
            if (g_sel_src >= 0 && g_sel_src < 12 &&
                g_sel_idx == g_sel_count - 1) {
                int *cv[] = {&g_app_config.theme.playlist_fg,&g_app_config.theme.playlist_bg,
                             &g_app_config.theme.controls_fg,&g_app_config.theme.controls_bg,
                             &g_app_config.theme.lyrics_fg,&g_app_config.theme.lyrics_bg,
                             &g_app_config.theme.sidebar_fg,&g_app_config.theme.sidebar_bg,
                             &g_app_config.theme.highlight_fg,&g_app_config.theme.highlight_bg,
                             &g_app_config.theme.border_fg,&g_app_config.theme.border_bg};
                char buf[8];
                int cur = *cv[g_sel_src];
                snprintf(buf, sizeof(buf), "%d", cur);
                int my, mx;
                getmaxyx(stdscr, my, mx);
                noecho();
                curs_set(1);
                prompt_text_input(stdscr, my - 3, mx / 4 + 2,
                    i18n_get("settings.color.prompt"),
                    buf, sizeof(buf), 0, 0, 0);
                curs_set(0);
                noecho();
                int v = atoi(buf);
                {
                    int paired_idx = (g_sel_src % 2 == 0) ? g_sel_src + 1 : g_sel_src - 1;
                    int paired_val = *cv[paired_idx];
                    if (v != cur && v >= 0 && v < COLORS && v != paired_val) {
                        *cv[g_sel_src] = v;
                        apply_color_theme();
                        player_config_persist();
                    } else if (v == paired_val) {
                        show_status_message(i18n_get("settings.color.same_error"));
                    }
                }
                g_sel_active = 0;
                g_sel_src = -1;
                rerender_settings_view();
                return 1;
            }
            close_sel_menu(1);
            rerender_settings_view();
            return 1;
        case ACTION_SELECT_CANCEL:
            close_sel_menu(0);
            rerender_settings_view();
            return 1;
        default:
            return 0;
    }
}

static void activate_settings_current_option(void)
{
    if (g_settings_current_option == SETTINGS_IDX_DEFAULT_PATH) {
        edit_default_startup_path();
        return;
    }

    /* 配置管理动作项（Task 5.5）：重新加载 / 恢复默认，均经门面下发。
     * 生效后由配置修订号变化驱动重刷配色与内容（sync_player_revisions），
     * 状态提示复用既有 status 机制。 */
    if (g_settings_current_option == SETTINGS_IDX_RELOAD_CONFIG) {
        if (player_config_reload() == 0) {
            show_status_message(i18n_get("menu.config_reloaded"));
        }
        return;
    }
    if (g_settings_current_option == SETTINGS_IDX_RESET_CONFIG) {
        if (player_config_reset() == 0) {
            show_status_message(i18n_get("menu.config_reset"));
        }
        return;
    }

    /* Toggle-only: ENTER flips the boolean */
    if (g_settings_current_option == SETTINGS_IDX_AUTO_PLAY ||
        g_settings_current_option == SETTINGS_IDX_REMEMBER_PATH ||
        g_settings_current_option == SETTINGS_IDX_CLEAR_HISTORY ||
        g_settings_current_option == SETTINGS_IDX_STOP_CORE_ON_EXIT ||
        g_settings_current_option == SETTINGS_IDX_SHOW_LYRICS ||
        g_settings_current_option == SETTINGS_IDX_SHOW_ALBUM_COVER ||
        g_settings_current_option == SETTINGS_IDX_SEAMLESS_PRELOAD ||
        g_settings_current_option == SETTINGS_IDX_ADVANCED_PLAY_MODES ||
        g_settings_current_option == SETTINGS_IDX_INFO_COVER ||
        g_settings_current_option == SETTINGS_IDX_INFO_PROGRESS ||
        g_settings_current_option == SETTINGS_IDX_EQ_ENABLED) {
        adjust_or_toggle_settings_option(g_settings_current_option, 0);
        return;
    }

    /* Multi-choice: ENTER opens the popup */
    if (g_settings_current_option == SETTINGS_IDX_LATENCY ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_PLAY_MODE ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_SPEED ||
        g_settings_current_option == SETTINGS_IDX_LYRICS_ALIGNMENT ||
        g_settings_current_option == SETTINGS_IDX_AUDIO_BACKEND ||
        g_settings_current_option == SETTINGS_IDX_SORT_MODE ||
        g_settings_current_option == SETTINGS_IDX_CUE_ENCODING ||
        g_settings_current_option == SETTINGS_IDX_INFO_PRESET ||
        g_settings_current_option == SETTINGS_IDX_INFO_FIELDS ||
        g_settings_current_option == SETTINGS_IDX_INFO_COVER_SIZE ||
        g_settings_current_option == SETTINGS_IDX_INFO_COVER_CHARSET ||
        g_settings_current_option == SETTINGS_IDX_INFO_PROGRESS_STYLE ||
        g_settings_current_option == SETTINGS_IDX_INFO_LYRICS) {
        open_sel_menu(g_settings_current_option);
        return;
    }

    /* Theme colors: ENTER opens color sel menu */
    if (g_settings_current_option >= 0 && g_settings_current_option < 12) {
        open_sel_menu(g_settings_current_option);
        return;
    }

    /* Fallback fallthrough (shouldn't normally reach here) */
    if (g_settings_current_option >= 0) {
        adjust_or_toggle_settings_option(g_settings_current_option, 1);
    }
}


/* ============================================================
 * Hotkey editor (settings -> hotkeys)
 * ============================================================ */

int settings_hotkey_capture_active(void)
{
    return g_hotkey_capturing;
}

static int hotkey_editing_available(void)
{
    return g_app_config.config_version >= 9;
}

static ActionId hotkey_selected_action(void)
{
    int total = actions_bindable_count();
    if (total <= 0) return ACTION_NONE;
    if (g_hotkey_selected < 0) g_hotkey_selected = 0;
    if (g_hotkey_selected >= total) g_hotkey_selected = total - 1;
    ActionId id = ACTION_NONE;
    if (!actions_bindable_at(g_hotkey_selected, &id)) {
        return ACTION_NONE;
    }
    return id;
}

static void build_hotkey_keys_text(ActionId action, char *out, size_t out_size)
{
    char keys[4][KEYNAME_MAX_LEN];
    int count = keymap_action_keys(action, keys, 4);

    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        char display[KEYNAME_MAX_LEN * 2];
        const char *text = keys[i];
        KeyStroke stroke;
        if (keyname_parse(keys[i], &stroke) == 0 &&
            keyname_format_display(stroke, display, sizeof(display)) == 0) {
            text = display;
        }
        if (i > 0) {
            strncat(out, " / ", out_size - strlen(out) - 1);
        }
        strncat(out, text, out_size - strlen(out) - 1);
    }
    if (out[0] == '\0') {
        snprintf(out, out_size, "%s", i18n_get("general.none"));
    }
}

static void render_hotkey_editor(int start_y, int max_y)
{
    MenuContentArea area;
    menu_content_area(&area);

    int total = actions_bindable_count();
    int message_row = max_y - 3;
    int list_top = start_y + 4;
    char buf[512];

    snprintf(buf, sizeof(buf), "%s", i18n_get("settings.hotkeys.editor_hint"));
    menu_content_puts(&area, start_y, buf);
    snprintf(buf, sizeof(buf), "%s", i18n_get("settings.hotkeys.editor_hint2"));
    menu_content_puts(&area, start_y + 1, buf);
    snprintf(buf, sizeof(buf), "%s", i18n_get("settings.hotkeys.reserved_note"));
    menu_content_puts(&area, start_y + 2, buf);

    /* 加载时被剔除的改绑（键冲突/非法/锁定）一次性提示，防止旧配置被静默
     * 剥离。加载层与 keymap_reload 层互斥计数（加载剔除的条目不会进入
     * reload），相加即本次启动真正未生效的改绑总数。 */
    int dropped_total = g_app_config.keybinding_dropped_count +
                        g_app_config.keybinding_reload_dropped_count;
    menu_content_clear_row(&area, start_y + 3);
    if (dropped_total > 0) {
        snprintf(buf, sizeof(buf),
                 i18n_get("settings.hotkeys.dropped_notice"), dropped_total);
        attron(A_BOLD);
        menu_content_puts(&area, start_y + 3, buf);
        attroff(A_BOLD);
    }

    if (total <= 0 || message_row <= list_top) return;

    if (g_hotkey_selected < 0) g_hotkey_selected = 0;
    if (g_hotkey_selected >= total) g_hotkey_selected = total - 1;

    int visible = message_row - 1 - list_top;
    if (visible < 1) visible = 1;

    if (g_hotkey_selected < g_hotkey_offset) {
        g_hotkey_offset = g_hotkey_selected;
    } else if (g_hotkey_selected >= g_hotkey_offset + visible) {
        g_hotkey_offset = g_hotkey_selected - visible + 1;
    }
    if (g_hotkey_offset < 0) g_hotkey_offset = 0;

    for (int row = 0; row < visible && g_hotkey_offset + row < total; row++) {
        ActionId id = ACTION_NONE;
        if (!actions_bindable_at(g_hotkey_offset + row, &id)) {
            continue;
        }
        char keys_text[128];
        char line[320];
        const char *mark = keymap_action_has_override(id)
            ? i18n_get("settings.hotkeys.override_mark")
            : " ";

        build_hotkey_keys_text(id, keys_text, sizeof(keys_text));
        snprintf(line, sizeof(line), "%s %s  %s", mark,
                 keymap_action_label(id), keys_text);

        int y = list_top + row;
        menu_content_clear_row(&area, y);
        if (g_hotkey_offset + row == g_hotkey_selected &&
            g_focus_area == FOCUS_CONTENT) {
            attron(A_REVERSE);
            menu_content_puts(&area, y, line);
            attroff(A_REVERSE);
        } else {
            menu_content_puts(&area, y, line);
        }
    }

    const char *message = g_hotkey_message;
    char capture_buf[256];
    if (g_hotkey_capturing) {
        snprintf(capture_buf, sizeof(capture_buf),
                 i18n_get("settings.hotkeys.capturing"),
                 keymap_action_label(hotkey_selected_action()));
        message = capture_buf;
    } else if (message[0] == '\0' && !hotkey_editing_available()) {
        message = i18n_get("settings.hotkeys.core_too_old");
    }
    if (message[0] != '\0') {
        if (message_row > area.max_y - 3) message_row = area.max_y - 3;
        menu_content_puts(&area, message_row, message);
    }
}

/* 处理快捷键页的编辑器动作；未处理返回 0，交由通用设置输入逻辑。 */
static int handle_hotkey_editor_action(ActionId action)
{
    switch (action) {
        case ACTION_SETTINGS_UP:
            if (actions_bindable_count() > 0) {
                if (g_hotkey_selected <= 0) {
                    g_hotkey_selected = actions_bindable_count() - 1;
                } else {
                    g_hotkey_selected--;
                }
            }
            g_hotkey_message[0] = '\0';
            render_settings_content();
            return 1;

        case ACTION_SETTINGS_DOWN:
            if (actions_bindable_count() > 0) {
                if (g_hotkey_selected >= actions_bindable_count() - 1) {
                    g_hotkey_selected = 0;
                } else {
                    g_hotkey_selected++;
                }
            }
            g_hotkey_message[0] = '\0';
            render_settings_content();
            return 1;

        case ACTION_SETTINGS_ACTIVATE:
        case ACTION_SETTINGS_RIGHT:
            if (!hotkey_editing_available()) {
                snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                         i18n_get("settings.hotkeys.core_too_old"));
            } else {
                g_hotkey_capturing = 1;
                g_hotkey_message[0] = '\0';
            }
            render_settings_content();
            return 1;

        case ACTION_SETTINGS_HOTKEY_RESET: {
            if (!hotkey_editing_available()) {
                snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                         i18n_get("settings.hotkeys.core_too_old"));
            } else {
                ActionId id = hotkey_selected_action();
                keymap_remove_override(id);
                snprintf(g_hotkey_message, sizeof(g_hotkey_message),
                         i18n_get("settings.hotkeys.reset_one"),
                         keymap_action_label(id));
            }
            render_settings_content();
            return 1;
        }

        case ACTION_SETTINGS_HOTKEY_RESET_ALL:
            if (!hotkey_editing_available()) {
                snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                         i18n_get("settings.hotkeys.core_too_old"));
            } else {
                keymap_remove_all_bindable_overrides();
                snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                         i18n_get("settings.hotkeys.reset_all"));
            }
            render_settings_content();
            return 1;

        default:
            return 0;
    }
}

/* 捕获模式：任何按键都不再走动作分发。 */
static int handle_hotkey_capture(int ch)
{
    ActionId id = hotkey_selected_action();
    g_hotkey_capturing = 0;

    if (id == ACTION_NONE || !actions_is_bindable(id)) {
        snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                 i18n_get("settings.hotkeys.locked_action"));
        render_settings_content();
        return 1;
    }

    if (ch == 27) {
        snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                 i18n_get("settings.hotkeys.capture_cancelled"));
        render_settings_content();
        return 1;
    }

    char key_name[KEYNAME_MAX_LEN];
    if (!hotkey_editing_available()) {
        snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                 i18n_get("settings.hotkeys.core_too_old"));
    } else if (keymap_keycode_to_name(ch, key_name, sizeof(key_name)) != 0 ||
               keymap_is_reserved_keycode(ch)) {
        snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s",
                 i18n_get("settings.hotkeys.invalid_key"));
    } else {
        char err[256] = "";
        if (keymap_set_override(id, key_name, err, sizeof(err)) == 0) {
            char display[KEYNAME_MAX_LEN * 2];
            KeyStroke stroke;
            if (keyname_parse(key_name, &stroke) == 0 &&
                keyname_format_display(stroke, display, sizeof(display)) == 0) {
                /* display ready */
            } else {
                snprintf(display, sizeof(display), "%s", key_name);
            }
            snprintf(g_hotkey_message, sizeof(g_hotkey_message),
                     i18n_get("settings.hotkeys.bound"),
                     keymap_action_label(id), display);
        } else {
            snprintf(g_hotkey_message, sizeof(g_hotkey_message), "%s", err);
        }
    }

    render_settings_content();
    return 1;
}

/* ============================================================
 * Settings content rendering (public API)
 * ============================================================ */

void render_settings_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);

    int start_y = 2;
    SettingsSectionSpec spec = get_active_settings_section_spec();
    char hint_buf[512];

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    menu_content_clear_rows(&area, 2, area.max_y - 3);

    if (g_menu_selected_idx == 0) {  /* 颜色主题 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.color.select_hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.color.auto_avoid_same"));
        menu_content_puts(&area, start_y, hint_buf);
    } else if (g_menu_selected_idx == 1) {  /* 默认路径 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.path.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.path.tilde_hint"));
        menu_content_puts(&area, start_y, hint_buf);
    } else if (g_menu_selected_idx == 2) {  /* 播放设置 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.playback.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.playback.language"));
        menu_content_puts(&area, start_y, hint_buf);
    } else if (g_menu_selected_idx == 3) {  /* 播放模式 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.play_mode.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec);
    } else if (g_menu_selected_idx == 4) {  /* 快捷键 */
        render_hotkey_editor(start_y, area.max_y);
    } else if (g_menu_selected_idx == 5) {  /* 远程设备 */
        remote_view_render();
    } else if (g_menu_selected_idx == 6) {  /* 均衡器 */
        render_eq_visual();
        attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    } else if (g_menu_selected_idx == 7) {  /* 信息显示（CLI / D-Bus） */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.info.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.info.fields_hint"));
        menu_content_puts(&area, start_y, hint_buf);
    } else {
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.hotkeys.enter_return"));
        menu_content_puts(&area, start_y, hint_buf);
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    /* Refresh stdscr first, then overlay selection menu sub-window */
    wnoutrefresh(stdscr);
    if (g_sel_active)
        draw_sel_menu();
    doupdate();
}

/* ============================================================
 * Settings view re-render helper
 * ============================================================ */

static void rerender_settings_view(void)
{
    render_menu_frame(i18n_get("menu.settings"));
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
    render_menu_hint_bar();
}

/* ============================================================
 * Settings input handling (declared in menu_internal.h)
 * ============================================================ */

/* ── EQ visual rendering ──────────────────────────────────── */

static void render_eq_visual(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int start_y = 2;

    /* Status header */
    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    const char *status = g_app_config.eq_enabled
        ? i18n_get("eq.title_enabled")
        : i18n_get("eq.title_disabled");

    /* Clear content area */
    menu_content_clear_rows(&area, start_y, area.max_y - 3);

    menu_content_puts(&area, start_y, status);
    start_y++;

    /* Help row (include pre-amp and enable toggle hint) */
    menu_content_puts(&area, start_y, i18n_get("eq.hint"));
    start_y++;

    /* Pre-amp display */
    {
        int pre_selected = (g_focus_area == FOCUS_CONTENT
                         && g_settings_current_option == SETTINGS_IDX_EQ_PREAMP);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d dB",
                 i18n_get("eq.preampl"), g_app_config.eq_preamp);
        if (pre_selected)
            attron(A_REVERSE);
        menu_content_puts(&area, start_y, buf);
        if (pre_selected)
            attroff(A_REVERSE);
    }

    /* Enable toggle */
    {
        int en_selected = (g_focus_area == FOCUS_CONTENT
                        && g_settings_current_option == SETTINGS_IDX_EQ_ENABLED);
        char buf[24];
        snprintf(buf, sizeof(buf), "  %s: %s",
                 i18n_get("eq.title"),
                 g_app_config.eq_enabled
                     ? i18n_get("general.on")
                     : i18n_get("general.off"));
        if (area.content_width > 20) {
            char clipped[64];
            utf8_str_truncate(clipped, buf, area.content_width - 20);
            if (en_selected)
                attron(A_REVERSE);
            mvaddstr(start_y, area.content_x + 20, clipped);
            if (en_selected)
                attroff(A_REVERSE);
        }
    }
    start_y++;

    /* Separator */
    menu_content_puts(&area, start_y, "─────────────────────────────────────────────────────");
    start_y++;

    /* Compute bar fill levels */
    int selected_band = -1;
    if (g_settings_current_option >= SETTINGS_IDX_EQ_BAND_0 &&
        g_settings_current_option <= SETTINGS_IDX_EQ_BAND_9)
        selected_band = g_settings_current_option - SETTINGS_IDX_EQ_BAND_0;

    /* Each band gets 5 columns: "  ██ " filled or "     " empty */
#define EQ_COL_W 5

    int max_bands = area.content_width / EQ_COL_W;
    if (max_bands > EQ_BAND_COUNT) max_bands = EQ_BAND_COUNT;
    if (max_bands < 0) max_bands = 0;

    int above[EQ_BAND_COUNT], below[EQ_BAND_COUNT];
    for (int i = 0; i < EQ_BAND_COUNT; i++) {
        int g = g_app_config.eq_band_gains[i];
        if (g >= 0) {
            above[i] = (g + 2) / 3;
            if (above[i] > 4) above[i] = 4;
            below[i] = 0;
        } else {
            above[i] = 0;
            below[i] = ((-g) + 2) / 3;
            if (below[i] > 4) below[i] = 4;
        }
    }

    /* Draw boost bars (row 0 = top = +12dB) */
    for (int row = 3; row >= 0; row--) {
        for (int b = 0; b < max_bands; b++) {
            int col = b * EQ_COL_W;
            if (above[b] > row) {
                if (g_focus_area == FOCUS_CONTENT && b == selected_band) {
                    attron(A_REVERSE);
                    mvaddstr(start_y, area.content_x + col, "  ██ ");
                    attroff(A_REVERSE);
                } else {
                    mvaddstr(start_y, area.content_x + col, "  ██ ");
                }
            } else {
                mvaddstr(start_y, area.content_x + col, "     ");
            }
        }
        start_y++;
    }

    /* 0dB line */
    {
        menu_content_puts(&area, start_y,
                          "─────────────────────┬─────────────────────────");
        if (area.content_width >= 25) {
            char zero[8];
            utf8_str_truncate(zero, "0dB", area.content_width - 22);
            mvaddstr(start_y, area.content_x + 22, zero);
        }
        start_y++;
    }

    /* Draw cut bars (row 0 = top of cut = -3dB, row 3 = -12dB) */
    for (int row = 0; row < 4; row++) {
        for (int b = 0; b < max_bands; b++) {
            int col = b * EQ_COL_W;
            if (below[b] > row) {
                if (g_focus_area == FOCUS_CONTENT && b == selected_band) {
                    attron(A_REVERSE);
                    mvaddstr(start_y, area.content_x + col, "  ░░ ");
                    attroff(A_REVERSE);
                } else {
                    mvaddstr(start_y, area.content_x + col, "  ░░ ");
                }
            } else {
                mvaddstr(start_y, area.content_x + col, "     ");
            }
        }
        start_y++;
    }

    /* Separator */
    menu_content_puts(&area, start_y, "─────────────────────────────────────────────────────");
    start_y++;

    /* Frequency labels */
    for (int b = 0; b < max_bands; b++) {
        char label[8];
        char cell[16];
        int f = eq_band_frequencies[b];
        if (f >= 1000)
            snprintf(label, sizeof(label), " %dk ", f / 1000);
        else if (f < 100)
            snprintf(label, sizeof(label), " %d ", f);
        else
            snprintf(label, sizeof(label), "%d", f);
        /* Center-pad to EQ_COL_W */
        int pad = EQ_COL_W - (int)strlen(label);
        if (pad > 0)
            snprintf(cell, sizeof(cell), "%*s%s%*s", pad / 2, "", label, (pad + 1) / 2, "");
        else
            snprintf(cell, sizeof(cell), "%-*s", EQ_COL_W, label);
        mvaddstr(start_y, area.content_x + b * EQ_COL_W, cell);
    }
    start_y++;

    /* Gain values */
    for (int b = 0; b < max_bands; b++) {
        int g = g_app_config.eq_band_gains[b];
        char gbuf[8];
        char cell[16];
        if (g > 0)      snprintf(gbuf, sizeof(gbuf), "+%d", g);
        else if (g == 0) snprintf(gbuf, sizeof(gbuf), " 0");
        else             snprintf(gbuf, sizeof(gbuf), "%d", g);
        snprintf(cell, sizeof(cell), "%-*s", EQ_COL_W, gbuf);
        if (g_focus_area == FOCUS_CONTENT && b == selected_band) {
            attron(A_REVERSE);
            mvaddstr(start_y, area.content_x + b * EQ_COL_W, cell);
            attroff(A_REVERSE);
        } else {
            mvaddstr(start_y, area.content_x + b * EQ_COL_W, cell);
        }
    }
    start_y++;

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    wnoutrefresh(stdscr);
    doupdate();
#undef EQ_COL_W
}

/* ── EQ visual input handler ─────────────────────────────── */

static int handle_eq_action(ActionId action)
{
    switch (action) {
        case ACTION_EQ_BAND_PREV:
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_settings_current_option > SETTINGS_IDX_EQ_ENABLED &&
                    g_settings_current_option <= SETTINGS_IDX_EQ_BAND_9) {
                    g_settings_current_option--;
                    rerender_settings_view();
                } else {
                    /* Already at first EQ option — move to sidebar */
                    g_focus_area = FOCUS_SIDEBAR;
                    rerender_settings_view();
                }
            }
            return 1;

        case ACTION_EQ_BAND_NEXT:
            if (g_focus_area == FOCUS_CONTENT) {
                if (g_settings_current_option >= SETTINGS_IDX_EQ_ENABLED &&
                    g_settings_current_option < SETTINGS_IDX_EQ_BAND_9) {
                    g_settings_current_option++;
                    rerender_settings_view();
                } else {
                    g_settings_current_option = SETTINGS_IDX_EQ_ENABLED;
                    rerender_settings_view();
                }
            }
            return 1;

        case ACTION_EQ_GAIN_UP:
            if (g_focus_area == FOCUS_CONTENT) {
                adjust_or_toggle_settings_option(g_settings_current_option, 1);
                rerender_settings_view();
            }
            return 1;

        case ACTION_EQ_GAIN_DOWN:
            if (g_focus_area == FOCUS_CONTENT) {
                adjust_or_toggle_settings_option(g_settings_current_option, -1);
                rerender_settings_view();
            }
            return 1;

        case ACTION_EQ_BACK:
            g_focus_area = FOCUS_SIDEBAR;
            rerender_settings_view();
            return 1;

        case ACTION_EQ_ACTIVATE:
            if (g_focus_area == FOCUS_CONTENT) {
                activate_settings_current_option();
                rerender_settings_view();
            }
            return 1;

        default:
            return 0;
    }
}

int handle_settings_input(int ch)
{
    /* Selection menu active — handle menu input first */
    if (g_sel_active) {
        handle_sel_input(ch);
        return 1;
    }

    ActionId action;

    /* 快捷键页捕获模式优先：不解析任何动作。 */
    if (g_menu_selected_idx == 4 && g_focus_area == FOCUS_CONTENT &&
        g_hotkey_capturing) {
        return handle_hotkey_capture(ch);
    }

    /* EQ section uses dedicated visual input handler */
    if (g_menu_selected_idx == 6 && g_focus_area == FOCUS_CONTENT) {
        action = keymap_resolve(CTX_SETTINGS_EQ, ch);
        if (action != ACTION_NONE && actions_context(action) == CTX_SETTINGS_EQ) {
            handle_eq_action(action);
            return 1;
        }
        /* 旧行为：EQ 内容区吞掉非全局键；仅全局动作交给上层分发。 */
        if (keymap_resolve(CTX_GLOBAL, ch) != ACTION_NONE) return 0;
        return 1;
    } else if (g_menu_selected_idx == 4 && g_focus_area == FOCUS_CONTENT) {
        /* 快捷键编辑器：↑/↓ 移动选中行，ENTER 捕获，r/R 恢复。 */
        action = keymap_resolve(CTX_SETTINGS, ch);
        if (handle_hotkey_editor_action(action)) return 1;
    } else {
        /* Remote section handles its own input when content-focused */
        if (g_menu_selected_idx == 5 && g_focus_area == FOCUS_CONTENT) {
            if (remote_view_handle_input(ch)) return 1;
            /* 旧行为：远程内容区吞掉非全局键；仅全局动作交给上层分发 */
            if (keymap_resolve(CTX_GLOBAL, ch) != ACTION_NONE) return 0;
            return 1;
        }
        action = keymap_resolve(CTX_SETTINGS, ch);
    }

    /* EQ preset popup（仅均衡器页） */
    if (action == ACTION_SETTINGS_EQ_PRESETS) {
        if (g_menu_selected_idx == 6) {
            open_sel_menu(SETTINGS_IDX_EQ_PRESET);
            draw_sel_menu();
            refresh();
        }
        return 1;
    }

    switch (action) {
        case ACTION_SETTINGS_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = SETTINGS_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            } else {
                move_settings_content_selection(-1);
                render_settings_content();
            }
            return 1;

        case ACTION_SETTINGS_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= SETTINGS_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            } else {
                move_settings_content_selection(1);
                render_settings_content();
            }
            return 1;

        case ACTION_SETTINGS_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
                render_settings_content();
            }
            return 1;

        case ACTION_SETTINGS_RIGHT:
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
                /* RIGHT in content mode behaves like ENTER */
                activate_settings_current_option();
                rerender_settings_view();
            }
            return 1;

        case ACTION_SETTINGS_TAB:
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
            return 1;

        case ACTION_SETTINGS_ACTIVATE:
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
            return 1;

        case ACTION_SETTINGS_SAVE:
            log_debug("settings", "saving config (keybinding overrides=%d)",
                      g_app_config.keybinding_override_count);
            player_config_persist();
            show_status_message(i18n_get("status.settings_saved"));
            rerender_settings_view();
            return 1;

        case ACTION_SETTINGS_HOTKEY_RESET:
        case ACTION_SETTINGS_HOTKEY_RESET_ALL:
            /* 仅快捷键页内容区处理（见 handle_hotkey_editor_action）。 */
            return 0;

        default:
            return 0;
    }
}
