/**
 * @file rainbow.c
 * @brief 科乐美彩蛋：只改前景的斜向流光合成层
 *
 * 旧实现直接改写 g_app_config.theme 再 apply_color_theme()，外观系统重构
 * 之后会被逐窗格颜色/配置热重载冲掉，且做不出整屏连贯的斜向纹理。
 * 现在改为在窗口内容之上逐格替换前景/反色高亮：
 *   - 不碰主题、布局、配置，关闭后重绘即恢复；
 *   - 普通格子只改前景，背景统一透明（终端不支持默认色时回退黑）；
 *   - 反色/高亮选中格（播放列表光标、菜单/弹层选中行）不再跳过：用
 *     “对比文字色 + 流光背景”的专用色对，并按前景/背景互换的语义
 *     去掉 A_REVERSE/A_STANDOUT，让高亮条本身随流光移动；
 *   - 中心封面、频谱柱保持原色；黑胶碟面由 vinyl_rainbow_apply() 专门
 *     重染 3×3 色对，保留三档明暗与旋转高光；
 *   - 宽字符按两格同色处理，避免拆开中日韩字形。
 *
 * 闪烁修复：主视图窗格统一经 ui_refresh_window() 刷新，彩蛋激活时先把
 * 当前前景合成好再 wrefresh；不再出现“基础色帧先刷到终端、下一轮才被
 * 流光覆盖”的交替闪动。UI 是单线程，这里不存在数据竞态。
 *
 * 菜单叠加：菜单展开且与窗口相交时，ui_refresh_window() 不再直接
 * wrefresh，而是把窗口压入虚拟屏后用菜单 backing 覆盖回来，最后只
 * doupdate 一次；终端不会收到“窗格盖住菜单”的中间帧。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/rainbow.h"

#include "i18n/i18n.h"
#include "logger/logger.h"
#include "player/player.h"
#include "ui/cover_art_cells.h"
#include "ui/context_menu.h"
#include "ui/lyrics.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/rainbow_gradient.h"
#include "ui/ui.h"
#include "ui/vinyl.h"

#include <ncursesw/ncurses.h>
#include <stdint.h>
#include <wchar.h>

/* 相位每 180ms 推进一格：整圈约 20~23 秒，流动感明显但不过快。 */
#define RAINBOW_STEP_MS 180ULL

/*
 * 彩蛋专用色对起始号。
 * 1..6 主题、10..15/20..25 布局、32..255 字符封面；512 起留给效果层。
 * 终端色对不够时退回下面这组预留低编号色对。
 * 封面色对区间是 [COVER_ART_PAIR_BASE, RAINBOW_PAIR_BASE)，合成时跳过。
 */
#define RAINBOW_PAIR_BASE 512
#define RAINBOW_MIN_STEPS 4

static const int k_low_pairs[] = {
    7, 8, 9, 16, 17, 18, 19, 26, 27, 28, 29, 30, 31
};

#define KONAMI_SEQ_LENGTH 12
static const int k_konami_expected[KONAMI_SEQ_LENGTH] = {
    KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN,
    KEY_LEFT, KEY_RIGHT, KEY_LEFT, KEY_RIGHT,
    'B', 'A', 'B', 'A'
};

static int g_active = 0;
static int g_steps = 0;
static int g_pairs[RAINBOW_MAX_STEPS];
static int g_rev_pairs[RAINBOW_MAX_STEPS];
static int g_fg[RAINBOW_MAX_STEPS];
static int g_bg = COLOR_BLACK;
static int g_last_vinyl_offset = -1;

static int g_konami_pos = 0;
static uint64_t g_konami_last_time = 0;

/*
 * 选择普通/反色两组色对：优先高编号，色对不足时用预留低编号。
 * 返回实际可用的档数（可能小于 steps）；反色高亮每个相位也要一对，
 * 因此低编号回退时最多用“可用对数 / 2”档。
 */
