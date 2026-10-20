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
#include "ui/keymap.h"
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

static int g_help_is_fallback = 0;

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
    g_help_is_fallback = 0;
}

/* Use full locale ID as help file suffix (matches XML lang id) */
static const char *lang_id_to_short(const char *lang_id)
{
    if (!lang_id) return "en_US";
    return lang_id;
}

/* Try to open a help text file; returns FILE* or NULL. */
static FILE *help_try_open(const char *suffix)
{
    char path[MAX_PATH_LEN];

    /* Priority 1: user help dir (~/.config/ter-music/help/) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/ter-music/help/help-quickstart-%s.txt", home, suffix);
        FILE *f = fopen(path, "r");
        if (f) return f;
    }

    /* Priority 2: TER_MUSIC_DATA_DIR */
    snprintf(path, sizeof(path), TER_MUSIC_DATA_DIR "/help/help-quickstart-%s.txt", suffix);
    FILE *f = fopen(path, "r");
    if (f) return f;

    /* Priority 3: /usr/share/ter-music/ */
    snprintf(path, sizeof(path), "/usr/share/ter-music/help/help-quickstart-%s.txt", suffix);
    f = fopen(path, "r");
    if (f) return f;

    /* Priority 4: relative to executable */
    char exe_path[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *dir = strrchr(exe_path, '/');
        if (dir) {
            *dir = '\0';
            snprintf(path, sizeof(path), "%s/../share/ter-music/help/help-quickstart-%s.txt",
                     exe_path, suffix);
            f = fopen(path, "r");
            if (f) return f;
        }
    }

    /* Priority 5: CWD dev path */
    snprintf(path, sizeof(path), "data/help/help-quickstart-%s.txt", suffix);
    f = fopen(path, "r");
    if (f) return f;

    /* Priority 6: source tree (build-dir safe) */
    snprintf(path, sizeof(path), TER_MUSIC_SOURCE_DIR "/data/help/help-quickstart-%s.txt", suffix);
    f = fopen(path, "r");
    if (f) return f;

    return NULL;
}

