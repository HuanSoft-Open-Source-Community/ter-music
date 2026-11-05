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
#include "ui/layout.h"
#include "ui/layout_editor.h"
#include "ui/context_menu_layout.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "ui/rainbow.h"
#include "ui/scrollbar.h"
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

/* 选择弹层的行几何：由 draw_sel_menu() 每帧记录，鼠标命中只读不推导。 */
static int g_sel_visible_opts = 0;
static int g_sel_scroll_offset = 0;
static int g_sel_count_shown = 0;

/* 选择弹层创建时的终端尺寸：窗口缩放后 render_settings_content() 据此
 * 重建并重新锚定，避免旧坐标越出新的终端边界。 */
static int g_sel_term_rows = 0;
static int g_sel_term_cols = 0;

/* 设置内容区列表的几何：由 render_settings_option_group() 每帧记录。
 * top<0 = 这一帧没画列表（分区为空或空间不足）。 */
static int g_settings_list_top = -1;
static int g_settings_list_visible = 0;
static int g_settings_list_off = 0;

/* 快捷键页列表的几何：由 render_hotkey_editor() 每帧记录。 */
static int g_hotkey_list_top = -1;
static int g_hotkey_list_visible = 0;

/* EQ 页的频带是按**列**排的（每 band 宽 EQ_COL_W 列，跨若干行），
 * 所以命中要记的是列起点与可用行范围，由 render_eq_visual() 每帧记录。
 * bottom<0 = 这一帧没画频带。 */
static int g_eq_bars_top = -1;
static int g_eq_bars_bottom = -1;
static int g_eq_col0 = 0;
static int g_eq_col_w = 5;
static int g_eq_max_bands = 0;

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
    "settings.opt.ui_layout",
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
    "settings.opt.stop_core_on_exit",
    "settings.opt.layout_reload",
    "settings.opt.layout_edit",
    "settings.opt.layout_new",
    /* ── 窗格颜色（尾部追加：当前布局的 pane fg/bg，见 ColourRow 注释） ── */
    "settings.opt.info_fg",
    "settings.opt.info_bg",
    "settings.opt.spectrum_fg",
    "settings.opt.spectrum_bg",
    "settings.opt.vinyl_fg",
    "settings.opt.vinyl_bg",
    /* ── 卡拉OK（消人声）：尾部追加，不重排既有下标 ── */
    "settings.opt.karaoke_mode"
};

/* 与 SETTINGS_IDX_* 一一对应的选项总数（含 eq_preset、reload/reset；Task 5.4/5.5，
 * 另含尾部追加的 stop_core_on_exit、layout_reload、layout_edit/layout_new，
 * 54..59 的窗格颜色项，以及 60 的卡拉OK（消人声）档位）。 */
#define SETTINGS_OPTION_COUNT 61

/* 编译期护栏：文案表必须与枚举/段数组使用同一数量。 */
typedef char settings_option_arrays_must_match_count[
    (sizeof(settings_options) / sizeof(settings_options[0]) == SETTINGS_OPTION_COUNT)
        ? 1 : -1];

enum {
    /* ── 窗格颜色行（绑定当前布局的 pane fg/bg；继承时可写下面的主题基色） ──
     * 下标语义变更：0/1=播放列表、2/3=控制区、4/5=歌词，与主题同名槽一一对应。 */
    SETTINGS_IDX_PANE_PLAYLIST_FG    = 0,
    SETTINGS_IDX_PANE_PLAYLIST_BG    = 1,
    SETTINGS_IDX_PANE_CONTROLS_FG    = 2,
    SETTINGS_IDX_PANE_CONTROLS_BG    = 3,
    SETTINGS_IDX_PANE_LYRICS_FG      = 4,
    SETTINGS_IDX_PANE_LYRICS_BG      = 5,
    /* ── 界面颜色行（侧边栏/高亮/边框，始终写 config.xml 主题） ── */
    SETTINGS_IDX_THEME_SIDEBAR_FG    = 6,
    SETTINGS_IDX_THEME_SIDEBAR_BG    = 7,
    SETTINGS_IDX_THEME_HIGHLIGHT_FG  = 8,
    SETTINGS_IDX_THEME_HIGHLIGHT_BG  = 9,
    SETTINGS_IDX_THEME_BORDER_FG     = 10,
    SETTINGS_IDX_THEME_BORDER_BG     = 11,
    SETTINGS_IDX_DEFAULT_PATH        = 12,
    SETTINGS_IDX_AUTO_PLAY           = 13,
    SETTINGS_IDX_REMEMBER_PATH       = 14,
    SETTINGS_IDX_CLEAR_HISTORY       = 15,
    SETTINGS_IDX_LATENCY             = 16,
    SETTINGS_IDX_UI_LAYOUT           = 17,
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
    SETTINGS_IDX_STOP_CORE_ON_EXIT   = 50,
    SETTINGS_IDX_LAYOUT_RELOAD       = 51,
    /* ── 布局编辑器入口（动作项：ENTER 打开，无 ←/→ 调整） ── */
    SETTINGS_IDX_LAYOUT_EDIT         = 52,
    SETTINGS_IDX_LAYOUT_NEW          = 53,
    /* ── 缺失的三个窗格配色（信息栏/频谱/黑胶）：尾部追加，不重排既有下标 ── */
    SETTINGS_IDX_PANE_INFO_FG        = 54,
    SETTINGS_IDX_PANE_INFO_BG        = 55,
    SETTINGS_IDX_PANE_SPECTRUM_FG    = 56,
    SETTINGS_IDX_PANE_SPECTRUM_BG    = 57,
    SETTINGS_IDX_PANE_VINYL_FG       = 58,
    SETTINGS_IDX_PANE_VINYL_BG       = 59,
    /* ── 卡拉OK（消人声）档位：尾部追加，不重排既有下标 ── */
    SETTINGS_IDX_KARAOKE_MODE        = 60
};

typedef struct {
    const int *indices;
    int count;
} SettingsSectionSpec;

