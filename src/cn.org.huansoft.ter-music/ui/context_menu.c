/**
 * @file context_menu.c
 * @brief 右键上下文菜单引擎：多层栈、渲染、输入、生命周期与竞态
 *
 * 纯几何推导在 ui/context_menu_layout.c；本文件只做条目模型、窗口、
 * 选中/滚动、执行与关闭后的界面恢复。菜单不阻塞事件循环：每轮由
 * ui_context_menu_refresh() 重新合成到最上层（含流光合成）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/context_menu.h"

#include "i18n/i18n.h"
#include "keymap/actions.h"
#include "logger/logger.h"
#include "ui/context_menu_layout.h"
#include "ui/keymap.h"
#include "ui/layout_editor.h"
#include "ui/menu_anim.h"
#include "ui/menu_internal.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "util/utf8.h"

#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>

#define CTX_MENU_PAIR COLOR_PAIR(COLOR_PAIR_HIGHLIGHT)

typedef struct {
    int active;
    ViewMode view;
    UIContextMenuContext ctx;
    UIContextMenuDispatch dispatch;
    int level_count;
    UIContextMenuLevel levels[UI_CTX_MAX_LEVELS];
    WINDOW *wins[UI_CTX_MAX_LEVELS];
} ContextMenuState;

static ContextMenuState g_ctx_menu;

/* ============================================================
 * 小工具
 * ============================================================ */

static const char *action_id_text(ActionId action)
{
    const KeyActionDef *def = actions_def(action);
    return (def && def->id) ? def->id : "?";
}

static void copy_text(char *dest, size_t size, const char *src)
{
    if (!dest || size == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, size, "%s", src);
}

static void fit_text(char *dest, size_t size, const char *src, int cols)
{
    if (!dest || size == 0) {
        return;
    }
    if (cols <= 0) {
        dest[0] = '\0';
        return;
    }
    format_display_text(dest, size, src ? src : "", cols, 0);
}

static int item_selectable(const UIContextMenuItem *item)
{
    /* 分隔线与禁用项都不是可选项：键盘/滚轮必须跳过，
     * 否则光标会停在一个按 Enter 也没反应的条目上。 */
    return item && item->kind != UI_CTX_ITEM_SEPARATOR && item->enabled;
}

static int first_selectable(const UIContextMenuLevel *level)
{
    for (int i = 0; i < level->count; i++) {
        if (item_selectable(&level->items[i])) {
            return i;
        }
    }
    return -1;
}

/* 在分隔线之间上下移动；到边界就停住（与现有 popup 的钳制语义一致）。 */
static void move_selection(UIContextMenuLevel *level, int delta)
{
    if (!level || level->count <= 0 || delta == 0) {
        return;
    }
    int index = level->selected;
    for (int step = 0; step < level->count; step++) {
        index += delta;
        if (index < 0 || index >= level->count) {
            return;
        }
        if (item_selectable(&level->items[index])) {
            level->selected = index;
            level->scroll = ui_ctx_menu_scroll_for(level->selected,
                                                   level->count,
                                                   level->visible,
                                                   level->scroll);
            return;
        }
    }
}

static int level_width(const UIContextMenuLevel *level)
{
    int width = 16;
    if (!level) {
        return width;
    }
    int title_w = utf8_str_width(level->title) + 6;
    if (title_w > width) {
        width = title_w;
    }
    /* 页脚提示也要放得下，否则窄菜单会把"Esc 关闭"截掉。 */
    int hint_w = utf8_str_width(i18n_get("context.hint")) + 6;
    if (hint_w > width) {
        width = hint_w;
    }
    for (int i = 0; i < level->count; i++) {
        if (level->items[i].kind == UI_CTX_ITEM_SEPARATOR) {
            continue;
        }
        /* 左右边框 + 勾选列 + 子菜单箭头列 + 左右内边距 */
        int w = utf8_str_width(level->items[i].label) + 8;
        if (w > width) {
            width = w;
        }
    }
    return width;
}

