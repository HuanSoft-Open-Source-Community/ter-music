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
#include "ui/layout_editor.h"
#include "ui/lyrics.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/menu_anim.h"
#include "cli/cli.h"
#include "ui/rainbow_band_policy.h"
#include "ui/rainbow_gradient.h"
#include "ui/tty_guard.h"
#include "ui/ui.h"
#include "ui/vinyl.h"

#include <ncursesw/ncurses.h>
#include <stdlib.h>
#include <string.h>

/* ncurses 5.x（EL7）没有 NCURSES_PAIRS_T 与 ext_color，这里做最小兼容：
 * 色对编号仍是 short；未启用 ext_colors 时色对编码在 attr 的 A_COLOR 位。 */
#ifndef NCURSES_PAIRS_T
#define NCURSES_PAIRS_T short
#endif
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

/* ── R3：分带摊薄（t18，人类已授权改流光观感） ─────────────────────
 *
 * 目标：相位绘制摊到多帧，单帧写入 <= UI_RAINBOW_BAND_BUDGET 字节；
 * 相位步长与“一轮从上到下的扫描”同步，绝不追帧（每帧最多推进一带）。
 * 禁止用 wredrawln/redrawwin（t6 已证其触发 ncurses 6.5 滚动优化误判）。
 *
 * 观感变化：整屏同时变色 -> 一条自上而下的色带扫描；一轮扫描（约 1-2s）
 * 推进一个色阶。TER_MUSIC_RAINBOW_BANDED=0 回退旧观感（A/B 用）。
 */
static int g_banded = 0;        /* 默认只在玲珑容器内启用（见 rainbow_init） */
static int g_band_index = 0;    /* 当前扫描到第几带 */
static int g_band_rows = 1;     /* 带高（行），1..UI_RAINBOW_BAND_MAX_ROWS 自适应 */
static unsigned long long g_band_frames = 0;      /* R3 扫描帧计数（证据用） */
static unsigned long long g_band_max_write = 0;   /* 单帧最大写入（字节） */
static unsigned long long g_band_over_budget = 0; /* 超过 4KiB 预算的帧数 */
#define UI_RAINBOW_BAND_BUDGET 4096ULL
/* 带高上限固定 1 行：实测 2 行时会出现 4452B 的单帧（>4KiB 预算），
 * 而 1 行最坏 ~3.6KB（237 列 × 每格 SGR+字形）必然满足预算。
 * 若人类接受更大的单帧预算，可把上限调高（带宽越大扫得越快）。 */
#define UI_RAINBOW_BAND_MAX_ROWS 1
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

static int rainbow_offset_now(void);

/* R3：分带模式的初始化（每次启用流光时调用）。
 * 默认只在玲珑容器内启用（直跑保持旧观感与旧字节量）；
 * TER_MUSIC_RAINBOW_BANDED=0/1 可强制关闭/开启（A/B 用）。 */
static void rainbow_banded_init(void)
{
    const char *env = getenv("TER_MUSIC_RAINBOW_BANDED");
    if (env && env[0] != '\0') {
        g_banded = (strcmp(env, "0") != 0);
    } else {
        g_banded = cli_in_sandbox() ? 1 : 0;
    }
    g_band_index = 0;
    g_band_rows = 1;                 /* 先 1 行保证 ≤4KiB，再按实测放宽 */
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
    rainbow_banded_init();
    return 0;
}

/* 该色对的背景色；用于 R1 判断“空格只改前景是否真的不可见”。
 * 返回 -2 表示无法读取（按“不能跳过”处理，保守）。 */
static int rainbow_pair_background(int pair)
{
    if (pair < 0 || pair >= (int)COLOR_PAIRS) {
        return -2;
    }
    NCURSES_PAIRS_T fg = 0;
    NCURSES_PAIRS_T bg = 0;
    (void)pair_content((NCURSES_PAIRS_T)pair, &fg, &bg);
    return (int)bg;
}

