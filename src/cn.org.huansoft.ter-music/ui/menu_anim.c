/**
 * @file menu_anim.c
 * @brief 菜单入场/消失动画的状态机、ghost 与合成实现
 *
 * 见 ui/menu_anim.h 的模块约束。这里的几何/缓动全部来自
 * ui/menu_anim_math.c；本文件只做窗口生命周期、z 序合成、降级调度与
 * 与流光/菜单模块的接线。
 */

#include "ui/menu_anim.h"

#include "logger/logger.h"
#include "ui/context_menu.h"
#include "ui/context_menu_layout.h"
#include "ui/layout_editor.h"
#include "ui/menu_internal.h"
#include "ui/rainbow.h"
#include "ui/ui.h"

#include <ncursesw/ncurses.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MENU_ANIM_MAX               8
#define MENU_ANIM_COST_SAMPLES      4
#define MENU_ANIM_MIN_COST_SAMPLES  3

typedef enum {
    MENU_ANIM_ENTRY_FREE = 0,
    MENU_ANIM_ENTRY_ENTER,
    MENU_ANIM_ENTRY_SETTLED,
    MENU_ANIM_ENTRY_GHOST
} MenuAnimEntryKind;

typedef struct {
    MenuAnimEntryKind kind;
    WINDOW *win;
    UIMenuAnimStyle style;
    UIMenuAnimPlan plan;
    unsigned duration_ms;
    uint64_t start_ms;
    int last_top;
    int last_left;
    int layer;
} MenuAnimEntry;

static MenuAnimEntry g_entries[MENU_ANIM_MAX];
static int g_initialized = 0;
static UIMenuAnimTier g_tier = UI_MENU_ANIM_TIER_FULL;

/* 最近若干次真正 doupdate 的合成耗时（毫秒），用于降级。 */
static unsigned g_costs[MENU_ANIM_COST_SAMPLES];
static int g_cost_count = 0;
static int g_cost_next = 0;

/* 新增/收尾一个动画项后，下一帧必须至少合成一次，让起始 ghost 立刻
 * 出现在已经重绘过的底层之上。 */
static int g_scene_dirty = 0;

/* ============================================================
 * 档位探测
 * ============================================================ */

static const char *tier_name(UIMenuAnimTier tier)
{
    switch (tier) {
        case UI_MENU_ANIM_TIER_FULL:    return "full";
        case UI_MENU_ANIM_TIER_REDUCED: return "reduced";
        case UI_MENU_ANIM_TIER_OFF:
        default:                       return "off";
    }
}

static UIMenuAnimTier detect_tier(void)
{
    const char *term = getenv("TERM");

    /* 非 TTY / dumb 终端：连位置动画都没有意义。 */
    if (!isatty(STDOUT_FILENO)) {
        return UI_MENU_ANIM_TIER_OFF;
    }
    if (term && strcasecmp(term, "dumb") == 0) {
        return UI_MENU_ANIM_TIER_OFF;
    }
    if (LINES < 8 || COLS < 24) {
        return UI_MENU_ANIM_TIER_OFF;
    }

    /* ASCII 回退/低色终端：保留动画但缩到 1 行/1 列。 */
    if (g_ascii_fallback_ui) {
        return UI_MENU_ANIM_TIER_REDUCED;
    }
    if (!has_colors() || COLORS < 8) {
        return UI_MENU_ANIM_TIER_REDUCED;
    }
    return UI_MENU_ANIM_TIER_FULL;
}

static void cost_reset(void)
{
    memset(g_costs, 0, sizeof(g_costs));
    g_cost_count = 0;
    g_cost_next = 0;
}

static void cost_note(unsigned cost_ms)
{
    g_costs[g_cost_next] = cost_ms;
    g_cost_next = (g_cost_next + 1) % MENU_ANIM_COST_SAMPLES;
    if (g_cost_count < MENU_ANIM_COST_SAMPLES) {
        g_cost_count++;
    }
}

