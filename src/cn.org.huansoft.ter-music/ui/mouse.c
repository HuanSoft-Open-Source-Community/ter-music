/**
 * @file mouse.c
 * @brief 鼠标事件边缘层：量窗格 → 归一化 → 纯逻辑判定 → 分发
 *
 * 分工：
 *   - ui/mouse_routing.c（纯逻辑，无 ncurses）决定"这次事件归谁"，
 *     包括弹层展开时与底层内容区的竞争态裁决；
 *   - 本文件（边缘层）负责用 getbegyx/getmaxyx 现场量出各区域矩形、
 *     把 MEVENT 归一化成 UIMouseEvent，并把判定结果派发给各模块。
 *
 * 覆盖范围：主视图（播放列表/曲库浏览器、控制区、歌词、信息、频谱、黑胶、
 * 提示条）与全部子视图（侧栏与内容区）、设置页各分区、选择弹层、
 * 布局编辑器（含其模态菜单）。裸右键与中键都按"在该点开右键菜单"处理；
 * 带修饰的右键/中键一律让给终端/窗口管理器；鼠标移动忽略。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "i18n/i18n.h"
#include "player/player.h"
#include "ui/context_menu.h"
#include "ui/ui.h"
#include "ui/menus.h"
#include "ui/menu_internal.h"
#include "ui/mouse.h"
#include "ui/layout_editor.h"
#include "playlist/playlist.h"
#include "search/search.h"
#include "library/browser/browser.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "ui/lyrics.h"
#include "config/config.h"
#include "logger/logger.h"
#include <ncursesw/ncurses.h>
#include <pthread.h>
#include <string.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

/* Playlist tab mode and queue selection (defined in playlist_render.c) */
extern int g_playlist_tab_mode;
extern int g_queue_selected_index;

/* ============================================================
 * Mouse event helpers
 * ============================================================ */

int is_primary_mouse_click(const MEVENT *event)
{
    if (!event) return 0;
    if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED)) return 1;
    return (event->bstate & BUTTON1_RELEASED) != 0 && (event->bstate & BUTTON1_PRESSED) == 0;
}

/* 把一个 ncurses 窗格量成区域表里的矩形。窗格不存在（当前布局没这块）时
 * 得到空矩形：ui_rect_contains 恒为假，自然不会被命中。 */
static UIRect rect_of_window(WINDOW *win)
{
    UIRect r;
    r.top = r.left = r.rows = r.cols = 0;
    if (!win) return r;
    int beg_y, beg_x, h, w;
    getbegyx(win, beg_y, beg_x);
    getmaxyx(win, h, w);
    r.top = beg_y;
    r.left = beg_x;
    r.rows = h;
    r.cols = w;
    return r;
}

/* ============================================================
 * MEVENT → UIMouseEvent 归一化
 * ============================================================ */

/*
 * 滚轮：xterm 协议里上滚=按钮4、下滚=按钮5，且**没有 release 语义**，
 * ncurses 只在我们申请了 BUTTON4/BUTTON5_PRESSED 时才上报（见 ui.c 的
 * mousemask）。一次滚轮事件可能被终端拆成 press+release 两条，ncurses
 * 会合并；这里只认 PRESSED 位，并把它当作一格增量。
 *
 * 点击：优先认 CLICKED/DOUBLE/TRIPLE；只有 RELEASED（某些终端不合成
 * click）时按一次单击处理。**无论终端上报单击、双击还是三击，本程序都
 * 归一成"一次点击"**：click_count 只写进调试日志，任何归属者都不得据此
 * 改变行为——单击即选中并执行，双击不会执行两次。滚轮位优先于点击位——
 * 一次滚动绝不允许既滚动又点击。
 */
