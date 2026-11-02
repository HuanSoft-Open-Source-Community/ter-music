/**
 * @file layout_editor.c
 * @brief TUI 布局编辑器实现
 *
 * 全屏编辑一份布局的工作副本：窗格可见性/几何（千分比）/逐面板颜色，
 * 右侧实时缩放预览与严格平铺校验状态。保存经 layout_user_save_layout()
 * 写入用户布局文件并热重载目录；Esc 丢弃工作副本。
 * 编辑器激活期间由 settings.c 的输入分发拦截全部按键（见 handle_settings_input）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/layout_editor.h"

#include "i18n/i18n.h"
#include "ui/dialog.h"
#include "ui/layout.h"
#include "ui/menu_internal.h"

#include <ncursesw/ncurses.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>     /* getpid()：id 生成兜底 */

/* ── 字段与列布局 ────────────────────────────────────────────────
 * 面板清单为定宽单元格：字段列位置同时用于渲染与选中高亮。
 * 颜色单元格用 ASCII 记号（inh=继承主题 / def=终端默认 / 数字=调色板号），
 * 避免在定宽列里做 CJK 宽度换算。
 */
typedef enum {
    ED_F_VISIBLE = 0,
    ED_F_X,
    ED_F_Y,
    ED_F_W,
    ED_F_H,
    ED_F_FG,
    ED_F_BG,
    ED_F_BFG,
    ED_F_BBG,
    ED_FIELD_COUNT
} EdField;

/* 每个字段在行内的起始列与宽度（含一个空格间隔） */
static const int k_field_col[ED_FIELD_COUNT] = {
    13, 17, 23, 29, 35, 41, 47, 53, 59
};
static const int k_field_width[ED_FIELD_COUNT] = {
    3, 5, 5, 5, 5, 5, 5, 5, 5
};
#define ED_LIST_WIDTH 65

typedef struct {
    LayoutSpec spec;        /* 工作副本 */
    int active;             /* 编辑器是否打开 */
    int is_new;             /* 新建模式（id/名已生成） */
    int dirty;              /* 有未保存更改 */
    int sel_pane;           /* 选中窗格行 */
    int sel_field;          /* 选中字段（EdField） */
    int esc_confirm;        /* Esc 放弃确认标记 */
    char err[192];          /* 最近一次校验错误（空 = 通过） */
} LayoutEditorState;

static LayoutEditorState g_ed;

/* ── 小工具 ────────────────────────────────────────────────────── */

/** 颜色值在 -2(继承) → -1(终端默认) → 0..255 循环里的序号 */
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
    if (value <= -2) snprintf(buf, size, "inh");
    else if (value == -1) snprintf(buf, size, "def");
    else snprintf(buf, size, "%d", value);
}

static void editor_set_status(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_ed.err, sizeof(g_ed.err), fmt, args);
    va_end(args);
}

/** 每次修改后重跑校验；通过时清空错误状态 */
static void editor_refresh_validation(void)
{
    g_ed.err[0] = '\0';
    layout_spec_validate(&g_ed.spec, g_ed.err, sizeof(g_ed.err));
    g_ed.dirty = 1;
}

/* ── 编辑动作 ──────────────────────────────────────────────────── */

static void editor_adjust_field(int delta)
{
    if (g_ed.sel_pane < 0 || g_ed.sel_pane >= g_ed.spec.pane_count) {
        return;
    }
    LayoutPaneSpec *pane = &g_ed.spec.panes[g_ed.sel_pane];
    switch (g_ed.sel_field) {
        case ED_F_VISIBLE:
            pane->visible = !pane->visible;
            break;
        case ED_F_X:
        case ED_F_Y:
        case ED_F_W:
        case ED_F_H: {
            int *target = (g_ed.sel_field == ED_F_X) ? &pane->x
                        : (g_ed.sel_field == ED_F_Y) ? &pane->y
                        : (g_ed.sel_field == ED_F_W) ? &pane->w
                        : &pane->h;
            *target += delta;
            if (*target < 0) *target = 0;
            if (*target > LAYOUT_AREA_UNITS) *target = LAYOUT_AREA_UNITS;
            break;
        }
        case ED_F_FG:
            pane->fg = color_cycle_value(color_cycle_index(pane->fg) + delta);
            break;
        case ED_F_BG:
            pane->bg = color_cycle_value(color_cycle_index(pane->bg) + delta);
            break;
        case ED_F_BFG:
            pane->border_fg = color_cycle_value(color_cycle_index(pane->border_fg) + delta);
            break;
        case ED_F_BBG:
            pane->border_bg = color_cycle_value(color_cycle_index(pane->border_bg) + delta);
            break;
        default:
            break;
    }
    editor_refresh_validation();
}