static unsigned cost_average(void)
{
    if (g_cost_count < MENU_ANIM_MIN_COST_SAMPLES) {
        return 0;
    }
    unsigned sum = 0;
    for (int i = 0; i < g_cost_count; i++) {
        sum += g_costs[i];
    }
    return sum / (unsigned)g_cost_count;
}

void ui_menu_anim_init(void)
{
    if (g_initialized) {
        return;
    }
    memset(g_entries, 0, sizeof(g_entries));
    cost_reset();
    g_tier = detect_tier();
    g_initialized = 1;
    log_info("menu_anim",
             "tier=%s ascii=%d term=%s colors=%d pairs=%d lines=%d cols=%d",
             tier_name(g_tier), g_ascii_fallback_ui,
             getenv("TERM") ? getenv("TERM") : "?",
             has_colors() ? (int)COLORS : 0,
             has_colors() ? (int)COLOR_PAIRS : 0,
             LINES, COLS);
}

void ui_menu_anim_reset_tier(void)
{
    if (!g_initialized) {
        ui_menu_anim_init();
        return;
    }
    UIMenuAnimTier next = detect_tier();
    if (next != g_tier) {
        log_info("menu_anim", "tier reset %s -> %s",
                 tier_name(g_tier), tier_name(next));
        g_tier = next;
    }
    cost_reset();
}

void ui_menu_anim_shutdown(void)
{
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_GHOST &&
            g_entries[i].win) {
            delwin(g_entries[i].win);
        }
        memset(&g_entries[i], 0, sizeof(g_entries[i]));
    }
    g_initialized = 0;
    g_scene_dirty = 0;
    cost_reset();
}

/* ============================================================
 * 条目管理
 * ============================================================ */

static MenuAnimEntry *entry_alloc(void)
{
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_FREE) {
            return &g_entries[i];
        }
    }
    return NULL;
}

static void entry_release(MenuAnimEntry *entry)
{
    if (entry) {
        memset(entry, 0, sizeof(*entry));
    }
}

/* 只找仍由调用方拥有的窗口（ENTER/SETTLED）；ghost 的窗口是副本。 */
static MenuAnimEntry *entry_find_live(WINDOW *win)
{
    if (!win) {
        return NULL;
    }
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if ((entry->kind == MENU_ANIM_ENTRY_ENTER ||
             entry->kind == MENU_ANIM_ENTRY_SETTLED) &&
            entry->win == win) {
            return entry;
        }
    }
    return NULL;
}

static MenuAnimEntry *ghost_at(int index)
{
    if (index < 0) {
        return NULL;
    }
    int seen = 0;
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind != MENU_ANIM_ENTRY_GHOST) {
            continue;
        }
        if (seen == index) {
            return &g_entries[i];
        }
        seen++;
    }
    return NULL;
}

static int active_count(void)
{
    int count = 0;
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_ENTER ||
            g_entries[i].kind == MENU_ANIM_ENTRY_GHOST) {
            count++;
        }
    }
    return count;
}

static int ghost_count(void)
{
    int count = 0;
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_GHOST) {
            count++;
        }
    }
    return count;
}

static int free_ghost(MenuAnimEntry *entry)
{
    if (!entry || entry->kind != MENU_ANIM_ENTRY_GHOST) {
        return 0;
    }
    if (entry->win) {
        delwin(entry->win);
    }
    entry_release(entry);
    return 1;
}

static int delete_all_ghosts(void)
{
    int deleted = 0;
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_GHOST) {
            deleted |= free_ghost(&g_entries[i]);
        }
    }
    return deleted;
}

/* ============================================================
 * 合成
 * ============================================================ */

static void push_plain(WINDOW *win)
{
    if (!win) {
        return;
    }
    touchwin(win);
    (void)wnoutrefresh(win);
}

/*
 * 普通模式的整帧合成：先压底层，再按 z 序压菜单各层与 ghost。
 * ghost 永远在最后——它代表正在退场的最高层。
 */
