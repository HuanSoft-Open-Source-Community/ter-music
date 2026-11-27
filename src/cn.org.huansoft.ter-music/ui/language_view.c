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
#include "ui/menu_anim.h"
#include "ui/appearance.h"
#include "ui/file_picker.h"
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

/* ── Pick a language pack file (desktop dialog → TTY fallback) ── */

/* 语言包是 .tar.gz（内含 lang.xml）。这里只负责把路径选回来：格式/内容是否
 * 合法仍由 i18n_add_language() 原有的校验/安装路径判断。取消返回 0（不改
 * 任何状态），选择器失败把可读原因放到状态行后返回 0。 */
static int lang_pick_pack(char *buf, size_t size)
{
    const char *initial_dir = getenv("HOME");
    char picker_err[256];
    int rc = file_picker_pick_file(i18n_get("lang_mgr.add_title"),
                                   i18n_get("lang_mgr.add_filter_name"),
                                   "*.tar.gz;*.tgz",
                                   initial_dir, buf, size,
                                   picker_err, sizeof(picker_err));
    /* 外部对话框前后由选择器暂停/恢复 curses；TTY 回退本身在 curses 里跑，
     * 两者都把光标收回去，随后 handle_language_action 会整体重绘内容区。 */
    curs_set(0);
    noecho();
    flushinp();

    if (rc == FILE_PICKER_CANCEL) {
        return 0;
    }
    if (rc != FILE_PICKER_OK) {
        show_status_message(picker_err[0] ? picker_err
                                          : i18n_get("lang_mgr.add_failed"));
        return 0;
    }
    return 1;
}

/* ── Rendering ───────────────────────────────────────────────── */

/* 渲染期记录的列表几何：屏幕行→语言条目下标的换算依据
 * （滚动偏移是 g_lang_scroll，与收藏/历史的 offset 变量各自独立）。
 * top<0 表示这一帧没画列表（没有可用语言包），鼠标命中自然全部落空。 */
static int g_lang_list_top = -1;
static int g_lang_list_visible = 0;

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
        /* 没有列表可命中：作废几何，滚轮与点击都只认领 */
        g_lang_list_top = -1;
        g_lang_list_visible = 0;
        snprintf(line, sizeof(line), "  %s", i18n_get("lang_mgr.no_langs"));
        menu_content_puts(&area, start_y, line);
        attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
        repaint_status_message();
        refresh();
        return;
    }

    /* 渲染期记下列表几何，供鼠标命中把"屏幕行"换成"条目下标"。
     * 只在此处记一次，命中判定不再抄第二遍行号推导。 */
    g_lang_list_top = start_y;
    g_lang_list_visible = visible;

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
    repaint_status_message();
    refresh();
}

/* ── Input handling ──────────────────────────────────────────── */

/* 动作入口：键盘解析出的动作与鼠标滚轮/单击共用同一条路径。
 * （鼠标绝不合成按键码：用户可改键，合成按键会在改键后失灵或做错事。） */