/*
 * R1：跳过“染了也看不见”的空白格。
 *
 * 空格没有墨迹，只改前景色在终端上逐像素不可见；只要该格背景已经等于
 * 流光背景（透明 -1 或黑 0），重染就是纯冗余输出。跳过它可以显著减少
 * ncurses 认为“变了”的格子，从而少触发整行 ClrToEOL+PutRange 重画
 * （T5 §4.1 R1；视觉等价，不改流光观感）。
 *
 * 排除项：反色/高亮格（前景会变成色块）、宽字符/组合字符（不是单空格）、
 * 以背景色为底的格子（重染会把底色改成流光黑，肉眼可见）。
 * 封面/黑胶 label/频谱柱在调用点已按色对区间排除，不会到这里。
 */
static int rainbow_blank_cell_skip(const cchar_t *cell, int pair, int span,
                                   int reversed)
{
    if (!cell || reversed || span != 1) {
        return 0;
    }
    if (cell->chars[0] != L' ' || cell->chars[1] != L'\0') {
        return 0;
    }
    return rainbow_pair_background(pair) == g_bg;
}

/*
 * 把一个窗口染成当前渐变；宽字符两格同色。
 * 普通格只改前景；反色/高亮格用“对比文字 + 流光背景”专用色对。
 * 封面/黑胶 label/频谱柱色对交给各模块，不在这里覆盖。
 */
static void rainbow_paint_window_range(WINDOW *win, int offset,
                                       int band_y0, int band_y1)
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
        int screen_y = begin_y + y;
        if (screen_y < band_y0 || screen_y >= band_y1) {
            continue;
        }
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
#ifdef NCURSES_EXT_COLORS
            int pair = cell.ext_color;
#else
            int pair = PAIR_NUMBER(cell.attr);
#endif
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
            if (!reversed && rainbow_blank_cell_skip(&cell, pair, span, reversed)) {
                x += span;
                continue;
            }
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

static void rainbow_paint_window(WINDOW *win, int offset)
{
    rainbow_paint_window_range(win, offset, 0, 1 << 30);
}

/* ── 帧合成与刷新（R4：流光期每帧只 doupdate 一次） ──────────────
 *
 * 事件循环每轮会从多个窗格刷新入口（频谱 40ms、歌词动画 100ms、进度条、
 * 控制区状态）回到这里。此前每个入口都 wrefresh() = 各自 doupdate 一次，
 * 流光期一帧会写多份几乎相同的整屏差量。现在：
 *   - 普通渲染仍走 wrefresh（语义不变，嵌套输入循环不受影响）；
 *   - 主循环的节拍刷新经 ui_refresh_begin_soft() 标记，改为 wnoutrefresh +
 *     帧末一次 doupdate（present_overlay_frame 里的 ui_frame_flush()）；
 *   - F1 修复帧用 clearok(curscr) 走 ClrUpdate 整屏强制重绘（单轮写入见实测）。
 */

static int g_frame_pending = 0;
static int g_last_paint_offset = -1;
static int g_soft_refresh = 0;
static uint64_t g_suppress_until_ms = 0;

/* 重建帧抑制流光上色的时间窗：覆盖当轮重建（40ms 主循环 + 余量）。 */
#define UI_RAINBOW_SUPPRESS_MS 50ULL

static int rainbow_painting_suppressed(void)
{
    return g_suppress_until_ms != 0 && get_ui_time_ms() < g_suppress_until_ms;
}

static int rainbow_wants_paint(void)
{
    return g_active && g_steps > 0 && has_colors() &&
           !rainbow_painting_suppressed();
}

void ui_rainbow_suppress_next_frame(void)
{
    /* X1：resize 重建那一帧只出基础色，避免“重建整屏 burst + 流光整屏 burst”
     * 叠在同一帧（T5 §4.4 第 4 步）。窗口过后自动恢复流光。 */
    g_suppress_until_ms = get_ui_time_ms() + UI_RAINBOW_SUPPRESS_MS;
    g_last_paint_offset = -1;
}

void ui_refresh_begin_soft(void)
{
    g_soft_refresh = 1;
}

void ui_refresh_end_soft(void)
{
    g_soft_refresh = 0;
}