static int build_pair_pools(int steps, int *normal, int *reverse, int max)
{
    if (!normal || !reverse || steps <= 0 || max <= 0) {
        return 0;
    }

    if ((int)COLOR_PAIRS >= RAINBOW_PAIR_BASE + 2 * steps) {
        for (int i = 0; i < steps && i < max; i++) {
            normal[i] = RAINBOW_PAIR_BASE + i;
            reverse[i] = RAINBOW_PAIR_BASE + steps + i;
        }
        return steps;
    }

    int available[sizeof(k_low_pairs) / sizeof(k_low_pairs[0])];
    int low_count = 0;
    for (size_t i = 0; i < sizeof(k_low_pairs) / sizeof(k_low_pairs[0]); i++) {
        if (k_low_pairs[i] < (int)COLOR_PAIRS) {
            available[low_count++] = k_low_pairs[i];
        }
    }

    int pair_steps = low_count / 2;
    if (pair_steps > steps) {
        pair_steps = steps;
    }
    if (pair_steps > max) {
        pair_steps = max;
    }
    if (pair_steps <= 0) {
        return 0;
    }

    for (int i = 0; i < pair_steps; i++) {
        normal[i] = available[i];
        reverse[i] = available[pair_steps + i];
    }
    return pair_steps;
}

static int rainbow_offset_now(void)
{
    if (g_steps <= 0) {
        return 0;
    }
    uint64_t tick = get_ui_time_ms() / RAINBOW_STEP_MS;
    return (int)(tick % (uint64_t)g_steps);
}

/* 按终端能力生成并注册前景色对；成功时 g_steps/g_bg 已就绪。 */
static int rainbow_init_colors(void)
{
    if (!has_colors()) {
        return -1;
    }

    int colors = (int)COLORS;
    int steps = rainbow_default_steps(colors);
    if (steps <= 0) {
        return -1;
    }

    int count = build_pair_pools(steps, g_pairs, g_rev_pairs,
                                 RAINBOW_MAX_STEPS);
    if (count < RAINBOW_MIN_STEPS) {
        return -1;
    }
    steps = count;

    if (rainbow_build_swatches(steps, colors, g_fg) != 0) {
        return -1;
    }

    int bg = ui_transparent_background();
    for (int i = 0; i < steps; i++) {
        /* 普通格子：流光前景 + 透明/黑背景。 */
        if (init_pair((NCURSES_PAIRS_T)g_pairs[i],
                      (NCURSES_COLOR_T)g_fg[i],
                      (NCURSES_COLOR_T)bg) != OK) {
            return -1;
        }
        /*
         * 反色高亮：保留 A_REVERSE，色对写成“流光前景 + 对比背景”；
         * 终端反转后实际显示为“对比文字 + 流光背景”。保留反转属性
         * 可让下一次合成仍认出这是高亮格，避免两次刷新在前景/背景
         * 两套语义之间来回切换造成闪烁。
         */
        if (init_pair((NCURSES_PAIRS_T)g_rev_pairs[i],
                      (NCURSES_COLOR_T)g_fg[i],
                      (NCURSES_COLOR_T)rainbow_contrast_fg(g_fg[i])) != OK) {
            return -1;
        }
    }

    g_bg = bg;
    g_steps = steps;
    return 0;
}

/*
 * 把一个窗口染成当前渐变；宽字符两格同色。
 * 普通格只改前景；反色/高亮格用“对比文字 + 流光背景”专用色对。
 * 封面/黑胶 label/频谱柱色对交给各模块，不在这里覆盖。
 */
