/**
 * @file history.c
 * @brief 历史视图 — 目录历史浏览和管理
 *
 * 从 menus.c 拆分而来，负责历史页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "player/player.h"
#include "ui/dialog.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "config/config.h"
#include "playlist/playlist.h"
#include "library/library.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <time.h>
#include <stddef.h>

/* ============================================================
 * History view rendering
 * ============================================================ */

/* 渲染期记录的列表几何：屏幕行→目录历史条目下标的换算依据。
 * top<0 表示这一帧没画列表（目录历史为空），鼠标命中自然全部落空。 */
static int g_history_list_top = -1;
static int g_history_list_visible = 0;

void render_history_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

    menu_content_clear_rows(&area, 2, max_y - 2);

    attron(COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    char header[256];
    snprintf(header, sizeof(header), i18n_get("history.title_fmt"),
             g_dir_history.count);
    menu_content_puts(&area, start_y, header);
    menu_content_puts(&area, start_y + 1, "----------------------------------------");
    start_y += 3;

    if (g_dir_history.count == 0) {
        g_history_list_top = -1;
        g_history_list_visible = 0;
        menu_content_puts(&area, start_y, i18n_get("history.empty"));
        menu_content_puts(&area, start_y + 1, i18n_get("history.auto_record"));
    } else {
        int visible_lines = max_y - start_y - 3;

        /* 渲染期记下列表几何，供鼠标命中把"屏幕行"换成"条目下标"。
         * 只在此处记一次，命中判定不再抄第二遍行号推导。 */
        g_history_list_top = start_y;
        g_history_list_visible = visible_lines;

        if (g_content_selected_idx >= g_dir_history.count) {
            g_content_selected_idx = g_dir_history.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;

        if (g_content_selected_idx < g_history_content_offset) {
            g_history_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_history_content_offset + visible_lines) {
            g_history_content_offset = g_content_selected_idx - visible_lines + 1;
        }

        for (int i = 0; i < visible_lines && (g_history_content_offset + i) < g_dir_history.count; i++) {
            int idx = g_history_content_offset + i;
            DirHistoryEntry *entry = &g_dir_history.entries[idx];

            char time_str[64];
            struct tm *tm_info = localtime(&entry->open_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);

            char display_path[MAX_PATH_LEN];
            int path_width = max_x - menu_width - 25;
            if (path_width > MAX_PATH_LEN - 1) path_width = MAX_PATH_LEN - 1;
            utf8_str_truncate(display_path, entry->path, path_width);

            char line[MAX_PATH_LEN + 96];
            snprintf(line, sizeof(line), " %s [%s]", display_path, time_str);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                menu_content_puts(&area, start_y + i, line);
                attroff(A_REVERSE);
            } else {
                menu_content_puts(&area, start_y + i, line);
            }
        }

        int bottom_y = max_y - 3;
        menu_content_puts(&area, bottom_y, i18n_get("history.hint_manage"));
        menu_content_puts(&area, bottom_y + 1, i18n_get("history.hint_delete"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    refresh();
}

/* ============================================================
 * History view input handling
 * ============================================================ */

/* 动作入口：键盘解析出的动作与鼠标滚轮/双击共用同一条路径。
 * （鼠标绝不合成按键码：用户可改键，合成按键会在改键后失灵或做错事。） */
int handle_history_action(ActionId action)
{
    switch (action) {
        case ACTION_HISTORY_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = HISTORY_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_dir_history.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= HISTORY_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_dir_history.count) g_content_selected_idx = 0;
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_ACTIVATE:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                } else if (g_menu_selected_idx == 1) {
                    clear_dir_history();
                    g_content_selected_idx = 0;
                    show_status_message(i18n_get("history.cleared"));
                    render_menu_frame(i18n_get("menu.history"));
                    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
                    render_history_content();
                    render_menu_hint_bar();
                } else if (g_menu_selected_idx == HISTORY_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_dir_history.count > 0 && g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_dir_history.count) {

                    const char *path = g_dir_history.entries[g_content_selected_idx].path;
                    player_stop();
                    int count = load_playlist(path);
                    /* 目录历史加载的内容需经门面下发给核心（remote 下 load_playlist
                     * 只写进程内镜像，不会把路径交给核心）（Task 4.3）。 */
                    if (count > 0) {
                        player_queue_push();
                    }

                    if (count > 0) {
                        g_selected_index = 0;
                        add_dir_history_entry(path);
                        exit_current_view();
                        show_status_message(i18n_get("status.folder_loaded"));
                    } else {
                        show_status_message(i18n_get("history.no_audio"));
                        render_history_content();
                    }
                }
            }
            return 1;

        case ACTION_HISTORY_QUEUE_APPEND:
            if (g_focus_area == FOCUS_CONTENT &&
                g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 &&
                g_content_selected_idx < g_dir_history.count) {

                const char *path = g_dir_history.entries[g_content_selected_idx].path;
                int count = append_playlist(path);

                if (count > 0) {
                    /* 追加的内容也要经门面下发给核心（remote 下 append_playlist
                     * 只写进程内镜像）（Task 4.3）。 */
                    player_queue_push();
                    add_dir_history_entry(path);
                    exit_current_view();
                    if (g_app_config.remember_last_path) {
                        snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", path);
                        player_config_persist();
                    }
                    char msg[96];
                    snprintf(msg, sizeof(msg), "%s %d %s",
                             i18n_get("status.appended"), count,
                             i18n_get("history.tracks_to_queue"));
                    show_status_message(msg);
                } else {
                    show_status_message(i18n_get("dialog.no_new_audio"));
                    render_history_content();
                }
            }
            return 1;

        case ACTION_HISTORY_DELETE:
            if (g_focus_area == FOCUS_CONTENT && g_dir_history.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_dir_history.count) {

                char removed_path[MAX_PATH_LEN] = "";
                strncpy(removed_path, g_dir_history.entries[g_content_selected_idx].path, MAX_PATH_LEN - 1);
                removed_path[MAX_PATH_LEN - 1] = '\0';

                for (int i = g_content_selected_idx; i < g_dir_history.count - 1; i++) {
                    g_dir_history.entries[i] = g_dir_history.entries[i + 1];
                }
                g_dir_history.count--;

                if (library_is_available())
                    library_dir_history_remove(removed_path);

                if (g_content_selected_idx >= g_dir_history.count) {
                    g_content_selected_idx = g_dir_history.count - 1;
                }

                show_status_message(i18n_get("history.entry_removed"));
                render_history_content();
            }
            return 1;

        case ACTION_HISTORY_CLEAR:
            if (g_focus_area == FOCUS_CONTENT) {
                clear_dir_history();
                g_content_selected_idx = 0;
                show_status_message(i18n_get("history.cleared"));
                render_history_content();
            }
            return 1;

        default:
            return 0;
    }
}

