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
#include "i18n/i18n.h"
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
    "sidebar.settings.theme",
    "sidebar.settings.default_path",
    "sidebar.settings.playback",
    "sidebar.settings.play_mode",
    "sidebar.settings.hotkeys",
    "sidebar.settings.remote_devices",
    "sidebar.settings.equalizer",
    "general.back"
};
const int SETTINGS_ITEM_COUNT = 8;

const char *history_sidebar_items[] = {
    "sidebar.history.folder_history",
    "sidebar.history.clear_history",
    "general.back"
};
const int HISTORY_ITEM_COUNT = 3;

const char *playlist_sidebar_items[] = {
    "sidebar.playlist.all_playlists",
    "sidebar.playlist.new_playlist",
    "general.back"
};
const int PLAYLIST_ITEM_COUNT = 3;

const char *favorites_sidebar_items[] = {
    "sidebar.favorites.all_favorites",
    "general.back"
};
const int FAVORITES_ITEM_COUNT = 2;

const char *info_sidebar_items[] = {
    "sidebar.info.about",
    "sidebar.info.repository",
    "general.back"
};
const int INFO_ITEM_COUNT = 3;

const char *help_sidebar_items[] = {
    "sidebar.help.quick_start",
    "general.back"
};
const int HELP_ITEM_COUNT = 2;

/* Language view sidebar */
const char *language_sidebar_items[] = {
    "general.back"
};
const int LANGUAGE_ITEM_COUNT = 1;

/* ── Color names (for settings display) ── */

const char *color_names[] = {
    "color.black", "color.red", "color.green", "color.yellow",
    "color.blue", "color.magenta", "color.cyan", "color.white",
    "color.bright_black", "color.bright_red", "color.bright_green", "color.bright_yellow",
    "color.bright_blue", "color.bright_magenta", "color.bright_cyan", "color.bright_white"
};

const int ncurses_colors[] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
};

/* ============================================================
 * Text helper functions
 * ============================================================ */

const char *menu_text(const char *key, const char *unused)
{
    (void)unused;
    return i18n_get(key);
}

const char *menu_bool_text(int enabled)
{
    return enabled ? i18n_get("general.yes") : i18n_get("general.no");
}

const char *menu_color_name(int color_value)
{
    if (color_value == -1)
        return i18n_get("general.transparent");
    if (color_value >= 0 && color_value < 16)
        return i18n_get(color_names[color_value]);
    switch (color_value) {
        case 208: return i18n_get("color.orange");
        case 130: return i18n_get("color.brown");
        case 198: return i18n_get("color.pink");
        case 93:  return i18n_get("color.purple");
        case 37:  return i18n_get("color.teal");
        case 75:  return i18n_get("color.sky_blue");
        case 203: return i18n_get("color.coral");
        case 118: return i18n_get("color.lime");
    }
    return i18n_get("general.unknown");
}

const char *menu_language_name(void)
{
    return i18n_get("lang.name");
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
    static char resolved_bufs[16][64];
    static const char *resolved_ptrs[16];
    int count = 0;

    if (items == settings_sidebar_items) count = SETTINGS_ITEM_COUNT;
    else if (items == history_sidebar_items) count = HISTORY_ITEM_COUNT;
    else if (items == playlist_sidebar_items) count = PLAYLIST_ITEM_COUNT;
    else if (items == favorites_sidebar_items) count = FAVORITES_ITEM_COUNT;
    else if (items == info_sidebar_items) count = INFO_ITEM_COUNT;
    else if (items == help_sidebar_items) count = HELP_ITEM_COUNT;
    else return items;

    for (int i = 0; i < count && i < 16; i++) {
        const char *resolved = i18n_get(items[i]);
        strncpy(resolved_bufs[i], resolved, sizeof(resolved_bufs[i]) - 1);
        resolved_bufs[i][sizeof(resolved_bufs[i]) - 1] = '\0';
        resolved_ptrs[i] = resolved_bufs[i];
    }
    return resolved_ptrs;
}

const char *resolve_menu_title(const char *title)
{
    if (!title) return title;
    if (strcmp(title, "设置 [F2]") == 0) return i18n_get("menu.settings");
    if (strcmp(title, "历史 [F3]") == 0) return i18n_get("menu.history");
    if (strcmp(title, "歌单 [F4]") == 0) return i18n_get("menu.playlists");
    if (strcmp(title, "收藏 [F5]") == 0) return i18n_get("menu.favorites");
    if (strcmp(title, "信息 [F6]") == 0) return i18n_get("menu.info");
    if (strcmp(title, "帮助 [F8]") == 0) return i18n_get("menu.help");
    if (strcmp(title, "Help [F8]") == 0) return i18n_get("menu.help");
    return i18n_get(title);
}
