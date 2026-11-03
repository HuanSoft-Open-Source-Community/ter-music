/**
 * @file layout_editor.c
 * @brief TUI 布局编辑器实现
 *
 * 编辑器只操作分栏树：分栏、删除、调比例、均分、换类型、交换都通过
 * layout_spec 的事务式操作完成，因此工作副本始终是强制平铺的。
 * 右侧实时预览与运行时使用同一套 resolve 结果，保存不再有几何校验门槛。
 *
 * 交互：方向键按预览几何选择相邻窗格，Tab 按树序循环；Enter 打开操作
 * 菜单，? 打开完整帮助，底部常驻上下文化按键提示。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/layout_editor.h"

#include "i18n/i18n.h"
#include "logger/logger.h"
#include "ui/dialog.h"
#include "ui/layout.h"
#include "ui/menu_internal.h"
#include "ui/ui.h"

#include <ncursesw/ncurses.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ED_MIN_COLS 60
#define ED_MIN_ROWS 16

typedef enum {
    ED_MODE_MAIN = 0,
    ED_MODE_ACTIONS,
    ED_MODE_KINDS,
    ED_MODE_COLORS,
    ED_MODE_HELP
} EdMode;

typedef enum {
    ED_PENDING_SPLIT = 0,
    ED_PENDING_CHANGE
} EdPending;

typedef struct {
    LayoutSpec spec;
    int active;
    int is_new;
    int dirty;
    int sel;                 /* 树序选中的窗格 */
    int esc_confirm;
    EdMode mode;
    int action_sel;
    EdPending pending;
    LayoutSplitDir pending_dir;
    int kind_sel;
    int color_field;
    char status[192];
    int status_error;
} LayoutEditorState;

static LayoutEditorState g_ed;

static const char *const k_action_keys[] = {
    "layout.editor.action_split_row",
    "layout.editor.action_split_col",
    "layout.editor.action_kind",
    "layout.editor.action_colors",
    "layout.editor.action_equalize",
    "layout.editor.action_swap",
    "layout.editor.action_remove",
    "layout.editor.action_rename",
};
#define ED_ACTION_COUNT ((int)(sizeof(k_action_keys) / sizeof(k_action_keys[0])))

static const char *const k_help_keys[] = {
    "layout.editor.help_nav",
    "layout.editor.help_ratio",
    "layout.editor.help_split",
    "layout.editor.help_edit",
    "layout.editor.help_colors",
    "layout.editor.help_save",
};
#define ED_HELP_COUNT ((int)(sizeof(k_help_keys) / sizeof(k_help_keys[0])))

/* ── 小工具 ────────────────────────────────────────────────────── */

static void editor_status_text(const char *text, int is_error)
{
    snprintf(g_ed.status, sizeof(g_ed.status), "%s", text ? text : "");
    g_ed.status_error = is_error ? 1 : 0;
}

static void editor_status_key(const char *key)
{
    editor_status_text(i18n_get(key), 0);
}

static void editor_status_error_key(const char *key)
{
    editor_status_text(i18n_get(key), 1);
}

static int editor_selected_kind(void)
{
    return layout_spec_pane_at(&g_ed.spec, g_ed.sel);
}

static int editor_kind_index(LayoutPaneKind kind)
{
    int count = layout_spec_pane_count(&g_ed.spec);
    for (int i = 0; i < count; i++) {
        if (layout_spec_pane_at(&g_ed.spec, i) == (int)kind) {
            return i;
        }
    }
    return -1;
}

static int editor_find_parent(int target, int *parent_out, int *slot_out)
{
    if (target == g_ed.spec.root) {
        return 0;
    }
    for (int i = 0; i < g_ed.spec.node_count; i++) {
        const LayoutNode *node = &g_ed.spec.nodes[i];
        if (node->is_leaf) {
            continue;
        }
        for (int slot = 0; slot < 2; slot++) {
            if (node->child[slot] == target) {
                if (parent_out) *parent_out = i;
                if (slot_out) *slot_out = slot;
                return 1;
            }
        }
    }
    return -1;
}

static int color_cycle_index(int value)
{
    if (value <= -2) return 0;
    if (value == -1) return 1;
    if (value > 255) return 255 + 2;
    return value + 2;
}

static int color_cycle_value(int index)
{
    int total = 258;    /* -2, -1, 0..255 */
    index = ((index % total) + total) % total;
    if (index == 0) return -2;
    if (index == 1) return -1;
    return index - 2;
}

static void format_color(char *buf, size_t size, int value)
{
    if (value <= -2) {
        snprintf(buf, size, "%s", i18n_get("layout.editor.color_inherit"));
    } else if (value == -1) {
        snprintf(buf, size, "%s", i18n_get("layout.editor.color_terminal"));
    } else {
        snprintf(buf, size, "%d", value);
    }
}

static int *color_field_ptr(LayoutPaneSpec *pane, int field)
{
    switch (field) {
        case 0: return &pane->fg;
        case 1: return &pane->bg;
        case 2: return &pane->border_fg;
        case 3: return &pane->border_bg;
        default: return NULL;
    }
}