static void normalize_mouse_event(const MEVENT *event, UIMouseEvent *out)
{
    memset(out, 0, sizeof(*out));
    if (!event) return;

    out->y = event->y;
    out->x = event->x;

    if (event->bstate & BUTTON4_PRESSED) {
        out->wheel = -1;          /* 上滚 */
        return;
    }
    if (event->bstate & BUTTON5_PRESSED) {
        out->wheel = +1;          /* 下滚 */
        return;
    }

    /* 右键：只认**不带修饰**的 BUTTON3_PRESSED（掩码见 ui.c）。按下即触发，
     * 不依赖终端合成 CLICKED，因此不会出现"一次右键收到 press+click 两条"
     * 导致刚开的菜单立刻被第二条关掉的竞态。
     *
     * 带 Ctrl/Alt/Shift 的右键一律不认，留给终端/窗口管理器。原因不是洁癖：
     * 有的终端（deepin-terminal 6.5.40）对任何右键都弹自家菜单，同时又把
     * 右键转发给程序；程序再认，就会同时出现两张菜单，且 TUI 的 overlay 会
     * 在终端菜单关闭后残留，下一次同键被 overlay 当成"取消"——正是
     * "第一次两张、第二次只剩终端菜单"的由来。终端把修饰位剥掉时（deepin-
     * terminal 就是如此），Ctrl+右键与裸右键在程序侧无法区分，故备选入口只能
     * 另找（中键；见下一个分支）。 */
    if ((event->bstate & BUTTON3_PRESSED) &&
        !(event->bstate & (BUTTON_CTRL | BUTTON_ALT | BUTTON_SHIFT))) {
        out->right_click = 1;
        return;
    }

    /* 中键（滚轮按下）= 右键菜单：语义与右键完全相同（在指针处开同一张菜单，
     * 指针下的行只选中、绝不播放）。这是右键被终端自家菜单占住时的鼠标备选：
     * deepin-terminal 在程序开启鼠标上报后会把中键原样转发，且不为中键弹
     * 终端菜单（见 TerminalDisplay::mousePressEvent 的 MID_BTN 分支）。
     *
     * 只认 CLICKED/DOUBLE/TRIPLE/RELEASED，不认 BUTTON2_PRESSED：同格按下+
     * 松开由 ncurses 合成 CLICKED；指针跨格时合并不成立，会分别投递 PRESSED
     * 与 RELEASED，只认后者才能保证一次点击只开一次（否则开完又被关）。
     * 带修饰的中键同样不认，留给终端（xterm 的 Ctrl+中键字体菜单等）。 */
    if (!(event->bstate & (BUTTON_CTRL | BUTTON_ALT | BUTTON_SHIFT)) &&
        (event->bstate & (BUTTON2_CLICKED | BUTTON2_DOUBLE_CLICKED |
                          BUTTON2_TRIPLE_CLICKED | BUTTON2_RELEASED))) {
        out->right_click = 1;
        return;
    }

    /* Ctrl/Alt + 左键 = 右键上下文菜单：全程序唯一让修饰位改变语义的地方。
     * 它只对**会转发修饰位**的终端有效（xterm、VTE 系、kitty 等）。deepin-
     * terminal 6.5.40 的 mouseSignal/sendMouseEvent 只编码按钮号、不附加
     * 修饰位，所以那里 Ctrl/Alt+左键等同普通左键（会选中/播放）；该终端
     * 请用上面的中键备选。
     *
     * 只认左键的点击位与 RELEASED，不认 BUTTON1_PRESSED：同格按下+松开会由
     * ncurses 合并成 BUTTON1_CLICKED|修饰位；指针在按下/松开间跨格时合并不
     * 成立，会分别投递 PRESSED 与 RELEASED，只认 RELEASED 正好开一次。
     * 若把 PRESSED 也认了，跨格/长按就会"开一次、关一次"——掩码也刻意不
     * 申请 BUTTON1_PRESSED（见 ui.c）。 */
    if ((event->bstate & (BUTTON_CTRL | BUTTON_ALT)) &&
        (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED |
                          BUTTON1_TRIPLE_CLICKED | BUTTON1_RELEASED))) {
        out->right_click = 1;
        return;
    }

    if (event->bstate & BUTTON1_CLICKED) {
        out->click = 1;
        out->click_count = 1;
    } else if (event->bstate & BUTTON1_DOUBLE_CLICKED) {
        out->click = 1;
        out->click_count = 2;
    } else if (event->bstate & BUTTON1_TRIPLE_CLICKED) {
        out->click = 1;
        out->click_count = 3;
    } else if (is_primary_mouse_click(event)) {
        out->click = 1;
        out->click_count = 1;
    }
    /* 其余输入（鼠标移动、带修饰的中键/右键等）：click/wheel/right_click
     * 都为 0，ui_mouse_resolve 直接判为"无事可做"。 */
}