/* 分隔线用 UTF-8 的 ─ 逐格写入（与 rounded_box 同一套字符）。
 * 不用 ACS_HLINE：在 UTF-8 终端 + 非 xterm 字体下会退化成 'q'。 */
static void draw_separator(WINDOW *win, int row, int width)
{
    wattrset(win, CTX_MENU_PAIR | A_DIM);
    for (int x = 1; x < width - 1; x++) {
        mvwaddstr(win, row, x, "\xe2\x94\x80");   /* ─ */
    }
}

static int level_height_needed(const UIContextMenuLevel *level)
{
    return (level ? level->count : 0) + 3;   /* 上边框 + 条目 + 页脚 + 下边框 */
}

static void compute_visible(UIContextMenuLevel *level)
{
    if (!level) {
        return;
    }
    int visible = level->rect.rows - 3;
    if (visible < 1) {
        visible = 1;
    }
    if (visible > level->count) {
        visible = level->count;
    }
    level->visible = visible;
    level->scroll = ui_ctx_menu_scroll_for(level->selected, level->count,
                                           level->visible, level->scroll);
}

/* ============================================================
 * 条目填充（builder API）
 * ============================================================ */

static UIContextMenuItem *next_item(UIContextMenuLevel *level)
{
    if (!level || level->count >= UI_CTX_MAX_ITEMS) {
        return NULL;
    }
    UIContextMenuItem *item = &level->items[level->count++];
    memset(item, 0, sizeof(*item));
    item->enabled = 1;
    return item;
}

void ui_ctx_menu_set_title(UIContextMenuLevel *level, const char *title)
{
    if (!level) {
        return;
    }
    copy_text(level->title, sizeof(level->title), title);
}

void ui_ctx_menu_add_action(UIContextMenuLevel *level, ActionId action,
                            int enabled, int checked)
{
    if (!level || action == ACTION_NONE) {
        return;
    }
    UIContextMenuItem *item = next_item(level);
    if (!item) {
        return;
    }
    item->kind = UI_CTX_ITEM_ACTION;
    item->action = action;
    item->enabled = enabled ? 1 : 0;
    item->checked = checked ? 1 : 0;
    copy_text(item->label, sizeof(item->label), keymap_action_label(action));
    if (item->label[0] == '\0') {
        copy_text(item->label, sizeof(item->label), action_id_text(action));
    }
}

void ui_ctx_menu_add_action_label(UIContextMenuLevel *level, const char *label,
                                  ActionId action, int enabled, int checked)
{
    ui_ctx_menu_add_action(level, action, enabled, checked);
    if (!level || level->count <= 0) {
        return;
    }
    UIContextMenuItem *item = &level->items[level->count - 1];
    if (label && label[0]) {
        copy_text(item->label, sizeof(item->label), label);
    }
}

void ui_ctx_menu_add_callback(UIContextMenuLevel *level, const char *label,
                              const char *debug_name,
                              UIContextMenuCallback callback, intptr_t value,
                              int enabled, int checked)
{
    if (!level || !callback) {
        return;
    }
    UIContextMenuItem *item = next_item(level);
    if (!item) {
        return;
    }
    item->kind = UI_CTX_ITEM_CALLBACK;
    item->callback = callback;
    item->value = value;
    item->enabled = enabled ? 1 : 0;
    item->checked = checked ? 1 : 0;
    item->debug_name = debug_name;
    copy_text(item->label, sizeof(item->label), label);
}

void ui_ctx_menu_add_submenu(UIContextMenuLevel *level, const char *label,
                             UIContextMenuBuilder build, int enabled)
{
    if (!level || !build) {
        return;
    }
    UIContextMenuItem *item = next_item(level);
    if (!item) {
        return;
    }
    item->kind = UI_CTX_ITEM_SUBMENU;
    item->submenu = build;
    item->enabled = enabled ? 1 : 0;
    item->debug_name = "submenu";
    copy_text(item->label, sizeof(item->label), label);
}