static const char *color_field_name(int field)
{
    switch (field) {
        case 0: return "fg";
        case 1: return "bg";
        case 2: return "border_fg";
        case 3: return "border_bg";
        default: return "";
    }
}

static void editor_draw_text(int row, int col, int width, const char *text,
                             int selected)
{
    if (width <= 0 || row < 0 || col < 0) {
        return;
    }
    char buffer[256];
    format_display_text(buffer, sizeof(buffer), text ? text : "", width, 1);
    if (selected) {
        attron(A_REVERSE);
    }
    mvaddstr(row, col, buffer);
    if (selected) {
        attroff(A_REVERSE);
    }
}

static void editor_draw_box(int row, int col, int h, int w, const char *title)
{
    if (h < 3 || w < 4) {
        return;
    }
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            mvaddch(row + r, col + c, ' ');
        }
    }
    mvaddch(row, col, ACS_ULCORNER);
    mvaddch(row, col + w - 1, ACS_URCORNER);
    mvaddch(row + h - 1, col, ACS_LLCORNER);
    mvaddch(row + h - 1, col + w - 1, ACS_LRCORNER);
    for (int c = 1; c < w - 1; c++) {
        mvaddch(row, col + c, ACS_HLINE);
        mvaddch(row + h - 1, col + c, ACS_HLINE);
    }
    for (int r = 1; r < h - 1; r++) {
        mvaddch(row + r, col, ACS_VLINE);
        mvaddch(row + r, col + w - 1, ACS_VLINE);
    }
    if (title && title[0]) {
        char buffer[128];
        format_display_text(buffer, sizeof(buffer), title, w - 4, 0);
        attron(A_BOLD);
        mvaddstr(row, col + 2, buffer);
        attroff(A_BOLD);
    }
}

/* ── 预览 ──────────────────────────────────────────────────────── */

static void editor_draw_preview(int base_row, int base_col, int pw, int ph)
{
    if (pw < 8 || ph < 4) {
        return;
    }
    editor_draw_box(base_row, base_col, ph, pw, NULL);

    LayoutResolved resolved;
    if (layout_spec_resolve(&g_ed.spec, &resolved) != 0) {
        return;
    }
    int inner_w = pw - 2;
    int inner_h = ph - 2;
    int current_kind = editor_selected_kind();

    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutResolvedPane *pane = &resolved.panes[i];
        int c0 = base_col + 1 + pane->x * inner_w / LAYOUT_AREA_UNITS;
        int c1 = base_col + 1 +
                 (pane->x + pane->w) * inner_w / LAYOUT_AREA_UNITS;
        int r0 = base_row + 1 + pane->y * inner_h / LAYOUT_AREA_UNITS;
        int r1 = base_row + 1 +
                 (pane->y + pane->h) * inner_h / LAYOUT_AREA_UNITS;
        if (c1 <= c0) c1 = c0 + 1;
        if (r1 <= r0) r1 = r0 + 1;
        if (c1 > base_col + pw - 1) c1 = base_col + pw - 1;
        if (r1 > base_row + ph - 1) r1 = base_row + ph - 1;
        if (c0 >= c1 || r0 >= r1) {
            continue;
        }

        int selected = (int)pane->kind == current_kind;
        attr_t attr = selected ? A_BOLD : A_DIM;
        attron(attr);
        for (int c = c0; c < c1; c++) {
            mvaddch(r0, c, ACS_HLINE);
        }
        for (int r = r0; r < r1; r++) {
            mvaddch(r, c0, ACS_VLINE);
        }
        if (pane->y + pane->h >= LAYOUT_AREA_UNITS) {
            for (int c = c0; c < c1; c++) {
                mvaddch(r1 - 1, c, ACS_HLINE);
            }
        }
        if (pane->x + pane->w >= LAYOUT_AREA_UNITS) {
            for (int r = r0; r < r1; r++) {
                mvaddch(r, c1 - 1, ACS_VLINE);
            }
        }
        attroff(attr);

        if (r1 - r0 >= 3 && c1 - c0 >= 4) {
            char label[64];
            format_display_text(label, sizeof(label),
                                layout_pane_id_name(pane->kind),
                                c1 - c0 - 2, 0);
            if (selected) {
                attron(A_REVERSE | A_BOLD);
            }
            mvaddstr(r0 + 1, c0 + 1, label);
            if (selected) {
                attroff(A_REVERSE | A_BOLD);
            }
        }
    }
}

/* ── 动作 ──────────────────────────────────────────────────────── */

static void editor_refresh_sel(void)
{
    int count = layout_spec_pane_count(&g_ed.spec);
    if (count <= 0) {
        g_ed.sel = 0;
    } else if (g_ed.sel >= count) {
        g_ed.sel = count - 1;
    }
}

static void editor_after_change(const char *status_key)
{
    g_ed.dirty = 1;
    g_ed.esc_confirm = 0;
    editor_refresh_sel();
    editor_status_key(status_key);
}

