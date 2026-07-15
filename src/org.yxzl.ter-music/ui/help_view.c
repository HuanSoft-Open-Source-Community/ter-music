/**
 * @file help_view.c
 * @brief 帮助视图 — 快速上手指南的加载、显示和搜索
 *
 * 从 menus.c 拆分而来，负责帮助页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/scrollbar.h"
#include "search/search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <unistd.h>

/* ============================================================
 * Help internal state
 * ============================================================ */

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

/* ============================================================
 * Help file loading
 * ============================================================ */

void reset_help_view(void)
{
    g_help_scroll_offset = 0;
    g_help_search_active = 0;
    g_help_search_count = 0;
    g_help_search_selected = -1;
}

void help_free_lines(void)
{
    for (int i = 0; i < g_help_line_count; i++) {
        free(g_help_lines[i]);
        g_help_lines[i] = NULL;
    }
    g_help_line_count = 0;
    g_help_loaded = 0;
}

static void help_load_file(void)
{
    if (g_help_loaded) return;

    help_free_lines();

    const char *suffix = i18n_current_lang();
    char path[MAX_PATH_LEN];
    FILE *f = NULL;

    snprintf(path, sizeof(path), TER_MUSIC_DATA_DIR "/help-quickstart-%s.txt", suffix);
    f = fopen(path, "r");

    if (!f) {
        snprintf(path, sizeof(path), "/usr/share/ter-music/help-quickstart-%s.txt", suffix);
        f = fopen(path, "r");
    }

    if (!f) {
        char exe_path[MAX_PATH_LEN];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            char *dir = strrchr(exe_path, '/');
            if (dir) {
                *dir = '\0';
                snprintf(path, sizeof(path), "%s/../share/ter-music/help-quickstart-%s.txt",
                         exe_path, suffix);
                f = fopen(path, "r");
            }
        }
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

/* ============================================================
 * Help search
 * ============================================================ */

static void help_search_prompt(void)
{
    curs_set(1);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int search_row = max_y - 3;

    mvprintw(search_row, content_start_x, "%s",
             i18n_get("search.prompt"));
    int input_start_x = getcurx(stdscr);
    clrtoeol();
    refresh();

    char input[MAX_META_LEN];
    memset(input, 0, sizeof(input));
    int ch;

    flushinp();

    while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != 27 && strlen(input) < MAX_META_LEN - 1) {
        if (ch == ERR) continue;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (strlen(input) > 0) {
                size_t len = strlen(input) - 1;
                while (len > 0 && ((unsigned char)input[len] & 0xC0) == 0x80)
                    len--;
                input[len] = '\0';
                move(search_row, input_start_x);
                clrtoeol();
                printw("%s", input);
                refresh();
            }
        } else if (ch >= 32) {
            size_t len = strlen(input);
            input[len] = (char)ch;
            input[len + 1] = '\0';
            move(search_row, input_start_x);
            clrtoeol();
            printw("%s", input);
            refresh();
        }
    }

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

/* ============================================================
 * Help view rendering
 * ============================================================ */

void render_help_content(void)
{
    help_load_file();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    int visible_lines = max_y - start_y - 5;
    if (visible_lines < 1) visible_lines = 1;

    int total_lines = g_help_line_count;

    /* Scroll to search result if active */
    if (g_help_search_active && g_help_search_count > 0 && g_help_search_selected >= 0) {
        int target = g_help_search_results[g_help_search_selected];
        g_help_scroll_offset = target - visible_lines / 2;
        if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
    }

    int max_offset = total_lines - visible_lines - 3;
    if (max_offset < 0) max_offset = 0;
    if (g_help_scroll_offset > max_offset) g_help_scroll_offset = max_offset;

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    mvprintw(start_y, content_start_x, "%s",
             i18n_get("menu.help"));
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

    /* Bottom hint bar */
    int hint_row = max_y - 3;
    mvhline(hint_row, content_start_x, ' ', max_x - content_start_x - 1);

    char hint[256];
    if (g_help_search_active && g_help_search_count > 0) {
        snprintf(hint, sizeof(hint),
                 use_english_ui()
                     ? "Search: %d matches | n/N: next | Left: sidebar"
                     : "搜索: %d 个匹配 | n/N: 下一个 | 左键: 侧栏",
                 g_help_search_count);
    } else {
        snprintf(hint, sizeof(hint),
                 use_english_ui()
                     ? "Up/Down: scroll 3 lines  |  /: search  |  Left: sidebar"
                     : "上/下: 滚动3行  |  /: 搜索  |  左键: 侧栏");
    }

    int display_end = g_help_scroll_offset + visible_lines;
    if (display_end > total_lines) display_end = total_lines;

    char pos[64];
    snprintf(pos, sizeof(pos), "%s %d-%d / %d",
             i18n_get("help.line"),
             g_help_scroll_offset + 1, display_end, total_lines);

    mvprintw(hint_row, content_start_x, "%s", hint);
    mvprintw(hint_row, max_x - (int)strlen(pos) - 3, "%s", pos);

    /* Draw scrollbar */
    scrollbar_draw(stdscr, start_y + 3, visible_lines,
                   total_lines, visible_lines, g_help_scroll_offset, max_x - 2);

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

/* ============================================================
 * Help view input handling
 * ============================================================ */

void handle_help_input(int ch)
{
    switch (ch) {
        case KEY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
                render_help_content();
            }
            break;

        case KEY_RIGHT:
        case '\n':
        case KEY_ENTER:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == HELP_ITEM_COUNT - 1) {
                    exit_current_view();
                } else {
                    g_focus_area = FOCUS_CONTENT;
                    render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
                    render_help_content();
                }
            }
            break;

        case KEY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx > 0)
                    g_menu_selected_idx--;
                else
                    g_menu_selected_idx = HELP_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            } else if (g_help_search_active && g_help_search_count > 0) {
                if (g_help_search_selected > 0)
                    g_help_search_selected--;
                else
                    g_help_search_selected = g_help_search_count - 1;
            } else {
                g_help_scroll_offset -= 3;
                if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
            }
            render_help_content();
            break;

        case KEY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= HELP_ITEM_COUNT)
                    g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
            } else if (g_help_search_active && g_help_search_count > 0) {
                g_help_search_selected++;
                if (g_help_search_selected >= g_help_search_count)
                    g_help_search_selected = 0;
            } else {
                g_help_scroll_offset += 3;
            }
            render_help_content();
            break;

        case '/':
            help_search_prompt();
            break;

        case 'n':
        case 'N':
            if (g_help_search_active && g_help_search_count > 0) {
                g_help_search_selected++;
                if (g_help_search_selected >= g_help_search_count)
                    g_help_search_selected = 0;
                render_help_content();
            }
            break;
    }
}