static void rainbow_paint_window(WINDOW *win, int offset)
{
    if (!win || g_steps <= 0) {
        return;
    }

    int height = 0;
    int width = 0;
    int begin_y = 0;
    int begin_x = 0;
    getmaxyx(win, height, width);
    getbegyx(win, begin_y, begin_x);
    if (height <= 0 || width <= 0) {
        return;
    }

    for (int y = 0; y < height; y++) {
        int x = 0;
        while (x < width) {
            cchar_t cell;
            if (mvwin_wch(win, y, x, &cell) != OK) {
                x++;
                continue;
            }

            attr_t keep = cell.attr & ~(attr_t)(A_COLOR | A_STANDOUT);
            int span = 1;
            if (wcwidth(cell.chars[0]) == 2 && x + 1 < width) {
                span = 2;
            }

            /* 字符封面/黑胶 label/频谱柱：保持原配色，避免破坏图像。 */
            int pair = cell.ext_color;
            if (pair >= COVER_ART_PAIR_BASE && pair < RAINBOW_PAIR_BASE) {
                x += span;
                continue;
            }

            /*
             * 反色高亮：保留 A_REVERSE（把 A_STANDOUT 统一成 A_REVERSE），
             * 配合“流光前景 + 对比背景”色对，反转后显示为
             * “对比文字 + 流光背景”。保留反转属性是幂等的关键：
             * 下一次合成仍能识别高亮格，不会退回普通前景语义。
             */
            int reversed = (cell.attr & (A_REVERSE | A_STANDOUT)) != 0;
            if (reversed) {
                keep |= A_REVERSE;
            }

            int band = rainbow_phase(begin_x + x, begin_y + y,
                                     offset, g_steps);
            (void)mvwchgat(win, y, x, span, keep,
                           (NCURSES_PAIRS_T)(reversed ? g_rev_pairs[band]
                                                      : g_pairs[band]), NULL);
            x += span;
        }
    }
}

/* 主视图窗格统一刷新入口：先合成前景，再刷新。
 *
 * 菜单展开且与 win 相交时不能直接 wrefresh：wrefresh 会 doupdate 一帧
 * 把菜单像素盖掉，下一轮才补回，形成可见闪烁。此时改为单次合成：
 *   wnoutrefresh(win)  -> 菜单 backing 逐层压回虚拟屏 -> 唯一一次 doupdate
 * 终端差异比较后菜单矩形内没有变化，永远看不到窗格像素。
 * 菜单自身/不相交窗口仍走原 wrefresh，零额外开销。 */
int ui_refresh_window(WINDOW *win)
{
    if (!win) {
        return ERR;
    }
    if (g_active && g_steps > 0) {
        rainbow_paint_window(win, rainbow_offset_now());
    }

    if (ui_context_menu_active() &&
        !ui_context_menu_owns_window((const void *)win)) {
        int top = 0, left = 0, rows = 0, cols = 0;
        getbegyx(win, top, left);
        getmaxyx(win, rows, cols);
        if (ui_context_menu_intersects_rect(top, left, rows, cols)) {
            if (wnoutrefresh(win) == ERR) {
                return ERR;
            }
            int levels = ui_context_menu_reassert();
            log_debug("context_menu",
                      "overlay reassert after refresh levels=%d", levels);
            return doupdate();
        }
    }

    return wrefresh(win);
}