static void editor_reset_pane_colors(void)
{
    if (g_ed.sel_pane < 0 || g_ed.sel_pane >= g_ed.spec.pane_count) {
        return;
    }
    LayoutPaneSpec *pane = &g_ed.spec.panes[g_ed.sel_pane];
    pane->fg = LAYOUT_COLOR_INHERIT;
    pane->bg = LAYOUT_COLOR_INHERIT;
    pane->border_fg = LAYOUT_COLOR_INHERIT;
    pane->border_bg = LAYOUT_COLOR_INHERIT;
    editor_refresh_validation();
}

static void editor_add_pane(void)
{
    if (g_ed.spec.pane_count >= LAYOUT_PANE_COUNT) {
        editor_set_status("%s", i18n_get("layout.editor.add_full"));
        return;
    }
    /* 按固定顺序挑第一个未使用的窗格种类 */
    for (int kind = 0; kind < LAYOUT_PANE_COUNT; kind++) {
        int used = 0;
        for (int i = 0; i < g_ed.spec.pane_count; i++) {
            if (g_ed.spec.panes[i].kind == (LayoutPaneKind)kind) {
                used = 1;
                break;
            }
        }
        if (used) {
            continue;
        }
        LayoutPaneSpec *pane = &g_ed.spec.panes[g_ed.spec.pane_count++];
        memset(pane, 0, sizeof(*pane));
        pane->kind = (LayoutPaneKind)kind;
        pane->visible = 1;
        /* 起始几何取左上 500×500，多半与现有窗格重叠——交给校验状态提示 */
        pane->x = 0;
        pane->y = 0;
        pane->w = 500;
        pane->h = 500;
        pane->fg = LAYOUT_COLOR_INHERIT;
        pane->bg = LAYOUT_COLOR_INHERIT;
        pane->border_fg = LAYOUT_COLOR_INHERIT;
        pane->border_bg = LAYOUT_COLOR_INHERIT;
        g_ed.sel_pane = g_ed.spec.pane_count - 1;
        g_ed.sel_field = ED_F_X;
        editor_refresh_validation();
        return;
    }
    editor_set_status("%s", i18n_get("layout.editor.add_full"));
}

static void editor_delete_pane(void)
{
    if (g_ed.spec.pane_count <= 1) {
        editor_set_status("%s", i18n_get("layout.editor.min_pane"));
        return;
    }
    if (g_ed.sel_pane < 0 || g_ed.sel_pane >= g_ed.spec.pane_count) {
        return;
    }
    for (int i = g_ed.sel_pane; i < g_ed.spec.pane_count - 1; i++) {
        g_ed.spec.panes[i] = g_ed.spec.panes[i + 1];
    }
    g_ed.spec.pane_count--;
    if (g_ed.sel_pane >= g_ed.spec.pane_count) {
        g_ed.sel_pane = g_ed.spec.pane_count - 1;
    }
    editor_refresh_validation();
}

static void editor_rename(void)
{
    char name[LAYOUT_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_ed.spec.name);
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    (void)cols;    /* 只用行号定位输入框 */
    if (prompt_text_input(stdscr, rows - 2, 2,
                          i18n_get("layout.editor.rename_prompt"),
                          name, sizeof(name), 1, 0, 1) == 0) {
        snprintf(g_ed.spec.name, sizeof(g_ed.spec.name), "%s", name);
        g_ed.dirty = 1;
    }
    editor_refresh_validation();
}