static void frame_compose_range(int offset, int paint, int band_y0, int band_y1)
{

    if (paint) {
        rainbow_paint_window_range(stdscr, offset, band_y0, band_y1);
    }
    (void)wnoutrefresh(stdscr);

    if (g_current_view == VIEW_MAIN) {
        WINDOW *panes[6] = {
            win_playlist, win_info, win_controls,
            win_spectrum, win_lyrics, win_vinyl
        };
        if (paint) {
            for (int i = 0; i < 6; i++) {
                rainbow_paint_window_range(panes[i], offset, band_y0, band_y1);
            }
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
            if (paint) {
                rainbow_paint_window_range(popup, offset, band_y0, band_y1);
            }
            (void)wnoutrefresh(popup);
        }
    } else if (g_current_view == VIEW_SETTINGS) {
        /* 设置页的颜色/选项弹层是独立窗口，必须排在 stdscr 之后。 */
        WINDOW *sel = settings_sel_window();
        if (sel) {
            if (paint) {
                rainbow_paint_window_range(sel, offset, band_y0, band_y1);
            }
            (void)wnoutrefresh(sel);
        }
        /* 布局编辑器的模态菜单同样是独立窗口，动画帧里也要逐帧重染。 */
        WINDOW *editor_menu = (WINDOW *)layout_editor_menu_window();
        if (editor_menu) {
            if (paint) {
                rainbow_paint_window_range(editor_menu, offset, band_y0, band_y1);
            }
            (void)wnoutrefresh(editor_menu);
        }
    }

    /* 右键菜单是最高层（根→子菜单顺序合成），必须盖在窗格与弹层之上。 */
    int menu_levels = ui_context_menu_level_count();
    for (int i = 0; i < menu_levels; i++) {
        WINDOW *menu = (WINDOW *)ui_context_menu_window_at(i);
        if (menu) {
            if (paint) {
                rainbow_paint_window_range(menu, offset, band_y0, band_y1);
            }
            (void)wnoutrefresh(menu);
        }
    }

    /* 消失动画的 ghost 是真正的最顶层：同样逐帧重染并压回，保证菜单
     * 退场时不会被流光合成或窗格刷新擦掉。 */
    int ghosts = ui_menu_anim_ghost_count();
    for (int i = 0; i < ghosts; i++) {
        WINDOW *ghost = ui_menu_anim_ghost_window(i);
        if (ghost) {
            if (paint) {
                rainbow_paint_window_range(ghost, offset, band_y0, band_y1);
            }
            (void)wnoutrefresh(ghost);
        }
    }

    g_frame_pending = 1;
}

void ui_frame_compose(void)
{
    frame_compose_range(rainbow_offset_now(), rainbow_wants_paint(), 0, 1 << 30);
}

/* R3：只重染 [y0,y1) 行窗口带，其余行保持上一次扫描的颜色。 */
static void ui_frame_compose_band(int offset, int y0, int y1)
{
    frame_compose_range(offset, 1, y0, y1);
}

int ui_frame_flush(void)
{
    if (!g_frame_pending) {
        return OK;
    }
    g_frame_pending = 0;
    return doupdate();
}

/* 主视图窗格统一刷新入口：先合成前景，再刷新。
 *
 * 菜单展开且与 win 相交时不能直接 wrefresh：wrefresh 会 doupdate 一帧
 * 把菜单像素盖掉，下一轮才补回，形成可见闪烁。此时改为单次合成：
 *   wnoutrefresh(win)  -> 菜单 backing 逐层压回虚拟屏 -> 唯一一次 doupdate
 * 终端差异比较后菜单矩形内没有变化，永远看不到窗格像素。
 * 菜单自身/不相交窗口仍走原 wrefresh，零额外开销。
 *
 * R4（T5 §4.1）：主循环的节拍刷新被 ui_refresh_begin_soft()/end_soft() 包住，
 * 期间 ui_refresh_window() 走 soft 路径 —— 只 wnoutrefresh 入虚拟屏，doupdate
 * 推迟到帧末（present_overlay_frame → ui_frame_flush），多个窗格一帧只写一次。
 * 非流光期、嵌套输入循环与模态框（begin/end 之外）仍然即时 wrefresh。 */