/* ============================================================
 * 区域表：现场量出
 * ============================================================ */

static void measure_main_view_regions(UIMouseRegions *r)
{
    r->playlist = rect_of_window(win_playlist);
    r->controls = rect_of_window(win_controls);
    r->lyrics   = rect_of_window(win_lyrics);
    r->info     = rect_of_window(win_info);
    r->visual   = rect_of_window(win_spectrum);
    r->vinyl    = rect_of_window(win_vinyl);

    /* 曲库浏览器打开时，那块窗格显示的不是播放列表 */
    r->library_browser = g_library_state.active ? 1 : 0;
}

static void measure_menu_view_regions(UIMouseRegions *r)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    const int menu_width = max_x / 4;
    /* 侧栏文字画在 x=2，清行范围 x=1..menu_width-1（见 render_menu_sidebar），
     * 分隔线 │ 占 menu_width 那一列，因此侧栏区取 1..menu_width-1。
     * 首行 y=2 与 render_menu_sidebar 的 start_y 同源。 */
    r->sidebar.top = 2;
    r->sidebar.left = 1;
    r->sidebar.rows = (max_y - 2 > 2) ? (max_y - 2 - 2) : 0;
    r->sidebar.cols = (menu_width - 1 > 0) ? (menu_width - 1) : 0;

    /* 内容区：分隔线右侧直到右边界。具体行→条目映射由各视图用自己在
     * 渲染期记下的几何算，这里只保证"这一列归内容区"。 */
    r->content.top = 1;
    r->content.left = menu_width + 1;
    r->content.rows = (max_y - 2 > 1) ? (max_y - 2 - 1) : 0;
    r->content.cols = (max_x - r->content.left > 0) ? (max_x - r->content.left) : 0;
}

static void build_regions(UIMouseRegions *r)
{
    memset(r, 0, sizeof(*r));

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    r->view = g_current_view;

    /* 快捷键捕获中：下一次输入被当作按键名，鼠标事件必须整个吞掉 */
    r->input_exclusive = settings_hotkey_capture_active() ? 1 : 0;

    /* 底部提示条：create_layout() 总会调用 render_menu_hint_bar()，
     * 它画在最后一行（见 menus.c 的 build_menu_hint_items 排版）。 */
    r->hint_bar.top = max_y - 1;
    r->hint_bar.left = 0;
    r->hint_bar.rows = 1;
    r->hint_bar.cols = max_x;

    /* 弹层：右键菜单优先，其余三个来源按视图互斥，判定层还会再 gate 一次 */
    if (ui_context_menu_active() && ui_context_menu_rect(&r->overlay_rect)) {
        r->overlay_expanded = 1;
        r->overlay = UI_MOUSE_TARGET_CONTEXT_MENU;
    } else if (r->view == VIEW_MAIN && controls_popup_expanded()) {
        r->overlay_expanded = 1;
        r->overlay = UI_MOUSE_TARGET_POPUP;
        controls_popup_rect(&r->overlay_rect);
    } else if (r->view == VIEW_SETTINGS) {
        /* 选择弹层用逻辑最终矩形：入场动画期间窗口在移动，鼠标命中
         * 仍以它落定后的位置为准（不追移动目标）。 */
        if (settings_sel_rect(&r->overlay_rect)) {
            r->overlay_expanded = 1;
            r->overlay = UI_MOUSE_TARGET_SEL_MENU;
        } else if (layout_editor_menu_open()) {
            r->overlay_expanded = 1;
            r->overlay = UI_MOUSE_TARGET_EDITOR_MENU;
            layout_editor_menu_rect(&r->overlay_rect);
        }
    }

    r->editor_active = layout_editor_active() ? 1 : 0;

    if (r->view == VIEW_MAIN) {
        measure_main_view_regions(r);
    } else {
        measure_menu_view_regions(r);
    }
}

