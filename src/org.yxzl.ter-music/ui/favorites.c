/**
 * @file favorites.c
 * @brief 收藏视图 — 收藏歌曲浏览和管理
 *
 * 从 menus.c 拆分而来，负责收藏页面的渲染和输入处理。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "player/player.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/keymap.h"
#include "playlist/playlist.h"
#include <stdio.h>
#include <string.h>
#include <ncursesw/ncurses.h>

/* ============================================================
 * Favorites view rendering
 * ============================================================ */

/* 渲染期记录的列表几何：屏幕行→收藏条目下标的换算依据。
 * top<0 表示这一帧没画列表（收藏为空），鼠标命中自然全部落空。 */
static int g_favorites_list_top = -1;
static int g_favorites_list_visible = 0;

void render_favorites_content(void)
{
    MenuContentArea area;
    menu_content_area(&area);
    int max_y = area.max_y;
    int max_x = area.max_x;
    int menu_width = area.menu_width;
    int start_y = 2;

    menu_content_clear_rows(&area, 2, max_y - 3);

    attron(COLOR_PAIR(COLOR_PAIR_LYRICS));

    char header[256];
    snprintf(header, sizeof(header), i18n_get("favorites.title_fmt"),
             g_favorites.count);
    menu_content_puts(&area, start_y, header);
    menu_content_puts(&area, start_y + 1, "----------------------------------------");
    start_y += 3;

    if (g_favorites.count == 0) {
        g_favorites_list_top = -1;
        g_favorites_list_visible = 0;
        menu_content_puts(&area, start_y, i18n_get("favorites.empty"));
        menu_content_puts(&area, start_y + 1, i18n_get("favorites.hint_f"));
    } else {
        int visible_lines = max_y - start_y - 3;

        /* 渲染期记下列表几何，供鼠标命中把"屏幕行"换成"条目下标"。
         * 只在此处记一次，命中判定不再抄第二遍行号推导。 */
        g_favorites_list_top = start_y;
        g_favorites_list_visible = visible_lines;

        if (g_content_selected_idx >= g_favorites.count) {
            g_content_selected_idx = g_favorites.count - 1;
        }
        if (g_content_selected_idx < 0) g_content_selected_idx = 0;

        if (g_content_selected_idx < g_favorites_content_offset) {
            g_favorites_content_offset = g_content_selected_idx;
        } else if (g_content_selected_idx >= g_favorites_content_offset + visible_lines) {
            g_favorites_content_offset = g_content_selected_idx - visible_lines + 1;
        }

        for (int i = 0; i < visible_lines && (g_favorites_content_offset + i) < g_favorites.count; i++) {
            int idx = g_favorites_content_offset + i;
            Track *t = &g_favorites.tracks[idx];

            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            char display_title[MAX_META_LEN + 32];
            char display_artist[MAX_META_LEN + 32];
            int title_width  = (max_x - menu_width - 10) * 3 / 5;
            int artist_width = (max_x - menu_width - 10) * 2 / 5;

            utf8_str_truncate(truncated_title, t->title, title_width > 0 ? title_width : 30);
            utf8_str_truncate(truncated_artist, t->artist, artist_width > 0 ? artist_width : 20);
            utf8_str_pad(display_title, sizeof(display_title), truncated_title, title_width > 0 ? title_width : 30);
            utf8_str_pad(display_artist, sizeof(display_artist), truncated_artist, artist_width > 0 ? artist_width : 20);

            char line[MAX_META_LEN * 2 + 64];
            snprintf(line, sizeof(line), " %s - %s", display_title, display_artist);

            if (idx == g_content_selected_idx && g_focus_area == FOCUS_CONTENT) {
                attron(A_REVERSE);
                menu_content_puts(&area, start_y + i, line);
                attroff(A_REVERSE);
            } else {
                menu_content_puts(&area, start_y + i, line);
            }
        }

        int bottom_y = max_y - 3;
        menu_content_puts(&area, bottom_y, i18n_get("favorites.hint_manage"));
    }

    attroff(COLOR_PAIR(COLOR_PAIR_LYRICS));
    refresh();
}

/* ============================================================
 * Favorites view input handling
 * ============================================================ */

/* 动作入口：键盘解析出的动作与鼠标滚轮/双击共用同一条路径。
 * （鼠标绝不合成按键码：用户可改键，合成按键会在改键后失灵或做错事。） */
