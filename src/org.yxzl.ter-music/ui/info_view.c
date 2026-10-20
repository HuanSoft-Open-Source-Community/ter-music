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

static void info_put_row(const MenuContentArea *area, int limit, int row, const char *text)
{
    if (row < 0 || row > limit) return;
    menu_content_puts(area, row, text);
}

void render_info_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);

    int start_y = 2;
    int limit = area.max_y - 3;
    char buf[512];

    menu_content_clear_rows(&area, 2, limit);

    attron(COLOR_PAIR(COLOR_PAIR_BORDER));

    snprintf(buf, sizeof(buf), "%s %s",
             i18n_get("info.about_title"), APP_NAME);
    info_put_row(&area, limit, start_y, buf);
    snprintf(buf, sizeof(buf), "========================================");
    info_put_row(&area, limit, start_y + 1, buf);
    start_y += 3;

    snprintf(buf, sizeof(buf), i18n_get("info.name_fmt"), APP_NAME);
    info_put_row(&area, limit, start_y, buf);
    snprintf(buf, sizeof(buf), i18n_get("info.version_fmt"), APP_VERSION);
    info_put_row(&area, limit, start_y + 1, buf);
    snprintf(buf, sizeof(buf), i18n_get("info.authors_fmt"), APP_AUTHORS);
    info_put_row(&area, limit, start_y + 2, buf);
    snprintf(buf, sizeof(buf), i18n_get("info.email_fmt"), APP_EMAIL);
    info_put_row(&area, limit, start_y + 3, buf);
    start_y += 5;

    snprintf(buf, sizeof(buf), "%s", i18n_get("info.summary"));
    info_put_row(&area, limit, start_y, buf);
    snprintf(buf, sizeof(buf), "%s", i18n_get("info.summary_line1"));
    info_put_row(&area, limit, start_y + 1, buf);
    snprintf(buf, sizeof(buf), "%s", i18n_get("info.summary_line2"));
    info_put_row(&area, limit, start_y + 2, buf);
    snprintf(buf, sizeof(buf), "%s", i18n_get("info.summary_line3"));
    info_put_row(&area, limit, start_y + 3, buf);
    start_y += 5;

    snprintf(buf, sizeof(buf), "%s", i18n_get("info.repository"));
    info_put_row(&area, limit, start_y, buf);
    snprintf(buf, sizeof(buf), "  %s", APP_REPO);
    info_put_row(&area, limit, start_y + 1, buf);
    start_y += 3;

    snprintf(buf, sizeof(buf), "%s", i18n_get("info.license"));
    info_put_row(&area, limit, start_y, buf);
    start_y += 2;

    snprintf(buf, sizeof(buf), "%s", i18n_get("info.readonly"));
    info_put_row(&area, limit, start_y, buf);

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