/* ============================================================
 * Playlist row hit detection
 * ============================================================ */

int get_playlist_index_from_window_row(int window_y, int *display_index, int *actual_index)
{
    if (!win_playlist) return 0;

    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;

    int content_height = h - 2;
    int visible_lines = content_height;
    if (visible_lines <= 0 || window_y < 1 || window_y >= 1 + visible_lines) return 0;

    int clicked_display_index = get_playlist_scroll_offset() + (window_y - 1);

    /* 队列页先按队列长度校验；队列可以比播放列表长/短，不能复用
     * 播放列表的 count/sort/tree 映射。 */
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        int queue_count = player_queue_count();
        if (clicked_display_index < 0 ||
            clicked_display_index >= queue_count) {
            return 0;
        }
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = player_queue_index_at(clicked_display_index);
        return 1;
    }

    if (g_search_state.active || g_search_state.in_progress) {
        pthread_mutex_lock(&g_search_mutex);
        int snap_count = g_search_state.result_count;
        int idx = (snap_count > 0 && clicked_display_index >= 0 && clicked_display_index < snap_count)
                  ? g_search_state.result_indices[clicked_display_index] : -1;
        pthread_mutex_unlock(&g_search_mutex);
        if (idx < 0) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = idx;
        return 1;
    }

    if (g_sort_state.active) {
        if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = g_sort_state.sorted_indices[clicked_display_index];
        return 1;
    }

    /* Tree mode: index is a visible line, actual is the track index */
    else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (clicked_display_index < 0 || clicked_display_index >= playlist_visible_count()) return 0;
        if (display_index) *display_index = clicked_display_index;
        if (actual_index)  *actual_index = get_visible_node_track_index(clicked_display_index);
        return 1;
    }

    if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) return 0;
    if (display_index) *display_index = clicked_display_index;
    if (actual_index)  *actual_index = clicked_display_index;
    return 1;
}

/* ============================================================
 * Control button hit detection
 * ============================================================ */

int get_control_index_from_window_point(int window_y, int window_x)
{
    if (!win_controls) return -1;

    int h, w;
    getmaxyx(win_controls, h, w);

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);

    for (int i = 0; i < CONTROL_COUNT - 1; i++) {
        if (layout.button_row[i] < 0 || layout.button_width[i] <= 0) continue;
        if (window_y == layout.button_row[i] &&
            window_x >= layout.button_col[i] &&
            window_x < layout.button_col[i] + layout.button_width[i]) {
            return i;
        }
    }

    return -1;
}

/* 滚轮要认进度条那一行（它不是按钮，get_control_index_from_window_point
 * 够不着）。与渲染共用 controls_compute_layout 的同一份行号。 */
static int controls_row_index(int window_y, int window_x)
{
    if (!win_controls) return -1;

    int h, w;
    getmaxyx(win_controls, h, w);

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);

    if (layout.progress_row >= 0 && window_y == layout.progress_row) {
        return CONTROL_IDX_PROGRESS;
    }
    return get_control_index_from_window_point(window_y, window_x);
}

/* ============================================================
 * Lyric line hit detection
 * ============================================================ */

