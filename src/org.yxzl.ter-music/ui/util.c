/**
 * @file util.c
 * @brief 菜单模块共享工具函数、全局变量和侧边栏数据
 *
 * 存放各视图模块共享的全局变量、侧边栏项目数组和文本辅助函数。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "config/config.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <ctype.h>
#include <time.h>
#include <stddef.h>

/* ============================================================
 * Global variable definitions
 *
 * Some have extern declarations in ui.h, config.h, playlist.h,
 * and menu_internal.h — these are the canonical definitions.
 * ============================================================ */

ViewMode g_current_view = VIEW_MAIN;
int g_menu_selected_idx = 0;
PlayHistory g_play_history = {0};
Favorites g_favorites = {0};
DirHistory g_dir_history = {0};
PlaylistManager g_playlist_manager = {0};
AppConfig g_app_config = {0};
int g_content_selected_idx = 0;
FocusArea g_focus_area = FOCUS_SIDEBAR;

/* ── Content offset / scroll state (declared extern in menu_internal.h) ── */
int g_history_content_offset = 0;
int g_favorites_content_offset = 0;
int g_playlist_content_offset = 0;

/* ── Status message state (accessed via show_status_message / getters) ── */
static char g_status_message[256] = "";
static time_t g_status_message_time = 0;

void show_status_message(const char *msg)
{
    if (use_ascii_fallback_ui()) {
        sanitize_ascii_menu_text(g_status_message, sizeof(g_status_message), msg);
    } else {
        strncpy(g_status_message, msg, sizeof(g_status_message) - 1);
        g_status_message[sizeof(g_status_message) - 1] = '\0';
    }
    g_status_message_time = time(NULL);
}

const char *get_status_message(void)
{
    return g_status_message;
}

time_t get_status_message_time(void)
{
    return g_status_message_time;
}

/* ============================================================
 * rounded_box — draw a rounded-corner border using Unicode
 * box-drawing characters (╭ ╮ ╰ ╯ ─ │)
 * ============================================================ */
void rounded_box(WINDOW *win)
{
    int h, w;
    getmaxyx(win, h, w);
    if (h < 2 || w < 2) return;

    /* corners */
    mvwaddstr(win, 0, 0, "\xe2\x95\xad");          /* ╭ U+256D */
    mvwaddstr(win, 0, w - 1, "\xe2\x95\xae");      /* ╮ U+256E */
    mvwaddstr(win, h - 1, 0, "\xe2\x95\xb0");      /* ╰ U+2570 */
    mvwaddstr(win, h - 1, w - 1, "\xe2\x95\xaf");  /* ╯ U+256F */

    /* horizontal lines */
    for (int x = 1; x < w - 1; x++) {
        mvwaddstr(win, 0, x, "\xe2\x94\x80");       /* ─ U+2500 */
        mvwaddstr(win, h - 1, x, "\xe2\x94\x80");   /* ─ U+2500 */
    }

    /* vertical lines */
    for (int y = 1; y < h - 1; y++) {
        mvwaddstr(win, y, 0, "\xe2\x94\x82");       /* │ U+2502 */
        mvwaddstr(win, y, w - 1, "\xe2\x94\x82");   /* │ U+2502 */
    }
}

/* ============================================================
 * Sidebar item arrays
 *
 * Declared extern in menu_internal.h so all view modules can
 * reference them.
 * ============================================================ */

const char *settings_sidebar_items[] = {
    "颜色主题",
    "默认路径",
    "播放设置",
    "播放模式",
    "快捷键",
    "远程设备",
    "均衡器",    /* ← 均衡器 */
    "← 返回"    /* ← 返回 */
};
const int SETTINGS_ITEM_COUNT = 8;

const char *history_sidebar_items[] = {
    "目录历史",
    "清空历史",
    "← 返回"
};
const int HISTORY_ITEM_COUNT = 3;

const char *playlist_sidebar_items[] = {
    "全部歌单",
    "新建歌单",
    "← 返回"
};
const int PLAYLIST_ITEM_COUNT = 3;

const char *favorites_sidebar_items[] = {
    "全部收藏",
    "← 返回"
};
const int FAVORITES_ITEM_COUNT = 2;

const char *info_sidebar_items[] = {
    "关于",
    "仓库地址",
    "← 返回"
};
const int INFO_ITEM_COUNT = 3;

const char *help_sidebar_items[] = {
    "快速上手",
    "← 返回"
};
const int HELP_ITEM_COUNT = 2;

/* ── ASCII (English) sidebar items ── */

const char *settings_sidebar_items_ascii[] = {
    "Theme",
    "Default Path",
    "Playback",
    "Play Mode",
    "Hotkeys",
    "Remote Devices",
    "Equalizer",
    "<- Back"
};

const char *history_sidebar_items_ascii[] = {
    "Folder History",
    "Clear History",
    "<- Back"
};

const char *playlist_sidebar_items_ascii[] = {
    "All Playlists",
    "New Playlist",
    "<- Back"
};

const char *favorites_sidebar_items_ascii[] = {
    "All Favorites",
    "<- Back"
};

const char *info_sidebar_items_ascii[] = {
    "About",
    "Repository",
    "<- Back"
};

const char *help_sidebar_items_ascii[] = {
    "Quick Start",
    "<- Back"
};

/* ── Color names (for settings display) ── */

const char *color_names[] = {
    "黑色", "红色", "绿色", "黄色",
    "蓝色", "洋红", "青色", "白色",
    "亮黑(灰)", "亮红", "亮绿", "亮黄",
    "亮蓝", "亮洋红", "亮青", "亮白"
};

const char *color_names_ascii[] = {
    "Black", "Red", "Green", "Yellow",
    "Blue", "Magenta", "Cyan", "White",
    "Bright Black", "Bright Red", "Bright Green", "Bright Yellow",
    "Bright Blue", "Bright Magenta", "Bright Cyan", "Bright White"
};

const int ncurses_colors[] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
};

/* ============================================================
 * Text helper functions
 * ============================================================ */

const char *menu_text(const char *utf8, const char *ascii)
{
    return use_english_ui() ? ascii : utf8;
}

const char *menu_bool_text(int enabled)
{
    return enabled ? menu_text("是", "On") : menu_text("否", "Off");
}

const char *menu_color_name(int color_value)
{
    const char **names = use_english_ui() ? color_names_ascii : color_names;
    if (color_value == -1)
        return menu_text("透明", "Transparent");
    if (color_value >= 0 && color_value < 16)
        return names[color_value];
    switch (color_value) {
        case 208: return menu_text("橙",   "Orange");
        case 130: return menu_text("棕",   "Brown");
        case 198: return menu_text("粉",   "Pink");
        case 93:  return menu_text("紫",   "Purple");
        case 37:  return menu_text("青绿", "Teal");
        case 75:  return menu_text("天蓝", "Sky Blue");
        case 203: return menu_text("珊瑚", "Coral");
        case 118: return menu_text("石灰", "Lime");
    }
    return menu_text("未知", "Unknown");
}

const char *menu_language_name(int language)
{
    return language == UI_LANG_EN ? "English" : menu_text("中文", "Chinese");
}

void sanitize_ascii_menu_text(char *dest, size_t dest_size, const char *src)
{
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

const char **resolve_sidebar_items(const char **items)
{
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

const char *resolve_menu_title(const char *title)
{
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