static int refresh_window_common(WINDOW *win, int soft)
{
    if (!win) {
        return ERR;
    }
    if (rainbow_wants_paint()) {
        rainbow_paint_window(win, rainbow_offset_now());
    }

    int top = 0, left = 0, rows = 0, cols = 0;
    getbegyx(win, top, left);
    getmaxyx(win, rows, cols);

    /* 活动中的右键菜单与退场 ghost 都是最高层：窗格刷新若与它们相交，
     * 先把窗格入虚屏，再把菜单各层/ghost 压回，最后只 doupdate 一次。 */
    int context_overlay = ui_context_menu_active() &&
                          !ui_context_menu_owns_window((const void *)win) &&
                          ui_context_menu_intersects_rect(top, left, rows, cols);
    int ghost_overlay = ui_menu_anim_exit_active() &&
                        ui_menu_anim_ghost_intersects_rect(top, left, rows, cols);

    if (context_overlay || ghost_overlay) {
        if (wnoutrefresh(win) == ERR) {
            return ERR;
        }
        if (context_overlay) {
            int levels = ui_context_menu_reassert();
            log_debug("context_menu",
                      "overlay reassert after refresh levels=%d", levels);
        }
        if (ghost_overlay) {
            int ghosts = ui_menu_anim_reassert_ghosts();
            log_debug("menu_anim",
                      "ghost reassert after refresh levels=%d", ghosts);
        }
        return doupdate();
    }

    if (soft && g_active && g_steps > 0) {
        /* R4：流光期把这一窗格的差量与其它节拍刷新合并，帧末统一 doupdate。 */
        if (wnoutrefresh(win) == ERR) {
            return ERR;
        }
        g_frame_pending = 1;
        return OK;
    }

    return wrefresh(win);
}

int ui_refresh_window(WINDOW *win)
{
    /* 由主循环的 begin/end_soft 区间决定是否走 soft（R4 接线点）。 */
    return refresh_window_common(win, g_soft_refresh);
}

