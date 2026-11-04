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
#include "ui/rainbow.h"
#include "ui/keymap.h"
#include <stdio.h>
#include <string.h>
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

/* 动作入口：键盘解析出的动作与鼠标点击共用同一条路径。
 * （鼠标绝不合成按键码：用户可改键，合成按键会在改键后失灵或做错事。） */
int handle_info_action(ActionId action)
{
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

int handle_info_input(int ch)
{
    return handle_info_action(keymap_resolve(CTX_INFO, ch));
}

/* ============================================================
 * Info view mouse handling
 * ============================================================ */

static void info_rainbow_callback(const UIContextMenuContext *ctx, intptr_t value)
{
    (void)ctx;
    (void)value;
    toggle_rainbow_mode();
}

static void info_rainbow_menu_build(UIContextMenuLevel *level,
                                    const UIContextMenuContext *ctx)
{
    (void)ctx;
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.info"));
    ui_ctx_menu_add_callback(
        level,
        ui_rainbow_active() ? i18n_get("context.info.rainbow_off")
                            : i18n_get("context.info.rainbow_on"),
        "rainbow", info_rainbow_callback, 0, 1, ui_rainbow_active());
}

/* F6 信息页专用：菜单严格只有一条"科乐美流光"。
 * 主视图信息窗格不设右键菜单（那里右键只被认领并吞掉）。 */
static void info_view_open_rainbow_menu(int anchor_y, int anchor_x)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_INFO;
    ctx.target = UI_MOUSE_TARGET_INFO;
    ctx.area = UI_CTX_AREA_INFO;
    ctx.valid = 1;
    (void)ui_context_menu_open(info_rainbow_menu_build, NULL, &ctx,
                               anchor_y, anchor_x);
}

int info_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        info_view_open_rainbow_menu(ev->y, ev->x);
        return 1;
    }

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (d->row < 0 || d->row >= INFO_ITEM_COUNT) return 1;

        /* 滚轮落在侧栏这一列：移动侧栏选中项（本视图上下键本来就只动侧栏） */
        if (ev->wheel) {
            g_focus_area = FOCUS_SIDEBAR;
            return handle_info_action(ev->wheel < 0 ? ACTION_INFO_UP : ACTION_INFO_DOWN);
        }
        if (!ev->click) return 1;

        const int already_selected = (g_menu_selected_idx == d->row);
        g_focus_area = FOCUS_SIDEBAR;
        g_menu_selected_idx = d->row;
        render_menu_sidebar(g_menu_selected_idx, info_sidebar_items, INFO_ITEM_COUNT);

        /* 双击或"再点一次已选中的那条"等同回车：关于 / 仓库 / 返回主界面 */
        if (already_selected || ev->click_count >= 2) {
            return handle_info_action(ACTION_INFO_ACTIVATE);
        }
        return 1;
    }

    /* 内容区是只读文本（版本号、作者、仓库地址、许可证），没有可移动的高亮，
     * 也就没有"滚轮滚什么/点谁"的答案：只认领事件，什么都不做，
     * 免得同一次滚动被后面的分发再做一遍。 */
    return 1;
}