void ui_ctx_menu_add_separator(UIContextMenuLevel *level)
{
    if (!level || level->count <= 0) {
        return;   /* 首条/连续分隔线没有意义 */
    }
    if (level->items[level->count - 1].kind == UI_CTX_ITEM_SEPARATOR) {
        return;
    }
    UIContextMenuItem *item = next_item(level);
    if (!item) {
        return;
    }
    item->kind = UI_CTX_ITEM_SEPARATOR;
    item->enabled = 0;
}

/* ============================================================
 * 公共页面跳转子菜单
 * ============================================================ */

static void pages_menu_build(UIContextMenuLevel *level,
                             const UIContextMenuContext *ctx)
{
    (void)ctx;
    ui_ctx_menu_set_title(level, i18n_get("context.menu.pages"));
    ui_ctx_menu_add_action(level, ACTION_VIEW_MAIN, 1, g_current_view == VIEW_MAIN);
    ui_ctx_menu_add_action(level, ACTION_VIEW_SETTINGS, 1, g_current_view == VIEW_SETTINGS);
    ui_ctx_menu_add_action(level, ACTION_VIEW_HISTORY, 1, g_current_view == VIEW_HISTORY);
    ui_ctx_menu_add_action(level, ACTION_VIEW_PLAYLIST, 1, g_current_view == VIEW_PLAYLIST);
    ui_ctx_menu_add_action(level, ACTION_VIEW_FAVORITES, 1, g_current_view == VIEW_FAVORITES);
    ui_ctx_menu_add_action(level, ACTION_VIEW_INFO, 1, g_current_view == VIEW_INFO);
    ui_ctx_menu_add_action(level, ACTION_VIEW_LANGUAGE, 1, g_current_view == VIEW_LANGUAGE);
    ui_ctx_menu_add_action(level, ACTION_VIEW_HELP, 1, g_current_view == VIEW_HELP);
}

void ui_context_menu_add_pages_submenu(UIContextMenuLevel *level)
{
    ui_ctx_menu_add_submenu(level, i18n_get("context.menu.pages"),
                            pages_menu_build, 1);
}

/* ============================================================
 * 窗口与绘制
 * ============================================================ */

static void destroy_windows(void)
{
    for (int i = 0; i < UI_CTX_MAX_LEVELS; i++) {
        if (g_ctx_menu.wins[i]) {
            /* 进入动画可能还在半途：先摘掉 borrowed 记录再销毁窗口，
             * 否则 registry 会留下悬空 WINDOW*。 */
            ui_menu_anim_cancel_window(g_ctx_menu.wins[i]);
            delwin(g_ctx_menu.wins[i]);
            g_ctx_menu.wins[i] = NULL;
        }
    }
}