void ui_rainbow_paint(void)
{
    /* F1 修复请求：并进本次帧合成——compose 之后对 curscr 置 clearok，
     * 这次 doupdate 同时完成“相位重绘 + 整屏强制重绘”，不额外产生 burst。 */
    int repair_due = ui_tty_guard_take_repair_due();

    if (!g_active || g_steps <= 0 || !has_colors()) {
        if (repair_due) {
            ui_frame_compose();
            (void)clearok(curscr, TRUE);
            (void)ui_frame_flush();
            ui_tty_guard_repair_flushed();
        }
        return;
    }

    int offset = rainbow_offset_now();
    int suppressed = rainbow_painting_suppressed();

    /* R2 相位去重（T5 §4.1 R2）：相位 180ms 才推进一格，而主循环 40ms 一轮。
     * 相位没变时本帧不会有任何格子改色；各窗格的增量刷新（visualizer /
     * lyrics / progress 的 soft 刷新）已经在自己的路径里合成过当前相位并压入
     * 虚拟屏，帧末由 ui_frame_flush() 统一刷出，所以这里整段跳过是视觉等价的。
     * 菜单动画帧必须逐帧合成（ghost/滑动），重建帧也要合成（只是不上色），
     * 两者都不参与去重；修复帧必须合成。 */
    if (!repair_due && !suppressed && offset == g_last_paint_offset &&
        !ui_menu_anim_active()) {
        return;
    }

    /* R3 分带摊薄：每帧只重染一个带；一轮自上而下的扫描推进一个色阶，
     * 绝不追帧（每帧最多一带）。带高按实测单帧写入自适应，保证 ≤4KiB。
     * 修复帧/菜单动画帧/重建帧仍走整屏路径（见上）。 */
    if (g_banded && !repair_due && !suppressed && !ui_menu_anim_active()) {
        int rows = 0;
        int cols_r3 = 0;
        getmaxyx(stdscr, rows, cols_r3);
        (void)cols_r3;
        if (rows > 0) {
            /* 纯决策：本帧行区间 / 下一带序号（每帧 ≤1 带，末尾回绕）。 */
            UiRainbowBandPlan plan =
                ui_rainbow_band_plan(rows, g_band_rows, g_band_index);
            int y0 = plan.y0;
            int y1 = plan.y1;

            /* 先把“内容更新”（频谱/歌词/进度条的 soft 刷新）单独刷掉，
             * 再做带绘制：否则内容与色带同帧写，单帧会超过 4KiB 预算
             * （实测未分离时 3000 帧里有 3 帧达 30KB）。内容更新本来就是
             * 独立于 R3 的既有输出，分帧写不改变总字节。 */
            if (g_frame_pending) {
                (void)ui_frame_flush();
            }
            /* 相位用墙钟（180ms 一格，与 R3 之前一致）；绘制节流到“每帧一带”。
             * 这样屏幕上是“一条自下而上流动的渐变”：每行取到自己被扫到那一刻的
             * 色阶，相邻行相差 0–1 格，整体看起来仍是流光，而不是整屏同色。
             * 若严格按“一轮扫描 = 一个色阶”（128 格 × 4.75s ≈ 10 分钟一个色彩
             * 周期），流光会静止——实测后放弃该读法，理由写进 IMPLEMENTATION.md。 */
            /* 墙钟相位：落后就丢弃，不快进、不补帧。 */
            int band_offset = ui_rainbow_band_phase(get_ui_time_ms(),
                                                    (unsigned long long)RAINBOW_STEP_MS,
                                                    g_steps);
            unsigned long long before = 0;
            (void)ui_tty_guard_wchar_now(&before);
            ui_frame_compose_band(band_offset, y0, y1);
            (void)ui_frame_flush();
            unsigned long long after = 0;
            if (before != 0 && ui_tty_guard_wchar_now(&after) == 0 && after >= before) {
                unsigned long long wrote = after - before;
                g_band_frames++;
                if (wrote > g_band_max_write) {
                    g_band_max_write = wrote;
                }
                if (wrote > UI_RAINBOW_BAND_BUDGET) {
                    g_band_over_budget++;
                }
                g_band_rows = ui_rainbow_band_rows_for_write(
                    g_band_rows, wrote, UI_RAINBOW_BAND_BUDGET,
                    UI_RAINBOW_BAND_MAX_ROWS);
                if ((g_band_frames % 200ULL) == 0) {
                    log_debug("ui",
                              "R3 banded: frames=%llu rows=%d max_frame_write=%lluB "
                              "over_budget=%llu",
                              g_band_frames, g_band_rows, g_band_max_write,
                              g_band_over_budget);
                }
            }

            g_last_paint_offset = band_offset;
            g_band_index = plan.next;   /* 一轮扫描结束则回到 0；相位由墙钟推进 */
            return;
        }
    }

    /* 黑胶碟面用三档流光色重染；相位变化时才重绘，避免每轮重复渲染。
     * 菜单动画期间不在这里另起整块黑胶/歌词重绘：动画帧只走
     * ui_menu_anim_frame() 的一次合成，避免同一拍内两次 doupdate。
     * R3 分带模式不逐相位重绘黑胶（会让单帧写入超出 4KiB 预算）：
     * 碟面保持启用时的三档色，这也是 R3 授权的观感变化之一。 */
    if (!g_banded && !ui_menu_anim_active() &&
        g_current_view == VIEW_MAIN && offset != g_last_vinyl_offset) {
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

    ui_frame_compose();
    if (repair_due) {
        (void)clearok(curscr, TRUE);
    }
    (void)ui_frame_flush();
    if (repair_due) {
        ui_tty_guard_repair_flushed();
    }

    if (!suppressed) {
        g_last_paint_offset = offset;
    }
}

void toggle_rainbow_mode(void)
{
    /* 流光开关会整屏重绘：先收掉菜单退场 ghost，并按新场景重新探测动画档位
     * （流光整屏合成的耗时不应永久拖累普通模式）。 */
    ui_menu_anim_cancel_all(0);
    ui_menu_anim_reset_tier();

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
    g_band_index = 0;
    g_band_rows = 1;
    g_band_frames = 0;
    g_band_max_write = 0;
    g_band_over_budget = 0;
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