int get_lyric_index_from_window_row(int window_y, int *lyric_index, double *timestamp)
{
    if (!win_lyrics) return 0;

    int h, w;
    getmaxyx(win_lyrics, h, w);
    (void)w;

    /* 歌词行与高亮行归后端（门面只读）；跳转光标属界面私有 */
    int highlight = -1;
    int total = player_lyrics_total();
    if (!player_lyrics_highlight(&highlight, NULL, NULL) || total <= 0 || highlight < 0) {
        return 0;
    }

    int content_top = 1;
    int visible_lines = h - content_top - 1;
    if (visible_lines <= 0 || window_y < content_top || window_y >= content_top + visible_lines) {
        return 0;
    }

    int current_center_idx = (g_lyric_cursor_mode && g_lyric_cursor_index >= 0)
        ? g_lyric_cursor_index
        : highlight;

    int start_idx = current_center_idx - (visible_lines / 2);
    if (start_idx < 0) start_idx = 0;
    if (start_idx + visible_lines > total) start_idx = total - visible_lines;
    if (start_idx < 0) start_idx = 0;

    int clicked_lyric_index = start_idx + (window_y - content_top);
    if (clicked_lyric_index < 0 || clicked_lyric_index >= total) {
        return 0;
    }

    LyricLine line;
    if (player_lyrics_line_at(clicked_lyric_index, &line) != 0) {
        return 0;
    }

    if (lyric_index) *lyric_index = clicked_lyric_index;
    if (timestamp)   *timestamp = line.timestamp;
    return 1;
}

/* ============================================================
 * Menu hint bar click (column -> action)
 * ============================================================ */

int get_menu_hint_action_from_column(int screen_x, ActionId *action)
{
    MenuHintItem items[9];
    int count = build_menu_hint_items(items, 9);
    int col = 2;

    for (int i = 0; i < count; i++) {
        if (screen_x >= col && screen_x < col + items[i].width) {
            if (action) *action = items[i].action;
            return 1;
        }
        col += items[i].width + 2;
    }
    return 0;
}

/* ============================================================
 * 各归属者的动作
 * ============================================================ */

static void dispatch_hint_bar(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    /* 提示条只有点击有意义：滚轮落在这里什么都不滚（事件仍被认领） */
    if (!ev->click) return;

    ActionId action = ACTION_NONE;
    if (get_menu_hint_action_from_column(ev->x, &action) && action != ACTION_NONE) {
        log_debug("mouse", "hint bar column %d -> action %d", d->col, (int)action);
        execute_global_action(action);
    }
}

static void dispatch_popup(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->wheel) {
        controls_popup_handle_wheel(ev->wheel);
        return;
    }
    if (!ev->click) return;

    /* 弹层内=选中该行并按下；弹层外=收起。两种情形都归弹层：
     * 收起之后绝不把这次点击再交给背后的窗格（旧代码会，于是
     * 一次点击既收了弹层又播了某首歌）。 */
    (void)d;
    popup_handle_mouse_click(ev->y, ev->x);
}

/* 播放列表窗格：点击沿用既有语义（单击即播放/展开目录），
 * 滚轮只移动选中项，并且只影响鼠标所在的那块窗格。 */