static void editor_save(void)
{
    char err[256];
    err[0] = '\0';
    if (layout_spec_validate(&g_ed.spec, err, sizeof(err)) != 0) {
        editor_set_status("%s", err[0] ? err : i18n_get("layout.editor.save_failed"));
        return;
    }
    char save_err[256];
    save_err[0] = '\0';
    if (layout_user_save_layout(&g_ed.spec, save_err, sizeof(save_err)) != 0) {
        editor_set_status("%s", save_err[0] ? save_err : i18n_get("layout.editor.save_failed"));
        return;
    }
    /* 热重载目录并选中刚保存的布局，主界面立即按新几何重建窗口 */
    ui_layout_reload();
    ui_layout_select(g_ed.spec.id);
    g_ed.active = 0;
    show_status_message(i18n_get("layout.editor.saved"));
}

/* ── 渲染 ──────────────────────────────────────────────────────── */

static const char *pane_kind_short(LayoutPaneKind kind)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST: return "playlist";
        case LAYOUT_PANE_INFO:     return "info";
        case LAYOUT_PANE_CONTROLS: return "controls";
        case LAYOUT_PANE_SPECTRUM: return "spectrum";
        case LAYOUT_PANE_LYRICS:   return "lyrics";
        case LAYOUT_PANE_VINYL:    return "vinyl";
        default:                   return "?";
    }
}