static void draw_level(int index)
{
    UIContextMenuLevel *level = &g_ctx_menu.levels[index];
    WINDOW *win = g_ctx_menu.wins[index];
    if (!win) {
        return;
    }
    const int w = level->rect.cols;
    const int h = level->rect.rows;
    if (w < 4 || h < 4) {
        return;
    }

    wbkgd(win, (chtype)CTX_MENU_PAIR);
    werase(win);
    wattrset(win, CTX_MENU_PAIR);
    rounded_box(win);

    if (level->title[0]) {
        char title[UI_CTX_LABEL_MAX];
        fit_text(title, sizeof(title), level->title, w - 6);
        mvwprintw(win, 0, 2, "%s", title);
    }

    wattrset(win, CTX_MENU_PAIR | A_DIM);
    if (level->scroll > 0) {
        mvwaddch(win, 0, w - 3, '^');
    }
    if (level->scroll + level->visible < level->count) {
        mvwaddch(win, h - 1, w - 3, 'v');
    }

    for (int i = 0; i < level->visible; i++) {
        const int idx = level->scroll + i;
        if (idx >= level->count) {
            break;
        }
        const int row = 1 + i;
        const UIContextMenuItem *item = &level->items[idx];

        if (item->kind == UI_CTX_ITEM_SEPARATOR) {
            draw_separator(win, row, w);
            continue;
        }

        attr_t attr = (attr_t)CTX_MENU_PAIR;
        if (!item->enabled) {
            attr |= A_DIM;
        }
        if (idx == level->selected) {
            attr |= A_REVERSE;
        }
        wattrset(win, attr);
        mvwhline(win, row, 1, ' ', w - 2);

        if (item->checked) {
            mvwaddstr(win, row, 1, "\xe2\x9c\x93");   /* ✓ */
        }
        char label[UI_CTX_LABEL_MAX];
        fit_text(label, sizeof(label), item->label, w - 7);
        mvwprintw(win, row, 3, "%s", label);
        if (item->kind == UI_CTX_ITEM_SUBMENU) {
            mvwaddstr(win, row, w - 3, "\xe2\x96\xb8");   /* ▸ */
        }
    }

    wattrset(win, CTX_MENU_PAIR | A_DIM);
    mvwhline(win, h - 2, 1, ' ', w - 2);
    char hint[UI_CTX_LABEL_MAX];
    fit_text(hint, sizeof(hint), i18n_get("context.hint"), w - 4);
    mvwprintw(win, h - 2, 2, "%s", hint);

    wattrset(win, A_NORMAL);
}

static void render_refresh(void)
{
    for (int i = 0; i < g_ctx_menu.level_count; i++) {
        draw_level(i);
        if (g_ctx_menu.wins[i]) {
            (void)ui_refresh_window(g_ctx_menu.wins[i]);
        }
    }
}

static int create_window_for(int index)
{
    UIContextMenuLevel *level = &g_ctx_menu.levels[index];
    WINDOW *win = newwin(level->rect.rows, level->rect.cols,
                         level->rect.top, level->rect.left);
    if (!win) {
        log_error("context_menu", "newwin failed for menu level %d (%dx%d at %d,%d)",
                  index, level->rect.cols, level->rect.rows,
                  level->rect.top, level->rect.left);
        return 0;
    }
    g_ctx_menu.wins[index] = win;
    /* 菜单从指针方向对角线滑入；tier 为 OFF 或起终点相同时 begin_enter
     * 返回 0，窗口留在最终位置，行为与旧版一致。 */
    (void)ui_menu_anim_begin_enter(win, UI_MENU_ANIM_DIAGONAL,
                                   level->rect.top, level->rect.left);
    return 1;
}

static void log_level(int index)
{
    const UIContextMenuLevel *level = &g_ctx_menu.levels[index];
    log_debug("context_menu",
              "level=%d title='%s' rect=%d,%d,%d,%d items=%d scroll=%d",
              index, level->title, level->rect.top, level->rect.left,
              level->rect.rows, level->rect.cols, level->count, level->scroll);
    for (int i = 0; i < level->count; i++) {
        const UIContextMenuItem *item = &level->items[i];
        if (item->kind == UI_CTX_ITEM_SEPARATOR) {
            log_debug("context_menu", "level=%d idx=%d separator", index, i);
        } else if (item->kind == UI_CTX_ITEM_ACTION) {
            log_debug("context_menu",
                      "level=%d idx=%d action=%s enabled=%d checked=%d",
                      index, i, action_id_text(item->action),
                      item->enabled, item->checked);
        } else if (item->kind == UI_CTX_ITEM_CALLBACK) {
            log_debug("context_menu",
                      "level=%d idx=%d callback=%s enabled=%d checked=%d",
                      index, i, item->debug_name ? item->debug_name : "callback",
                      item->enabled, item->checked);
        } else {
            log_debug("context_menu", "level=%d idx=%d submenu=%s enabled=%d",
                      index, i, item->label, item->enabled);
        }
    }
}

