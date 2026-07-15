/**
 * @file language_view.c
 * @brief Language management view — list, switch, add, delete language packs
 *
 * @author ter-music team
 * @date 2026-07
 */

#include "types.h"
#include "i18n/i18n.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/scrollbar.h"
#include "config/config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>

/* ── Internal state ──────────────────────────────────────────── */

static int g_lang_selected = 0;
static int g_lang_scroll = 0;
static int g_lang_count = 0;
static I18nLanguage g_langs[I18N_MAX_LANGS];

/* ── Helpers ─────────────────────────────────────────────────── */

static void lang_refresh_list(void)
{
    g_lang_count = i18n_available_languages(g_langs, I18N_MAX_LANGS);
    if (g_lang_selected >= g_lang_count && g_lang_count > 0)
        g_lang_selected = g_lang_count - 1;
}

void reset_language_view(void)
{
    g_lang_selected = 0;
    g_lang_scroll = 0;
    lang_refresh_list();
}

/* ── Prompt for file path (inline input) ─────────────────────── */

static int lang_prompt_path(char *buf, size_t size)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int x = menu_width + 2;
    int y = max_y - 4;

    curs_set(1);
    mvprintw(y, x, "%s", i18n_get("lang_mgr.add_prompt"));
    clrtoeol();
    int input_x = getcurx(stdscr);
    refresh();

    buf[0] = '\0';
    size_t pos = 0;
    int ch;
    flushinp();

    while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != 27 && pos < size - 1) {
        if (ch == ERR) continue;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                size_t back = 1;
                while (pos > back && ((unsigned char)buf[pos - back] & 0xC0) == 0x80)
                    back++;
                pos -= back;
                buf[pos] = '\0';
                move(y, input_x);
                clrtoeol();
                printw("%s", buf);
                refresh();
            }
        } else if (ch >= 32) {
            buf[pos++] = (char)ch;
            buf[pos] = '\0';
            move(y, input_x);
            clrtoeol();
            printw("%s", buf);
            refresh();
        }
    }

    curs_set(0);

    /* Clean up prompt line */
    int cw = max_x / 4;
    mvhline(y, cw + 2, ' ', max_x - cw - 3);

    if (ch == 27) {
        buf[0] = '\0';
        return 0;
    }
    return 1;
}

/* ── Rendering ───────────────────────────────────────────────── */