static void dispatch_playlist(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->right_click) {
        /* 右键只做"选中该行 + 在该点弹菜单"，绝不播放。
         * 选中状态按当前形态写到对应变量，菜单动作与键盘走同一路径。 */
        g_control_focus = 0;

        UIContextMenuContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.view = VIEW_MAIN;
        ctx.target = UI_MOUSE_TARGET_PLAYLIST;
        ctx.area = UI_CTX_AREA_LIST;
        ctx.list_mode = g_playlist_tab_mode;
        ctx.queue_mode = (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE);
        ctx.search_active = (g_search_state.active || g_search_state.in_progress) ? 1 : 0;
        ctx.queue_revision = player_queue_revision();
        ctx.track_index = -1;

        int display_index = -1;
        int actual_index = -1;
        if (!g_search_state.in_progress &&
            get_playlist_index_from_window_row(d->row, &display_index, &actual_index)) {
            ctx.valid = 1;
            ctx.index = display_index;

            if (g_search_state.active) {
                pthread_mutex_lock(&g_search_mutex);
                g_search_state.selected_index = display_index;
                pthread_mutex_unlock(&g_search_mutex);
                ctx.track_index = actual_index;
            } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                g_queue_selected_index = display_index;
                ctx.track_index = player_queue_index_at(display_index);
            } else {
                g_selected_index = display_index;
                if (playlist_tree_is_active() &&
                    g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                    int node_type = get_visible_node_type(display_index);
                    if (node_type == TREE_NODE_DIRECTORY) {
                        ctx.is_dir = 1;
                        ctx.track_index = -1;
                    } else {
                        ctx.track_index = get_visible_node_track_index(display_index);
                    }
                } else {
                    ctx.track_index = actual_index;
                }
            }
        }

        render_playlist_content();
        render_controls();
        (void)ui_context_menu_open(main_list_context_menu_build,
                                   main_list_context_dispatch,
                                   &ctx, ev->y, ev->x);
        return;
    }

    if (ev->wheel) {
        /* 列表高亮只在 g_control_focus==0 时画得出来：滚轮要让焦点跟着指针走 */
        g_control_focus = 0;
        ui_move_main_list_selection(ev->wheel, 0);
        render_controls();
        return;
    }
    if (!ev->click) return;

    const int window_y = d->row;
    const int window_x = d->col;
    (void)window_x;

    int display_index = -1, actual_index = -1;
    if (!get_playlist_index_from_window_row(window_y, &display_index, &actual_index)) {
        log_debug("mouse", "playlist row=%d 未命中条目（tree=%d mode=%d 可见=%d 总数=%d）",
                  window_y, (int)playlist_tree_is_active(), (int)g_playlist_tab_mode,
                  playlist_visible_count(), playlist_count());
        return;   /* 空行/边框行：事件已归本窗格，不再往下穿透 */
    }
    log_debug("mouse", "playlist row=%d → display=%d actual=%d", window_y,
              display_index, actual_index);

    g_control_focus = 0;
    if (g_search_state.active || g_search_state.in_progress) {
        if (g_search_state.in_progress) {
            search_async_cancel();
            pthread_mutex_lock(&g_search_mutex);
            g_search_state.in_progress = 0;
            pthread_mutex_unlock(&g_search_mutex);
        }
        g_search_state.selected_index = display_index;
        ui_play_track_index(actual_index);
        g_search_state.active = 0;
        g_selected_index = g_sort_state.active ? 0 : actual_index;
        render_playlist_content();
    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        g_queue_selected_index = display_index;
        if (display_index >= 0 && display_index < player_queue_count()) {
            player_queue_play(display_index);
        }
    } else {
        g_selected_index = display_index;
        /* Tree mode: toggle directory or play file */
        if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
            && !g_search_state.active) {
            int node_type = get_visible_node_type(display_index);
            if (node_type == TREE_NODE_DIRECTORY) {
                int tree_idx = get_visible_node_tree_index(display_index);
                playlist_toggle_directory_expand(tree_idx);
                if (g_selected_index >= playlist_visible_count())
                    g_selected_index = playlist_visible_count() - 1;
                if (g_selected_index < 0) g_selected_index = 0;
                render_playlist_content();
                return;
            }
            int track_idx = get_visible_node_track_index(display_index);
            if (track_idx >= 0) ui_play_track_index(track_idx);
        } else {
            ui_play_track_index(actual_index);
        }
    }
    render_controls();
}