static void editor_adjust_ratio(LayoutSplitDir dir, int delta)
{
    int kind = editor_selected_kind();
    if (kind < 0) {
        return;
    }
    int rc = layout_spec_adjust_ratio(&g_ed.spec, (LayoutPaneKind)kind,
                                      dir, delta);
    if (rc == -2) {
        editor_status_error_key("layout.editor.no_axis_split");
        return;
    }
    if (rc == -3) {
        editor_status_error_key("layout.editor.ratio_at_limit");
        return;
    }
    if (rc != 0) {
        editor_status_error_key("layout.editor.too_small_pane");
        return;
    }
    editor_after_change("layout.editor.status_ratio");
}

static void editor_equalize(void)
{
    int kind = editor_selected_kind();
    if (kind < 0) {
        return;
    }
    if (layout_spec_equalize_pane(&g_ed.spec, (LayoutPaneKind)kind) != 0) {
        editor_status_error_key("layout.editor.no_parent_split");
        return;
    }
    editor_after_change("layout.editor.status_equalized");
}

static void editor_remove(void)
{
    int kind = editor_selected_kind();
    if (kind < 0) {
        return;
    }
    if (layout_spec_remove_pane(&g_ed.spec, (LayoutPaneKind)kind) != 0) {
        editor_status_error_key("layout.editor.min_pane");
        return;
    }
    editor_after_change("layout.editor.status_removed");
}

static void editor_swap(void)
{
    int count = layout_spec_pane_count(&g_ed.spec);
    if (count < 2 || g_ed.sel < 0 || g_ed.sel >= count) {
        editor_status_error_key("layout.editor.status_swap_none");
        return;
    }
    int a = layout_spec_pane_at(&g_ed.spec, g_ed.sel);
    int next = (g_ed.sel + 1) % count;
    int b = layout_spec_pane_at(&g_ed.spec, next);
    if (a < 0 || b < 0 ||
        layout_spec_swap_panes(&g_ed.spec, (LayoutPaneKind)a,
                               (LayoutPaneKind)b) != 0) {
        editor_status_error_key("layout.editor.status_swap_none");
        return;
    }
    editor_after_change("layout.editor.status_swapped");
}

static void editor_open_kind_picker(EdPending pending, LayoutSplitDir dir)
{
    g_ed.pending = pending;
    g_ed.pending_dir = dir;
    g_ed.kind_sel = 0;
    g_ed.mode = ED_MODE_KINDS;
    g_ed.esc_confirm = 0;
}

static void editor_apply_kind_pick(LayoutPaneKind new_kind)
{
    int selected = editor_selected_kind();
    if (selected < 0) {
        return;
    }
    int rc;
    if (g_ed.pending == ED_PENDING_SPLIT) {
        rc = layout_spec_split_pane(&g_ed.spec, (LayoutPaneKind)selected,
                                    g_ed.pending_dir, new_kind);
    } else {
        rc = layout_spec_change_pane_kind(&g_ed.spec, (LayoutPaneKind)selected,
                                          new_kind);
    }
    if (rc != 0) {
        editor_status_error_key("layout.editor.too_small_pane");
        return;
    }
    g_ed.mode = ED_MODE_MAIN;
    int index = editor_kind_index(new_kind);
    if (index >= 0) {
        g_ed.sel = index;
    }
    editor_after_change(g_ed.pending == ED_PENDING_SPLIT
                            ? "layout.editor.status_added"
                            : "layout.editor.status_kind");
}

static void editor_open_colors(void)
{
    g_ed.color_field = 0;
    g_ed.mode = ED_MODE_COLORS;
    g_ed.esc_confirm = 0;
}

static void editor_rename(void)
{
    char name[LAYOUT_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_ed.spec.name);
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    if (prompt_text_input(stdscr, rows - 2, 2,
                          i18n_get("layout.editor.rename_prompt"),
                          name, sizeof(name), 1, 0, 1) == 0) {
        snprintf(g_ed.spec.name, sizeof(g_ed.spec.name), "%s", name);
        g_ed.spec.name_key[0] = '\0';
        g_ed.dirty = 1;
    }
    g_ed.mode = ED_MODE_MAIN;
    layout_editor_render();
}

static void editor_save(void)
{
    log_debug("layout_editor", "Save requested for '%s' (%d panes)",
              g_ed.spec.id, layout_spec_pane_count(&g_ed.spec));
    char err[192] = "";
    if (layout_spec_validate(&g_ed.spec, err, sizeof(err)) != 0) {
        /* 正常编辑不可能触发；保留错误而不是静默保存坏树。 */
        log_warn("layout_editor", "Refusing to save invalid layout: %s",
                 err[0] ? err : "unknown");
        editor_status_text(err[0] ? err
                                  : i18n_get("layout.editor.internal_error"), 1);
        return;
    }
    char save_err[256] = "";
    if (layout_user_save_layout(&g_ed.spec, save_err, sizeof(save_err)) != 0) {
        log_warn("layout_editor", "Save failed: %s",
                 save_err[0] ? save_err : "unknown");
        editor_status_text(save_err[0] ? save_err
                                       : i18n_get("layout.editor.save_failed"), 1);
        return;
    }
    ui_layout_reload();
    ui_layout_select(g_ed.spec.id);
    g_ed.active = 0;
    show_status_message(i18n_get("layout.editor.saved"));
}