static void compose_frame(void)
{
    if (ui_rainbow_active()) {
        ui_rainbow_paint();
        return;
    }

    push_plain(stdscr);

    if (g_current_view == VIEW_MAIN) {
        WINDOW *panes[6] = {
            win_playlist, win_info, win_controls,
            win_spectrum, win_lyrics, win_vinyl
        };
        for (size_t i = 0; i < sizeof(panes) / sizeof(panes[0]); i++) {
            push_plain(panes[i]);
        }
        push_plain(controls_popup_window());
    } else if (g_current_view == VIEW_SETTINGS) {
        push_plain(settings_sel_window());
        /* 布局编辑器的模态菜单也是独立窗口；它进入/消失时走本模块的
         * 位移帧，这里必须把它压到 stdscr 之上。 */
        push_plain((WINDOW *)layout_editor_menu_window());
    }

    int levels = ui_context_menu_level_count();
    for (int i = 0; i < levels; i++) {
        push_plain((WINDOW *)ui_context_menu_window_at(i));
    }

    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        if (g_entries[i].kind == MENU_ANIM_ENTRY_GHOST) {
            push_plain(g_entries[i].win);
        }
    }

    (void)doupdate();
}

/* ============================================================
 * 降级
 * ============================================================ */

static void snap_all_and_stop(void)
{
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if (entry->kind == MENU_ANIM_ENTRY_ENTER) {
            if (entry->win) {
                (void)mvwin(entry->win, entry->plan.to_top, entry->plan.to_left);
            }
            entry_release(entry);
        } else if (entry->kind == MENU_ANIM_ENTRY_GHOST) {
            free_ghost(entry);
        } else if (entry->kind == MENU_ANIM_ENTRY_SETTLED) {
            entry_release(entry);
        }
    }
}

static void maybe_degrade(void)
{
    if (g_cost_count < MENU_ANIM_MIN_COST_SAMPLES) {
        return;
    }
    unsigned avg = cost_average();
    UIMenuAnimTier next = ui_menu_anim_tier_after_frame(g_tier, avg);
    if (next == g_tier) {
        return;
    }

    log_info("menu_anim", "degrade tier=%s -> %s avg_frame=%ums samples=%d",
             tier_name(g_tier), tier_name(next), avg, g_cost_count);
    g_tier = next;
    cost_reset();

    if (next == UI_MENU_ANIM_TIER_OFF) {
        snap_all_and_stop();
        compose_frame();
    }
}

/* ============================================================
 * 进入
 * ============================================================ */

