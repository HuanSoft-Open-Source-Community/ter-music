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
#include "ui/keymap.h"
#include "ui/scrollbar.h"
#include "config/config.h"
#include "player/player.h"
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

/* 仅在内容区内重绘提示行：清行 + 按显示宽度截断，避免换行与残影。 */
static void lang_prompt_redraw(const MenuContentArea *area, int row,
                               const char *prompt, const char *input)
{
    char line[MAX_PATH_LEN + 512];
    snprintf(line, sizeof(line), "%s%s", prompt, input);
    menu_content_clear_row(area, row);
    menu_content_puts(area, row, line);

    int prompt_width = (int)utf8_str_width(prompt);
    if (prompt_width > area->content_width) prompt_width = area->content_width;
    int avail = area->content_width - prompt_width;
    if (avail < 1) avail = 1;

    char view[MAX_PATH_LEN];
    utf8_str_truncate(view, input, avail);

    int cx = area->content_x + prompt_width + (int)utf8_str_width(view);
    int max_cx = area->content_x + area->content_width - 1;
    if (cx > max_cx) cx = max_cx;
    if (cx < area->content_x) cx = area->content_x;
    move(row, cx);
    refresh();
}

static int lang_prompt_path(char *buf, size_t size)
{
    MenuContentArea area;
    menu_content_area(&area);

    int y = area.max_y - 4;
    const char *prompt = i18n_get("lang_mgr.add_prompt");

    curs_set(1);
    lang_prompt_redraw(&area, y, prompt, "");

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
                lang_prompt_redraw(&area, y, prompt, buf);
            }
        } else if (ch >= 32) {
            buf[pos++] = (char)ch;
            buf[pos] = '\0';
            lang_prompt_redraw(&area, y, prompt, buf);
        }
    }

    curs_set(0);

    /* Clean up prompt line (content area only) */
    menu_content_clear_row(&area, y);

    if (ch == 27) {
        buf[0] = '\0';
        return 0;
    }
    return 1;
}

/* ── Rendering ───────────────────────────────────────────────── */

void render_language_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);

    int max_y = area.max_y;
    int start_y = 2;
    char line[512];

    lang_refresh_list();

    menu_content_clear_rows(&area, start_y, max_y - 2);

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    snprintf(line, sizeof(line), "%s", i18n_get("lang_mgr.title"));
    menu_content_puts(&area, start_y, line);

    int sep_width = area.content_width < 40 ? area.content_width : 40;
    char sep[41];
    for (int i = 0; i < sep_width; i++) sep[i] = '=';
    sep[sep_width] = '\0';
    menu_content_puts(&area, start_y + 1, sep);
    start_y += 3;

    const char *cur_lang = i18n_current_lang();
    int visible = max_y - start_y - 3;
    if (visible < 1) visible = 1;

    if (g_lang_count == 0) {
        snprintf(line, sizeof(line), "  %s", i18n_get("lang_mgr.no_langs"));
        menu_content_puts(&area, start_y, line);
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
        if (row > max_y - 4) break;

        int is_current = (strcmp(g_langs[idx].id, cur_lang) == 0);
        int is_selected = (idx == g_lang_selected && g_focus_area == FOCUS_CONTENT);

        if (is_selected) attron(A_REVERSE);

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
        int avail = area.content_width - 2 - 1 - (int)utf8_str_width(tag) - 2 - cov_width;
        if (avail < 6) avail = 6;
        if (avail > 120) avail = 120;
        char name_buf[128];
        utf8_str_truncate(name_buf, g_langs[idx].name, avail);
        char padded_name[256];
        utf8_str_pad(padded_name, sizeof(padded_name), name_buf, avail);

        snprintf(line, sizeof(line), "%s %s  %s%s",
                 marker, padded_name, tag, cov_buf);
        menu_content_puts(&area, row, line);

        if (is_selected) attroff(A_REVERSE);
    }

    /* Draw scrollbar */
    scrollbar_draw(stdscr, start_y, visible,
                   g_lang_count, visible, g_lang_scroll, area.max_x - 2);

    /* Hint bar */
    int hint_row = max_y - 3;
    menu_content_clear_row(&area, hint_row);
    snprintf(line, sizeof(line), "%s", i18n_get("lang_mgr.hint"));
    menu_content_puts(&area, hint_row, line);

    /* Action bar */
    int act_row = max_y - 2;
    menu_content_clear_row(&area, act_row);
    attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    snprintf(line, sizeof(line), "%s", i18n_get("lang_mgr.actions"));
    menu_content_puts(&area, act_row, line);
    attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

/* ── Input handling ──────────────────────────────────────────── */

int handle_language_input(int ch)
{
    ActionId action = keymap_resolve(CTX_LANGUAGE, ch);

    switch (action) {
        case ACTION_LANGUAGE_UP:
            if (g_lang_selected > 0)
                g_lang_selected--;
            else
                g_lang_selected = g_lang_count > 0 ? g_lang_count - 1 : 0;
            break;

        case ACTION_LANGUAGE_DOWN:
            if (g_lang_count > 0) {
                g_lang_selected++;
                if (g_lang_selected >= g_lang_count)
                    g_lang_selected = 0;
            }
            break;

        case ACTION_LANGUAGE_ACTIVATE: {
            if (g_lang_count == 0) break;
            const char *sel = g_langs[g_lang_selected].id;
            const char *cur = i18n_current_lang();
            if (strcmp(sel, cur) != 0) {
                log_debug("lang_view", "Activate language: %s -> %s", cur, sel);
                if (i18n_reload(sel) == 0) {
                    strncpy(g_app_config.ui_language, sel,
                            sizeof(g_app_config.ui_language) - 1);
                    g_app_config.ui_language[sizeof(g_app_config.ui_language) - 1] = '\0';
                    player_config_persist();
                    help_free_lines();
                    show_status_message(i18n_get("lang_mgr.switched"));
                    rerender_active_view();
                    return 1;
                }
            }
            break;
        }

        case ACTION_LANGUAGE_ADD: {
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

        case ACTION_LANGUAGE_DELETE: {
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
                        player_config_persist();
                        help_free_lines();
                        lang_refresh_list();
                        show_status_message(i18n_get("lang_mgr.deleted"));
                        rerender_active_view();
                        return 1;
                    }
                    lang_refresh_list();
                    show_status_message(i18n_get("lang_mgr.deleted"));
                } else {
                    show_status_message(i18n_get("lang_mgr.delete_failed"));
                }
            }
            break;
        }

        case ACTION_LANGUAGE_BACK:
            exit_current_view();
            return 1;

        default:
            return 0;
    }

    render_language_content();
    return 1;
}