static void help_load_file(void)
{
    if (g_help_loaded) return;

    help_free_lines();

    const char *lang_id = i18n_current_lang();
    const char *suffix = lang_id_to_short(lang_id);
    FILE *f = NULL;

    /* First try: current language's help */
    f = help_try_open(suffix);

    /* If not found, fall back to English */
    if (!f) {
        f = help_try_open("en_US");
        if (f) {
            g_help_is_fallback = 1;
        }
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

/* 仅在内容区内重绘提示行：清行 + 按显示宽度截断，避免换行与残影。 */
static void help_search_redraw(const MenuContentArea *area, int row,
                               const char *prompt, const char *input)
{
    char line[MAX_META_LEN + 512];
    snprintf(line, sizeof(line), "%s%s", prompt, input);
    menu_content_clear_row(area, row);
    menu_content_puts(area, row, line);

    int prompt_width = (int)utf8_str_width(prompt);
    if (prompt_width > area->content_width) prompt_width = area->content_width;
    int avail = area->content_width - prompt_width;
    if (avail < 1) avail = 1;

    char view[MAX_META_LEN];
    utf8_str_truncate(view, input, avail);

    int cx = area->content_x + prompt_width + (int)utf8_str_width(view);
    int max_cx = area->content_x + area->content_width - 1;
    if (cx > max_cx) cx = max_cx;
    if (cx < area->content_x) cx = area->content_x;
    move(row, cx);
    refresh();
}

static void help_search_prompt(void)
{
    curs_set(1);

    MenuContentArea area;
    menu_content_area(&area);

    int search_row = area.max_y - 3;
    const char *prompt = i18n_get("search.prompt");
    help_search_redraw(&area, search_row, prompt, "");

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
                help_search_redraw(&area, search_row, prompt, input);
            }
        } else if (ch >= 32) {
            size_t len = strlen(input);
            input[len] = (char)ch;
            input[len + 1] = '\0';
            help_search_redraw(&area, search_row, prompt, input);
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

    MenuContentArea area;
    menu_content_area(&area);

    int max_y = area.max_y;
    int max_x = area.max_x;
    int start_y = 2;

    /* Reserve an extra line for fallback notice if active */
    int fallback_banner = g_help_is_fallback ? 1 : 0;
    int visible_lines = max_y - start_y - 6 - fallback_banner;
    if (visible_lines < 1) visible_lines = 1;

    int total_lines = g_help_line_count;

    /* Scroll to search result if active */
    if (g_help_search_active && g_help_search_count > 0 && g_help_search_selected >= 0) {
        int target = g_help_search_results[g_help_search_selected];
        g_help_scroll_offset = target - visible_lines / 2;
        if (g_help_scroll_offset < 0) g_help_scroll_offset = 0;
    }

    int max_offset = total_lines - visible_lines;
    if (max_offset < 0) max_offset = 0;
    if (g_help_scroll_offset > max_offset) g_help_scroll_offset = max_offset;

    int content_row = start_y + 3 + fallback_banner;

    /* 正文区重绘前统一清空，避免末行重叠与旧内容残影 */
    menu_content_clear_rows(&area, content_row, max_y - 4);

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    char buf[512];

    snprintf(buf, sizeof(buf), "%s", i18n_get("menu.help"));
    menu_content_puts(&area, start_y, buf);
    snprintf(buf, sizeof(buf), "========================================");
    menu_content_puts(&area, start_y + 1, buf);

    /* Fallback notice: show when help is from English but current language is not */
    if (fallback_banner) {
        const char *tmpl = i18n_get("help.fallback_notice");
        const char *lname = i18n_current_lang_name();
        char notice[512];
        const char *placeholder = strstr(tmpl, "{name}");
        if (placeholder) {
            size_t pre_len = (size_t)(placeholder - tmpl);
            snprintf(notice, sizeof(notice), "%.*s%s%s",
                     (int)pre_len, tmpl, lname, placeholder + 6);
        } else {
            strncpy(notice, tmpl, sizeof(notice) - 1);
            notice[sizeof(notice) - 1] = '\0';
        }
        attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        menu_content_puts(&area, start_y + 2, notice);
        attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    }

    for (int i = 0; i < visible_lines && (g_help_scroll_offset + i) < total_lines; i++) {
        int line_idx = g_help_scroll_offset + i;
        int row = content_row + i;
        if (row > max_y - 4) break;

        int is_match = 0;
        if (g_help_search_active && g_help_search_count > 0) {
            for (int j = 0; j < g_help_search_count; j++) {
                if (g_help_search_results[j] == line_idx) {
                    is_match = 1;
                    break;
                }
            }
        }

        const char *line = g_help_lines[line_idx];
        if (!line) line = "";

        if (is_match) attron(A_BOLD);
        menu_content_puts(&area, row, line);
        if (is_match) attroff(A_BOLD);
    }

    /* Bottom hint bar */
    int hint_row = max_y - 3;
    menu_content_clear_row(&area, hint_row);

    char hint[512];
    if (g_help_search_active && g_help_search_count > 0) {
        snprintf(hint, sizeof(hint),
                 i18n_get("help.search_matches"),
                 g_help_search_count);
    } else {
        snprintf(hint, sizeof(hint), "%s",
                 i18n_get("help.scroll_hint"));
    }

    int display_end = g_help_scroll_offset + visible_lines;
    if (display_end > total_lines) display_end = total_lines;

    char pos[64];
    snprintf(pos, sizeof(pos), "%s %d-%d / %d",
             i18n_get("help.line"),
             g_help_scroll_offset + 1, display_end, total_lines);

    /* 右侧行号：按显示宽度定位，并与内容区右边界对齐裁剪 */
    int pos_width = (int)utf8_str_width(pos);
    int pos_x = max_x - 1 - 1 - pos_width;
    if (pos_x < area.content_x + 20) pos_x = area.content_x + 20;
    int pos_max_x = area.content_x + area.content_width;
    if (pos_x > pos_max_x) pos_x = pos_max_x;

    /* 左侧提示截断到行号之前的空隙，避免重叠 */
    int hint_cols = pos_x - area.content_x - 2;
    if (hint_cols < 0) hint_cols = 0;
    char hint_buf[512];
    utf8_str_truncate(hint_buf, hint, hint_cols);
    menu_content_puts(&area, hint_row, hint_buf);

    if (pos_max_x > pos_x) {
        char pos_buf[64];
        utf8_str_truncate(pos_buf, pos, pos_max_x - pos_x);
        mvaddstr(hint_row, pos_x, pos_buf);
    }

    /* Draw scrollbar */
    scrollbar_draw(stdscr, content_row, visible_lines,
                   total_lines, visible_lines, g_help_scroll_offset, max_x - 2);

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

/* ============================================================
 * Help view input handling
 * ============================================================ */

int handle_help_input(int ch)
{
    ActionId action = keymap_resolve(CTX_HELP, ch);

    switch (action) {
        case ACTION_HELP_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
                render_help_content();
            }
            return 1;

        case ACTION_HELP_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == HELP_ITEM_COUNT - 1) {
                    exit_current_view();
                } else {
                    g_focus_area = FOCUS_CONTENT;
                    render_menu_sidebar(g_menu_selected_idx, help_sidebar_items, HELP_ITEM_COUNT);
                    render_help_content();
                }
            }
            return 1;

        case ACTION_HELP_UP:
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
            return 1;

        case ACTION_HELP_DOWN:
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
            return 1;

        case ACTION_HELP_SEARCH:
            help_search_prompt();
            return 1;

        case ACTION_HELP_NEXT_MATCH:
            if (g_help_search_active && g_help_search_count > 0) {
                g_help_search_selected++;
                if (g_help_search_selected >= g_help_search_count)
                    g_help_search_selected = 0;
                render_help_content();
            }
            return 1;

        default:
            return 0;
    }
}
