/**
 * @file test_mouse_routing.c
 * @brief ui/mouse_routing.c 的单元测试：鼠标事件归属与竞争态
 *
 * 覆盖（与 include/org.yxzl.ter-music/ui/mouse.h 头注的规则表逐条对应）：
 *   - ui_rect_contains 的边界与"无效矩形"；
 *   - 优先级 1：模态输入独占（点击与滚轮都吞）；
 *   - 优先级 2：弹层展开时滚轮**只滚弹层**（指针在别的窗格也一样）、
 *     弹层内点击归弹层、弹层外点击"收起 + 吞掉"不穿透；
 *   - 展开标志按视图 gate：陈旧标志不能跨视图偷事件；
 *   - 优先级 3~6：编辑器主模式、提示条、主视图窗格（含曲库浏览器优先于
 *     播放列表）、子视图侧栏/内容列分界；
 *   - 空档区域无人认领、非点击非滚轮事件不产生动作；
 *   - 极小终端（所有矩形无效）不崩、不吞；
 *   - 穷举矩阵：任意「视图 × 标志 × 指针位置 × 事件」的组合，
 *     被吞掉的事件必定有且仅有一个归属者。
 *
 * 运行：scripts/test/run-unit-tests.sh test_mouse_routing
 * 依赖（见同名 .srcs）：src/org.yxzl.ter-music/ui/mouse_routing.c
 */

#include "ui/mouse.h"

#include <stdio.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

/* ── 夹具：一个 80×24 终端的常见几何 ─────────────────────────────── */

static UIRect R(int top, int left, int rows, int cols)
{
    UIRect r;
    r.top = top; r.left = left; r.rows = rows; r.cols = cols;
    return r;
}

static UIMouseRegions fixture(void)
{
    UIMouseRegions r;
    r.view = VIEW_MAIN;
    r.input_exclusive = 0;
    r.overlay_expanded = 0;
    r.overlay = UI_MOUSE_TARGET_NONE;
    r.overlay_rect = R(12, 40, 5, 12);     /* 落在控制区窗格内部 */
    r.editor_active = 0;
    r.library_browser = 0;
    r.hint_bar  = R(23, 0, 1, 80);
    r.playlist  = R(1, 1, 10, 30);         /* 行 1..10 */
    r.controls  = R(11, 1, 6, 60);         /* 行 11..16 */
    r.lyrics    = R(1, 32, 10, 20);        /* 行 1..10，列 32..51 */
    r.info      = R(17, 1, 5, 30);         /* 行 17..21 */
    r.visual    = R(17, 32, 5, 40);
    r.vinyl     = R(0, 0, 0, 0);           /* 黑胶：默认不占位，由专调用例摆放 */
    r.sidebar   = R(2, 1, 8, 18);          /* 子视图 */
    r.content   = R(2, 21, 20, 58);
    return r;
}

static UIMouseEvent ev_click(int y, int x, int count)
{
    UIMouseEvent e;
    e.y = y; e.x = x; e.click = 1; e.click_count = count; e.wheel = 0;
    e.right_click = 0;
    return e;
}

static UIMouseEvent ev_right(int y, int x)
{
    UIMouseEvent e;
    e.y = y; e.x = x; e.click = 0; e.click_count = 0; e.wheel = 0;
    e.right_click = 1;
    return e;
}

static UIMouseEvent ev_wheel(int y, int x, int delta)
{
    UIMouseEvent e;
    e.y = y; e.x = x; e.click = 0; e.click_count = 0; e.wheel = delta;
    e.right_click = 0;
    return e;
}