int handle_language_action(ActionId action)
{
    switch (action) {
        case ACTION_LANGUAGE_UP:
        case ACTION_LANGUAGE_DOWN:
            /* 侧栏只有“返回”一项。上下键先离开侧栏进入内容列表，
             * 否则会在侧栏仍高亮时移动看不见的选中项。 */
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                render_menu_sidebar(g_menu_selected_idx, language_sidebar_items,
                                    LANGUAGE_ITEM_COUNT);
                render_language_content();
                return 1;
            }
            if (action == ACTION_LANGUAGE_UP) {
                if (g_lang_selected > 0)
                    g_lang_selected--;
                else
                    g_lang_selected = g_lang_count > 0 ? g_lang_count - 1 : 0;
            } else if (g_lang_count > 0) {
                g_lang_selected++;
                if (g_lang_selected >= g_lang_count)
                    g_lang_selected = 0;
            }
            break;

        case ACTION_LANGUAGE_ACTIVATE:
            /* 焦点在侧栏时 Enter 确认的是“返回”；只有内容区才切语言。 */
            if (g_focus_area == FOCUS_SIDEBAR) {
                exit_current_view();
                return 1;
            }
            if (g_lang_count == 0) break;
            {
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
            }
            break;

        case ACTION_LANGUAGE_ADD: {
            char path[MAX_PATH_LEN];
            if (lang_pick_pack(path, sizeof(path)) && path[0]) {
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

int handle_language_input(int ch)
{
    return handle_language_action(keymap_resolve(CTX_LANGUAGE, ch));
}

/* ── Mouse handling ──────────────────────────────────────────── */

/* 内容区某屏幕行对应哪一个语言条目；不在列表上返回 -1。 */
static int language_index_at_row(int screen_row)
{
    if (g_lang_list_top < 0 || g_lang_list_visible <= 0) return -1;

    int line = screen_row - g_lang_list_top;
    if (line < 0 || line >= g_lang_list_visible) return -1;

    int idx = g_lang_scroll + line;
    if (idx < 0 || idx >= g_lang_count) return -1;
    return idx;
}

/* ============================================================
 * Right-click context menu
 * ============================================================ */

void language_context_menu_build(UIContextMenuLevel *level,
                                 const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.language"));

    if (ctx->area == UI_CTX_AREA_CONTENT) {
        ui_ctx_menu_add_action(level, ACTION_LANGUAGE_ACTIVATE, ctx->valid, 0);
        ui_ctx_menu_add_action(level, ACTION_LANGUAGE_ADD, 1, 0);
        ui_ctx_menu_add_action(level, ACTION_LANGUAGE_DELETE, ctx->valid, 0);
    }
    ui_ctx_menu_add_action(level, ACTION_LANGUAGE_BACK, 1, 0);
    ui_ctx_menu_add_separator(level);
    ui_context_menu_add_pages_submenu(level);
}

int language_context_dispatch(ActionId action, const UIContextMenuContext *ctx)
{
    if ((action == ACTION_LANGUAGE_ACTIVATE ||
         action == ACTION_LANGUAGE_DELETE) &&
        (!ctx || !ctx->valid || ctx->index < 0 || ctx->index >= g_lang_count)) {
        ui_context_menu_report_stale("language");
        return 1;
    }
    return handle_language_action(action);
}

static void language_open_context_menu(const UIMouseEvent *ev,
                                       const UIMouseDecision *d)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_LANGUAGE;
    ctx.target = d->target;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        ctx.area = UI_CTX_AREA_SIDEBAR;
        if (d->row >= 0 && d->row < LANGUAGE_ITEM_COUNT) {
            g_focus_area = FOCUS_SIDEBAR;
            g_menu_selected_idx = d->row;
            ctx.valid = 1;
            ctx.index = d->row;
            render_menu_sidebar(g_menu_selected_idx, language_sidebar_items,
                                LANGUAGE_ITEM_COUNT);
            render_language_content();
        }
    } else {
        ctx.area = UI_CTX_AREA_CONTENT;
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, language_sidebar_items,
                                LANGUAGE_ITEM_COUNT);
        }
        int idx = language_index_at_row(ev->y);
        if (idx >= 0) {
            g_lang_selected = idx;
            ctx.valid = 1;
            ctx.index = idx;
            render_language_content();
        }
    }

    (void)ui_context_menu_open(language_context_menu_build,
                               language_context_dispatch,
                               &ctx, ev->y, ev->x);
}

int language_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        language_open_context_menu(ev, d);
        return 1;
    }

    /* 本视图侧栏只有"返回"一项（LANGUAGE_ITEM_COUNT==1），
     * 语言列表在内容区，所以两者的"回车"动作名不同，不能混用。 */
    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (d->row < 0 || d->row >= LANGUAGE_ITEM_COUNT) return 1;

        /* 侧栏只有"返回"一项，没有可移动的选中；侧栏滚轮只认领、不动作，
         * 避免滚一下把焦点切到内容区并改掉语言选择。 */
        if (ev->wheel) return 1;
        if (!ev->click) return 1;

        g_focus_area = FOCUS_SIDEBAR;
        g_menu_selected_idx = d->row;
        render_menu_sidebar(g_menu_selected_idx, language_sidebar_items, LANGUAGE_ITEM_COUNT);
        render_language_content();

        /* 单击即选中并执行；侧栏那条是返回主界面 */
        return handle_language_action(ACTION_LANGUAGE_BACK);
    }

    /* 内容区：语言列表 */
    if (ev->wheel) {
        /* 列表高亮只在焦点位于内容区时画得出来，
         * 所以滚轮要先把焦点带到指针所在这一列，再走键盘那条上下动作。 */
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, language_sidebar_items, LANGUAGE_ITEM_COUNT);
        }
        return handle_language_action(ev->wheel < 0 ? ACTION_LANGUAGE_UP
                                                    : ACTION_LANGUAGE_DOWN);
    }
    if (!ev->click) return 1;

    /* 内容区几何记的是**屏幕行**（menu_content_puts 的行号就是 stdscr 行号），
     * 而 d->row 是内容区矩形的本地行号（该矩形 top=1），两者差一行，
     * 所以这里用 ev->y；侧栏那一路才用 d->row（侧栏 top=2 与 start_y 同源）。 */
    const int idx = language_index_at_row(ev->y);
    if (idx < 0) return 1;   /* 标题、分隔线、底部提示与操作行：只认领不动作 */

    g_focus_area = FOCUS_CONTENT;   /* 同上：让选中高亮真的画得出来 */
    g_lang_selected = idx;
    render_menu_sidebar(g_menu_selected_idx, language_sidebar_items, LANGUAGE_ITEM_COUNT);
    render_language_content();

    /* 本视图的 ACTIVATE 就是"切换到选中的语言"（见 keymap: language.activate）；
     * 单击即执行。 */
    return handle_language_action(ACTION_LANGUAGE_ACTIVATE);
}