static void editor_request_close(void)
{
    if (g_ed.dirty && !g_ed.esc_confirm) {
        g_ed.esc_confirm = 1;
        editor_status_error_key("layout.editor.dirty_exit");
        return;
    }
    g_ed.active = 0;
}

/* ── 空间导航 ──────────────────────────────────────────────────── */

static void editor_move_focus(int dx, int dy)
{
    LayoutResolved resolved;
    if (layout_spec_resolve(&g_ed.spec, &resolved) != 0) {
        return;
    }
    int current_kind = editor_selected_kind();
    const LayoutResolvedPane *cur = NULL;
    for (int i = 0; i < resolved.pane_count; i++) {
        if ((int)resolved.panes[i].kind == current_kind) {
            cur = &resolved.panes[i];
            break;
        }
    }
    if (!cur) {
        return;
    }

    int best = -1;
    long best_score = 0;
    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutResolvedPane *pane = &resolved.panes[i];
        if ((int)pane->kind == current_kind) {
            continue;
        }
        int overlap = 0;
        int distance = 0;
        if (dy < 0) {
            if (pane->y + pane->h > cur->y) continue;
            distance = cur->y - (pane->y + pane->h);
            int left = cur->x > pane->x ? cur->x : pane->x;
            int right = (cur->x + cur->w) < (pane->x + pane->w)
                            ? (cur->x + cur->w) : (pane->x + pane->w);
            overlap = right - left;
        } else if (dy > 0) {
            if (pane->y < cur->y + cur->h) continue;
            distance = pane->y - (cur->y + cur->h);
            int left = cur->x > pane->x ? cur->x : pane->x;
            int right = (cur->x + cur->w) < (pane->x + pane->w)
                            ? (cur->x + cur->w) : (pane->x + pane->w);
            overlap = right - left;
        } else if (dx < 0) {
            if (pane->x + pane->w > cur->x) continue;
            distance = cur->x - (pane->x + pane->w);
            int top = cur->y > pane->y ? cur->y : pane->y;
            int bottom = (cur->y + cur->h) < (pane->y + pane->h)
                             ? (cur->y + cur->h) : (pane->y + pane->h);
            overlap = bottom - top;
        } else {
            if (pane->x < cur->x + cur->w) continue;
            distance = pane->x - (cur->x + cur->w);
            int top = cur->y > pane->y ? cur->y : pane->y;
            int bottom = (cur->y + cur->h) < (pane->y + pane->h)
                             ? (cur->y + cur->h) : (pane->y + pane->h);
            overlap = bottom - top;
        }
        if (overlap <= 0) {
            continue;
        }
        long score = (long)distance * 10000L - (long)overlap;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best >= 0) {
        int index = editor_kind_index((LayoutPaneKind)resolved.panes[best].kind);
        if (index >= 0) {
            g_ed.sel = index;
        }
    }
}

/* ── 模态渲染 ──────────────────────────────────────────────────── */

static void editor_draw_actions(int rows, int cols)
{
    int w = 44;
    int h = ED_ACTION_COUNT + 4;
    if (w > cols - 4) w = cols - 4;
    if (h > rows - 4) h = rows - 4;
    int row = (rows - h) / 2;
    int col = (cols - w) / 2;
    editor_draw_box(row, col, h, w, i18n_get("layout.editor.actions_title"));
    for (int i = 0; i < ED_ACTION_COUNT; i++) {
        if (row + 1 + i >= row + h - 1) break;
        editor_draw_text(row + 1 + i, col + 2, w - 4,
                         i18n_get(k_action_keys[i]), i == g_ed.action_sel);
    }
    if (row + h - 2 > row + 1 + ED_ACTION_COUNT) {
        editor_draw_text(row + h - 2, col + 2, w - 4,
                         i18n_get("layout.editor.hint_menu"), 0);
    }
}

static int editor_kind_allowed(LayoutPaneKind kind)
{
    /* 换类型（t）时那一格自己用着的种类不算候选，其余未用种类都可选。 */
    if (g_ed.pending == ED_PENDING_CHANGE &&
        (int)kind == editor_selected_kind()) {
        return 0;
    }
    return layout_spec_pane_available(&g_ed.spec, kind);
}

static int editor_available_kinds(LayoutPaneKind *out, int capacity)
{
    int count = 0;
    for (int i = 0; i < LAYOUT_PANE_COUNT && count < capacity; i++) {
        if (editor_kind_allowed((LayoutPaneKind)i)) {
            out[count++] = (LayoutPaneKind)i;
        }
    }
    return count;
}

