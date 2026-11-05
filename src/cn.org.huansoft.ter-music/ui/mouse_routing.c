/**
 * @file mouse_routing.c
 * @brief 鼠标事件归属判定（纯逻辑，无 ncurses 依赖）
 *
 * 本文件是"谁该响应这次鼠标事件"的**唯一权威**，尤其负责菜单展开场景下
 * 弹层与底层内容区的竞争态：判定只依赖传入的区域表与标志位，不读任何
 * ncurses 窗格，因此可以在 scripts/test/test_mouse_routing.c 里对
 * 「展开标志 × 指针区域 × 事件类型」做穷举断言。
 *
 * 规则表（与 include/cn.org.huansoft.ter-music/ui/mouse.h 的头注一致）：
 *
 *   优先级 1  input_exclusive      → 吞掉一切（模态输入/快捷键捕获进行中）
 *   优先级 2  overlay_expanded     → 弹层拥有本次事件
 *              · 滚轮：无论指针在哪，只滚弹层
 *              · 点击在弹层内：交给弹层
 *              · 点击在弹层外：收起弹层 + 吞掉（不穿透到背后窗格）
 *              · 标志按视图 gate：陈旧标志不能跨视图偷事件
 *   优先级 3  编辑器主模式          → 整屏窗格都是编辑对象（按屏幕坐标就近选窗格）
 *   优先级 4  hint_bar 行           → 归属提示条（滚轮落在这里不滚任何东西）
 *   优先级 5  VIEW_MAIN 窗格        → 浏览器优先于播放列表（同一窗格两种语义）
 *                                    播放列表 / 控制区 / 歌词 / 信息 / 频谱黑胶
 *   优先级 6  子视图                → 侧栏列 vs 内容列
 *
 * 右键（right_click）与左键同权：只改变"打开菜单"这一语义，绝不播放/执行；
 * overlay 展开时右键由各视图取消弹层并吞掉，不叠加第二层菜单。
 *
 * 判定只回答"归谁"，不回答"做什么"：无鼠标动作的区域（信息、频谱、黑胶、
 * 提示条上的滚轮）照样被认领并吞掉，避免同一事件被两个属主各做一次。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/mouse.h"

/* ── 矩形工具 ─────────────────────────────────────────────────────── */

int ui_rect_contains(const UIRect *r, int y, int x)
{
    if (!r || r->rows <= 0 || r->cols <= 0) return 0;
    if (y < r->top || y >= r->top + r->rows) return 0;
    if (x < r->left || x >= r->left + r->cols) return 0;
    return 1;
}

/* ── 判定内部助手 ─────────────────────────────────────────────────── */

static UIMouseDecision decision_for(const UIMouseEvent *ev)
{
    UIMouseDecision d;
    d.target = UI_MOUSE_TARGET_NONE;
    d.swallow = 0;
    d.inside_overlay = 0;
    d.row = -1;
    d.col = -1;
    d.wheel = ev ? ev->wheel : 0;
    d.click_count = (ev && ev->click) ? ev->click_count : 0;
    d.right_click = ev ? ev->right_click : 0;
    return d;
}

/* 弹层标志是否属于当前视图：跨视图残留的"展开"一律视为无效。
 * 上下文菜单由边缘层按 ui_context_menu_active()（含视图匹配）置位，
 * 因此这里放行；陈旧标志不会跨视图生效。 */
static int overlay_allowed_in_view(const UIMouseRegions *r)
{
    switch (r->overlay) {
        case UI_MOUSE_TARGET_CONTEXT_MENU:
            return 1;
        case UI_MOUSE_TARGET_POPUP:
            return r->view == VIEW_MAIN;
        case UI_MOUSE_TARGET_SEL_MENU:
        case UI_MOUSE_TARGET_EDITOR_MENU:
            return r->view == VIEW_SETTINGS;
        default:
            return 0;
    }
}

/* 命中某区域：填本地坐标（区域无效时返回 0）。
 * 点击与滚轮都算消费——一次事件只归一个属主，不允许"这个窗格不响应，
 * 于是下一个窗格接手"；无鼠标动作的窗格照样吞掉。 */
static int hit_region(const UIRect *rect, UIMouseTarget target,
                      const UIMouseEvent *ev, UIMouseDecision *d)
{
    if (!ui_rect_contains(rect, ev->y, ev->x)) return 0;

    d->target = target;
    d->row = ev->y - rect->top;
    d->col = ev->x - rect->left;
    d->swallow = 1;
    return 1;
}

/* ── 判定主入口 ───────────────────────────────────────────────────── */

UIMouseDecision ui_mouse_resolve(const UIMouseRegions *r, const UIMouseEvent *ev)
{
    UIMouseDecision d = decision_for(ev);

    if (!r || !ev) return d;
    if (!ev->click && !ev->wheel && !ev->right_click) return d;   /* 只有移动/其它按钮：无事可做 */

    /* 优先级 1：模态输入独占 */
    if (r->input_exclusive) {
        d.target = UI_MOUSE_TARGET_INPUT;
        d.swallow = 1;
        return d;
    }

    /* 优先级 2：弹层展开 */
    if (r->overlay_expanded && overlay_allowed_in_view(r)) {
        d.target = r->overlay;
        d.swallow = 1;                       /* 展开期间事件一律不落到内容区 */
        d.inside_overlay = ui_rect_contains(&r->overlay_rect, ev->y, ev->x);
        if (d.inside_overlay) {
            d.row = ev->y - r->overlay_rect.top;
            d.col = ev->x - r->overlay_rect.left;
        }
        return d;
    }

    /* 优先级 3：布局编辑器主模式（整屏窗格都是它的编辑对象） */
    if (r->view == VIEW_SETTINGS && r->editor_active) {
        d.target = UI_MOUSE_TARGET_EDITOR_PANE;
        d.swallow = 1;
        d.row = ev->y;                        /* 编辑器按屏幕坐标就近选窗格 */
        d.col = ev->x;
        return d;
    }

    /* 优先级 4：底部提示条（所有视图都有） */
    if (ui_rect_contains(&r->hint_bar, ev->y, ev->x)) {
        d.target = UI_MOUSE_TARGET_HINT_BAR;
        d.swallow = 1;
        d.col = ev->x - r->hint_bar.left;
        d.row = 0;
        return d;
    }

    /* 优先级 5：主视图窗格 */
    if (r->view == VIEW_MAIN) {
        /* 曲库浏览器与播放列表共用同一块窗格，浏览器必须先判 */
        if (hit_region(&r->playlist,
                       r->library_browser ? UI_MOUSE_TARGET_BROWSER
                                          : UI_MOUSE_TARGET_PLAYLIST,
                       ev, &d)) {
            return d;
        }
        if (hit_region(&r->controls, UI_MOUSE_TARGET_CONTROLS, ev, &d))
            return d;
        if (hit_region(&r->lyrics, UI_MOUSE_TARGET_LYRICS, ev, &d))
            return d;
        if (hit_region(&r->info, UI_MOUSE_TARGET_INFO, ev, &d))
            return d;
        if (hit_region(&r->visual, UI_MOUSE_TARGET_VISUAL, ev, &d))
            return d;
        if (hit_region(&r->vinyl, UI_MOUSE_TARGET_VISUAL, ev, &d))
            return d;
        return d;
    }

    /* 优先级 6：子视图（侧栏列 vs 内容列） */
    if (hit_region(&r->sidebar, UI_MOUSE_TARGET_SIDEBAR, ev, &d))
        return d;
    if (hit_region(&r->content, UI_MOUSE_TARGET_CONTENT, ev, &d))
        return d;

    return d;
}