static const int settings_theme_option_indices[] = {
    /* 窗格颜色：按屏幕上的阅读顺序（左列上→下、右列上→下）成对排列。
     * 值来自当前布局，切换/重载布局后这些行立刻变成该布局的配色。 */
    SETTINGS_IDX_PANE_PLAYLIST_FG,
    SETTINGS_IDX_PANE_PLAYLIST_BG,
    SETTINGS_IDX_PANE_INFO_FG,
    SETTINGS_IDX_PANE_INFO_BG,
    SETTINGS_IDX_PANE_CONTROLS_FG,
    SETTINGS_IDX_PANE_CONTROLS_BG,
    SETTINGS_IDX_PANE_SPECTRUM_FG,
    SETTINGS_IDX_PANE_SPECTRUM_BG,
    SETTINGS_IDX_PANE_LYRICS_FG,
    SETTINGS_IDX_PANE_LYRICS_BG,
    SETTINGS_IDX_PANE_VINYL_FG,
    SETTINGS_IDX_PANE_VINYL_BG,
    /* 界面颜色：全局主题，存 config.xml。 */
    SETTINGS_IDX_THEME_SIDEBAR_FG,
    SETTINGS_IDX_THEME_SIDEBAR_BG,
    SETTINGS_IDX_THEME_HIGHLIGHT_FG,
    SETTINGS_IDX_THEME_HIGHLIGHT_BG,
    SETTINGS_IDX_THEME_BORDER_FG,
    SETTINGS_IDX_THEME_BORDER_BG,
    SETTINGS_IDX_UI_LAYOUT,
    SETTINGS_IDX_LAYOUT_RELOAD,
    SETTINGS_IDX_LAYOUT_EDIT,
    SETTINGS_IDX_LAYOUT_NEW
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
    SETTINGS_IDX_SHOW_ALBUM_COVER,
    SETTINGS_IDX_SEAMLESS_PRELOAD,
    SETTINGS_IDX_LYRICS_ALIGNMENT,
    SETTINGS_IDX_DEFAULT_SPEED,
    SETTINGS_IDX_AUDIO_BACKEND,
    SETTINGS_IDX_SORT_MODE,
    SETTINGS_IDX_CUE_ENCODING,
    /* 卡拉OK（消人声）：与延迟/后端同属音频链路，尾部追加 */
    SETTINGS_IDX_KARAOKE_MODE
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
/* 设置页动作入口：键盘解析结果与鼠标共用（定义在本文件末尾的输入分发处）。 */
static int handle_settings_action(ActionId action);

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

/* 卡拉OK（消人声）档位的显示名：取值域就是 audio/karaoke.h 的 KARAOKE_MODE_*。
 * 与主页控制窗格的弹层用同一组 i18n 键，两处不会各说各话。 */
static const char *karaoke_mode_label(int mode)
{
    switch (mode) {
        case KARAOKE_MODE_SOFT: return i18n_get("karaoke.level.soft");
        case KARAOKE_MODE_FULL: return i18n_get("karaoke.level.full");
        case KARAOKE_MODE_OFF:
        default:                return i18n_get("karaoke.level.off");
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
    switch (charset) {
        case INFO_COVER_ASCII: return i18n_get("settings.info.charset.ascii");
        case INFO_COVER_HALF:  return i18n_get("settings.info.charset.half");
        case INFO_COVER_BRAILLE:
        default:               return i18n_get("settings.info.charset.braille");
    }
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
            if (field) {
                const char *cli_only = field->pane_field
                    ? "" : i18n_get("settings.info.field_cli_only");
                snprintf(out, out_size, "[%c] %s%s%s",
                         (g_sel_work_mask & field->bit) ? 'x' : ' ',
                         i18n_get(field->label_key),
                         cli_only[0] ? " " : "", cli_only);
            }
            break;
        }
        case SETTINGS_IDX_INFO_COVER_SIZE:
            if (index >= 0 && index < k_info_cover_size_count)
                snprintf(out, out_size, "%dx%d",
                         k_info_cover_sizes[index][0], k_info_cover_sizes[index][1]);
            break;
        case SETTINGS_IDX_INFO_COVER_CHARSET:
            if (index >= 0 && index < INFO_COVER_CHARSET_COUNT)
                snprintf(out, out_size, "%s", info_charset_label(index));
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

/* 当前 EQ 增益与哪个内置预设完全一致；都不一致返回 0（Rock）。
 * 预设弹窗据此把光标放到当前档，而不是永远从第一项开始。 */
static int eq_preset_index_from_config(void)
{
    for (int preset = 0; preset < EQ_PRESET_COUNT; preset++) {
        const int *gains = eq_preset_gains(preset);
        if (!gains) {
            continue;
        }
        int match = 1;
        for (int band = 0; band < EQ_BAND_COUNT; band++) {
            if (g_app_config.eq_band_gains[band] != gains[band]) {
                match = 0;
                break;
            }
        }
        if (match) {
            return preset;
        }
    }
    return 0;
}

/* ============================================================
 * 颜色行统一目标：窗格色（当前布局）+ 界面色（全局主题）
 *
 * 两套颜色定义在这里合流。一行代表的字段可能由三种地方决定：
 *   1) 布局显式写了这个字段 → 改动写回布局（内置预设自动落成同 id
 *      用户覆盖，来源为 TER_MUSIC_LAYOUTS 时自增副本 id）；
 *   2) 字段继承、且该窗格在主题里有自己的基色（播放列表/控制区/歌词）
 *      → 改动写主题，与所有继承该基色的布局一起变（等价于旧行为）；
 *   3) 字段继承、但没有自己的基色（信息栏/频谱/黑胶）→ 只能写布局，
 *      这正是这三个窗格"颜色独立了却没有选项"要补的入口。
 * 侧边栏/高亮/边框是界面色，永远走主题（config.xml）。
 * ============================================================ */

/* 主题里的 12 个颜色槽，顺序与 config.xml 的 <theme> 字段一致。 */
enum {
    THEME_SLOT_PLAYLIST_FG = 0, THEME_SLOT_PLAYLIST_BG,
    THEME_SLOT_CONTROLS_FG,     THEME_SLOT_CONTROLS_BG,
    THEME_SLOT_LYRICS_FG,       THEME_SLOT_LYRICS_BG,
    THEME_SLOT_SIDEBAR_FG,      THEME_SLOT_SIDEBAR_BG,
    THEME_SLOT_HIGHLIGHT_FG,    THEME_SLOT_HIGHLIGHT_BG,
    THEME_SLOT_BORDER_FG,       THEME_SLOT_BORDER_BG
};

/* 全文件唯一的"主题槽 → 字段指针"表（取代原先散落在 5 处的重复数组）。 */
static int *theme_color_slot(int slot)
{
    ColorTheme *t = &g_app_config.theme;
    switch (slot) {
        case THEME_SLOT_PLAYLIST_FG:  return &t->playlist_fg;
        case THEME_SLOT_PLAYLIST_BG:  return &t->playlist_bg;
        case THEME_SLOT_CONTROLS_FG:  return &t->controls_fg;
        case THEME_SLOT_CONTROLS_BG:  return &t->controls_bg;
        case THEME_SLOT_LYRICS_FG:    return &t->lyrics_fg;
        case THEME_SLOT_LYRICS_BG:    return &t->lyrics_bg;
        case THEME_SLOT_SIDEBAR_FG:   return &t->sidebar_fg;
        case THEME_SLOT_SIDEBAR_BG:   return &t->sidebar_bg;
        case THEME_SLOT_HIGHLIGHT_FG: return &t->highlight_fg;
        case THEME_SLOT_HIGHLIGHT_BG: return &t->highlight_bg;
        case THEME_SLOT_BORDER_FG:    return &t->border_fg;
        case THEME_SLOT_BORDER_BG:    return &t->border_bg;
        default:                      return NULL;
    }
}

typedef struct {
    int option;        /* SETTINGS_IDX_* */
    int paired;        /* 配对行（前景↔背景），沿用"前后景不重色"保护 */
    signed char kind;  /* LayoutPaneKind；-1 = 界面色行，只写主题 */
    signed char field; /* LAYOUT_COLOUR_FG / LAYOUT_COLOUR_BG */
    signed char slot;  /* 界面色行：自身主题槽；窗格行：继承时可写的基色槽，-1 = 无基色 */
} ColourRow;

static const ColourRow k_colour_rows[] = {
    { SETTINGS_IDX_PANE_PLAYLIST_FG, SETTINGS_IDX_PANE_PLAYLIST_BG,
      LAYOUT_PANE_PLAYLIST, LAYOUT_COLOUR_FG, THEME_SLOT_PLAYLIST_FG },
    { SETTINGS_IDX_PANE_PLAYLIST_BG, SETTINGS_IDX_PANE_PLAYLIST_FG,
      LAYOUT_PANE_PLAYLIST, LAYOUT_COLOUR_BG, THEME_SLOT_PLAYLIST_BG },
    { SETTINGS_IDX_PANE_INFO_FG, SETTINGS_IDX_PANE_INFO_BG,
      LAYOUT_PANE_INFO, LAYOUT_COLOUR_FG, -1 },
    { SETTINGS_IDX_PANE_INFO_BG, SETTINGS_IDX_PANE_INFO_FG,
      LAYOUT_PANE_INFO, LAYOUT_COLOUR_BG, -1 },
    { SETTINGS_IDX_PANE_CONTROLS_FG, SETTINGS_IDX_PANE_CONTROLS_BG,
      LAYOUT_PANE_CONTROLS, LAYOUT_COLOUR_FG, THEME_SLOT_CONTROLS_FG },
    { SETTINGS_IDX_PANE_CONTROLS_BG, SETTINGS_IDX_PANE_CONTROLS_FG,
      LAYOUT_PANE_CONTROLS, LAYOUT_COLOUR_BG, THEME_SLOT_CONTROLS_BG },
    { SETTINGS_IDX_PANE_SPECTRUM_FG, SETTINGS_IDX_PANE_SPECTRUM_BG,
      LAYOUT_PANE_SPECTRUM, LAYOUT_COLOUR_FG, -1 },
    { SETTINGS_IDX_PANE_SPECTRUM_BG, SETTINGS_IDX_PANE_SPECTRUM_FG,
      LAYOUT_PANE_SPECTRUM, LAYOUT_COLOUR_BG, -1 },
    { SETTINGS_IDX_PANE_LYRICS_FG, SETTINGS_IDX_PANE_LYRICS_BG,
      LAYOUT_PANE_LYRICS, LAYOUT_COLOUR_FG, THEME_SLOT_LYRICS_FG },
    { SETTINGS_IDX_PANE_LYRICS_BG, SETTINGS_IDX_PANE_LYRICS_FG,
      LAYOUT_PANE_LYRICS, LAYOUT_COLOUR_BG, THEME_SLOT_LYRICS_BG },
    { SETTINGS_IDX_PANE_VINYL_FG, SETTINGS_IDX_PANE_VINYL_BG,
      LAYOUT_PANE_VINYL, LAYOUT_COLOUR_FG, -1 },
    { SETTINGS_IDX_PANE_VINYL_BG, SETTINGS_IDX_PANE_VINYL_FG,
      LAYOUT_PANE_VINYL, LAYOUT_COLOUR_BG, -1 },
    { SETTINGS_IDX_THEME_SIDEBAR_FG, SETTINGS_IDX_THEME_SIDEBAR_BG,
      -1, 0, THEME_SLOT_SIDEBAR_FG },
    { SETTINGS_IDX_THEME_SIDEBAR_BG, SETTINGS_IDX_THEME_SIDEBAR_FG,
      -1, 0, THEME_SLOT_SIDEBAR_BG },
    { SETTINGS_IDX_THEME_HIGHLIGHT_FG, SETTINGS_IDX_THEME_HIGHLIGHT_BG,
      -1, 0, THEME_SLOT_HIGHLIGHT_FG },
    { SETTINGS_IDX_THEME_HIGHLIGHT_BG, SETTINGS_IDX_THEME_HIGHLIGHT_FG,
      -1, 0, THEME_SLOT_HIGHLIGHT_BG },
    { SETTINGS_IDX_THEME_BORDER_FG, SETTINGS_IDX_THEME_BORDER_BG,
      -1, 0, THEME_SLOT_BORDER_FG },
    { SETTINGS_IDX_THEME_BORDER_BG, SETTINGS_IDX_THEME_BORDER_FG,
      -1, 0, THEME_SLOT_BORDER_BG },
};

/* 编译期护栏：颜色行数必须等于 12 个窗格字段 + 6 个界面字段。 */
typedef char colour_rows_must_cover_all_colour_options[
    (sizeof(k_colour_rows) / sizeof(k_colour_rows[0]) == 18) ? 1 : -1];

static const ColourRow *colour_row(int option_index)
{
    for (size_t i = 0; i < sizeof(k_colour_rows) / sizeof(k_colour_rows[0]); i++) {
        if (k_colour_rows[i].option == option_index) {
            return &k_colour_rows[i];
        }
    }
    return NULL;
}

/* 该行当前生效的颜色。*from_layout（可为 NULL）= 1 表示由布局显式指定。 */
static int colour_row_value(const ColourRow *row, int *from_layout)
{
    if (from_layout) {
        *from_layout = 0;
    }
    if (!row) {
        return 0;
    }
    if (row->kind < 0) {
        int *slot = theme_color_slot(row->slot);
        return slot ? *slot : 0;
    }
    return ui_layout_pane_colour((LayoutPaneKind)row->kind, row->field, from_layout);
}

/* 颜色弹层：候选值表 + 两个哨兵（都不会与真实颜色值相撞）。 */
#define COLOUR_NO_PAIR     (-9999) /* 没有配对行，无需避让 */
#define COLOUR_MENU_CUSTOM (-9998) /* "自定义…"槽，不是颜色值 */
#define COLOUR_MENU_MAX    34      /* 1 继承 + 1 透明 + 24 调色板 + 1 自定义 */

/* 配对行的生效颜色；没有配对行时返回一个不可能撞色的哨兵值。 */
static int colour_row_paired_value(const ColourRow *row)
{
    const ColourRow *paired = row ? colour_row(row->paired) : NULL;
    if (!paired) {
        return COLOUR_NO_PAIR;
    }
    return colour_row_value(paired, NULL);
}

/* colour_row_commit() 的结果。 */
#define COLOUR_WRITE_LAYOUT    1   /* 已写入当前布局（out_id 给出布局 id） */
#define COLOUR_WRITE_THEME     0   /* 已写入全局主题（config.xml 已持久化） */
#define COLOUR_WRITE_FAILED   (-1) /* 落盘失败，内存与界面均未改动 */
#define COLOUR_WRITE_PANE_OFF (-2) /* 窗格不在当前布局里，改动无法持久化 */
#define COLOUR_WRITE_UNCHANGED (-3) /* 已经是这个状态，无需写入 */

/**
 * @brief 写入一个颜色行，按上面三条规则选择目标层。
 *
 * 两条路径都会调用 apply_color_theme()；主题路径另外 player_config_persist()。
 * 布局路径由 ui_layout_set_pane_colour() 负责"先落盘再改内存"。
 */
static int colour_row_commit(const ColourRow *row, int value,
                             char *out_id, size_t out_id_size,
                             char *err, size_t err_size)
{
    if (out_id && out_id_size) {
        out_id[0] = '\0';
    }
    if (err && err_size) {
        err[0] = '\0';
    }
    if (!row) {
        return COLOUR_WRITE_FAILED;
    }

    if (row->kind >= 0) {
        int from_layout = 0;
        colour_row_value(row, &from_layout);
        /* "跟随主题"只清布局覆盖：主题字段里没有 -2 这个取值，本来就没覆盖
         * 时不需要写任何东西。 */
        int back_to_theme = (value == LAYOUT_COLOR_INHERIT);
        if (back_to_theme && !from_layout) {
            return COLOUR_WRITE_UNCHANGED;
        }
        /* 继承 + 有自有基色：改基色（全局），不产生布局覆盖。 */
        if (!back_to_theme && !from_layout && row->slot >= 0) {
            int *slot = theme_color_slot(row->slot);
            if (!slot) {
                return COLOUR_WRITE_FAILED;
            }
            *slot = value;
            apply_color_theme();
            player_config_persist();
            return COLOUR_WRITE_THEME;
        }
        /* 序列化只写分栏树里的叶子：窗格没启用时写了也会在重载后消失。 */
        if (!ui_layout_pane_enabled((LayoutPaneKind)row->kind)) {
            return COLOUR_WRITE_PANE_OFF;
        }
        if (ui_layout_set_pane_colour((LayoutPaneKind)row->kind, row->field,
                                     value, out_id, out_id_size,
                                     err, err_size) != 0) {
            return COLOUR_WRITE_FAILED;
        }
        apply_color_theme();
        return COLOUR_WRITE_LAYOUT;
    }

    /* 界面色行（侧边栏/高亮/边框）只有主题这一层，没有"跟随主题"可回退。 */
    if (value == LAYOUT_COLOR_INHERIT) {
        return COLOUR_WRITE_UNCHANGED;
    }
    int *slot = theme_color_slot(row->slot);
    if (!slot) {
        return COLOUR_WRITE_FAILED;
    }
    *slot = value;
    apply_color_theme();
    player_config_persist();
    return COLOUR_WRITE_THEME;
}

/**
 * @brief 提交颜色行改动并把结果反映到底部状态栏。
 * @param announce 1 = 成功也提示（弹层选定）；0 = 只提示异常（←/→ 连按不刷屏）。
 */
static void commit_colour_option(int option_index, int value, int announce)
{
    const ColourRow *row = colour_row(option_index);
    if (!row) {
        return;
    }

    char before[LAYOUT_ID_MAX];
    snprintf(before, sizeof(before), "%s", ui_layout_current_id());

    char out_id[LAYOUT_ID_MAX] = "";
    char err[192] = "";
    int rc = colour_row_commit(row, value, out_id, sizeof(out_id),
                               err, sizeof(err));
    char msg[256];

    if (rc == COLOUR_WRITE_UNCHANGED) {
        return;   /* 已经是"跟随主题"，无需写盘也无需提示 */
    }
    if (rc == COLOUR_WRITE_PANE_OFF) {
        show_status_message(i18n_get("settings.color.pane_disabled"));
        return;
    }
    if (rc == COLOUR_WRITE_FAILED) {
        /* 失败原因可能是英文的底层错误（目录满、写入失败），只进日志，
         * 状态栏给一句本地化的短话。 */
        snprintf(msg, sizeof(msg), "%s", i18n_get("settings.color.save_failed"));
        show_status_message(msg);
        log_warn("settings", "Colour write failed (option %d, value %d): %s",
                 option_index, value, err);
        return;
    }
    if (rc == COLOUR_WRITE_LAYOUT && announce) {
        /* 副本路径（来源为 TER_MUSIC_LAYOUTS）会换 id，值得单独说明。 */
        /* 底部状态栏很窄，常规保存只说一句；换了 id（来源 TER_MUSIC_LAYOUTS
         * 的副本路径）才把新 id 报出来。 */
        if (strcmp(out_id, before) == 0) {
            snprintf(msg, sizeof(msg), "%s", i18n_get("settings.color.saved_layout"));
        } else {
            snprintf(msg, sizeof(msg), i18n_get("settings.color.saved_copy"), out_id);
        }
        if (ui_rainbow_active()) {
            char both[sizeof(msg) + 160];
            snprintf(both, sizeof(both), "%s  %s", msg,
                     i18n_get("settings.color.rainbow_pending"));
            snprintf(msg, sizeof(msg), "%s", both);
        }
        show_status_message(msg);
    }
}

static void format_settings_option_line(int option_index, char *line, size_t line_size)
{
    const char **current_settings_options = settings_options;
    const char *separator = i18n_get("settings.separator");
    const char *unset_label = i18n_get("general.not_set");

    if (!line || line_size == 0 || option_index < 0 || option_index >= SETTINGS_OPTION_COUNT) return;

    const ColourRow *colour = colour_row(option_index);
    if (colour) {
        /* 窗格行额外标注生效来源与"该布局没有这个窗格"，界面行不需要标注。 */
        int from_layout = 0;
        int color_val = colour_row_value(colour, &from_layout);
        char mark[64];
        mark[0] = '\0';
        if (colour->kind >= 0) {
            const char *src = i18n_get(from_layout ? "settings.color.from_layout"
                                                   : "settings.color.from_theme");
            if (ui_layout_pane_enabled((LayoutPaneKind)colour->kind)) {
                snprintf(mark, sizeof(mark), " [%s]", src);
            } else {
                snprintf(mark, sizeof(mark), " [%s %s]", src,
                         i18n_get("settings.color.pane_disabled"));
            }
        }
        snprintf(line, line_size, "%s%s%s (%d)%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 menu_color_name(color_val), color_val, mark);
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
    } else if (option_index == SETTINGS_IDX_UI_LAYOUT) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 ui_layout_current_name());
    } else if (option_index == SETTINGS_IDX_LAYOUT_RELOAD ||
               option_index == SETTINGS_IDX_LAYOUT_EDIT ||
               option_index == SETTINGS_IDX_LAYOUT_NEW) {
        /* 动作项：只显示标签本身 */
        snprintf(line, line_size, "%s",
                 i18n_get(current_settings_options[option_index]));
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
    } else if (option_index == SETTINGS_IDX_KARAOKE_MODE) {
        snprintf(line, line_size, "%s%s%s",
                 i18n_get(current_settings_options[option_index]), separator,
                 karaoke_mode_label(g_app_config.karaoke_mode));
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

/**
 * @brief 渲染一个分区的选项列表，放不下时按选中项滑动窗口。
 * @param reserved_bottom 列表下方留给说明文字的行数。
 *
 * 界面外观分区共 22 行（18 个颜色 + 4 个布局项），加上两行说明与底部三条
 * 提示，32 行以下终端就放不下了（24 行终端只剩 14 行列表位），因此这里做
 * 滑动窗口并在保留列画滚动条（与快捷键编辑器、选择弹层同一套做法）。
 * 偏移量直接由选中位置推出，不引入额外状态。
 */
static void render_settings_option_group(int start_y, int max_y,
                                        SettingsSectionSpec spec,
                                        int reserved_bottom)
{
    MenuContentArea area;
    menu_content_area(&area);

    int last_row = max_y - 3 - reserved_bottom;
    int visible = last_row - start_y + 1;
    if (visible < 1) {
        visible = 1;
    }
    if (spec.count <= 0 || start_y > last_row) {
        g_settings_list_top = -1;
        g_settings_list_visible = 0;
        g_settings_list_off = 0;
        return;
    }

    int selected = get_settings_section_position(spec, g_settings_current_option);
    if (selected < 0) {
        selected = 0;
    }
    int off = 0;
    if (selected >= visible) {
        off = selected - visible + 1;
    }
    if (off > spec.count - visible) {
        off = spec.count - visible;
    }
    if (off < 0) {
        off = 0;
    }

    /* 渲染期记下列表几何，鼠标命中直接按这一帧换算（滚动条也是同一份 off） */
    g_settings_list_top = start_y;
    g_settings_list_visible = visible;
    g_settings_list_off = off;

    int shown = 0;
    for (int i = 0; i < visible && off + i < spec.count; i++) {
        int option_index = spec.indices[off + i];
        char line[256];
        format_settings_option_line(option_index, line, sizeof(line));
        int row = start_y + i;
        shown++;
        menu_content_clear_row(&area, row);
        if (option_index == g_settings_current_option && g_focus_area == FOCUS_CONTENT) {
            attron(A_REVERSE);
            menu_content_puts(&area, row, line);
            attroff(A_REVERSE);
        } else {
            menu_content_puts(&area, row, line);
        }
    }

    /* 滚动条列（max_x-2）在内容区之外，menu_content_clear_rows 擦不到，
     * 换到不需要滚动条的分区时会留下上一页的残影；scrollbar_draw 内部只会
     * 清自己那一段，所以这里只在它不画的时候整列擦干净。 */
    if (!scrollbar_draw(stdscr, start_y, shown, spec.count, shown, off,
                        area.max_x - 2)) {
        for (int i = 0; i < visible; i++) {
            mvaddch(start_y + i, area.max_x - 2, ' ');
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
        case SETTINGS_IDX_UI_LAYOUT: {
            int count = ui_layout_count();
            if (count <= 0) {
                break;
            }
            int index = ui_layout_current_index();
            index = (delta < 0) ? (index - 1 + count) % count : (index + 1) % count;
            const char *layout_id = ui_layout_id_at(index);
            if (layout_id && ui_layout_select(layout_id) == 0) {
                show_status_message(i18n_get("settings.layout.saved"));
            } else {
                show_status_message(i18n_get("settings.layout.save_failed"));
            }
            break;
        }
        case SETTINGS_IDX_SHOW_ALBUM_COVER:
            g_app_config.show_album_cover = !g_app_config.show_album_cover;
            player_config_persist();
            break;
        case SETTINGS_IDX_SEAMLESS_PRELOAD:
            g_app_config.seamless_preload = !g_app_config.seamless_preload;
            player_config_persist();
            break;
        case SETTINGS_IDX_ADVANCED_PLAY_MODES:
            g_app_config.advanced_play_modes_enabled = !g_app_config.advanced_play_modes_enabled;
            player_config_persist();
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

/* 弹层打开期间的候选颜色（值，不是下标）；由 colour_menu_build() 填充。 */
static int g_colour_vals[COLOUR_MENU_MAX];
static int g_colour_n = 0;

/**
 * @brief 组装一行颜色的候选表。
 *
 * 窗格行第一项是"跟随主题"（LAYOUT_COLOR_INHERIT），界面行没有这一项；
 * 随后是调色板，与配对行同色的预置项被跳过（沿用原有"前后景不重色"保护）；
 * 最后一项固定为自定义输入槽。调用方把返回值当作 g_sel_count。
 */
static int colour_menu_build(const ColourRow *row)
{
    int n = 0;
    int paired = colour_row_paired_value(row);

    if (row->kind >= 0 && n < COLOUR_MENU_MAX - 1) {
        g_colour_vals[n++] = LAYOUT_COLOR_INHERIT;
    }
    int palette = sel_color_count();
    for (int i = 0; i < palette && n < COLOUR_MENU_MAX - 1; i++) {
        int v = sel_color_val(i);
        if (v == paired && v != -1) {
            continue;
        }
        g_colour_vals[n++] = v;
    }
    g_colour_vals[n++] = COLOUR_MENU_CUSTOM;
    return n;
}

/* 当前生效值在候选表里的位置；不在预置里就落到最后的自定义槽。 */
static int colour_menu_cursor(const ColourRow *row, int n)
{
    int current = colour_row_value(row, NULL);
    for (int i = 0; i < n; i++) {
        if (g_colour_vals[i] == current) {
            return i;
        }
    }
    return n - 1;
}

/* 颜色弹层第 i 项的文本（create_sel_window 与 draw_sel_menu 共用）。 */
static void colour_sel_option_text(const ColourRow *row, int i,
                                   char *out, size_t out_size)
{
    out[0] = '\0';
    if (!row || i < 0 || i >= g_colour_n) {
        return;
    }
    int v = g_colour_vals[i];
    if (v == COLOUR_MENU_CUSTOM) {
        /* 自定义槽：当前值本来就落在某个预置上时不重复显示数值。 */
        int val = colour_row_value(row, NULL);
        int in_pre = 0;
        for (int k = 0; k < sel_color_count(); k++) {
            if (sel_color_val(k) == val) {
                in_pre = 1;
                break;
            }
        }
        if (in_pre || val < 0 || val >= COLORS) {
            snprintf(out, out_size, "[%s...]", i18n_get("general.custom"));
        } else {
            snprintf(out, out_size, "%s: %d", i18n_get("general.custom"), val);
        }
        return;
    }
    if (v == LAYOUT_COLOR_INHERIT) {
        snprintf(out, out_size, "%s", i18n_get("settings.color.follow_theme"));
        return;
    }
    snprintf(out, out_size, "%s (%d)", menu_color_name(v), v);
}

/* ============================================================
 * Selection menu — close / apply
 * ============================================================ */

static void close_sel_menu(int apply)
{
    if (!g_sel_active) return;

    log_debug("settings", "sel menu close apply=%d", apply);

    if (apply) {
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

            case SETTINGS_IDX_KARAOKE_MODE:
                /* 只走门面：本地 = 引擎 + save_config；远端 = Control.SetKaraoke，
                 * 由核心同步它自己的镜像并去抖落盘。这里不另发 Config.Set：
                 * 契约里没有这个键的旧核心会把整包补丁一起拒掉。 */
                player_set_karaoke_mode(g_sel_idx);
                if (player_karaoke_mode() == g_sel_idx) {
                    show_status_message(i18n_get("settings.karaoke.saved"));
                } else {
                    /* 核心没接受（版本过旧或被拒）：如实说明，不谎报已保存 */
                    show_status_message(i18n_get("settings.karaoke.unsupported"));
                }
                break;

            case SETTINGS_IDX_LYRICS_ALIGNMENT:
                g_app_config.lyrics_alignment = g_sel_idx;
                player_config_persist();
                break;

            case SETTINGS_IDX_UI_LAYOUT: {
                const char *layout_id = ui_layout_id_at(g_sel_idx);
                if (layout_id && ui_layout_select(layout_id) == 0) {
                    show_status_message(i18n_get("settings.layout.saved"));
                } else {
                    show_status_message(i18n_get("settings.layout.save_failed"));
                }
                break;
            }

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

            default: {
                /* 颜色行：候选表由 open_sel_menu() 建好，这里按位置取值提交。 */
                const ColourRow *row = colour_row(g_sel_src);
                if (row && g_sel_idx >= 0 && g_sel_idx < g_colour_n &&
                    g_colour_vals[g_sel_idx] != COLOUR_MENU_CUSTOM) {
                    commit_colour_option(g_sel_src, g_colour_vals[g_sel_idx], 1);
                }
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
    g_sel_term_rows = 0;
    g_sel_term_cols = 0;
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
        {
            int step_count = 0;
            const float *steps = player_speed_steps(&step_count);
            count = step_count;
            cur = 0;
            if (steps && step_count > 0) {
                float best = fabsf(g_app_config.default_playback_speed - steps[0]);
                for (int i = 1; i < step_count; i++) {
                    float distance = fabsf(g_app_config.default_playback_speed - steps[i]);
                    if (distance < best) {
                        best = distance;
                        cur = i;
                    }
                }
            }
            break;
        }
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
        case SETTINGS_IDX_KARAOKE_MODE:
            /* 值域与弹层下标一一对应（0/1/2），不需要映射表 */
            count = KARAOKE_MODE_COUNT;
            cur   = g_app_config.karaoke_mode;
            break;
        case SETTINGS_IDX_EQ_PRESET:
            count = EQ_PRESET_COUNT;
            cur   = eq_preset_index_from_config();
            break;
        case SETTINGS_IDX_LYRICS_ALIGNMENT:
            count = 3;
            cur   = g_app_config.lyrics_alignment;
            break;
        case SETTINGS_IDX_UI_LAYOUT:
            count = ui_layout_count();
            cur   = ui_layout_current_index();
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
        default: {
            const ColourRow *row = colour_row(option_index);
            if (row) {
                g_colour_n = colour_menu_build(row);
                count = g_colour_n;
                cur   = colour_menu_cursor(row, g_colour_n);
            }
            break;
        }
    }

    if (count <= 0) return;

    g_sel_active = 1;
    g_sel_src    = option_index;
    g_sel_idx    = cur;
    g_sel_count  = count;

    create_sel_window();
}

/* ============================================================
 * Selection menu — geometry / window
 * ============================================================ */

/**
 * @brief 选择弹层的锚点：触发它的选项在屏幕上的左上角。
 *
 * 行号取渲染期记录的真实列表几何（g_settings_list_top + 可见位置 - 滚动
 * 偏移），列取内容区文字起始列；绝不在这里另抄一份 start_y/off 公式。
 * 均衡器页的预设弹窗不对应列表行，锚定当前选中频带所在列与频带顶行。
 */
static void settings_sel_anchor(int src, int *out_y, int *out_x)
{
    MenuContentArea area;
    menu_content_area(&area);

    int anchor_y = 2;
    int anchor_x = area.content_x;

    if (src == SETTINGS_IDX_EQ_PRESET && g_menu_selected_idx == 6) {
        if (g_eq_bars_top >= 2) {
            anchor_y = g_eq_bars_top;
        }
        const int band = g_settings_current_option - SETTINGS_IDX_EQ_BAND_0;
        if (band >= 0 && band < g_eq_max_bands && g_eq_col_w > 0) {
            anchor_x = g_eq_col0 + band * g_eq_col_w;
        }
        if (anchor_x < area.content_x) {
            anchor_x = area.content_x;
        }
    } else {
        SettingsSectionSpec spec = get_active_settings_section_spec();
        const int pos = get_settings_section_position(spec, src);
        if (g_settings_list_top >= 0) {
            if (pos >= 0) {
                const int line = pos - g_settings_list_off;
                anchor_y = (line >= 0 && line < g_settings_list_visible)
                               ? (g_settings_list_top + line)
                               : g_settings_list_top;
            } else {
                anchor_y = g_settings_list_top;
            }
        }
    }

    if (anchor_y < 2) anchor_y = 2;
    if (anchor_y > area.max_y - 4) anchor_y = area.max_y - 4;
    if (anchor_x < area.content_x) anchor_x = area.content_x;
    if (anchor_x > area.max_x - 3) anchor_x = area.max_x - 3;

    if (out_y) *out_y = anchor_y;
    if (out_x) *out_x = anchor_x;
}

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
                if (i<5) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_CUE_ENCODING:{
                const char *a[]={i18n_get("general.auto"),"UTF-8","GB18030","GBK",
                                 "BIG5","Shift-JIS"};
                if (i<CUE_ENCODING_COUNT) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_LYRICS_ALIGNMENT:{
                const char *a[]={i18n_get("align.left"),i18n_get("align.center"),
                                 i18n_get("align.right")};
                if (i<3) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_KARAOKE_MODE:{
                /* 卡拉OK（消人声）三档：与主页控制窗格弹层同一组文案键 */
                const char *a[]={i18n_get("karaoke.level.off"),
                                 i18n_get("karaoke.level.soft"),
                                 i18n_get("karaoke.level.full")};
                if (i<KARAOKE_MODE_COUNT) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
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
            case SETTINGS_IDX_UI_LAYOUT: {
                const char *name = ui_layout_name_at(i);
                snprintf(opts[i], 48, "%s", name ? name : "");
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
            default: {
                const ColourRow *row = colour_row(src);
                if (row) {
                    colour_sel_option_text(row, i, opts[i], 48);
                }
            }
            break;
        }
        int l = utf8_str_width(opts[i]);
        if (l > max_w) max_w = l;
    }

    MenuContentArea area;
    menu_content_area(&area);

    int box_w = max_w + 4;   /* 1 space + text + 1 space + border */
    if (box_w < 20) box_w = 20;
    if (box_w > 50) box_w = 50;
    if (box_w > area.content_width) box_w = area.content_width;
    if (box_w < 8) {
        log_debug("settings", "sel menu refused: content width=%d", area.content_width);
        g_sel_active = 0;
        g_sel_src = -1;
        return;
    }

    const int max_bottom = max_y - 4;   /* 保留空行 + 状态行 + 功能键提示行 */
    int box_h = n + 2;
    if (box_h > 20) box_h = 20;
    const int avail_h = max_bottom - 2 + 1;
    if (box_h > avail_h) box_h = avail_h;
    if (box_h < 3 || max_bottom < 2 || area.max_x - 3 < area.content_x) {
        log_debug("settings", "sel menu refused: available %dx%d",
                  area.content_width, avail_h);
        g_sel_active = 0;
        g_sel_src = -1;
        return;
    }

    int anchor_y = 2;
    int anchor_x = area.content_x;
    settings_sel_anchor(src, &anchor_y, &anchor_x);

    UIRect rect;
    if (!ui_popup_place_rect(anchor_y, anchor_x, box_w, box_h,
                             2, max_bottom, area.content_x,
                             area.max_x - 3, &rect)) {
        log_debug("settings", "sel menu refused: place failed");
        g_sel_active = 0;
        g_sel_src = -1;
        return;
    }

    g_sel_win = newwin(rect.rows, rect.cols, rect.top, rect.left);
    if (!g_sel_win) {
        g_sel_win = NULL;
        g_sel_active = 0;
        g_sel_src = -1;
        return;
    }

    g_sel_term_rows = max_y;
    g_sel_term_cols = max_x;
    log_debug("settings", "sel menu open src=%d anchor=%d,%d rect=%d,%d,%d,%d",
              src, anchor_y, anchor_x, rect.top, rect.left, rect.rows, rect.cols);
}

/* 流光彩蛋整屏合成用：选择弹层当前是否可见，以及它的窗口。 */
WINDOW *settings_sel_window(void)
{
    return g_sel_active ? g_sel_win : NULL;
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
                if (i<5) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_CUE_ENCODING:{
                const char *a[]={i18n_get("general.auto"),"UTF-8","GB18030","GBK",
                                 "BIG5","Shift-JIS"};
                if (i<CUE_ENCODING_COUNT) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_LYRICS_ALIGNMENT:{
                const char *a[]={i18n_get("align.left"),i18n_get("align.center"),
                                 i18n_get("align.right")};
                if (i<3) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
            }
            case SETTINGS_IDX_KARAOKE_MODE:{
                /* 卡拉OK（消人声）三档：与主页控制窗格弹层同一组文案键 */
                const char *a[]={i18n_get("karaoke.level.off"),
                                 i18n_get("karaoke.level.soft"),
                                 i18n_get("karaoke.level.full")};
                if (i<KARAOKE_MODE_COUNT) {
                    snprintf(opts[i],48,"%s",a[i]);
                }
                break;
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
            case SETTINGS_IDX_UI_LAYOUT: {
                const char *name = ui_layout_name_at(i);
                snprintf(opts[i], 48, "%s", name ? name : "");
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
            default: {
                const ColourRow *row = colour_row(src);
                if (row) {
                    colour_sel_option_text(row, i, opts[i], 48);
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
        case SETTINGS_IDX_KARAOKE_MODE:      title = i18n_get("popup.karaoke"); break;
        case SETTINGS_IDX_EQ_PRESET:        title = i18n_get("popup.eq_preset"); break;
        case SETTINGS_IDX_UI_LAYOUT:         title = i18n_get("settings.opt.ui_layout"); break;
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
            if (colour_row(src))
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

    /* 渲染期记下弹层的行几何：鼠标命中用"画出来的这一帧"，
     * 不在命中判定里再抄一遍 scroll_offset 的推导。 */
    g_sel_visible_opts = visible_opts;
    g_sel_scroll_offset = scroll_offset;
    g_sel_count_shown = n;

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

static int handle_sel_action(ActionId action)
{
    if (!g_sel_active) return 0;

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
            const ColourRow *crow = colour_row(g_sel_src);
            if (crow && g_sel_idx == g_sel_count - 1) {
                char buf[8];
                int cur = colour_row_value(crow, NULL);
                if (cur == LAYOUT_COLOR_INHERIT) {
                    cur = -1;         /* 继承态下提示行按"透明"起步 */
                }
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
                int paired_val = colour_row_paired_value(crow);
                if (v != cur && v >= 0 && v < COLORS && v != paired_val) {
                    commit_colour_option(g_sel_src, v, 1);
                } else if (v == paired_val) {
                    show_status_message(i18n_get("settings.color.same_error"));
                }
                close_sel_menu(0);
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

/* 右键上下文菜单的竞争规则：设置选择弹层展开时右键=取消并吞掉。 */
int settings_cancel_sel_menu(void)
{
    if (!g_sel_active) {
        return 0;
    }
    handle_sel_action(ACTION_SELECT_CANCEL);
    return 1;
}

static int handle_sel_input(int ch)
{
    /* Esc 是系统保留键；其余键位由 select_menu 上下文解析。 */
    ActionId action = (ch == 27) ? ACTION_SELECT_CANCEL
                                 : keymap_resolve(CTX_SELECT_MENU, ch);
    return handle_sel_action(action);
}

static void activate_settings_current_option(void)
{
    if (g_settings_current_option == SETTINGS_IDX_DEFAULT_PATH) {
        edit_default_startup_path();
        return;
    }

    if (g_settings_current_option == SETTINGS_IDX_LAYOUT_EDIT ||
        g_settings_current_option == SETTINGS_IDX_LAYOUT_NEW) {
        LayoutSpec base;
        if (ui_layout_current_spec(&base) != 0) {
            show_status_message(i18n_get("layout.editor.open_failed"));
            return;
        }
        /* 保留预设的本地化名字键：用户没改名就继续跟随语言，
         * 只有编辑器里按 n 改名时才会把它清掉。 */
        int is_new = (g_settings_current_option == SETTINGS_IDX_LAYOUT_NEW);
        layout_editor_open(&base, is_new);
        return;
    }

    if (g_settings_current_option == SETTINGS_IDX_LAYOUT_RELOAD) {
        char previous_id[LAYOUT_ID_MAX];
        snprintf(previous_id, sizeof(previous_id), "%s", ui_layout_current_id());
        int count = ui_layout_reload();
        if (count < 0) {
            show_status_message(i18n_get("settings.layout.reload_failed"));
        } else if (strcmp(previous_id, ui_layout_current_id()) != 0) {
            show_status_message(i18n_get("settings.layout.fallback"));
        } else {
            char msg[160];
            int adjusted = ui_layout_last_adjusted();
            if (adjusted > 0) {
                snprintf(msg, sizeof(msg),
                         i18n_get("settings.layout.reload_ok_adjusted"),
                         count, adjusted);
            } else {
                snprintf(msg, sizeof(msg),
                         i18n_get("settings.layout.reload_ok"), count);
            }
            show_status_message(msg);
        }
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
        g_settings_current_option == SETTINGS_IDX_UI_LAYOUT ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_PLAY_MODE ||
        g_settings_current_option == SETTINGS_IDX_DEFAULT_SPEED ||
        g_settings_current_option == SETTINGS_IDX_LYRICS_ALIGNMENT ||
        g_settings_current_option == SETTINGS_IDX_AUDIO_BACKEND ||
        g_settings_current_option == SETTINGS_IDX_SORT_MODE ||
        g_settings_current_option == SETTINGS_IDX_CUE_ENCODING ||
        g_settings_current_option == SETTINGS_IDX_KARAOKE_MODE ||
        g_settings_current_option == SETTINGS_IDX_EQ_PRESET ||
        g_settings_current_option == SETTINGS_IDX_INFO_PRESET ||
        g_settings_current_option == SETTINGS_IDX_INFO_FIELDS ||
        g_settings_current_option == SETTINGS_IDX_INFO_COVER_SIZE ||
        g_settings_current_option == SETTINGS_IDX_INFO_COVER_CHARSET ||
        g_settings_current_option == SETTINGS_IDX_INFO_PROGRESS_STYLE ||
        g_settings_current_option == SETTINGS_IDX_INFO_LYRICS) {
        open_sel_menu(g_settings_current_option);
        return;
    }

    /* 颜色行：ENTER 打开调色板（窗格行多一项"跟随主题"） */
    if (colour_row(g_settings_current_option)) {
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

    if (total <= 0 || message_row <= list_top) {
        g_hotkey_list_top = -1;
        g_hotkey_list_visible = 0;
        return;
    }

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

    /* 渲染期记下快捷键列表几何，供鼠标命中（g_hotkey_offset 本身就是滚动量） */
    g_hotkey_list_top = list_top;
    g_hotkey_list_visible = visible;

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
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.color.layer_hint"));
        menu_content_puts(&area, start_y + 1, hint_buf);
        start_y += 2;
        /* 三条说明钉在底部（多留一行间隔），列表在剩余空间里滑动。 */
        const int hint_rows = 3;
        render_settings_option_group(start_y, area.max_y, spec, hint_rows + 1);
        int hint_y = area.max_y - 3 - (hint_rows - 1);
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.color.auto_avoid_same"));
        menu_content_puts(&area, hint_y, hint_buf);
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.layout.hint"));
        menu_content_puts(&area, hint_y + 1, hint_buf);
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.layout.edit_hint"));
        menu_content_puts(&area, hint_y + 2, hint_buf);
    } else if (g_menu_selected_idx == 1) {  /* 默认路径 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.path.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec, 0);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.path.tilde_hint"));
        menu_content_puts(&area, start_y, hint_buf);
    } else if (g_menu_selected_idx == 2) {  /* 播放设置 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.playback.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec, 0);
        start_y += spec.count + 1;
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.playback.language"));
        menu_content_puts(&area, start_y, hint_buf);
    } else if (g_menu_selected_idx == 3) {  /* 播放模式 */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.play_mode.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        start_y += 2;
        render_settings_option_group(start_y, area.max_y, spec, 0);
    } else if (g_menu_selected_idx == 4) {  /* 快捷键 */
        render_hotkey_editor(start_y, area.max_y);
    } else if (g_menu_selected_idx == 5) {  /* 远程设备 */
        remote_view_render();
    } else if (g_menu_selected_idx == 6) {  /* 均衡器 */
        render_eq_visual();
        attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    } else if (g_menu_selected_idx == 7) {  /* 信息显示（窗格 + CLI/D-Bus） */
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.info.hint"));
        menu_content_puts(&area, start_y, hint_buf);
        snprintf(hint_buf, sizeof(hint_buf), "%s",
                 i18n_get("settings.info.pane_hint"));
        menu_content_puts(&area, start_y + 1, hint_buf);
        start_y += 3;
        render_settings_option_group(start_y, area.max_y, spec, 0);
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
    repaint_status_message();

    /* 终端缩放后旧窗口坐标会越界：按新尺寸重建并重新锚定/钳制。
     * 新空间不足时 create_sel_window() 会把弹层关闭。 */
    if (g_sel_active) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (rows != g_sel_term_rows || cols != g_sel_term_cols) {
            create_sel_window();
        }
    }

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
    /* 布局编辑器是模态全屏：任何"重绘设置页"的路径都不能把它的首帧盖掉。 */
    if (layout_editor_active()) {
        layout_editor_render();
        return;
    }
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
    g_eq_bars_top = start_y;      /* 频带命中区的第一行（渲染期记录） */
    g_eq_bars_bottom = -1;
    g_eq_col0 = area.content_x;
    g_eq_col_w = EQ_COL_W;
    g_eq_max_bands = max_bands;
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
    g_eq_bars_bottom = start_y;   /* 命中区最后一行＝增益值那一行 */
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

        case ACTION_SETTINGS_EQ_PRESETS:
            /* 键盘 P 与右键菜单共用：均衡器页的预设弹窗。 */
            if (g_focus_area == FOCUS_CONTENT) {
                open_sel_menu(SETTINGS_IDX_EQ_PRESET);
                draw_sel_menu();
                refresh();
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

    /* 布局编辑器激活：全按键拦截；关闭的那次按键也被消费，
     * 关闭后立刻重绘设置界面（编辑器自身的全屏画面被覆盖掉）。 */
    if (layout_editor_active()) {
        layout_editor_handle_key(ch);
        if (!layout_editor_active()) {
            rerender_settings_view();
        }
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
        /* P 属于 CTX_SETTINGS（settings.eq_presets），不在 EQ 上下文里；
         * 这里显式放行，否则提示里承诺的 P:预设 会被下面整段吞掉。 */
        ActionId settings_action = keymap_resolve(CTX_SETTINGS, ch);
        if (settings_action == ACTION_SETTINGS_EQ_PRESETS) {
            handle_eq_action(settings_action);
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

    /* 键盘与鼠标共用同一条动作路径。鼠标**不合成按键码**：用户可以改键，
     * 合成按键在改键后会失灵或做错事。 */
    return handle_settings_action(action);
}

/* 设置页动作入口（CTX_SETTINGS 解析出的动作，或鼠标滚轮/点击映射出的动作）。 */
static int handle_settings_action(ActionId action)
{
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
                if (g_settings_current_option == SETTINGS_IDX_UI_LAYOUT) {
                    adjust_or_toggle_settings_option(SETTINGS_IDX_UI_LAYOUT, -1);
                    rerender_settings_view();
                    return 1;
                }
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
                if (g_settings_current_option == SETTINGS_IDX_UI_LAYOUT) {
                    adjust_or_toggle_settings_option(SETTINGS_IDX_UI_LAYOUT, 1);
                } else {
                    /* RIGHT in content mode behaves like ENTER */
                    activate_settings_current_option();
                }
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

/* ============================================================
 * Settings view mouse handling
 *
 * 归属（在不在弹层里、侧栏列还是内容列）已由 ui/mouse_routing.c 判完，
 * 这里只把 d->row / 屏幕坐标解释成本页的条目，并复用键盘那条动作路径。
 * 竞争态要点：选择弹层展开时事件**不会**走到下面的侧栏/内容分支，
 * 弹层外的点击在上层已经"收起 + 吞掉"，因此不存在"一次点击既关弹层
 * 又改了背后那一项"的情况。
 * ============================================================ */

/* 内容区列表的某一行对应哪个设置项。只读渲染期记下的那一帧几何，
 * 不在这里重抄 start_y / off 的推导（各分区的算法并不一样）。 */
static int settings_option_at_screen_row(int screen_y)
{
    if (g_settings_list_top < 0 || g_settings_list_visible <= 0) return -1;

    SettingsSectionSpec spec = get_active_settings_section_spec();
    if (spec.count <= 0) return -1;

    int line = screen_y - g_settings_list_top;
    if (line < 0 || line >= g_settings_list_visible) return -1;

    int pos = g_settings_list_off + line;
    if (pos < 0 || pos >= spec.count) return -1;
    return spec.indices[pos];
}

/* 快捷键页：行 → 可绑定动作序号 */
static int hotkey_row_at_screen_row(int screen_y)
{
    if (g_hotkey_list_top < 0 || g_hotkey_list_visible <= 0) return -1;

    int line = screen_y - g_hotkey_list_top;
    if (line < 0 || line >= g_hotkey_list_visible) return -1;

    int idx = g_hotkey_offset + line;
    if (idx < 0 || idx >= actions_bindable_count()) return -1;
    return idx;
}

/* 均衡器页：频带按**列**排，命中要看列起点与行范围（都来自 render_eq_visual） */
static int eq_band_at_point(int screen_y, int screen_x)
{
    if (g_eq_bars_top < 0 || g_eq_bars_bottom < g_eq_bars_top) return -1;
    if (screen_y < g_eq_bars_top || screen_y > g_eq_bars_bottom) return -1;
    if (g_eq_max_bands <= 0 || g_eq_col_w <= 0) return -1;

    int rel = screen_x - g_eq_col0;
    if (rel < 0) return -1;

    int band = rel / g_eq_col_w;
    if (band >= g_eq_max_bands) return -1;
    return band;
}

/* 把焦点交给内容列：内容项的高亮只在 g_focus_area==FOCUS_CONTENT 时画得出来，
 * 所以滚轮/点击要让焦点跟着指针走，并重绘侧栏去掉旧高亮。 */
static void settings_focus_content_area(void)
{
    if (g_focus_area == FOCUS_CONTENT) return;
    g_focus_area = FOCUS_CONTENT;
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
}

static void settings_select_sidebar_row(int row)
{
    g_focus_area = FOCUS_SIDEBAR;
    g_menu_selected_idx = row;
    render_menu_sidebar(g_menu_selected_idx, settings_sidebar_items, SETTINGS_ITEM_COUNT);
    render_settings_content();
}

/* 选择弹层（含颜色、EQ 预设、信息显示字段多选）上的鼠标事件。
 * d->row 是弹层窗口本地行：0 是标题行，1.. 是候选项行。 */
static void settings_handle_sel_menu_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->wheel) {
        handle_sel_action(ev->wheel < 0 ? ACTION_SELECT_UP : ACTION_SELECT_DOWN);
        return;
    }
    if (!ev->click) return;

    if (d->row < 0) {
        /* 弹层外：只收起，绝不穿透（穿透会连带改到背后的那一项） */
        handle_sel_action(ACTION_SELECT_CANCEL);
        return;
    }

    const int line = d->row - 1;   /* 候选项从窗口第 1 行起，与 draw_sel_menu 同构 */
    if (line < 0 || line >= g_sel_visible_opts) return;   /* 标题/边框/提示行 */

    const int opt = g_sel_scroll_offset + line;
    if (opt < 0 || opt >= g_sel_count_shown) return;      /* 空行 */

    g_sel_idx = opt;

    /* 单击即执行：单选直接落定；多选等同空格勾选并保持弹层打开。 */
    handle_sel_action(g_sel_multiselect ? ACTION_SELECT_TOGGLE : ACTION_SELECT_ACCEPT);
}

/* ============================================================
 * Right-click context menu
 * ============================================================ */

void settings_context_menu_build(UIContextMenuLevel *level,
                                 const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.settings"));

    const int content = (ctx->area == UI_CTX_AREA_CONTENT);
    const int valid = ctx->valid ? 1 : 0;

    ui_ctx_menu_add_action(level, ACTION_SETTINGS_ACTIVATE, valid, 0);
    if (content) {
        ui_ctx_menu_add_action(level, ACTION_SETTINGS_RIGHT, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_SETTINGS_LEFT,
                               g_focus_area == FOCUS_CONTENT, 0);
    }
    ui_ctx_menu_add_action(level, ACTION_SETTINGS_SAVE, 1, 0);

    if (content && g_menu_selected_idx == 4) {          /* hotkeys */
        ui_ctx_menu_add_separator(level);
        ui_ctx_menu_add_action(level, ACTION_SETTINGS_HOTKEY_RESET,
                               valid && g_focus_area == FOCUS_CONTENT, 0);
        ui_ctx_menu_add_action(level, ACTION_SETTINGS_HOTKEY_RESET_ALL,
                               g_focus_area == FOCUS_CONTENT, 0);
    } else if (content && g_menu_selected_idx == 5) {   /* remote devices */
        ui_ctx_menu_add_separator(level);
        ui_ctx_menu_add_action(level, ACTION_REMOTE_ACTIVATE, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_REMOTE_ADJUST_UP, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_REMOTE_ADJUST_DOWN, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_REMOTE_SAVE,
                               remote_view_form_active() ? 1 : 0, 0);
    } else if (content && g_menu_selected_idx == 6) {   /* equalizer */
        ui_ctx_menu_add_separator(level);
        ui_ctx_menu_add_action(level, ACTION_SETTINGS_EQ_PRESETS, 1, 0);
        ui_ctx_menu_add_action(level, ACTION_EQ_GAIN_UP, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_EQ_GAIN_DOWN, valid, 0);
        ui_ctx_menu_add_action(level, ACTION_EQ_BAND_PREV, 1, 0);
        ui_ctx_menu_add_action(level, ACTION_EQ_BAND_NEXT, 1, 0);
    }

    ui_ctx_menu_add_separator(level);
    ui_context_menu_add_pages_submenu(level);
}

int settings_context_dispatch(ActionId action, const UIContextMenuContext *ctx)
{
    (void)ctx;
    int handled = 0;
    if (g_menu_selected_idx == 6) {
        handled = handle_eq_action(action);
    } else if (g_menu_selected_idx == 4) {
        handled = handle_hotkey_editor_action(action);
    } else if (g_menu_selected_idx == 5) {
        handled = remote_view_handle_action(action);
    }
    if (!handled) {
        handled = handle_settings_action(action);
    }
    return handled;
}

/* 右键：先把指针所在行/分区选中（不执行），再在指针处弹菜单。 */
static void settings_open_context_menu(const UIMouseEvent *ev,
                                       const UIMouseDecision *d)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_SETTINGS;
    ctx.target = d->target;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        ctx.area = UI_CTX_AREA_SIDEBAR;
        if (d->row >= 0 && d->row < SETTINGS_ITEM_COUNT) {
            settings_select_sidebar_row(d->row);
            ctx.valid = 1;
            ctx.index = d->row;
        }
    } else {
        ctx.area = UI_CTX_AREA_CONTENT;
        settings_focus_content_area();

        if (g_menu_selected_idx == 6) {
            int band = eq_band_at_point(ev->y, ev->x);
            if (band >= 0) {
                g_settings_current_option = SETTINGS_IDX_EQ_BAND_0 + band;
                ctx.eq_band = band;
                ctx.valid = 1;
                render_settings_content();
            }
        } else if (g_menu_selected_idx == 4) {
            int idx = hotkey_row_at_screen_row(ev->y);
            if (idx >= 0) {
                g_hotkey_selected = idx;
                g_hotkey_message[0] = '\0';
                ctx.hotkey_index = idx;
                ctx.valid = 1;
                render_settings_content();
            }
        } else if (g_menu_selected_idx == 5) {
            ctx.valid = remote_view_handle_click(ev->y, 0) ? 1 : 0;
        } else {
            int option = settings_option_at_screen_row(ev->y);
            if (option >= 0) {
                g_settings_current_option = option;
                ctx.index = option;
                ctx.valid = 1;
                render_settings_content();
            }
        }
    }

    (void)ui_context_menu_open(settings_context_menu_build,
                               settings_context_dispatch,
                               &ctx, ev->y, ev->x);
}

int settings_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        settings_open_context_menu(ev, d);
        return 1;
    }

    if (d->target == UI_MOUSE_TARGET_SEL_MENU) {
        settings_handle_sel_menu_mouse(ev, d);
        return 1;
    }

    /* 布局编辑器（含它的模态菜单）由分发直接交给 layout_editor_handle_mouse；
     * 走到这里只可能是标志不同步：认领但绝不动设置页。 */
    if (layout_editor_active()) return 1;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (ev->wheel) {
            settings_select_sidebar_row(g_menu_selected_idx);
            return handle_settings_action(ev->wheel < 0 ? ACTION_SETTINGS_UP
                                                        : ACTION_SETTINGS_DOWN);
        }
        if (!ev->click) return 1;

        if (d->row < 0 || d->row >= SETTINGS_ITEM_COUNT) return 1;  /* 框线/空白行 */

        /* 单击即选中并执行：进入分区；最后一项直接返回主界面。 */
        settings_select_sidebar_row(d->row);
        return handle_settings_action(ACTION_SETTINGS_ACTIVATE);
    }

    /* ── 内容区 ── */
    settings_focus_content_area();

    if (ev->wheel) {
        if (g_menu_selected_idx == 6) {
            /* 均衡器：滚轮只在频带之间移动，绝不改增益——
             * 一格滚轮就动 EQ 参数是破坏性的，必须留给明确的按键。 */
            return handle_eq_action(ev->wheel < 0 ? ACTION_EQ_BAND_PREV
                                                  : ACTION_EQ_BAND_NEXT);
        }
        if (g_menu_selected_idx == 4) {
            return handle_hotkey_editor_action(ev->wheel < 0 ? ACTION_SETTINGS_UP
                                                             : ACTION_SETTINGS_DOWN);
        }
        if (g_menu_selected_idx == 5) {
            return remote_view_handle_action(ev->wheel < 0 ? ACTION_REMOTE_UP
                                                           : ACTION_REMOTE_DOWN);
        }
        return handle_settings_action(ev->wheel < 0 ? ACTION_SETTINGS_UP
                                                    : ACTION_SETTINGS_DOWN);
    }
    if (!ev->click) return 1;

    /* 三个特殊分区：行/列→条目各不相同，命中依据都来自它们自己的渲染函数 */
    if (g_menu_selected_idx == 6) {
        int band = eq_band_at_point(ev->y, ev->x);
        if (band < 0) return 1;

        const int option = SETTINGS_IDX_EQ_BAND_0 + band;
        g_settings_current_option = option;
        render_settings_content();
        return handle_eq_action(ACTION_EQ_ACTIVATE);
    }

    if (g_menu_selected_idx == 4) {
        int idx = hotkey_row_at_screen_row(ev->y);
        if (idx < 0) return 1;

        g_hotkey_selected = idx;
        g_hotkey_message[0] = '\0';
        render_settings_content();
        return handle_hotkey_editor_action(ACTION_SETTINGS_ACTIVATE);
    }

    if (g_menu_selected_idx == 5) {
        /* 远程页的选中状态归 remote_view.c（四种模式行号推导各不相同，
         * 目录模式还有滚动偏移），这里只把屏幕行交给它；activate=1 表示
         * 单击即执行（右键菜单路径传 0，只选中）。 */
        remote_view_handle_click(ev->y, 1);
        return 1;
    }

    /* 普通设置分区：行 → 设置项 */
    {
        int option = settings_option_at_screen_row(ev->y);
        if (option < 0) return 1;

        g_settings_current_option = option;
        render_settings_content();
        return handle_settings_action(ACTION_SETTINGS_ACTIVATE);
    }
}