/** 在 (row, col) 打印宽 width 的单元格，选中时反显 */
static void editor_print_cell(int row, int col, int width, int selected,
                              const char *fmt, ...)
{
    char buf[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (selected) {
        attron(A_REVERSE);
    }
    mvprintw(row, col, "%-*.*s", width, width, buf);
    if (selected) {
        attroff(A_REVERSE);
    }
}

/**
 * 缩放预览：把主区域（1000×1000 千分比）映射进 pw×ph 的单元格框，
 * 按 resolve 后的可见窗格画矩形边框 + 种类缩写（示意，非等比像素）。
 */
static void editor_draw_preview(int base_row, int base_col, int pw, int ph)
{
    if (pw < 8 || ph < 4) {
        return;
    }
    /* 外框 */
    mvaddch(base_row, base_col, ACS_ULCORNER);
    mvaddch(base_row, base_col + pw - 1, ACS_URCORNER);
    mvaddch(base_row + ph - 1, base_col, ACS_LLCORNER);
    mvaddch(base_row + ph - 1, base_col + pw - 1, ACS_LRCORNER);
    for (int c = 1; c < pw - 1; c++) {
        mvaddch(base_row, base_col + c, ACS_HLINE);
        mvaddch(base_row + ph - 1, base_col + c, ACS_HLINE);
    }
    for (int r = 1; r < ph - 1; r++) {
        mvaddch(base_row + r, base_col, ACS_VLINE);
        mvaddch(base_row + r, base_col + pw - 1, ACS_VLINE);
    }

    LayoutSpec resolved;
    layout_spec_resolve(&g_ed.spec, &resolved);
    for (int i = 0; i < resolved.pane_count; i++) {
        const LayoutPaneSpec *pane = &resolved.panes[i];
        if (!pane->visible) {
            continue;
        }
        int c0 = base_col + 1 + (int)((long long)pane->x * (pw - 2) / LAYOUT_AREA_UNITS);
        int c1 = base_col + 1 + (int)((long long)(pane->x + pane->w) * (pw - 2) / LAYOUT_AREA_UNITS);
        int r0 = base_row + 1 + (int)((long long)pane->y * (ph - 2) / LAYOUT_AREA_UNITS);
        int r1 = base_row + 1 + (int)((long long)(pane->y + pane->h) * (ph - 2) / LAYOUT_AREA_UNITS);
        if (c1 <= c0 || r1 <= r0) {
            continue;   /* 该窗格在预览分辨率下不可见 */
        }
        if (c1 > base_col + pw - 1) c1 = base_col + pw - 1;
        if (r1 > base_row + ph - 1) r1 = base_row + ph - 1;
        for (int c = c0; c < c1; c++) {
            mvaddch(r0, c, '-');
            mvaddch(r1 - 1, c, '-');
        }
        for (int r = r0; r < r1; r++) {
            mvaddch(r, c0, '|');
            mvaddch(r, c1 - 1, '|');
        }
        const char *label = pane_kind_short(pane->kind);
        int label_row = r0 + (r1 - r0) / 2;
        int label_col = c0 + 1;
        if (label_row > r0 && label_row < r1 - 1 && label_col < c1 - 2) {
            mvprintw(label_row, label_col, "%.4s", label);
        }
    }
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

    /* 标题行 */
    attron(A_BOLD);
    mvprintw(0, 0, "%s — %s [%s]%s", i18n_get("layout.editor.title"),
             g_ed.spec.name[0] ? g_ed.spec.name : g_ed.spec.id,
             g_ed.spec.id, g_ed.dirty ? " *" : "");
    attroff(A_BOLD);

    if (rows < 14 || cols < ED_LIST_WIDTH + 4) {
        mvprintw(rows / 2, 2, "%s", i18n_get("layout.editor.too_small"));
        refresh();
        return;
    }

    /* 列表：表头 + 每窗格一行 */
    int list_row = 2;
    attron(A_BOLD);
    mvprintw(list_row, 0, "  %-10s %-3s %-5s %-5s %-5s %-5s %-5s %-5s %-5s %-5s",
             i18n_get("layout.editor.col_kind"),
             i18n_get("layout.editor.col_vis"),
             "X", "Y", "W", "H",
             i18n_get("layout.editor.col_fg"),
             i18n_get("layout.editor.col_bg"),
             i18n_get("layout.editor.col_bfg"),
             i18n_get("layout.editor.col_bbg"));
    attroff(A_BOLD);

    for (int i = 0; i < g_ed.spec.pane_count; i++) {
        int row = list_row + 1 + i;
        const LayoutPaneSpec *pane = &g_ed.spec.panes[i];
        int selected = (i == g_ed.sel_pane);

        mvprintw(row, 0, "%c", selected ? '>' : ' ');
        editor_print_cell(row, 2, 10, 0, "%s", pane_kind_short(pane->kind));

        char color_buf[16];
        editor_print_cell(row, k_field_col[ED_F_VISIBLE],
                          k_field_width[ED_F_VISIBLE],
                          selected && g_ed.sel_field == ED_F_VISIBLE,
                          pane->visible ? "[v]" : "[x]");
        editor_print_cell(row, k_field_col[ED_F_X], k_field_width[ED_F_X],
                          selected && g_ed.sel_field == ED_F_X, "%d", pane->x);
        editor_print_cell(row, k_field_col[ED_F_Y], k_field_width[ED_F_Y],
                          selected && g_ed.sel_field == ED_F_Y, "%d", pane->y);
        editor_print_cell(row, k_field_col[ED_F_W], k_field_width[ED_F_W],
                          selected && g_ed.sel_field == ED_F_W, "%d", pane->w);
        editor_print_cell(row, k_field_col[ED_F_H], k_field_width[ED_F_H],
                          selected && g_ed.sel_field == ED_F_H, "%d", pane->h);
        format_color(color_buf, sizeof(color_buf), pane->fg);
        editor_print_cell(row, k_field_col[ED_F_FG], k_field_width[ED_F_FG],
                          selected && g_ed.sel_field == ED_F_FG, "%s", color_buf);
        format_color(color_buf, sizeof(color_buf), pane->bg);
        editor_print_cell(row, k_field_col[ED_F_BG], k_field_width[ED_F_BG],
                          selected && g_ed.sel_field == ED_F_BG, "%s", color_buf);
        format_color(color_buf, sizeof(color_buf), pane->border_fg);
        editor_print_cell(row, k_field_col[ED_F_BFG], k_field_width[ED_F_BFG],
                          selected && g_ed.sel_field == ED_F_BFG, "%s", color_buf);
        format_color(color_buf, sizeof(color_buf), pane->border_bg);
        editor_print_cell(row, k_field_col[ED_F_BBG], k_field_width[ED_F_BBG],
                          selected && g_ed.sel_field == ED_F_BBG, "%s", color_buf);
    }

    /* 预览：宽终端放右侧，窄终端放列表下方 */
    int preview_row = 2;
    int preview_col = ED_LIST_WIDTH + 2;
    int preview_h = rows - 4;
    int preview_w = cols - preview_col - 2;
    if (cols < ED_LIST_WIDTH + 26) {
        preview_row = list_row + g_ed.spec.pane_count + 2;
        preview_col = 2;
        preview_h = rows - preview_row - 3;
        preview_w = cols - 4;
    }
    if (preview_h >= 4 && preview_w >= 8) {
        editor_draw_preview(preview_row, preview_col, preview_w, preview_h);
    }

    /* 校验状态行 */
    int status_row = rows - 2;
    if (g_ed.err[0] != '\0') {
        attron(A_BOLD);
        mvprintw(status_row, 2, "%s", g_ed.err);
        attroff(A_BOLD);
    } else {
        mvprintw(status_row, 2, "%s", i18n_get("layout.editor.valid_ok"));
    }

    /* 底部按键提示 */
    attron(A_DIM);
    mvprintw(rows - 1, 0, "%s", i18n_get("layout.editor.hint"));
    attroff(A_DIM);

    refresh();
}

/* ── 按键处理 ──────────────────────────────────────────────────── */

/** 选中字段按 delta 调整；颜色字段 PAGE 键与 +/- 等价（循环） */
static void editor_step_value(int delta)
{
    EdField field = (EdField)g_ed.sel_field;
    if (field == ED_F_VISIBLE) {
        editor_adjust_field(0);    /* 可见性只有开关一种调整 */
        return;
    }
    if (field >= ED_F_FG) {
        editor_adjust_field(delta == 0 ? 1 : (delta > 0 ? 1 : -1));
        return;
    }
    editor_adjust_field(delta);    /* 几何：±1 / ±10 */
}

int layout_editor_handle_key(int ch)
{
    if (!g_ed.active) {
        return 0;
    }

    switch (ch) {
        case KEY_UP:
        case 'k':
            if (g_ed.sel_pane > 0) {
                g_ed.sel_pane--;
            }
            break;
        case KEY_DOWN:
        case 'j':
            if (g_ed.sel_pane < g_ed.spec.pane_count - 1) {
                g_ed.sel_pane++;
            }
            break;
        case KEY_LEFT:
        case 'h':
            g_ed.sel_field = (g_ed.sel_field + ED_FIELD_COUNT - 1) % ED_FIELD_COUNT;
            break;
        case KEY_RIGHT:
        case 'l':
            g_ed.sel_field = (g_ed.sel_field + 1) % ED_FIELD_COUNT;
            break;
        case '+':
        case '=':
            editor_step_value(1);
            break;
        case '-':
        case '_':
            editor_step_value(-1);
            break;
        case KEY_PPAGE:
            editor_step_value(10);
            break;
        case KEY_NPAGE:
            editor_step_value(-10);
            break;
        case 'v':
        case ' ':
            if (g_ed.sel_field == ED_F_VISIBLE) {
                editor_adjust_field(0);
            } else {
                editor_set_status("%s", i18n_get("layout.editor.hint_visible"));
            }
            break;
        case 'a':
            editor_add_pane();
            break;
        case 'd':
            editor_delete_pane();
            break;
        case 'r':
            editor_reset_pane_colors();
            break;
        case 'n':
            editor_rename();
            break;
        case 's':
        case 'S':
            editor_save();
            break;
        case 27:    /* Esc：有更改时先确认再放弃 */
            if (g_ed.dirty && !g_ed.esc_confirm) {
                g_ed.esc_confirm = 1;
                editor_set_status("%s", i18n_get("layout.editor.dirty_exit"));
                layout_editor_render();
                return 1;
            }
            g_ed.active = 0;
            return 1;
        default:
            layout_editor_render();
            return 1;
    }

    g_ed.esc_confirm = 0;
    layout_editor_render();
    return 1;
}

/* ── 生命周期 ──────────────────────────────────────────────────── */

/** 为新建布局生成目录内唯一的 user_N id 与默认名 */
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
    /* 999 个都撞了（理论不可能）：退化用时间戳 */
    snprintf(g_ed.spec.id, sizeof(g_ed.spec.id), "user_%ld", (long)getpid());
}

void layout_editor_open(const LayoutSpec *base, int is_new)
{
    memset(&g_ed, 0, sizeof(g_ed));
    if (base) {
        g_ed.spec = *base;
    }
    g_ed.is_new = is_new ? 1 : 0;
    if (g_ed.is_new) {
        editor_generate_identity();
        /* 用户覆盖不应带着内置预设的 i18n 名字键，否则显示回译名 */
        g_ed.spec.name_key[0] = '\0';
    }
    g_ed.sel_pane = 0;
    g_ed.sel_field = ED_F_X;
    g_ed.active = 1;
    layout_editor_render();
}

int layout_editor_active(void)
{
    return g_ed.active;
}