void render_language_content(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    lang_refresh_list();

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    mvprintw(start_y, content_start_x, "%s", i18n_get("lang_mgr.title"));
    mvhline(start_y + 1, content_start_x, '=', max_x - content_start_x - 1);
    start_y += 3;

    const char *cur_lang = i18n_current_lang();
    int visible = max_y - start_y - 6;
    if (visible < 3) visible = 3;

    if (g_lang_count == 0) {
        mvprintw(start_y, content_start_x, "  %s", i18n_get("lang_mgr.no_langs"));
        attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
        refresh();
        return;
    }

    if (g_lang_selected < g_lang_scroll)
        g_lang_scroll = g_lang_selected;
    if (g_lang_selected >= g_lang_scroll + visible)
        g_lang_scroll = g_lang_selected - visible + 1;
    if (g_lang_scroll < 0) g_lang_scroll = 0;

    for (int i = 0; i < visible && (g_lang_scroll + i) < g_lang_count; i++) {
        int idx = g_lang_scroll + i;
        int row = start_y + i;

        mvhline(row, content_start_x, ' ', max_x - content_start_x - 1);

        int is_current = (strcmp(g_langs[idx].id, cur_lang) == 0);

        if (idx == g_lang_selected) attron(A_REVERSE);

        char marker[4] = "  ";
        if (is_current) strcpy(marker, " *");

        const char *tag = g_langs[idx].builtin
            ? i18n_get("lang_mgr.builtin")
            : i18n_get("lang_mgr.user");
        double cov = i18n_coverage(g_langs[idx].id);
        char cov_buf[12];
        if (cov >= 0.0)
            snprintf(cov_buf, sizeof(cov_buf), " %7.3f%%", cov);
        else
            cov_buf[0] = '\0';
        int cov_width = 9;  /* fixed: " 100.000%" */
        int avail = max_x - content_start_x - 2 - 1 - (int)utf8_str_width(tag) - 2 - cov_width - 3;
        if (avail < 6) avail = 6;
        char name_buf[128];
        utf8_str_truncate(name_buf, g_langs[idx].name, avail);
        char padded_name[256];
        utf8_str_pad(padded_name, sizeof(padded_name), name_buf, avail);

        mvprintw(row, content_start_x, "%s %s  %s%s",
                 marker, padded_name, tag, cov_buf);

        if (idx == g_lang_selected) attroff(A_REVERSE);
    }

    /* Draw scrollbar */
    scrollbar_draw(stdscr, start_y, visible,
                   g_lang_count, visible, g_lang_scroll, max_x - 2);

    /* Hint bar */
    int hint_row = max_y - 3;
    mvhline(hint_row, content_start_x, ' ', max_x - content_start_x - 1);
    mvprintw(hint_row, content_start_x, "%s", i18n_get("lang_mgr.hint"));

    /* Action bar */
    int act_row = max_y - 2;
    mvhline(act_row, content_start_x, ' ', max_x - content_start_x - 1);
    attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    mvprintw(act_row, content_start_x, "%s", i18n_get("lang_mgr.actions"));
    attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

/* ── Input handling ──────────────────────────────────────────── */

void handle_language_input(int ch)
{
    switch (ch) {
        case KEY_UP:
            if (g_lang_selected > 0)
                g_lang_selected--;
            else
                g_lang_selected = g_lang_count > 0 ? g_lang_count - 1 : 0;
            break;

        case KEY_DOWN:
            if (g_lang_count > 0) {
                g_lang_selected++;
                if (g_lang_selected >= g_lang_count)
                    g_lang_selected = 0;
            }
            break;

        case '\n':
        case ' ':
        case KEY_ENTER: {
            if (g_lang_count == 0) break;
            const char *sel = g_langs[g_lang_selected].id;
            const char *cur = i18n_current_lang();
            if (strcmp(sel, cur) != 0) {
                log_debug("lang_view", "Activate language: %s -> %s", cur, sel);
                if (i18n_reload(sel) == 0) {
                    strncpy(g_app_config.ui_language, sel,
                            sizeof(g_app_config.ui_language) - 1);
                    g_app_config.ui_language[sizeof(g_app_config.ui_language) - 1] = '\0';
                    save_config();
                    help_free_lines();
                    show_status_message(i18n_get("lang_mgr.switched"));
                }
            }
            break;
        }

        case 'a':
        case 'A': {
            char path[MAX_PATH_LEN];
            if (lang_prompt_path(path, sizeof(path)) && path[0]) {
                log_debug("lang_view", "Add language request: path=%s", path);
                if (i18n_add_language(path) == 0) {
                    lang_refresh_list();
                    show_status_message(i18n_get("lang_mgr.added"));
                } else {
                    show_status_message(i18n_get("lang_mgr.add_failed"));
                }
            }
            break;
        }

        case 'd':
        case 'D': {
            if (g_lang_count == 0) break;
            const char *sel = g_langs[g_lang_selected].id;
            if (i18n_is_builtin(sel)) {
                show_status_message(i18n_get("lang_mgr.cannot_delete_builtin"));
            } else {
                log_debug("lang_view", "Delete language request: %s", sel);
                if (i18n_delete_language(sel) == 0) {
                    if (strcmp(sel, i18n_current_lang()) == 0) {
                        i18n_reload("zh_CN");
                        strcpy(g_app_config.ui_language, "zh_CN");
                        save_config();
                        help_free_lines();
                    }
                    lang_refresh_list();
                    show_status_message(i18n_get("lang_mgr.deleted"));
                } else {
                    show_status_message(i18n_get("lang_mgr.delete_failed"));
                }
            }
            break;
        }

        case 27:
        case KEY_LEFT:
            exit_current_view();
            return;
    }

    render_language_content();
}