static void editor_draw_kinds(int rows, int cols)
{
    LayoutPaneKind kinds[LAYOUT_PANE_COUNT];
    int count = editor_available_kinds(kinds, LAYOUT_PANE_COUNT);
    int w = 40;
    int h = count + 5;
    if (h < 6) h = 6;
    if (w > cols - 4) w = cols - 4;
    if (h > rows - 4) h = rows - 4;
    int row = (rows - h) / 2;
    int col = (cols - w) / 2;
    editor_draw_box(row, col, h, w, i18n_get("layout.editor.pick_kind"));
    if (count == 0) {
        editor_draw_text(row + 2, col + 2, w - 4,
                         i18n_get("layout.editor.no_kind"), 0);
        return;
    }
    for (int i = 0; i < count; i++) {
        int pane_row = row + 1 + i;
        if (pane_row >= row + h - 1) break;
        editor_draw_text(pane_row, col + 2, w - 4,
                         ui_layout_pane_title(kinds[i]), i == g_ed.kind_sel);
    }
    editor_draw_text(row + h - 2, col + 2, w - 4,
                     i18n_get("layout.editor.hint_pick"), 0);
}

static void editor_draw_colors(int rows, int cols)
{
    int w = 44;
    int h = 10;
    if (w > cols - 4) w = cols - 4;
    if (h > rows - 4) h = rows - 4;
    int row = (rows - h) / 2;
    int col = (cols - w) / 2;
    editor_draw_box(row, col, h, w, i18n_get("layout.editor.colors_title"));

    int kind = editor_selected_kind();
    if (kind < 0) {
        return;
    }
    const LayoutPaneSpec *pane = layout_spec_pane_config(
        &g_ed.spec, (LayoutPaneKind)kind);
    if (!pane) {
        return;
    }
    const int values[4] = { pane->fg, pane->bg, pane->border_fg,
                            pane->border_bg };
    for (int i = 0; i < 4; i++) {
        char color[32];
        char line[96];
        format_color(color, sizeof(color), values[i]);
        snprintf(line, sizeof(line), "%-10s %s", color_field_name(i), color);
        editor_draw_text(row + 1 + i, col + 2, w - 4, line,
                         i == g_ed.color_field);
    }
    editor_draw_text(row + h - 2, col + 2, w - 4,
                     i18n_get("layout.editor.hint_colors"), 0);
}

static void editor_draw_help(int rows, int cols)
{
    int w = 62;
    int h = ED_HELP_COUNT + 4;
    if (w > cols - 4) w = cols - 4;
    if (h > rows - 4) h = rows - 4;
    int row = (rows - h) / 2;
    int col = (cols - w) / 2;
    editor_draw_box(row, col, h, w, i18n_get("layout.editor.help_title"));
    for (int i = 0; i < ED_HELP_COUNT; i++) {
        if (row + 1 + i >= row + h - 1) break;
        editor_draw_text(row + 1 + i, col + 2, w - 4,
                         i18n_get(k_help_keys[i]), 0);
    }
    if (row + h - 2 > row + 1 + ED_HELP_COUNT) {
        editor_draw_text(row + h - 2, col + 2, w - 4,
                         i18n_get("layout.editor.help_tip"), 0);
    }
}

/* ── 主界面 ────────────────────────────────────────────────────── */

static void editor_right_text(int row, int cols, const char *text)
{
    int width = utf8_str_width(text);
    if (width > 0 && width < cols) {
        mvaddstr(row, cols - width, text);
    }
}

static void editor_draw_detail(int row, int col, int width)
{
    int kind = editor_selected_kind();
    if (kind < 0) {
        return;
    }
    int index = layout_spec_find_pane(&g_ed.spec, (LayoutPaneKind)kind);
    int parent = -1;
    int slot = 0;
    if (index < 0 || editor_find_parent(index, &parent, &slot) != 1) {
        editor_draw_text(row, col, width,
                         i18n_get("layout.editor.root_pane"), 0);
        return;
    }
    const LayoutNode *node = &g_ed.spec.nodes[parent];
    int first = node->ratio;
    int selected_pct = slot == 0 ? first : 1000 - first;
    char text[128];
    snprintf(text, sizeof(text),
             i18n_get("layout.editor.split_fmt"),
             i18n_get(node->dir == LAYOUT_SPLIT_ROW
                          ? "layout.editor.split_row"
                          : "layout.editor.split_col"),
             (selected_pct + 5) / 10, (1000 - selected_pct + 5) / 10);
    editor_draw_text(row, col, width, text, 0);
}