static UIMouseEvent ev_nothing(int y, int x)
{
    UIMouseEvent e;
    e.y = y; e.x = x; e.click = 0; e.click_count = 0; e.wheel = 0;
    e.right_click = 0;
    return e;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

static void test_rect_contains(void)
{
    UIRect r = R(5, 10, 3, 4);            /* 行 5..7，列 10..13 */

    CHECK(ui_rect_contains(&r, 5, 10) == 1, "矩形左上角含入");
    CHECK(ui_rect_contains(&r, 7, 13) == 1, "矩形右下角含入");
    CHECK(ui_rect_contains(&r, 4, 10) == 0, "上一行不含");
    CHECK(ui_rect_contains(&r, 8, 13) == 0, "下一行不含");
    CHECK(ui_rect_contains(&r, 5, 9) == 0, "左一列不含");
    CHECK(ui_rect_contains(&r, 5, 14) == 0, "右一列不含");
    CHECK(ui_rect_contains(NULL, 5, 10) == 0, "空矩形不命中");

    UIRect zero = R(0, 0, 0, 0);
    CHECK(ui_rect_contains(&zero, 0, 0) == 0, "rows=0 视为不存在");
    UIRect neg = R(0, 0, 3, -1);
    CHECK(ui_rect_contains(&neg, 1, 1) == 0, "cols<0 视为不存在");
}

static void test_priority1_input_exclusive(void)
{
    UIMouseRegions r = fixture();
    r.input_exclusive = 1;

    UIMouseEvent click = ev_click(5, 5, 1);      /* 播放列表正中 */
    UIMouseDecision b = ui_mouse_resolve(&r, &click);
    CHECK(b.target == UI_MOUSE_TARGET_INPUT && b.swallow == 1,
          "模态输入：点击归 INPUT 并吞掉");

    UIMouseEvent wheel = ev_wheel(13, 45, 1);    /* 弹层内也一样 */
    r.overlay_expanded = 1;
    r.overlay = UI_MOUSE_TARGET_POPUP;
    UIMouseDecision c = ui_mouse_resolve(&r, &wheel);
    CHECK(c.target == UI_MOUSE_TARGET_INPUT && c.swallow == 1,
          "模态输入优先于弹层");
}

static void test_priority2_overlay_owns_wheel(void)
{
    UIMouseRegions r = fixture();
    r.overlay_expanded = 1;
    r.overlay = UI_MOUSE_TARGET_POPUP;

    /* 滚轮在播放列表窗格上：必须归弹层，绝不能滚列表 */
    UIMouseEvent w1 = ev_wheel(3, 5, -1);
    UIMouseDecision d1 = ui_mouse_resolve(&r, &w1);
    CHECK(d1.target == UI_MOUSE_TARGET_POPUP && d1.swallow == 1 && d1.wheel == -1,
          "弹层展开：列表窗格上的滚轮归弹层");
    CHECK(d1.inside_overlay == 0, "该滚轮位置在弹层外（只影响归属，不影响吞掉）");

    /* 滚轮在子视图内容区位置（主视图里该处无窗格）：仍归弹层 */
    UIMouseEvent w2 = ev_wheel(20, 70, 1);
    UIMouseDecision d2 = ui_mouse_resolve(&r, &w2);
    CHECK(d2.target == UI_MOUSE_TARGET_POPUP && d2.swallow == 1 && d2.wheel == 1,
          "弹层展开：任意位置的滚轮都只滚弹层");

    /* 滚轮在弹层内 */
    UIMouseEvent w3 = ev_wheel(13, 45, 1);
    UIMouseDecision d3 = ui_mouse_resolve(&r, &w3);
    CHECK(d3.target == UI_MOUSE_TARGET_POPUP && d3.inside_overlay == 1,
          "弹层内的滚轮同样归弹层");
    CHECK(d3.row == 13 - r.overlay_rect.top && d3.col == 45 - r.overlay_rect.left,
          "弹层内坐标转成弹层本地坐标");
}

static void test_priority2_overlay_click_inside_outside(void)
{
    UIMouseRegions r = fixture();
    r.overlay_expanded = 1;
    r.overlay = UI_MOUSE_TARGET_POPUP;

    UIMouseEvent in = ev_click(14, 46, 1);
    UIMouseDecision di = ui_mouse_resolve(&r, &in);
    CHECK(di.target == UI_MOUSE_TARGET_POPUP && di.inside_overlay == 1 && di.swallow == 1,
          "弹层内点击交给弹层");
    CHECK(di.row == 2 && di.col == 6, "弹层内点击的本地行列正确");

    /* 弹层外、播放列表上：收起 + 吞掉（不穿透成"点第 N 行"） */
    UIMouseEvent out = ev_click(4, 5, 1);
    UIMouseDecision do_ = ui_mouse_resolve(&r, &out);
    CHECK(do_.target == UI_MOUSE_TARGET_POPUP && do_.inside_overlay == 0 && do_.swallow == 1,
          "弹层外点击被吞掉，不落到播放列表");
    CHECK(do_.row == -1 && do_.col == -1, "弹层外点击不给窗格本地坐标");

    /* 弹层外、提示条上：同样不穿透（展开期间弹层优先于提示条） */
    UIMouseEvent hint = ev_click(23, 10, 1);
    UIMouseDecision dh = ui_mouse_resolve(&r, &hint);
    CHECK(dh.target == UI_MOUSE_TARGET_POPUP && dh.inside_overlay == 0 && dh.swallow == 1,
          "弹层外点击提示条也先收起弹层，不做切页");

    /* 弹层外、歌词窗格上 */
    UIMouseEvent lyr = ev_click(2, 40, 1);
    UIMouseDecision dl = ui_mouse_resolve(&r, &lyr);
    CHECK(dl.target == UI_MOUSE_TARGET_POPUP && dl.swallow == 1,
          "弹层外点击歌词窗格同样被吞掉");
}

static void test_priority2_stale_flag_gated_by_view(void)
{
    /* 控制区弹层标志残留到设置视图：不得偷事件 */
    UIMouseRegions r = fixture();
    r.view = VIEW_SETTINGS;
    r.overlay_expanded = 1;
    r.overlay = UI_MOUSE_TARGET_POPUP;

    UIMouseEvent w = ev_wheel(5, 30, 1);
    UIMouseDecision d = ui_mouse_resolve(&r, &w);
    CHECK(d.target == UI_MOUSE_TARGET_CONTENT && d.swallow == 1,
          "跨视图残留的弹层标志无效：滚轮照常归内容区");

    /* 反过来：设置选择弹层标志残留到主视图 */
    UIMouseRegions m = fixture();
    m.view = VIEW_MAIN;
    m.overlay_expanded = 1;
    m.overlay = UI_MOUSE_TARGET_SEL_MENU;
    UIMouseEvent c = ev_click(5, 5, 1);
    UIMouseDecision d2 = ui_mouse_resolve(&m, &c);
    CHECK(d2.target == UI_MOUSE_TARGET_PLAYLIST && d2.swallow == 1,
          "主视图里残留的设置弹层标志不生效");

    /* 未知道也当作无效标志 */
    UIMouseRegions u = fixture();
    u.overlay_expanded = 1;
    u.overlay = UI_MOUSE_TARGET_NONE;
    UIMouseEvent c2 = ev_click(5, 5, 1);
    UIMouseDecision d3 = ui_mouse_resolve(&u, &c2);
    CHECK(d3.target == UI_MOUSE_TARGET_PLAYLIST, "overlay 目标未填写时不吞事件");
}

static void test_priority3_editor(void)
{
    UIMouseRegions r = fixture();
    r.view = VIEW_SETTINGS;
    r.editor_active = 1;

    UIMouseEvent c = ev_click(6, 8, 1);
    UIMouseDecision d = ui_mouse_resolve(&r, &c);
    CHECK(d.target == UI_MOUSE_TARGET_EDITOR_PANE && d.swallow == 1,
          "编辑器主模式：点击交给编辑器选窗格");
    CHECK(d.row == 6 && d.col == 8, "编辑器拿的是屏幕坐标（就近选窗格用）");

    UIMouseEvent w = ev_wheel(6, 8, 1);
    UIMouseDecision dw = ui_mouse_resolve(&r, &w);
    CHECK(dw.target == UI_MOUSE_TARGET_EDITOR_PANE && dw.swallow == 1,
          "编辑器主模式：滚轮也归编辑器（其内不滚动，但不穿透）");

    /* 编辑器的模态菜单展开时：滚轮只滚菜单 */
    r.overlay_expanded = 1;
    r.overlay = UI_MOUSE_TARGET_EDITOR_MENU;
    r.overlay_rect = R(4, 20, 7, 40);
    UIMouseEvent wm = ev_wheel(2, 2, -1);
    UIMouseDecision dme = ui_mouse_resolve(&r, &wm);
    CHECK(dme.target == UI_MOUSE_TARGET_EDITOR_MENU && dme.swallow == 1,
          "编辑器模态菜单展开：滚轮归菜单，不选窗格");

    UIMouseEvent cm = ev_click(2, 2, 1);
    UIMouseDecision dmc = ui_mouse_resolve(&r, &cm);
    CHECK(dmc.target == UI_MOUSE_TARGET_EDITOR_MENU && dmc.inside_overlay == 0,
          "编辑器菜单外点击＝收起并吞掉，不选中背后的窗格");
}

static void test_priority4_hint_bar(void)
{
    UIMouseRegions r = fixture();

    UIMouseEvent c = ev_click(23, 10, 1);
    UIMouseDecision d = ui_mouse_resolve(&r, &c);
    CHECK(d.target == UI_MOUSE_TARGET_HINT_BAR && d.swallow == 1,
          "提示条行点击归提示条");
    CHECK(d.col == 10, "提示条给出本地列（供功能键栏分列命中）");

    UIMouseEvent w = ev_wheel(23, 10, 1);
    UIMouseDecision dw = ui_mouse_resolve(&r, &w);
    CHECK(dw.target == UI_MOUSE_TARGET_HINT_BAR && dw.swallow == 1,
          "提示条行上的滚轮被吞掉（提示条不可滚）");

    /* 极小布局里窗格与提示条重叠：提示条优先（与改名前的检查顺序一致） */
    UIMouseRegions tight = fixture();
    tight.playlist = R(1, 1, 23, 30);      /* 一直铺到提示条行 */
    UIMouseEvent tc = ev_click(23, 5, 1);
    UIMouseDecision td = ui_mouse_resolve(&tight, &tc);
    CHECK(td.target == UI_MOUSE_TARGET_HINT_BAR, "重叠时提示条仍优先于窗格");
}

static void test_priority5_main_view_panes(void)
{
    UIMouseRegions r = fixture();

    UIMouseEvent pc = ev_click(4, 5, 1);
    UIMouseDecision pd = ui_mouse_resolve(&r, &pc);
    CHECK(pd.target == UI_MOUSE_TARGET_PLAYLIST && pd.swallow == 1,
          "播放列表窗格命中");
    CHECK(pd.row == 3 && pd.col == 4, "播放列表本地行列 = 屏幕坐标减窗格原点");

    UIMouseEvent cc = ev_click(12, 20, 1);
    UIMouseDecision cd = ui_mouse_resolve(&r, &cc);
    CHECK(cd.target == UI_MOUSE_TARGET_CONTROLS && cd.row == 1,
          "控制区窗格命中");

    UIMouseEvent lc = ev_click(3, 40, 1);
    UIMouseDecision ld = ui_mouse_resolve(&r, &lc);
    CHECK(ld.target == UI_MOUSE_TARGET_LYRICS && ld.row == 2 && ld.col == 8,
          "歌词窗格命中，列也给出（跳句只用行，列留给后续扩展）");

    UIMouseEvent ic = ev_click(19, 5, 1);
    UIMouseDecision id = ui_mouse_resolve(&r, &ic);
    CHECK(id.target == UI_MOUSE_TARGET_INFO && id.swallow == 1, "信息窗格命中并被吞掉");

    UIMouseEvent vc = ev_click(19, 50, 1);
    UIMouseDecision vd = ui_mouse_resolve(&r, &vc);
    CHECK(vd.target == UI_MOUSE_TARGET_VISUAL && vd.swallow == 1, "频谱/黑胶窗格命中并被吞掉");

    /* 窗格之间空档：无人认领，也不吞（留给后续可能的处理） */
    UIMouseEvent gap = ev_click(0, 0, 1);
    UIMouseDecision gd = ui_mouse_resolve(&r, &gap);
    CHECK(gd.target == UI_MOUSE_TARGET_NONE && gd.swallow == 0, "标题行空档无人认领");

    /* 滚轮同样按窗格归属，且只归指针所在的那个 */
    UIMouseEvent pw = ev_wheel(6, 5, 1);
    UIMouseDecision pwd = ui_mouse_resolve(&r, &pw);
    CHECK(pwd.target == UI_MOUSE_TARGET_PLAYLIST && pwd.wheel == 1,
          "列表窗格滚轮归列表");

    UIMouseEvent lw = ev_wheel(6, 5, -1);
    UIMouseDecision lwd = ui_mouse_resolve(&r, &lw);
    CHECK(lwd.wheel == -1 && lwd.target == UI_MOUSE_TARGET_PLAYLIST,
          "反向滚轮把 -1 原样传出去");
}

static void test_priority5_browser_precedence(void)
{
    UIMouseRegions r = fixture();
    r.library_browser = 1;

    UIMouseEvent c = ev_click(5, 5, 1);
    UIMouseDecision d = ui_mouse_resolve(&r, &c);
    CHECK(d.target == UI_MOUSE_TARGET_BROWSER && d.row == 4,
          "浏览器激活时同一窗格的点击归浏览器（修掉按播放列表行号误播放）");

    UIMouseEvent w = ev_wheel(5, 5, 1);
    UIMouseDecision dw = ui_mouse_resolve(&r, &w);
    CHECK(dw.target == UI_MOUSE_TARGET_BROWSER,
          "浏览器激活时滚轮也归浏览器，不移动播放列表选中项");

    /* 浏览器只在主视图有意义；子视图标志残留不影响子视图分派 */
    UIMouseRegions s = fixture();
    s.view = VIEW_HELP;
    s.library_browser = 1;
    UIMouseEvent sc = ev_click(5, 30, 1);
    UIMouseDecision sd = ui_mouse_resolve(&s, &sc);
    CHECK(sd.target == UI_MOUSE_TARGET_CONTENT, "浏览器标志不改变子视图归属");
}

static void test_priority6_subviews(void)
{
    for (int v = VIEW_SETTINGS; v <= VIEW_LANGUAGE; v++) {
        UIMouseRegions r = fixture();
        r.view = (ViewMode)v;

        UIMouseEvent sb = ev_click(4, 5, 1);
        UIMouseDecision db = ui_mouse_resolve(&r, &sb);
        CHECK(db.target == UI_MOUSE_TARGET_SIDEBAR && db.row == 2 && db.swallow == 1,
              "子视图侧栏列命中（含滚轮/点击通用）");

        UIMouseEvent ct = ev_wheel(6, 40, 1);
        UIMouseDecision dc = ui_mouse_resolve(&r, &ct);
        CHECK(dc.target == UI_MOUSE_TARGET_CONTENT && dc.row == 4 && dc.wheel == 1,
              "子视图内容列命中并给出本地行");

        /* 侧栏与内容之间的空档列 */
        UIMouseEvent gap = ev_click(6, 20, 1);
        UIMouseDecision dg = ui_mouse_resolve(&r, &gap);
        CHECK(dg.target == UI_MOUSE_TARGET_NONE && dg.swallow == 0,
              "侧栏与内容之间的空档不归属");
    }
}

static void test_edge_cases(void)
{
    /* 非点击非滚轮（例如仅移动、横向滚轮）：不产生任何归属 */
    UIMouseRegions r = fixture();
    UIMouseEvent none = ev_nothing(5, 5);
    UIMouseDecision d = ui_mouse_resolve(&r, &none);
    CHECK(d.target == UI_MOUSE_TARGET_NONE && d.swallow == 0, "纯移动事件无人认领");
    CHECK(d.wheel == 0 && d.click_count == 0, "纯移动事件不带增量与点击数");

    /* 双击：click_count 原样传出（属主用它决定"激活"） */
    UIMouseEvent dbl = ev_click(5, 5, 2);
    UIMouseDecision dd = ui_mouse_resolve(&r, &dbl);
    CHECK(dd.click_count == 2, "双击次数传出");

    /* 极小终端：所有矩形都无效 */
    UIMouseRegions tiny = fixture();
    tiny.hint_bar = R(0, 0, 0, 0);
    tiny.playlist = R(0, 0, 0, 0);
    tiny.controls = R(0, 0, 0, 0);
    tiny.lyrics = R(0, 0, 0, 0);
    tiny.info = R(0, 0, 0, 0);
    tiny.visual = R(0, 0, 0, 0);
    tiny.vinyl = R(0, 0, 0, 0);
    tiny.sidebar = R(0, 0, 0, 0);
    tiny.content = R(0, 0, 0, 0);
    UIMouseEvent tc = ev_click(0, 0, 1);
    UIMouseDecision td = ui_mouse_resolve(&tiny, &tc);
    CHECK(td.target == UI_MOUSE_TARGET_NONE && td.swallow == 0, "全无效矩形时不崩不吞");

    /* 弹层展开但弹层窗格没建起来（rows=0）：仍算展开，点击一律收起并吞掉 */
    UIMouseRegions broken = fixture();
    broken.overlay_expanded = 1;
    broken.overlay = UI_MOUSE_TARGET_POPUP;
    broken.overlay_rect = R(12, 40, 0, 0);
    UIMouseEvent bc = ev_click(4, 4, 1);
    UIMouseDecision bd = ui_mouse_resolve(&broken, &bc);
    CHECK(bd.target == UI_MOUSE_TARGET_POPUP && bd.inside_overlay == 0 && bd.swallow == 1,
          "弹层矩形无效时外部点击依然被吞（不穿透）");

    /* 空参数 */
    UIMouseDecision null1 = ui_mouse_resolve(NULL, &bc);
    UIMouseDecision null2 = ui_mouse_resolve(&r, NULL);
    CHECK(null1.target == UI_MOUSE_TARGET_NONE && null2.target == UI_MOUSE_TARGET_NONE,
          "空参数安全返回 NONE");
}

/* 频谱与黑胶是两块窗格、同一个归属（都没有鼠标动作）：
 * 都必须认领自己的区域，谁也不能穿透到别处。 */
static void test_priority5_visual_and_vinyl(void)
{
    /* 只有黑胶（当前布局没开频谱窗格） */
    UIMouseRegions r = fixture();
    r.visual = R(0, 0, 0, 0);
    r.vinyl  = R(17, 32, 5, 40);        /* 行 17..21，列 32..71 */

    UIMouseEvent click = ev_click(19, 50, 1);
    UIMouseDecision d = ui_mouse_resolve(&r, &click);
    CHECK(d.target == UI_MOUSE_TARGET_VISUAL && d.swallow == 1,
          "黑胶单独存在时点击归视觉窗格");
    CHECK(d.row == 19 - 17 && d.col == 50 - 32, "黑胶本地坐标按自身矩形折算");

    UIMouseEvent wheel = ev_wheel(19, 50, 1);
    UIMouseDecision dw = ui_mouse_resolve(&r, &wheel);
    CHECK(dw.target == UI_MOUSE_TARGET_VISUAL && dw.swallow == 1,
          "黑胶上的滚轮被认领（什么都不滚，但不穿透）");

    /* 两块同时在（自定义布局把频谱与黑胶拆成并排两格） */
    UIMouseRegions both = fixture();
    both.visual = R(17, 32, 5, 20);     /* 列 32..51 */
    both.vinyl  = R(17, 52, 5, 20);     /* 列 52..71 */
    UIMouseEvent in_spectrum = ev_click(18, 40, 1);
    UIMouseEvent in_vinyl    = ev_click(18, 60, 1);
    UIMouseDecision ds = ui_mouse_resolve(&both, &in_spectrum);
    UIMouseDecision dv = ui_mouse_resolve(&both, &in_vinyl);
    CHECK(ds.target == UI_MOUSE_TARGET_VISUAL && ds.col == 40 - 32,
          "并排时频谱格归视觉窗格（本地列以频谱为基准）");
    CHECK(dv.target == UI_MOUSE_TARGET_VISUAL && dv.col == 60 - 52,
          "并排时黑胶格归视觉窗格（本地列以黑胶为基准）");

    /* 优先级：视觉/黑胶排在主视图最后，不得抢走播放列表的事件 */
    UIMouseRegions overlap = fixture();
    overlap.vinyl = R(1, 1, 10, 30);     /* 与播放列表矩形完全重叠 */
    UIMouseEvent on_list = ev_click(5, 5, 1);
    UIMouseDecision do_ = ui_mouse_resolve(&overlap, &on_list);
    CHECK(do_.target == UI_MOUSE_TARGET_PLAYLIST,
          "黑胶矩形与播放列表重叠时列表优先");

    /* 子视图：两块窗格即使摆满整屏也不得认领（视图 gate） */
    UIMouseRegions sub = fixture();
    sub.view = VIEW_SETTINGS;
    sub.visual = R(0, 0, 24, 80);
    sub.vinyl  = R(0, 0, 24, 80);
    UIMouseEvent in_sub = ev_click(19, 50, 1);
    UIMouseDecision dsub = ui_mouse_resolve(&sub, &in_sub);
    CHECK(dsub.target != UI_MOUSE_TARGET_VISUAL,
          "子视图不产生视觉窗格归属（标志/矩形按视图隔离）");
}

/* 穷举矩阵：视图 × 标志 × 指针位置 × 事件类型 */
static void test_right_click_ownership(void)
{
    /* 无 overlay：右键与左键一样归指针所在区域，但不带 click_count */
    UIMouseRegions r = fixture();
    UIMouseEvent right = ev_right(5, 5);          /* 播放列表内 */
    UIMouseDecision dr = ui_mouse_resolve(&r, &right);
    CHECK(dr.target == UI_MOUSE_TARGET_PLAYLIST && dr.swallow == 1,
          "右键：无弹层时归播放列表并吞掉");
    CHECK(dr.right_click == 1 && dr.click_count == 0,
          "右键：right_click=1 且不伪造左键 click_count");
    CHECK(dr.row == 5 - r.playlist.top, "右键：窗格本地行正确");

    /* 右键在模态输入中：吞掉，归 INPUT */
    UIMouseRegions input = fixture();
    input.input_exclusive = 1;
    UIMouseDecision di = ui_mouse_resolve(&input, &right);
    CHECK(di.target == UI_MOUSE_TARGET_INPUT && di.swallow == 1,
          "右键：模态输入优先并吞掉");

    /* 右键在控制区弹层展开时：归弹层，inside_overlay 指示内外 */
    UIMouseRegions popup = fixture();
    popup.overlay_expanded = 1;
    popup.overlay = UI_MOUSE_TARGET_POPUP;
    UIMouseEvent in = ev_right(14, 46);
    UIMouseDecision dp = ui_mouse_resolve(&popup, &in);
    CHECK(dp.target == UI_MOUSE_TARGET_POPUP && dp.inside_overlay == 1 &&
          dp.swallow == 1, "右键：弹层内归弹层");
    UIMouseEvent out = ev_right(4, 5);
    UIMouseDecision dpo = ui_mouse_resolve(&popup, &out);
    CHECK(dpo.target == UI_MOUSE_TARGET_POPUP && dpo.inside_overlay == 0 &&
          dpo.swallow == 1, "右键：弹层外仍归弹层（收起+吞掉）");

    /* 上下文菜单 overlay 在任意视图都放行（边缘层按 active 门控） */
    for (int view = VIEW_MAIN; view <= VIEW_LANGUAGE; view++) {
        UIMouseRegions cm = fixture();
        cm.view = (ViewMode)view;
        cm.overlay_expanded = 1;
        cm.overlay = UI_MOUSE_TARGET_CONTEXT_MENU;
        cm.overlay_rect = R(5, 10, 5, 20);
        UIMouseEvent h = ev_right(6, 12);
        UIMouseDecision dc = ui_mouse_resolve(&cm, &h);
        CHECK(dc.target == UI_MOUSE_TARGET_CONTEXT_MENU && dc.inside_overlay == 1 &&
              dc.swallow == 1, "右键：上下文菜单在任意视图独占");
    }

    /* 右键在布局编辑器主模式：归编辑器（按屏幕坐标选窗格） */
    UIMouseRegions ed = fixture();
    ed.view = VIEW_SETTINGS;
    ed.editor_active = 1;
    UIMouseEvent ec = ev_right(6, 8);
    UIMouseDecision de = ui_mouse_resolve(&ed, &ec);
    CHECK(de.target == UI_MOUSE_TARGET_EDITOR_PANE && de.swallow == 1 &&
          de.row == 6 && de.col == 8, "右键：编辑器主模式归编辑器");
}

static void test_exhaustive_unique_owner(void)
{
    static const int xs[] = {0, 5, 25, 45, 60, 79};
    static const int ys[] = {0, 3, 6, 13, 19, 23};
    int cases = 0;

    for (int view = VIEW_MAIN; view <= VIEW_LANGUAGE; view++) {
        for (int flag = 0; flag < 8; flag++) {
            for (int ei = 0; ei < 4; ei++) {
                UIMouseRegions r = fixture();
                r.view = (ViewMode)view;
                r.input_exclusive = (flag & 1) ? 1 : 0;
                r.overlay_expanded = (flag & 2) ? 1 : 0;
                r.overlay = (flag & 2)
                    ? (view == VIEW_MAIN ? UI_MOUSE_TARGET_POPUP : UI_MOUSE_TARGET_SEL_MENU)
                    : UI_MOUSE_TARGET_NONE;
                r.editor_active = (flag & 4) ? 1 : 0;
                r.library_browser = (flag & 8) ? 1 : 0;

                for (unsigned yi = 0; yi < sizeof(ys)/sizeof(ys[0]); yi++) {
                    for (unsigned xi = 0; xi < sizeof(xs)/sizeof(xs[0]); xi++) {
                        UIMouseEvent ev;
                        if (ei == 0)      ev = ev_click(ys[yi], xs[xi], 1);
                        else if (ei == 1) ev = ev_wheel(ys[yi], xs[xi], -1);
                        else if (ei == 2) ev = ev_wheel(ys[yi], xs[xi], 1);
                        else              ev = ev_right(ys[yi], xs[xi]);

                        UIMouseDecision d = ui_mouse_resolve(&r, &ev);
                        cases++;

                        /* 不变量 1：吞掉的事件必须有归属 */
                        CHECK(!d.swallow || d.target != UI_MOUSE_TARGET_NONE,
                              "矩阵：swallow ⇒ 有归属");
                        /* 不变量 2：归属唯一（enum 单值，不可能两个） */
                        CHECK(d.target >= UI_MOUSE_TARGET_NONE &&
                              d.target < UI_MOUSE_TARGET_COUNT,
                              "矩阵：归属在枚举范围内");
                        /* 不变量 3：弹层展开且非模态输入时，事件必归弹层 */
                        if (d.swallow && !r.input_exclusive && r.overlay_expanded) {
                            int gated = (r.overlay == UI_MOUSE_TARGET_POPUP && view == VIEW_MAIN) ||
                                        (r.overlay != UI_MOUSE_TARGET_POPUP && view == VIEW_SETTINGS);
                            if (gated)
                                CHECK(d.target == r.overlay, "矩阵：弹层展开时事件必归弹层");
                        }
                        /* 不变量 4：主视图窗格与子视图区域互斥 */
                        if (view == VIEW_MAIN)
                            CHECK(d.target != UI_MOUSE_TARGET_SIDEBAR &&
                                  d.target != UI_MOUSE_TARGET_CONTENT,
                                  "矩阵：主视图不产生子视图归属");
                        else
                            CHECK(d.target != UI_MOUSE_TARGET_PLAYLIST &&
                                  d.target != UI_MOUSE_TARGET_BROWSER &&
                                  d.target != UI_MOUSE_TARGET_LYRICS,
                                  "矩阵：子视图不产生主视图窗格归属");
                        /* 不变量 5：滚轮增量原样传出（右键/点击为 0） */
                        if (ei != 0) CHECK(d.wheel == ev.wheel, "矩阵：滚轮增量不被改写");
                        /* 不变量 6：右键事件确实被识别为右键 */
                        if (ei == 3) CHECK(d.right_click == 1, "矩阵：右键标志原样传出");
                    }
                }
            }
        }
    }
    printf("  穷举矩阵用例数：%d\n", cases);
    CHECK(cases == 9 * 8 * 4 * 36, "矩阵覆盖完整");
}

int main(void)
{
    printf("test_mouse_routing\n");

    test_rect_contains();
    test_priority1_input_exclusive();
    test_priority2_overlay_owns_wheel();
    test_priority2_overlay_click_inside_outside();
    test_priority2_stale_flag_gated_by_view();
    test_priority3_editor();
    test_priority4_hint_bar();
    test_priority5_main_view_panes();
    test_priority5_visual_and_vinyl();
    test_priority5_browser_precedence();
    test_priority6_subviews();
    test_edge_cases();
    test_right_click_ownership();
    test_exhaustive_unique_owner();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