int handle_history_input(int ch)
{
    return handle_history_action(keymap_resolve(CTX_HISTORY, ch));
}

/* ============================================================
 * History view mouse handling
 * ============================================================ */

/* 内容区某屏幕行对应哪一条目录历史；不在列表上返回 -1。 */
static int history_index_at_row(int screen_row)
{
    if (g_history_list_top < 0 || g_history_list_visible <= 0) return -1;

    int line = screen_row - g_history_list_top;
    if (line < 0 || line >= g_history_list_visible) return -1;

    int idx = g_history_content_offset + line;
    if (idx < 0 || idx >= g_dir_history.count) return -1;
    return idx;
}

/* ============================================================
 * Right-click context menu
 * ============================================================ */

static void history_clear_callback(const UIContextMenuContext *ctx,
                                   intptr_t value)
{
    (void)ctx;
    (void)value;
    clear_dir_history();
    g_content_selected_idx = 0;
    show_status_message(i18n_get("history.cleared"));
    render_menu_frame(i18n_get("menu.history"));
    render_menu_sidebar(g_menu_selected_idx, history_sidebar_items,
                        HISTORY_ITEM_COUNT);
    render_history_content();
    render_menu_hint_bar();
}

void history_context_menu_build(UIContextMenuLevel *level,
                                const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.history"));

    const int content = (ctx->area == UI_CTX_AREA_CONTENT) && ctx->valid;
    ui_ctx_menu_add_action(level, ACTION_HISTORY_ACTIVATE, ctx->valid, 0);
    if (ctx->area == UI_CTX_AREA_CONTENT) {
        ui_ctx_menu_add_action(level, ACTION_HISTORY_QUEUE_APPEND, content, 0);
        ui_ctx_menu_add_action(level, ACTION_HISTORY_DELETE, content, 0);
    }
    ui_ctx_menu_add_callback(level,
                             keymap_action_label(ACTION_HISTORY_CLEAR),
                             "history.clear", history_clear_callback, 0, 1, 0);
    ui_ctx_menu_add_separator(level);
    ui_context_menu_add_pages_submenu(level);
}