static void dispatch_controls(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    const int window_y = d->row;
    const int window_x = d->col;

    /* 命中表随标签文字变化（"播放/暂停"、"倍速:1.25x"、"模式:单曲循环"宽度都不同），
     * 每次落到控制区都打一行：排查"滚轮/点击在这一格没反应"时不必再猜坐标。 */
    if ((ev->wheel || ev->click) && win_controls) {
        int ch, cw;
        getmaxyx(win_controls, ch, cw);
        ControlsLayout lay;
        controls_compute_layout(ch, cw, &lay);
        log_debug("mouse",
                  "控制区命中表 窗%dx%d 进度行=%d 格%d=%d:%d:%d 格%d=%d:%d:%d 格%d=%d:%d:%d 格%d=%d:%d:%d",
                  ch, cw, lay.progress_row,
                  CONTROL_IDX_LOOP, lay.button_row[CONTROL_IDX_LOOP],
                  lay.button_col[CONTROL_IDX_LOOP], lay.button_width[CONTROL_IDX_LOOP],
                  CONTROL_IDX_SPEED, lay.button_row[CONTROL_IDX_SPEED],
                  lay.button_col[CONTROL_IDX_SPEED], lay.button_width[CONTROL_IDX_SPEED],
                  CONTROL_IDX_VOLUME, lay.button_row[CONTROL_IDX_VOLUME],
                  lay.button_col[CONTROL_IDX_VOLUME], lay.button_width[CONTROL_IDX_VOLUME],
                  CONTROL_IDX_KARAOKE, lay.button_row[CONTROL_IDX_KARAOKE],
                  lay.button_col[CONTROL_IDX_KARAOKE], lay.button_width[CONTROL_IDX_KARAOKE]);
    }

    if (ev->wheel || ev->click) {
        const int probe = ev->wheel ? controls_row_index(window_y, window_x)
                                    : get_control_index_from_window_point(window_y, window_x);
        log_debug("mouse", "控制区 窗口(%d,%d) → 控件 %d", window_y, window_x, probe);
    }

    if (ev->wheel) {
        /* 滚轮只作用于指针所在的那一行：进度 ±5s、音量 ±5%、倍速换档。
         * 与点击一样先把焦点交给控制区，否则高亮不跟着走。 */
        int idx = controls_row_index(window_y, window_x);
        if (idx < 0) return;                 /* 状态行/可视化行：无滚轮语义 */
        g_control_focus = 1;
        controls_handle_wheel_control(idx, ev->wheel);
        return;
    }
    if (!ev->click) return;

    int control_index = get_control_index_from_window_point(window_y, window_x);
    if (control_index < 0) return;

    g_control_focus = 1;
    /* 进度条控件不可选中（且其所在行不在按钮命中区内）：
     * 仅在点击到按钮控件时才更新当前选中索引 */
    if (control_index != CONTROL_IDX_PROGRESS) {
        g_current_control_idx = control_index;
    }

    if (control_index != CONTROL_IDX_PROGRESS) {
        activate_current_control();
        render_playlist_content();
    }
    render_controls();
}

static void dispatch_lyrics(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    if (ev->wheel) {
        /* 歌词窗格的滚轮＝快进/快退一句：直接移动跳转光标并跳过去，
         * 不改用户的"歌词光标模式"开关（那是 Ctrl+L 的语义）。 */
        ui_lyric_cursor_step(ev->wheel);
        return;
    }
    if (!ev->click) return;

    int lyric_index = -1;
    double target_timestamp = 0.0;
    if (!get_lyric_index_from_window_row(d->row, &lyric_index, &target_timestamp)) {
        return;
    }

    /* 跳转光标是界面私有状态：点哪一行就记哪一行，然后请后端跳转 */
    g_lyric_cursor_index = lyric_index;

    player_seek_seconds((int)target_timestamp);
    render_lyrics();
}

/* 子视图：侧栏/内容区的命中由各视图自己解释（它才知道自己的行号）。 */
static void dispatch_menu_view(const UIMouseEvent *ev, const UIMouseDecision *d)
{
    switch (g_current_view) {
        case VIEW_SETTINGS:  settings_handle_mouse(ev, d);   break;
        case VIEW_HISTORY:   history_handle_mouse(ev, d);    break;
        case VIEW_FAVORITES: favorites_handle_mouse(ev, d);  break;
        case VIEW_PLAYLIST:  playlist_view_handle_mouse(ev, d); break;
        case VIEW_INFO:      info_view_handle_mouse(ev, d);  break;
        case VIEW_HELP:      help_view_handle_mouse(ev, d);  break;
        case VIEW_LANGUAGE:  language_view_handle_mouse(ev, d); break;
        default:
            /* VIEW_MAIN 不走这里；其余子视图均已覆盖。 */
            break;
    }
}

