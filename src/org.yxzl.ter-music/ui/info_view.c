/**
 * @file info_view.c
 * @brief 信息视图 — 关于、仓库地址
 *
 * 从 menus.c 拆分而来，负责信息页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include <stdio.h>
#include <ncursesw/ncurses.h>

/* ============================================================
 * Info view rendering
 * ============================================================ */

void render_info_content(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_width = max_x / 4;
    int content_start_x = menu_width + 2;
    int start_y = 2;

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    mvprintw(start_y, content_start_x, "%s %s",
             i18n_get("info.about_title"), APP_NAME);
    mvprintw(start_y + 1, content_start_x, "========================================");
    start_y += 3;

    mvprintw(start_y, content_start_x,
             i18n_get("info.name_fmt"), APP_NAME);
    mvprintw(start_y + 1, content_start_x,
             i18n_get("info.version_fmt"), APP_VERSION);
    mvprintw(start_y + 2, content_start_x,
             i18n_get("info.authors_fmt"), APP_AUTHORS);
    mvprintw(start_y + 3, content_start_x,
             i18n_get("info.email_fmt"), APP_EMAIL);
    start_y += 5;

    mvprintw(start_y, content_start_x, "%s",
             i18n_get("info.summary"));
    mvprintw(start_y + 1, content_start_x, "%s",
             i18n_get("info.summary_line1"));
    mvprintw(start_y + 2, content_start_x, "%s",
             i18n_get("info.summary_line2"));
    mvprintw(start_y + 3, content_start_x, "%s",
             i18n_get("info.summary_line3"));
    start_y += 5;

    mvprintw(start_y, content_start_x, "%s",
             i18n_get("info.repository"));
    mvprintw(start_y + 1, content_start_x, "  %s", APP_REPO);
    start_y += 3;

    mvprintw(start_y, content_start_x, "%s",
             i18n_get("info.license"));
    start_y += 2;

    mvprintw(start_y, content_start_x, "%s",
             i18n_get("info.readonly"));

    attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
    refresh();
}

/* ============================================================
 * Info view input handling
 * ============================================================ */

int handle_info_input(int ch)
{
    ActionId action = keymap_resolve(CTX_INFO, ch);

    switch (action) {
        case ACTION_INFO_UP:
            g_menu_selected_idx--;
            if (g_menu_selected_idx < 0) g_menu_selected_idx = INFO_ITEM_COUNT - 1;
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            return 1;

        case ACTION_INFO_DOWN:
            g_menu_selected_idx++;
            if (g_menu_selected_idx >= INFO_ITEM_COUNT) g_menu_selected_idx = 0;
            render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);
            return 1;

        case ACTION_INFO_ACTIVATE:
            if (g_menu_selected_idx == INFO_ITEM_COUNT - 1) {
                exit_current_view();
            }
            return 1;

        default:
            return 0;
    }
}