int history_context_dispatch(ActionId action, const UIContextMenuContext *ctx)
{
    /* 侧栏的 ACTIVATE 是"清空/返回"，不指向某一行；只有内容区的行动作
     * 才需要做路径竞态校验。 */
    const int row_action =
        (action == ACTION_HISTORY_QUEUE_APPEND ||
         action == ACTION_HISTORY_DELETE ||
         (action == ACTION_HISTORY_ACTIVATE && ctx &&
          ctx->area == UI_CTX_AREA_CONTENT));
    if (row_action &&
        (!ctx || !ctx->valid || ctx->index < 0 ||
         ctx->index >= g_dir_history.count ||
         strcmp(g_dir_history.entries[ctx->index].path, ctx->item_path) != 0)) {
        ui_context_menu_report_stale("history");
        return 1;
    }
    return handle_history_action(action);
}

static void history_open_context_menu(const UIMouseEvent *ev,
                                      const UIMouseDecision *d)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_HISTORY;
    ctx.target = d->target;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        ctx.area = UI_CTX_AREA_SIDEBAR;
        if (d->row >= 0 && d->row < HISTORY_ITEM_COUNT) {
            g_focus_area = FOCUS_SIDEBAR;
            g_menu_selected_idx = d->row;
            ctx.valid = 1;
            ctx.index = d->row;
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items,
                                HISTORY_ITEM_COUNT);
            render_history_content();
        }
    } else {
        ctx.area = UI_CTX_AREA_CONTENT;
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items,
                                HISTORY_ITEM_COUNT);
        }
        int idx = history_index_at_row(ev->y);
        if (idx >= 0) {
            g_content_selected_idx = idx;
            ctx.valid = 1;
            ctx.index = idx;
            snprintf(ctx.item_path, sizeof(ctx.item_path), "%s",
                     g_dir_history.entries[idx].path);
            render_history_content();
        }
    }

    (void)ui_context_menu_open(history_context_menu_build,
                               history_context_dispatch,
                               &ctx, ev->y, ev->x);
}

int history_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        history_open_context_menu(ev, d);
        return 1;
    }

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (d->row < 0 || d->row >= HISTORY_ITEM_COUNT) return 1;

        /* 滚轮落在侧栏这一列：移动侧栏选中项。动作内部按 g_focus_area 分流，
         * 所以先把焦点带到侧栏（否则滚的是内容列）。 */
        if (ev->wheel) {
            g_focus_area = FOCUS_SIDEBAR;
            return handle_history_action(ev->wheel < 0 ? ACTION_HISTORY_UP
                                                       : ACTION_HISTORY_DOWN);
        }
        if (!ev->click) return 1;

        const int already_selected = (g_menu_selected_idx == d->row);
        g_focus_area = FOCUS_SIDEBAR;
        g_menu_selected_idx = d->row;
        render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
        render_history_content();

        /* 双击或"再点一次已选中的那条"等同回车：进入列表 / 清空历史 / 返回主界面 */
        if (already_selected || ev->click_count >= 2) {
            return handle_history_action(ACTION_HISTORY_ACTIVATE);
        }
        return 1;
    }

    /* 内容区 */
    if (ev->wheel) {
        /* 历史列表的高亮只在焦点位于内容区时画得出来，
         * 所以滚轮要先把焦点带到指针所在这一列，再走键盘那条上下动作。 */
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
        }
        return handle_history_action(ev->wheel < 0 ? ACTION_HISTORY_UP
                                                   : ACTION_HISTORY_DOWN);
    }
    if (!ev->click) return 1;

    /* 内容区几何记的是**屏幕行**（menu_content_puts 的行号就是 stdscr 行号），
     * 而 d->row 是内容区矩形的本地行号（该矩形 top=1），两者差一行，
     * 所以这里用 ev->y；侧栏那一路才用 d->row（侧栏 top=2 与 start_y 同源）。 */
    const int idx = history_index_at_row(ev->y);
    if (idx < 0) return 1;   /* 标题行、分隔线、底部提示行：只认领不动作 */

    const int already_selected = (g_content_selected_idx == idx);
    /* 把焦点带到指针这一列：一是选中高亮只在焦点为内容区时才画得出来，
     * 二是 ACTION_HISTORY_ACTIVATE 自己按 g_focus_area 分流，
     * 焦点若还留在侧栏，双击会去执行侧栏那一条（清空历史/返回）而不是打开目录。 */
    if (g_focus_area != FOCUS_CONTENT) {
        g_focus_area = FOCUS_CONTENT;
        render_menu_sidebar(g_menu_selected_idx, history_sidebar_items, HISTORY_ITEM_COUNT);
    }
    g_content_selected_idx = idx;
    render_history_content();

    if (already_selected || ev->click_count >= 2) {
        return handle_history_action(ACTION_HISTORY_ACTIVATE);
    }
    return 1;
}