int handle_favorites_action(ActionId action)
{
    switch (action) {
        case ACTION_FAVORITES_UP:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx--;
                if (g_menu_selected_idx < 0) g_menu_selected_idx = FAVORITES_ITEM_COUNT - 1;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx--;
                if (g_content_selected_idx < 0) g_content_selected_idx = g_favorites.count - 1;
                if (g_content_selected_idx < 0) g_content_selected_idx = 0;
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_DOWN:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_menu_selected_idx++;
                if (g_menu_selected_idx >= FAVORITES_ITEM_COUNT) g_menu_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
            } else {
                g_content_selected_idx++;
                if (g_content_selected_idx >= g_favorites.count) g_content_selected_idx = 0;
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_RIGHT:
            if (g_focus_area == FOCUS_SIDEBAR) {
                g_focus_area = FOCUS_CONTENT;
                g_content_selected_idx = 0;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_LEFT:
            if (g_focus_area == FOCUS_CONTENT) {
                g_focus_area = FOCUS_SIDEBAR;
                render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                render_favorites_content();
            }
            return 1;

        case ACTION_FAVORITES_ACTIVATE:
            if (g_focus_area == FOCUS_SIDEBAR) {
                if (g_menu_selected_idx == 0) {
                    g_focus_area = FOCUS_CONTENT;
                    g_content_selected_idx = 0;
                    render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
                    render_favorites_content();
                } else if (g_menu_selected_idx == FAVORITES_ITEM_COUNT - 1) {
                    exit_current_view();
                }
            } else {
                if (g_favorites.count > 0 && g_content_selected_idx >= 0 &&
                    g_content_selected_idx < g_favorites.count) {

                    Track *t = &g_favorites.tracks[g_content_selected_idx];
                    int found = playlist_find_track_index_by_path(t->path);

                    /* 直播不在当前列表的曲目（Task 5.2）：追加到前端内容列表尾部，
                     * 再经 ui_play_track_index 反查队列位置播放（其内部会在需要时
                     * 整表重推给核心）；复用既有内容→队列桥，不新增核心方法。 */
                    if (found < 0 && append_playlist(t->path) > 0) {
                        found = playlist_find_track_index_by_path(t->path);
                    }

                    if (found >= 0) {
                        ui_play_track_index(found);
                        exit_current_view();
                    } else {
                        show_status_message(i18n_get("playlist_mgr.track_not_in_queue"));
                    }
                }
            }
            return 1;

        case ACTION_FAVORITES_REMOVE:
            if (g_focus_area == FOCUS_CONTENT && g_favorites.count > 0 &&
                g_content_selected_idx >= 0 && g_content_selected_idx < g_favorites.count) {

                remove_from_favorites(g_content_selected_idx);
                if (g_content_selected_idx >= g_favorites.count) {
                    g_content_selected_idx = g_favorites.count - 1;
                }
                show_status_message(i18n_get("favorites.removed"));
                render_favorites_content();
            }
            return 1;

        default:
            return 0;
    }
}

int handle_favorites_input(int ch)
{
    return handle_favorites_action(keymap_resolve(CTX_FAVORITES, ch));
}

/* ============================================================
 * Favorites view mouse handling
 * ============================================================ */

/* 内容区某屏幕行对应哪一条收藏；不在列表上返回 -1。 */
static int favorites_index_at_row(int screen_row)
{
    if (g_favorites_list_top < 0 || g_favorites_list_visible <= 0) return -1;

    int line = screen_row - g_favorites_list_top;
    if (line < 0 || line >= g_favorites_list_visible) return -1;

    int idx = g_favorites_content_offset + line;
    if (idx < 0 || idx >= g_favorites.count) return -1;
    return idx;
}

/* ============================================================
 * Right-click context menu
 * ============================================================ */

void favorites_context_menu_build(UIContextMenuLevel *level,
                                  const UIContextMenuContext *ctx)
{
    if (!level || !ctx) {
        return;
    }
    ui_ctx_menu_set_title(level, i18n_get("keyaction.view.favorites"));

    ui_ctx_menu_add_action(level, ACTION_FAVORITES_ACTIVATE, ctx->valid, 0);
    if (ctx->area == UI_CTX_AREA_CONTENT) {
        ui_ctx_menu_add_action(level, ACTION_FAVORITES_REMOVE, ctx->valid, 0);
    }
    ui_ctx_menu_add_separator(level);
    ui_context_menu_add_pages_submenu(level);
}

int favorites_context_dispatch(ActionId action, const UIContextMenuContext *ctx)
{
    /* 侧栏 ACTIVATE（全部收藏/返回）不指向行，不能按收藏下标校验。 */
    const int row_action =
        (action == ACTION_FAVORITES_REMOVE ||
         (action == ACTION_FAVORITES_ACTIVATE && ctx &&
          ctx->area == UI_CTX_AREA_CONTENT));
    if (row_action &&
        (!ctx || !ctx->valid || ctx->index < 0 ||
         ctx->index >= g_favorites.count)) {
        ui_context_menu_report_stale("favorites");
        return 1;
    }
    return handle_favorites_action(action);
}

static void favorites_open_context_menu(const UIMouseEvent *ev,
                                        const UIMouseDecision *d)
{
    UIContextMenuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view = VIEW_FAVORITES;
    ctx.target = d->target;

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        ctx.area = UI_CTX_AREA_SIDEBAR;
        if (d->row >= 0 && d->row < FAVORITES_ITEM_COUNT) {
            g_focus_area = FOCUS_SIDEBAR;
            g_menu_selected_idx = d->row;
            ctx.valid = 1;
            ctx.index = d->row;
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items,
                                FAVORITES_ITEM_COUNT);
            render_favorites_content();
        }
    } else {
        ctx.area = UI_CTX_AREA_CONTENT;
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items,
                                FAVORITES_ITEM_COUNT);
        }
        int idx = favorites_index_at_row(ev->y);
        if (idx >= 0) {
            g_content_selected_idx = idx;
            ctx.valid = 1;
            ctx.index = idx;
            render_favorites_content();
        }
    }

    (void)ui_context_menu_open(favorites_context_menu_build,
                               favorites_context_dispatch,
                               &ctx, ev->y, ev->x);
}