int ui_menu_anim_begin_enter(WINDOW *win, UIMenuAnimStyle style,
                             int final_top, int final_left)
{
    ui_menu_anim_init();
    if (!win || g_tier == UI_MENU_ANIM_TIER_OFF) {
        return 0;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(win, rows, cols);
    if (rows <= 0 || cols <= 0) {
        return 0;
    }

    UIRect final;
    final.top = final_top;
    final.left = final_left;
    final.rows = rows;
    final.cols = cols;

    UIMenuAnimPlan plan = ui_menu_anim_plan(style, g_tier, final, LINES, COLS);
    if (plan.from_top == plan.to_top && plan.from_left == plan.to_left) {
        return 0;
    }

    MenuAnimEntry *entry = entry_find_live(win);
    if (entry) {
        entry_release(entry);
    }
    entry = entry_alloc();
    if (!entry) {
        log_debug("menu_anim", "enter refused: registry full");
        return 0;
    }

    if (mvwin(win, plan.from_top, plan.from_left) != OK) {
        log_debug("menu_anim", "enter refused: mvwin(%d,%d) failed",
                  plan.from_top, plan.from_left);
        entry_release(entry);
        return 0;
    }

    entry->kind = MENU_ANIM_ENTRY_ENTER;
    entry->win = win;
    entry->style = style;
    entry->plan = plan;
    entry->duration_ms = ui_menu_anim_duration_ms(g_tier, style, 0);
    entry->start_ms = get_ui_time_ms();
    entry->last_top = plan.from_top;
    entry->last_left = plan.from_left;
    g_scene_dirty = 1;

    log_debug("menu_anim",
              "enter style=%d from=%d,%d to=%d,%d duration=%ums tier=%s",
              (int)style, plan.from_top, plan.from_left,
              plan.to_top, plan.to_left, entry->duration_ms, tier_name(g_tier));
    return 1;
}

/* ============================================================
 * 消失
 * ============================================================ */

int ui_menu_anim_begin_exit(WINDOW *win, UIMenuAnimStyle style, int layer)
{
    ui_menu_anim_init();
    if (!win || g_tier == UI_MENU_ANIM_TIER_OFF) {
        return 0;
    }

    int rows = 0;
    int cols = 0;
    int cur_top = 0;
    int cur_left = 0;
    getmaxyx(win, rows, cols);
    getbegyx(win, cur_top, cur_left);
    if (rows <= 0 || cols <= 0) {
        return 0;
    }

    MenuAnimEntry *src = entry_find_live(win);
    if (src) {
        style = src->style;
    }

    UIMenuAnimPlan plan;
    plan.from_top = cur_top;
    plan.from_left = cur_left;

    if (src) {
        /* 进入动画可能还在半途，也可能已 SETTLED；都退回它原来的起点。 */
        plan.to_top = src->plan.from_top;
        plan.to_left = src->plan.from_left;
    } else {
        UIRect final;
        final.top = cur_top;
        final.left = cur_left;
        final.rows = rows;
        final.cols = cols;
        UIMenuAnimPlan origin = ui_menu_anim_plan(style, g_tier, final, LINES, COLS);
        plan.to_top = origin.from_top;
        plan.to_left = origin.from_left;
    }

    if (plan.from_top == plan.to_top && plan.from_left == plan.to_left) {
        if (src) {
            entry_release(src);
        }
        return 0;
    }

    WINDOW *ghost = dupwin(win);
    if (!ghost) {
        log_debug("menu_anim", "exit refused: dupwin failed");
        if (src) {
            entry_release(src);
        }
        return 0;
    }

    if (src) {
        entry_release(src);
    }

    MenuAnimEntry *entry = entry_alloc();
    if (!entry) {
        log_debug("menu_anim", "exit refused: registry full");
        delwin(ghost);
        return 0;
    }

    entry->kind = MENU_ANIM_ENTRY_GHOST;
    entry->win = ghost;
    entry->style = style;
    entry->plan = plan;
    entry->duration_ms = ui_menu_anim_duration_ms(g_tier, style, 1);
    entry->start_ms = get_ui_time_ms();
    entry->last_top = cur_top;
    entry->last_left = cur_left;
    entry->layer = layer;
    g_scene_dirty = 1;

    log_debug("menu_anim",
              "exit style=%d from=%d,%d to=%d,%d duration=%ums layer=%d tier=%s",
              (int)style, plan.from_top, plan.from_left,
              plan.to_top, plan.to_left, entry->duration_ms, layer,
              tier_name(g_tier));
    return 1;
}

/* ============================================================
 * 取消 / 收尾
 * ============================================================ */

void ui_menu_anim_cancel_window(WINDOW *win)
{
    if (!win) {
        return;
    }
    MenuAnimEntry *entry = entry_find_live(win);
    if (!entry) {
        return;
    }
    if (entry->kind == MENU_ANIM_ENTRY_ENTER) {
        /* 窗口即将销毁，但仍把它落到终点，避免留下中间帧像素。 */
        (void)mvwin(win, entry->plan.to_top, entry->plan.to_left);
    }
    entry_release(entry);
}

void ui_menu_anim_cancel_all(int repaint)
{
    if (!g_initialized) {
        return;
    }
    const int ghosts_before = ghost_count();
    int scene_changed = delete_all_ghosts();

    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if (entry->kind == MENU_ANIM_ENTRY_ENTER) {
            if (entry->win) {
                (void)mvwin(entry->win, entry->plan.to_top, entry->plan.to_left);
            }
            entry_release(entry);
            /* 活动窗口被 snap 到终点：如果调用方紧接着不会再整屏重绘
             * （模态输入入口），需要补一帧把窗口放到终点。 */
            scene_changed = 1;
        } else if (entry->kind == MENU_ANIM_ENTRY_SETTLED) {
            entry_release(entry);
        }
    }

    log_debug("menu_anim", "cancel-all ghosts=%d scene_changed=%d repaint=%d",
              ghosts_before, scene_changed, repaint);
    if (scene_changed && repaint) {
        compose_frame();
    }
}