/* ============================================================
 * 构建/放置/下钻
 * ============================================================ */

static int build_level(UIContextMenuLevel *level,
                       UIContextMenuBuilder build,
                       const UIContextMenuContext *ctx)
{
    memset(level, 0, sizeof(*level));
    build(level, ctx);
    level->selected = first_selectable(level);
    if (level->count <= 0 || level->selected < 0) {
        return 0;
    }
    level->scroll = ui_ctx_menu_scroll_for(level->selected, level->count, 1, 0);
    return 1;
}

static int place_root_level(int anchor_y, int anchor_x)
{
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    UIContextMenuLevel *level = &g_ctx_menu.levels[0];
    int w = level_width(level);
    int h = level_height_needed(level);
    if (!ui_ctx_menu_place_rect(anchor_y, anchor_x, w, h, rows, cols, &level->rect)) {
        log_debug("context_menu", "refused: terminal too small (%dx%d)", rows, cols);
        return 0;
    }
    compute_visible(level);
    return 1;
}

static int push_submenu(void)
{
    if (g_ctx_menu.level_count >= UI_CTX_MAX_LEVELS) {
        log_debug("context_menu", "submenu refused: max depth %d",
                  UI_CTX_MAX_LEVELS);
        return 0;
    }
    const int parent_index = g_ctx_menu.level_count - 1;
    const UIContextMenuLevel *parent = &g_ctx_menu.levels[parent_index];
    if (parent->selected < 0 || parent->selected >= parent->count) {
        return 0;
    }
    const UIContextMenuItem *item = &parent->items[parent->selected];
    if (item->kind != UI_CTX_ITEM_SUBMENU || !item->submenu || !item->enabled) {
        return 0;
    }

    UIContextMenuLevel next;
    if (!build_level(&next, item->submenu, &g_ctx_menu.ctx)) {
        return 0;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    int w = level_width(&next);
    int h = level_height_needed(&next);
    const int item_row = parent->rect.top + 1 +
                         (parent->selected - parent->scroll);
    if (!ui_ctx_menu_place_submenu(&parent->rect, item_row, w, h,
                                   rows, cols, &next.rect)) {
        return 0;
    }
    compute_visible(&next);

    g_ctx_menu.levels[g_ctx_menu.level_count] = next;
    const int index = g_ctx_menu.level_count;
    if (!create_window_for(index)) {
        return 0;
    }
    g_ctx_menu.level_count++;
    log_debug("context_menu", "push depth=%d", g_ctx_menu.level_count);
    log_level(index);
    render_refresh();
    return 1;
}

static void pop_submenu(void)
{
    if (g_ctx_menu.level_count <= 1) {
        ui_context_menu_close(1);
        return;
    }
    const int index = g_ctx_menu.level_count - 1;
    if (g_ctx_menu.wins[index]) {
        /* 子菜单退场：复制一份 ghost 滑回父层，再销毁原窗口。 */
        (void)ui_menu_anim_begin_exit(g_ctx_menu.wins[index],
                                      UI_MENU_ANIM_DIAGONAL, index);
        ui_menu_anim_cancel_window(g_ctx_menu.wins[index]);
        delwin(g_ctx_menu.wins[index]);
        g_ctx_menu.wins[index] = NULL;
    }
    memset(&g_ctx_menu.levels[index], 0, sizeof(g_ctx_menu.levels[index]));
    g_ctx_menu.level_count--;
    log_debug("context_menu", "pop depth=%d", g_ctx_menu.level_count);
    render_refresh();
}

/* ============================================================
 * 执行与关闭
 * ============================================================ */

static void repaint_underlying(void)
{
    if (layout_editor_active()) {
        layout_editor_render();
        return;   /* 编辑器全屏绘制，不能再用页面提示条覆盖 */
    }
    rerender_active_view();
    render_menu_hint_bar();
}

static void activate_current(void)
{
    if (g_ctx_menu.level_count <= 0) {
        return;
    }
    UIContextMenuLevel *level = &g_ctx_menu.levels[g_ctx_menu.level_count - 1];
    if (level->selected < 0 || level->selected >= level->count) {
        return;
    }
    UIContextMenuItem *item = &level->items[level->selected];
    if (!item->enabled || item->kind == UI_CTX_ITEM_SEPARATOR) {
        return;
    }
    if (item->kind == UI_CTX_ITEM_SUBMENU) {
        (void)push_submenu();
        return;
    }

    /* 先复制条目与上下文，再关闭菜单：回调/动作里可能切视图、开阻塞输入框，
     * 绝不能让它们看到已释放的菜单状态。 */
    UIContextMenuItem action = *item;
    UIContextMenuContext ctx = g_ctx_menu.ctx;
    UIContextMenuDispatch dispatch = g_ctx_menu.dispatch;

    ui_context_menu_close(0);

    if (action.kind == UI_CTX_ITEM_CALLBACK) {
        log_debug("context_menu", "callback=%s",
                  action.debug_name ? action.debug_name : "callback");
        if (action.callback) {
            action.callback(&ctx, action.value);
        }
    } else if (action.kind == UI_CTX_ITEM_ACTION) {
        log_debug("context_menu", "action=%s", action_id_text(action.action));
        int handled = dispatch ? dispatch(action.action, &ctx) : 0;
        if (!handled) {
            execute_global_action(action.action);
        }
    }

    repaint_underlying();
}

void ui_context_menu_close(int repaint)
{
    if (!g_ctx_menu.active) {
        return;
    }

    /* 右边菜单逐层退场：先为每层复制 ghost（按层号登记保证 z 序），
     * 再统一销毁原窗口。tier 为 OFF 时 begin_exit 返回 0，等价于旧版
     * 瞬时关闭。 */
    int animated = 0;
    for (int i = 0; i < g_ctx_menu.level_count; i++) {
        if (g_ctx_menu.wins[i]) {
            animated |= ui_menu_anim_begin_exit(g_ctx_menu.wins[i],
                                                UI_MENU_ANIM_DIAGONAL, i);
        }
    }

    destroy_windows();
    g_ctx_menu.active = 0;
    g_ctx_menu.level_count = 0;
    log_debug("context_menu", "close repaint=%d animated=%d",
              repaint ? 1 : 0, animated);

    if (repaint) {
        repaint_underlying();
    }
}

void ui_context_menu_close_now(int repaint)
{
    if (!g_ctx_menu.active) {
        return;
    }
    destroy_windows();
    g_ctx_menu.active = 0;
    g_ctx_menu.level_count = 0;
    log_debug("context_menu", "close-now repaint=%d", repaint ? 1 : 0);
    if (repaint) {
        repaint_underlying();
    }
}

/* ============================================================
 * 打开与刷新
 * ============================================================ */

int ui_context_menu_open(UIContextMenuBuilder build,
                         UIContextMenuDispatch dispatch,
                         const UIContextMenuContext *ctx,
                         int anchor_y, int anchor_x)
{
    if (!build || !ctx) {
        return 0;
    }
    /* 新 overlay 打开前把上一轮退场 ghost 收干净，避免两层菜单叠影。 */
    ui_menu_anim_finish_exits(1);
    if (g_ctx_menu.active) {
        ui_context_menu_close_now(0);
    }

    memset(&g_ctx_menu, 0, sizeof(g_ctx_menu));
    g_ctx_menu.ctx = *ctx;
    g_ctx_menu.view = ctx->view;
    g_ctx_menu.dispatch = dispatch;

    if (!build_level(&g_ctx_menu.levels[0], build, &g_ctx_menu.ctx)) {
        log_debug("context_menu", "refused: empty menu");
        return 0;
    }
    if (!place_root_level(anchor_y, anchor_x)) {
        return 0;
    }
    if (!create_window_for(0)) {
        return 0;
    }

    g_ctx_menu.level_count = 1;
    g_ctx_menu.active = 1;
    log_debug("context_menu",
              "open view=%d target=%d area=%d valid=%d anchor=%d,%d rect=%d,%d,%d,%d",
              (int)ctx->view, (int)ctx->target, (int)ctx->area, ctx->valid,
              anchor_y, anchor_x,
              g_ctx_menu.levels[0].rect.top, g_ctx_menu.levels[0].rect.left,
              g_ctx_menu.levels[0].rect.rows, g_ctx_menu.levels[0].rect.cols);
    log_level(0);
    render_refresh();
    return 1;
}

int ui_context_menu_active(void)
{
    return g_ctx_menu.active &&
           g_current_view == g_ctx_menu.view &&
           !layout_editor_active();
}

void ui_context_menu_refresh(void)
{
    if (!g_ctx_menu.active) {
        return;
    }
    if (!ui_context_menu_active()) {
        /* 视图/编辑器已经变了：界面变更不播消失动画。 */
        ui_context_menu_close_now(1);
        return;
    }
    if (ui_menu_anim_active()) {
        /* 动画帧由 ui_menu_anim_frame() 统一合成，别在这里另刷一帧。 */
        return;
    }
    render_refresh();
}

/* ============================================================
 * 输入
 * ============================================================ */

static void ensure_top_visible(void)
{
    UIContextMenuLevel *level = &g_ctx_menu.levels[g_ctx_menu.level_count - 1];
    level->scroll = ui_ctx_menu_scroll_for(level->selected, level->count,
                                           level->visible, level->scroll);
}

int ui_context_menu_handle_key(int ch)
{
    if (!g_ctx_menu.active) {
        return 0;
    }
    UIContextMenuLevel *level = &g_ctx_menu.levels[g_ctx_menu.level_count - 1];

    if (ch == 27) {   /* Esc：关闭全部层级 */
        ui_context_menu_close(1);
        return 1;
    }

    const ActionId action = keymap_resolve(CTX_POPUP, ch);
    if (action == ACTION_POPUP_CANCEL) {
        pop_submenu();
        return 1;
    }
    if (action == ACTION_POPUP_UP || ch == KEY_UP) {
        move_selection(level, -1);
        log_debug("context_menu", "key up -> selected=%d", level->selected);
        draw_level(g_ctx_menu.level_count - 1);
        (void)ui_refresh_window(g_ctx_menu.wins[g_ctx_menu.level_count - 1]);
        return 1;
    }
    if (action == ACTION_POPUP_DOWN || ch == KEY_DOWN) {
        move_selection(level, 1);
        log_debug("context_menu", "key down -> selected=%d", level->selected);
        draw_level(g_ctx_menu.level_count - 1);
        (void)ui_refresh_window(g_ctx_menu.wins[g_ctx_menu.level_count - 1]);
        return 1;
    }
    if (ch == KEY_RIGHT) {
        if (level->selected >= 0 && level->selected < level->count &&
            level->items[level->selected].kind == UI_CTX_ITEM_SUBMENU) {
            (void)push_submenu();
        }
        return 1;
    }
    if (action == ACTION_POPUP_ACCEPT || ch == '\n' || ch == '\r' ||
        ch == KEY_ENTER) {
        activate_current();
        return 1;
    }

    /* 未识别的键：关闭并吞掉，避免误触背后界面。 */
    ui_context_menu_close(1);
    return 1;
}

int ui_context_menu_handle_mouse(const UIMouseEvent *ev,
                                 const UIMouseDecision *d)
{
    if (!g_ctx_menu.active) {
        return 0;
    }
    if (ev->right_click) {
        ui_context_menu_close(1);
        return 1;
    }
    if (ev->wheel) {
        UIContextMenuLevel *level = &g_ctx_menu.levels[g_ctx_menu.level_count - 1];
        move_selection(level, ev->wheel);
        ensure_top_visible();
        draw_level(g_ctx_menu.level_count - 1);
        (void)ui_refresh_window(g_ctx_menu.wins[g_ctx_menu.level_count - 1]);
        return 1;
    }
    if (!ev->click) {
        return 1;
    }
    if (!d || d->row < 0 || !d->inside_overlay) {
        ui_context_menu_close(1);
        return 1;
    }

    UIContextMenuLevel *level = &g_ctx_menu.levels[g_ctx_menu.level_count - 1];
    const int idx = ui_ctx_menu_item_index(d->row, level->visible,
                                           level->scroll, level->count);
    if (idx < 0) {
        return 1;   /* 标题/页脚/边框：只认领 */
    }
    if (!item_selectable(&level->items[idx])) {
        return 1;   /* 分隔线/禁用项：只认领，不选中 */
    }

    if (idx != level->selected) {
        level->selected = idx;
        ensure_top_visible();
    }
    /* 单击即执行：动作项直接运行，子菜单项直接进入下一层。
     * 不在这里先画一帧“只选中”的状态——那会让一次点击变成两段语义。 */
    activate_current();
    return 1;
}

int ui_context_menu_rect(UIRect *out)
{
    if (!ui_context_menu_active() || !out || g_ctx_menu.level_count <= 0) {
        return 0;
    }
    *out = g_ctx_menu.levels[g_ctx_menu.level_count - 1].rect;
    return 1;
}

int ui_context_menu_level_count(void)
{
    return g_ctx_menu.active ? g_ctx_menu.level_count : 0;
}

void *ui_context_menu_window_at(int level)
{
    if (!g_ctx_menu.active || level < 0 || level >= g_ctx_menu.level_count) {
        return NULL;
    }
    return (void *)g_ctx_menu.wins[level];
}

int ui_context_menu_owns_window(const void *win)
{
    if (!g_ctx_menu.active || !win) {
        return 0;
    }
    for (int i = 0; i < g_ctx_menu.level_count; i++) {
        if ((const void *)g_ctx_menu.wins[i] == win) {
            return 1;
        }
    }
    return 0;
}

int ui_context_menu_intersects_rect(int top, int left, int rows, int cols)
{
    if (!g_ctx_menu.active || rows <= 0 || cols <= 0) {
        return 0;
    }
    const UIRect rect = { .top = top, .left = left,
                          .rows = rows, .cols = cols };
    for (int i = 0; i < g_ctx_menu.level_count; i++) {
        WINDOW *win = g_ctx_menu.wins[i];
        if (!win) {
            continue;
        }
        /* 动画期间菜单窗口会被 mvwin 移走；相交判定必须用**当前**窗口
         * 矩形，否则窗格刷新会在移动路径上留下旧像素。 */
        UIRect current;
        getbegyx(win, current.top, current.left);
        getmaxyx(win, current.rows, current.cols);
        if (ui_rect_intersects(&rect, &current)) {
            return 1;
        }
    }
    return 0;
}

int ui_context_menu_reassert(void)
{
    if (!g_ctx_menu.active) {
        return 0;
    }
    int reasserted = 0;
    for (int i = 0; i < g_ctx_menu.level_count; i++) {
        WINDOW *win = g_ctx_menu.wins[i];
        if (!win) {
            continue;
        }
        /* 菜单窗口此前 refresh 过，touched 标志已被清空；不 touchwin
         * 的话第二次 wnoutrefresh 不会把 backing 压回虚拟屏。 */
        touchwin(win);
        if (wnoutrefresh(win) != ERR) {
            reasserted++;
        }
    }
    return reasserted;
}

void ui_context_menu_report_stale(const char *what)
{
    log_debug("context_menu", "stale action=%s", what ? what : "?");
    if (g_current_view == VIEW_MAIN) {
        update_controls_status(i18n_get("context.stale"));
    } else {
        show_status_message(i18n_get("context.stale"));
    }
}