void ui_rainbow_paint(void)
{
    if (!g_active || g_steps <= 0 || !has_colors()) {
        return;
    }

    int offset = rainbow_offset_now();

    /* 黑胶碟面用三档流光色重染；相位变化时才重绘，避免每轮重复渲染。 */
    if (g_current_view == VIEW_MAIN && offset != g_last_vinyl_offset) {
        int shades[3];
        rainbow_pick_shades(g_fg, g_steps, offset, shades);
        if (vinyl_rainbow_apply(shades) == 0) {
            if (win_vinyl) {
                render_vinyl_pane();
            }
            if (win_lyrics && player_lyrics_total() <= 0) {
                render_lyrics();
            }
        }
        g_last_vinyl_offset = offset;
    }

    rainbow_paint_window(stdscr, offset);
    (void)wnoutrefresh(stdscr);

    if (g_current_view == VIEW_MAIN) {
        WINDOW *panes[6] = {
            win_playlist, win_info, win_controls,
            win_spectrum, win_lyrics, win_vinyl
        };
        for (int i = 0; i < 6; i++) {
            rainbow_paint_window(panes[i], offset);
        }
        /* stdscr 是底层，窗格覆盖其上；顺序与建窗/渲染的 z 序一致。 */
        for (int i = 0; i < 6; i++) {
            if (panes[i]) {
                (void)wnoutrefresh(panes[i]);
            }
        }

        /* 控制区弹层只在主视图可见；切到菜单视图后不能再把它刷上来。 */
        WINDOW *popup = controls_popup_window();
        if (popup) {
            rainbow_paint_window(popup, offset);
            (void)wnoutrefresh(popup);
        }
    } else if (g_current_view == VIEW_SETTINGS) {
        /* 设置页的颜色/选项弹层是独立窗口，必须排在 stdscr 之后。 */
        WINDOW *sel = settings_sel_window();
        if (sel) {
            rainbow_paint_window(sel, offset);
            (void)wnoutrefresh(sel);
        }
    }

    /* 右键菜单是最高层（根→子菜单顺序合成），必须盖在窗格与弹层之上。 */
    int menu_levels = ui_context_menu_level_count();
    for (int i = 0; i < menu_levels; i++) {
        WINDOW *menu = (WINDOW *)ui_context_menu_window_at(i);
        if (menu) {
            rainbow_paint_window(menu, offset);
            (void)wnoutrefresh(menu);
        }
    }

    (void)doupdate();
}

void toggle_rainbow_mode(void)
{
    if (g_active) {
        g_active = 0;
        vinyl_rainbow_restore();
        g_last_vinyl_offset = -1;
        show_status_message(i18n_get("status.rainbow_off"));
        update_controls_status(i18n_get("status.rainbow_off"));
        clear();
        rerender_active_view();
        log_info("ui", "Aurora flow disabled");
        return;
    }

    /* 黑白/无彩色终端：静默不触发，不提示、不重绘。 */
    if (!has_colors() || (int)COLORS < 8) {
        log_info("ui", "Aurora flow ignored: terminal has no chromatic colours");
        return;
    }

    if (rainbow_init_colors() != 0) {
        show_status_message(i18n_get("status.rainbow_unavailable"));
        update_controls_status(i18n_get("status.rainbow_unavailable"));
        rerender_active_view();
        log_warn("ui", "Aurora flow unavailable (colors=%d, pairs=%d)",
                 has_colors() ? (int)COLORS : 0,
                 has_colors() ? (int)COLOR_PAIRS : 0);
        return;
    }

    g_active = 1;
    g_last_vinyl_offset = -1;
    show_status_message(i18n_get("status.rainbow_on"));
    update_controls_status(i18n_get("status.rainbow_on"));
    rerender_active_view();
    ui_rainbow_paint();
    log_info("ui", "Aurora flow enabled (%d bands, fg-only+reverse, bg=%s)",
             g_steps, g_bg == -1 ? "default" : "black");
}

int ui_rainbow_active(void)
{
    return g_active;
}

void check_konami_input(int ch)
{
    uint64_t now = get_ui_time_ms();
    if (g_konami_pos > 0 && (now - g_konami_last_time) > 3000) {
        g_konami_pos = 0;
    }

    int expected = k_konami_expected[g_konami_pos];
    int matched = 0;
    if (ch == expected) {
        matched = 1;
    } else if ((expected == 'B' && (ch == 'b' || ch == 'B')) ||
               (expected == 'A' && (ch == 'a' || ch == 'A'))) {
        matched = 1;
    }

    if (matched) {
        g_konami_pos++;
        g_konami_last_time = now;
        if (g_konami_pos == KONAMI_SEQ_LENGTH) {
            toggle_rainbow_mode();
            g_konami_pos = 0;
        }
        return;
    }

    g_konami_pos = 0;
    if (ch == k_konami_expected[0]) {
        g_konami_pos = 1;
        g_konami_last_time = now;
    }
}