void ui_menu_anim_finish_exits(int repaint)
{
    if (!g_initialized) {
        return;
    }
    int deleted_ghost = delete_all_ghosts();
    if (deleted_ghost && repaint) {
        compose_frame();
    }
}

/* ============================================================
 * 查询
 * ============================================================ */

int ui_menu_anim_active(void)
{
    return active_count() > 0 ? 1 : 0;
}

int ui_menu_anim_exit_active(void)
{
    return ghost_count() > 0 ? 1 : 0;
}

int ui_menu_anim_ghost_count(void)
{
    return ghost_count();
}

WINDOW *ui_menu_anim_ghost_window(int index)
{
    MenuAnimEntry *entry = ghost_at(index);
    return entry ? entry->win : NULL;
}

int ui_menu_anim_ghost_intersects_rect(int top, int left, int rows, int cols)
{
    if (rows <= 0 || cols <= 0) {
        return 0;
    }
    UIRect target;
    target.top = top;
    target.left = left;
    target.rows = rows;
    target.cols = cols;

    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if (entry->kind != MENU_ANIM_ENTRY_GHOST || !entry->win) {
            continue;
        }
        UIRect rect;
        int begin_y = 0;
        int begin_x = 0;
        getbegyx(entry->win, begin_y, begin_x);
        getmaxyx(entry->win, rect.rows, rect.cols);
        rect.top = begin_y;
        rect.left = begin_x;
        if (ui_rect_intersects(&rect, &target)) {
            return 1;
        }
    }
    return 0;
}

int ui_menu_anim_reassert_ghosts(void)
{
    int reasserted = 0;
    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if (entry->kind != MENU_ANIM_ENTRY_GHOST || !entry->win) {
            continue;
        }
        touchwin(entry->win);
        if (wnoutrefresh(entry->win) != ERR) {
            reasserted++;
        }
    }
    return reasserted;
}

/* ============================================================
 * 帧推进
 * ============================================================ */

int ui_menu_anim_frame(void)
{
    if (!g_initialized || active_count() == 0) {
        return 0;
    }

    const uint64_t now = get_ui_time_ms();
    int changed = 0;
    int removed_ghost = 0;

    for (int i = 0; i < MENU_ANIM_MAX; i++) {
        MenuAnimEntry *entry = &g_entries[i];
        if (entry->kind != MENU_ANIM_ENTRY_ENTER &&
            entry->kind != MENU_ANIM_ENTRY_GHOST) {
            continue;
        }

        const int exiting = (entry->kind == MENU_ANIM_ENTRY_GHOST);
        int done = (now >= entry->start_ms + (uint64_t)entry->duration_ms);

        int top = entry->plan.to_top;
        int left = entry->plan.to_left;
        if (!done) {
            ui_menu_anim_position(&entry->plan,
                                  (unsigned)(now - entry->start_ms),
                                  entry->duration_ms, exiting, &top, &left);
        }

        if (top != entry->last_top || left != entry->last_left) {
            if (mvwin(entry->win, top, left) == OK) {
                entry->last_top = top;
                entry->last_left = left;
                changed = 1;
            } else {
                log_debug("menu_anim", "mvwin(%d,%d) failed; finish entry", top, left);
                done = 1;
            }
        }

        if (!done) {
            continue;
        }

        if (exiting) {
            removed_ghost |= free_ghost(entry);
        } else {
            /* 进入结束：保留 origin 供退出反推。 */
            entry->kind = MENU_ANIM_ENTRY_SETTLED;
        }
    }

    if (changed || removed_ghost || g_scene_dirty) {
        g_scene_dirty = 0;
        uint64_t begin_ms = get_ui_time_ms();
        compose_frame();
        cost_note((unsigned)(get_ui_time_ms() - begin_ms));
        maybe_degrade();
    }

    return 1;
}