/* ============================================================
 * 全局入口
 * ============================================================ */

/*
 * 唯一的鼠标入口（ui.c 的 KEY_MOUSE 分支只调它）。
 * 返回 1 = 本次事件已被某个归属者消费。
 */
int handle_global_mouse_event(const MEVENT *event)
{
    UIMouseEvent ev;
    normalize_mouse_event(event, &ev);
    if (!ev.click && !ev.wheel && !ev.right_click) return 0;

    UIMouseRegions regions;
    build_regions(&regions);

    UIMouseDecision d = ui_mouse_resolve(&regions, &ev);
    if (!d.swallow) return 0;

    log_debug("mouse", "(%d,%d) %s%s wheel=%d -> target=%d row=%d col=%d",
              ev.y, ev.x,
              ev.click ? "click" : (ev.right_click ? "right-click" : "wheel"),
              ev.click && ev.click_count > 1 ? "(multi)" : "",
              ev.wheel, (int)d.target, d.row, d.col);

    switch (d.target) {
        case UI_MOUSE_TARGET_INPUT:
        case UI_MOUSE_TARGET_INFO:
        case UI_MOUSE_TARGET_VISUAL:
            /* 已认领但不作为：模态输入独占，或该窗格没有左键/滚轮/右键动作。
             * 主视图信息窗格不设右键菜单；科乐美流光菜单只属于 F6 信息页。 */
            break;

        case UI_MOUSE_TARGET_CONTEXT_MENU:
            ui_context_menu_handle_mouse(&ev, &d);
            break;

        case UI_MOUSE_TARGET_POPUP:
            if (ev.right_click) {
                /* 弹层展开时右键 = 取消弹层并吞掉，不叠加新菜单。 */
                log_debug("mouse", "right-click cancels controls popup");
                (void)controls_popup_cancel();
            } else {
                dispatch_popup(&ev, &d);
            }
            break;

        case UI_MOUSE_TARGET_SEL_MENU:
            if (ev.right_click) {
                log_debug("mouse", "right-click cancels settings select menu");
                (void)settings_cancel_sel_menu();
            } else {
                settings_handle_mouse(&ev, &d);
            }
            break;

        case UI_MOUSE_TARGET_EDITOR_MENU:
            if (ev.right_click) {
                log_debug("mouse", "right-click cancels layout editor menu");
                (void)layout_editor_handle_key(27);
            } else {
                layout_editor_handle_mouse(&ev, &d);
            }
            if (!layout_editor_active()) {
                rerender_active_view();
            }
            break;

        case UI_MOUSE_TARGET_EDITOR_PANE:
            /* 编辑器主模式：右键在指针处打开"窗格操作"菜单（由编辑器处理）。 */
            layout_editor_handle_mouse(&ev, &d);
            if (!layout_editor_active()) {
                rerender_active_view();
            }
            break;

        case UI_MOUSE_TARGET_HINT_BAR:
            dispatch_hint_bar(&ev, &d);
            break;

        case UI_MOUSE_TARGET_BROWSER:
            library_handle_mouse(&ev, &d);
            break;

        case UI_MOUSE_TARGET_PLAYLIST:
            dispatch_playlist(&ev, &d);
            break;

        case UI_MOUSE_TARGET_CONTROLS:
            dispatch_controls(&ev, &d);
            break;

        case UI_MOUSE_TARGET_LYRICS:
            dispatch_lyrics(&ev, &d);
            break;

        case UI_MOUSE_TARGET_SIDEBAR:
        case UI_MOUSE_TARGET_CONTENT:
            dispatch_menu_view(&ev, &d);
            break;

        case UI_MOUSE_TARGET_NONE:
        case UI_MOUSE_TARGET_COUNT:
        default:
            break;
    }

    return 1;
}