void layout_editor_render(void)
{
    if (!g_ed.active) {
        return;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    /* 预设保留 name_key 时标题显示本地化名字，用户自定的布局显示自己的名字。 */
    const char *display_name = g_ed.spec.name;
    if (g_ed.spec.name_key[0]) {
        const char *translated = i18n_get(g_ed.spec.name_key);
        if (translated && translated[0]) {
            display_name = translated;
        }
    }
    if (!display_name || !display_name[0]) {
        display_name = g_ed.spec.id;
    }
    char title[160];
    snprintf(title, sizeof(title), "%s — %s [%s]%s",
             i18n_get("layout.editor.title"), display_name,
             g_ed.spec.id, g_ed.dirty ? " *" : "");
    attron(A_BOLD);
    editor_draw_text(0, 0, cols - 12, title, 0);
    attroff(A_BOLD);
    editor_right_text(0, cols, i18n_get("layout.editor.forced_ok"));

    if (rows < ED_MIN_ROWS || cols < ED_MIN_COLS) {
        editor_draw_text(rows / 2, 2, cols - 4,
                         i18n_get("layout.editor.too_small"), 0);
        editor_draw_text(rows - 1, 0, cols,
                         i18n_get("layout.editor.hint_global"), 0);
        refresh();
        return;
    }

    int list_w = cols >= 88 ? 40 : cols / 2;
    if (list_w < 30) list_w = cols - 4;

    char header[64];
    snprintf(header, sizeof(header), "%s (%d/%d)",
             i18n_get("layout.editor.panes"),
             layout_spec_pane_count(&g_ed.spec), LAYOUT_MAX_PANES);
    attron(A_BOLD);
    editor_draw_text(1, 0, list_w, header, 0);
    editor_draw_text(1, list_w + 2, cols - list_w - 2,
                     i18n_get("layout.editor.preview"), 0);
    attroff(A_BOLD);

    LayoutResolved resolved;
    int have_resolved = layout_spec_resolve(&g_ed.spec, &resolved) == 0;
    int count = layout_spec_pane_count(&g_ed.spec);
    for (int i = 0; i < count && i < LAYOUT_MAX_PANES; i++) {
        int row = 2 + i;
        if (row >= rows - 5) break;
        int kind = layout_spec_pane_at(&g_ed.spec, i);
        if (kind < 0) continue;
        char line[128];
        int pct_w = 0;
        int pct_h = 0;
        if (have_resolved) {
            for (int j = 0; j < resolved.pane_count; j++) {
                if ((int)resolved.panes[j].kind == kind) {
                    pct_w = (resolved.panes[j].w + 5) / 10;
                    pct_h = (resolved.panes[j].h + 5) / 10;
                    break;
                }
            }
        }
        snprintf(line, sizeof(line), "%s  %3d%% × %3d%%",
                 ui_layout_pane_title((LayoutPaneKind)kind), pct_w, pct_h);
        editor_draw_text(row, 0, list_w, line, i == g_ed.sel);
    }

    int detail_row = 2 + LAYOUT_MAX_PANES;
    if (detail_row < rows - 5) {
        editor_draw_detail(detail_row, 0, list_w);
    }

    int preview_row = 2;
    int preview_col = list_w + 2;
    int preview_w = cols - preview_col - 2;
    int preview_h = rows - 6;
    if (preview_w >= 8 && preview_h >= 4) {
        editor_draw_preview(preview_row, preview_col, preview_w, preview_h);
    }

    int status_row = rows - 4;
    if (g_ed.status[0]) {
        if (g_ed.status_error) {
            attron(A_BOLD);
        }
        editor_draw_text(status_row, 2, cols - 4, g_ed.status, 0);
        if (g_ed.status_error) {
            attroff(A_BOLD);
        }
    } else {
        editor_draw_text(status_row, 2, cols - 4,
                         i18n_get("layout.editor.forced_ok"), 0);
    }

    attron(A_DIM);
    editor_draw_text(rows - 3, 0, cols, i18n_get("layout.editor.hint_nav"), 0);
    editor_draw_text(rows - 2, 0, cols, i18n_get("layout.editor.hint_edit"), 0);
    editor_draw_text(rows - 1, 0, cols, i18n_get("layout.editor.hint_global"), 0);
    attroff(A_DIM);

    switch (g_ed.mode) {
        case ED_MODE_ACTIONS:
            editor_draw_actions(rows, cols);
            break;
        case ED_MODE_KINDS:
            editor_draw_kinds(rows, cols);
            break;
        case ED_MODE_COLORS:
            editor_draw_colors(rows, cols);
            break;
        case ED_MODE_HELP:
            editor_draw_help(rows, cols);
            break;
        default:
            break;
    }
    refresh();
}

/* ── 按键处理 ──────────────────────────────────────────────────── */

static int editor_handle_main(int ch)
{
    if (ch != 27 && ch != 'q' && ch != 'Q') {
        g_ed.esc_confirm = 0;
    }
    switch (ch) {
        case KEY_UP:
            editor_move_focus(0, -1);
            break;
        case KEY_DOWN:
            editor_move_focus(0, 1);
            break;
        case KEY_LEFT:
            editor_move_focus(-1, 0);
            break;
        case KEY_RIGHT:
            editor_move_focus(1, 0);
            break;
        case '\t':
            if (layout_spec_pane_count(&g_ed.spec) > 0) {
                g_ed.sel = (g_ed.sel + 1) %
                           layout_spec_pane_count(&g_ed.spec);
            }
            break;
        case KEY_BTAB:
            if (layout_spec_pane_count(&g_ed.spec) > 0) {
                g_ed.sel = (g_ed.sel + layout_spec_pane_count(&g_ed.spec) - 1) %
                           layout_spec_pane_count(&g_ed.spec);
            }
            break;
        case '[':
            editor_adjust_ratio(LAYOUT_SPLIT_ROW, -10);
            break;
        case ']':
            editor_adjust_ratio(LAYOUT_SPLIT_ROW, 10);
            break;
        case '{':
            editor_adjust_ratio(LAYOUT_SPLIT_COL, -10);
            break;
        case '}':
            editor_adjust_ratio(LAYOUT_SPLIT_COL, 10);
            break;
        case KEY_PPAGE:
            editor_adjust_ratio(LAYOUT_SPLIT_ROW, 50);
            break;
        case KEY_NPAGE:
            editor_adjust_ratio(LAYOUT_SPLIT_ROW, -50);
            break;
        case '=':
            editor_equalize();
            break;
        case '|':
            editor_open_kind_picker(ED_PENDING_SPLIT, LAYOUT_SPLIT_ROW);
            break;
        case '-':
            editor_open_kind_picker(ED_PENDING_SPLIT, LAYOUT_SPLIT_COL);
            break;
        case 'x':
        case 'X':
            editor_remove();
            break;
        case 't':
        case 'T':
            editor_open_kind_picker(ED_PENDING_CHANGE, LAYOUT_SPLIT_ROW);
            break;
        case 'w':
        case 'W':
            editor_swap();
            break;
        case 'c':
        case 'C':
            editor_open_colors();
            break;
        case 'n':
        case 'N':
            editor_rename();
            return 1;
        case '?':
        case KEY_F(1):
            g_ed.mode = ED_MODE_HELP;
            g_ed.esc_confirm = 0;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            g_ed.mode = ED_MODE_ACTIONS;
            g_ed.action_sel = 0;
            g_ed.esc_confirm = 0;
            break;
        case 's':
        case 'S':
        case 19:    /* Ctrl+S */
            editor_save();
            layout_editor_render();
            return 1;
        case 'q':
        case 'Q':
        case 27:
            editor_request_close();
            if (!g_ed.active) {
                return 1;
            }
            break;
        default:
            g_ed.esc_confirm = 0;
            break;
    }
    layout_editor_render();
    return 1;
}

static void editor_action_execute(int index)
{
    switch (index) {
        case 0:
            editor_open_kind_picker(ED_PENDING_SPLIT, LAYOUT_SPLIT_ROW);
            return;
        case 1:
            editor_open_kind_picker(ED_PENDING_SPLIT, LAYOUT_SPLIT_COL);
            return;
        case 2:
            editor_open_kind_picker(ED_PENDING_CHANGE, LAYOUT_SPLIT_ROW);
            return;
        case 3:
            editor_open_colors();
            return;
        case 4:
            editor_equalize();
            break;
        case 5:
            editor_swap();
            break;
        case 6:
            editor_remove();
            break;
        case 7:
            editor_rename();
            return;
        default:
            break;
    }
    g_ed.mode = ED_MODE_MAIN;
}

static int editor_handle_actions(int ch)
{
    switch (ch) {
        case KEY_UP:
            g_ed.action_sel = (g_ed.action_sel + ED_ACTION_COUNT - 1) %
                              ED_ACTION_COUNT;
            break;
        case KEY_DOWN:
            g_ed.action_sel = (g_ed.action_sel + 1) % ED_ACTION_COUNT;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            editor_action_execute(g_ed.action_sel);
            break;
        case 27:
        case 'q':
        case 'Q':
            g_ed.mode = ED_MODE_MAIN;
            break;
        default:
            g_ed.esc_confirm = 0;
            break;
    }
    layout_editor_render();
    return 1;
}

static int editor_handle_kinds(int ch)
{
    LayoutPaneKind kinds[LAYOUT_PANE_COUNT];
    int count = editor_available_kinds(kinds, LAYOUT_PANE_COUNT);
    switch (ch) {
        case KEY_UP:
            if (count > 0) {
                g_ed.kind_sel = (g_ed.kind_sel + count - 1) % count;
            }
            break;
        case KEY_DOWN:
            if (count > 0) {
                g_ed.kind_sel = (g_ed.kind_sel + 1) % count;
            }
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (count > 0 && g_ed.kind_sel < count) {
                editor_apply_kind_pick(kinds[g_ed.kind_sel]);
            } else {
                g_ed.mode = ED_MODE_MAIN;
                editor_status_error_key("layout.editor.no_kind");
            }
            break;
        case 27:
        case 'q':
        case 'Q':
            g_ed.mode = ED_MODE_MAIN;
            break;
        default:
            break;
    }
    layout_editor_render();
    return 1;
}

static int editor_handle_colors(int ch)
{
    int kind = editor_selected_kind();
    if (kind < 0) {
        g_ed.mode = ED_MODE_MAIN;
        return 1;
    }
    LayoutPaneSpec *pane = &g_ed.spec.panes[kind];
    int *field = color_field_ptr(pane, g_ed.color_field);
    if (!field) {
        g_ed.mode = ED_MODE_MAIN;
        return 1;
    }

    switch (ch) {
        case KEY_UP:
            g_ed.color_field = (g_ed.color_field + 3) % 4;
            break;
        case KEY_DOWN:
            g_ed.color_field = (g_ed.color_field + 1) % 4;
            break;
        case KEY_LEFT:
            *field = color_cycle_value(color_cycle_index(*field) - 1);
            g_ed.dirty = 1;
            break;
        case KEY_RIGHT:
            *field = color_cycle_value(color_cycle_index(*field) + 1);
            g_ed.dirty = 1;
            break;
        case KEY_PPAGE:
            *field = color_cycle_value(color_cycle_index(*field) + 10);
            g_ed.dirty = 1;
            break;
        case KEY_NPAGE:
            *field = color_cycle_value(color_cycle_index(*field) - 10);
            g_ed.dirty = 1;
            break;
        case KEY_HOME:
            *field = LAYOUT_COLOR_INHERIT;
            g_ed.dirty = 1;
            break;
        case KEY_END:
            *field = -1;
            g_ed.dirty = 1;
            break;
        case 'r':
            *field = LAYOUT_COLOR_INHERIT;
            g_ed.dirty = 1;
            break;
        case 'R':
            pane->fg = LAYOUT_COLOR_INHERIT;
            pane->bg = LAYOUT_COLOR_INHERIT;
            pane->border_fg = LAYOUT_COLOR_INHERIT;
            pane->border_bg = LAYOUT_COLOR_INHERIT;
            g_ed.dirty = 1;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
        case 27:
        case 'q':
        case 'Q':
            g_ed.mode = ED_MODE_MAIN;
            break;
        default:
            break;
    }
    layout_editor_render();
    return 1;
}

static int editor_handle_help(int ch)
{
    switch (ch) {
        case 27:
        case '?':
        case KEY_F(1):
        case 'q':
        case 'Q':
        case '\n':
        case '\r':
        case KEY_ENTER:
            g_ed.mode = ED_MODE_MAIN;
            break;
        default:
            break;
    }
    layout_editor_render();
    return 1;
}

int layout_editor_handle_key(int ch)
{
    if (!g_ed.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
        layout_editor_render();
        return 1;
    }
    switch (g_ed.mode) {
        case ED_MODE_ACTIONS:
            return editor_handle_actions(ch);
        case ED_MODE_KINDS:
            return editor_handle_kinds(ch);
        case ED_MODE_COLORS:
            return editor_handle_colors(ch);
        case ED_MODE_HELP:
            return editor_handle_help(ch);
        default:
            return editor_handle_main(ch);
    }
}

/* ── 生命周期 ──────────────────────────────────────────────────── */

static void editor_generate_identity(void)
{
    for (int n = 1; n <= 999; n++) {
        char id[LAYOUT_ID_MAX];
        snprintf(id, sizeof(id), "user_%d", n);
        int taken = 0;
        int count = ui_layout_count();
        for (int i = 0; i < count; i++) {
            if (strcmp(ui_layout_id_at(i), id) == 0) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            snprintf(g_ed.spec.id, sizeof(g_ed.spec.id), "%s", id);
            snprintf(g_ed.spec.name, sizeof(g_ed.spec.name),
                     i18n_get("layout.editor.custom_name"), n);
            return;
        }
    }
    snprintf(g_ed.spec.id, sizeof(g_ed.spec.id), "user_%ld", (long)getpid());
}

void layout_editor_open(const LayoutSpec *base, int is_new)
{
    memset(&g_ed, 0, sizeof(g_ed));
    if (base) {
        g_ed.spec = *base;
    } else {
        layout_spec_build_fallback(&g_ed.spec);
    }
    if (g_ed.spec.root < 0) {
        layout_spec_build_fallback(&g_ed.spec);
    }

    int was_normalized = g_ed.spec.normalized;
    g_ed.spec.normalized = 0;
    g_ed.is_new = is_new ? 1 : 0;
    if (g_ed.is_new) {
        editor_generate_identity();
        g_ed.spec.name_key[0] = '\0';
    }
    g_ed.sel = 0;
    g_ed.mode = ED_MODE_MAIN;
    g_ed.active = 1;
    log_debug("layout_editor", "Opened layout '%s' (%d panes, new=%d, normalized=%d)",
              g_ed.spec.id, layout_spec_pane_count(&g_ed.spec), g_ed.is_new,
              was_normalized);
    if (was_normalized && !g_ed.is_new) {
        editor_status_key("layout.editor.normalized_notice");
    }
    layout_editor_render();
}

int layout_editor_active(void)
{
    return g_ed.active;
}