int favorites_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        favorites_open_context_menu(ev, d);
        return 1;
    }

    if (d->target == UI_MOUSE_TARGET_SIDEBAR) {
        if (d->row < 0 || d->row >= FAVORITES_ITEM_COUNT) return 1;

        /* 滚轮落在侧栏这一列：移动侧栏选中项。动作内部按 g_focus_area 分流，
         * 所以先把焦点带到侧栏（否则滚的是内容列）。 */
        if (ev->wheel) {
            g_focus_area = FOCUS_SIDEBAR;
            return handle_favorites_action(ev->wheel < 0 ? ACTION_FAVORITES_UP
                                                         : ACTION_FAVORITES_DOWN);
        }
        if (!ev->click) return 1;

        const int already_selected = (g_menu_selected_idx == d->row);
        g_focus_area = FOCUS_SIDEBAR;
        g_menu_selected_idx = d->row;
        render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
        render_favorites_content();

        /* 双击或"再点一次已选中的那条"等同回车：进入列表 / 返回主界面 */
        if (already_selected || ev->click_count >= 2) {
            return handle_favorites_action(ACTION_FAVORITES_ACTIVATE);
        }
        return 1;
    }

    /* 内容区 */
    if (ev->wheel) {
        /* 收藏列表的高亮只在焦点位于内容区时画得出来，
         * 所以滚轮要先把焦点带到指针所在这一列，再走键盘那条上下动作。 */
        if (g_focus_area != FOCUS_CONTENT) {
            g_focus_area = FOCUS_CONTENT;
            render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
        }
        return handle_favorites_action(ev->wheel < 0 ? ACTION_FAVORITES_UP
                                                     : ACTION_FAVORITES_DOWN);
    }
    if (!ev->click) return 1;

    /* 收藏列表几何记的是**屏幕行**（menu_content_puts 的行号就是 stdscr 行号），
     * 而 d->row 是内容区矩形的本地行号（该矩形 top=1），两者差一行，
     * 所以这里用 ev->y；侧栏那一路才用 d->row（侧栏 top=2 与 start_y 同源）。 */
    const int idx = favorites_index_at_row(ev->y);
    if (idx < 0) return 1;   /* 标题行、分隔线、底部提示行：只认领不动作 */

    const int already_selected = (g_content_selected_idx == idx);
    /* 把焦点带到指针这一列：一是选中高亮只在焦点为内容区时才画得出来，
     * 二是 ACTION_FAVORITES_ACTIVATE 自己按 g_focus_area 分流，
     * 焦点若还留在侧栏，双击会去执行侧栏那一条（进入列表/返回）而不是收藏项。 */
    if (g_focus_area != FOCUS_CONTENT) {
        g_focus_area = FOCUS_CONTENT;
        render_menu_sidebar(g_menu_selected_idx, favorites_sidebar_items, FAVORITES_ITEM_COUNT);
    }
    g_content_selected_idx = idx;
    render_favorites_content();

    if (already_selected || ev->click_count >= 2) {
        return handle_favorites_action(ACTION_FAVORITES_ACTIVATE);
    }
    return 1;
}
