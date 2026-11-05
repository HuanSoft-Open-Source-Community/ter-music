/**
 * @file mouse.h
 * @brief 全局鼠标事件：事件规范化、区域归属判定与分发门面
 *
 * 分两层，边界刻意清晰：
 *
 *   1) **纯逻辑层**（`ui/mouse_routing.c`，本头文件的类型）：不 include
 *      ncurses，只吃"归一化事件 + 区域表"，吐出"这次事件归谁、要不要吞掉、
 *      区域本地坐标是第几行"。菜单展开与底层内容区的竞争态全部在这张表里
 *      判定，因此可以在单测里对「展开标志 × 指针区域 × 事件类型」穷举。
 *   2) **ncurses 边缘层**（`ui/mouse.c`）：`MEVENT → UIMouseEvent` 归一化、
 *      用 `getbegyx()/getmaxyx()` 现场量出区域表、按判定结果调用各属主模块
 *      （弹层、窗格、子视图、浏览器、布局编辑器）。
 *
 * 归属规则（唯一权威，代码与文档都以它为准）：
 *   - 弹层（右键上下文菜单 / 控制区弹层 / 设置项选择菜单 / 布局编辑器模态菜单）
 *     展开时，**滚轮无论指针在哪都只滚弹层**；点击落在弹层内＝选中并执行该项，
 *     落在弹层外＝收起弹层并**吞掉**该次点击，绝不穿透到背后窗格（一次点击只做一件事）。
 *   - 无弹层时，事件只归**指针所在的窗格/区域**，滚轮移动该区域的选中项，
 *     不影响其它窗格。
 *   - 右键与左键走同一套归属优先级；右键只负责"选中 + 在该点开菜单"，
 *     绝不播放/执行；弹层展开期间右键只取消弹层并吞掉。
 *   - 快捷键捕获等模态输入进行中：吞掉一切鼠标事件。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_MOUSE_H
#define UI_MOUSE_H

#include "types.h"

/**
 * @brief 归一化后的鼠标事件（屏幕坐标，与 ncurses MEVENT 同一坐标系）。
 *
 * 滚轮方向约定：`wheel = -1` 向上/往前一格，`+1` 向下/往后一格
 * （ncurses 的 BUTTON4_PRESSED / BUTTON5_PRESSED）；横向滚轮
 * （BUTTON6/BUTTON7）忽略，保持 0。
 */
typedef struct {
    int y;            /* 屏幕行（0 起） */
    int x;            /* 屏幕列（0 起） */
    int click;        /* 1 = 主键（左键）点击；单击/双击/三击/release 一律算一次点击 */
    int click_count;  /* 1 / 2 / 3，仅用于日志；非左键点击为 0，不参与任何行为分支 */
    int right_click;  /* 1 = 右键按下（一次按下只算一次，见 ui/mouse.c 归一化） */
    int wheel;        /* -1 / 0 / +1 */
} UIMouseEvent;

/**
 * @brief 屏幕坐标矩形：左上角 (top,left)，尺寸 rows×cols。
 *
 * rows/cols <= 0 表示"该区域当前不存在"（窗格被隐藏、终端太小）。
 */
typedef struct {
    int top;
    int left;
    int rows;
    int cols;
} UIRect;

/** @brief 点是否落在矩形内（含首尾、不含右下越界）。矩形无效返回 0 */
int ui_rect_contains(const UIRect *r, int y, int x);

/** @brief 鼠标事件归属者。 */
typedef enum {
    UI_MOUSE_TARGET_NONE = 0,
    UI_MOUSE_TARGET_INPUT,        /* 模态输入/快捷键捕获进行中：吞掉 */
    UI_MOUSE_TARGET_POPUP,        /* 控制区弹层（循环模式/倍速/音量） */
    UI_MOUSE_TARGET_SEL_MENU,     /* 设置项选择弹层（含颜色、EQ 预设） */
    UI_MOUSE_TARGET_EDITOR_MENU,  /* 布局编辑器模态菜单（动作/类型/颜色/帮助） */
    UI_MOUSE_TARGET_EDITOR_PANE,  /* 布局编辑器主模式：按坐标选窗格 */
    UI_MOUSE_TARGET_HINT_BAR,     /* 底部功能键提示条 */
    UI_MOUSE_TARGET_BROWSER,      /* 曲库浏览器（占用播放列表窗格） */
    UI_MOUSE_TARGET_PLAYLIST,     /* 主视图播放列表窗格 */
    UI_MOUSE_TARGET_CONTROLS,     /* 主视图控制区窗格 */
    UI_MOUSE_TARGET_LYRICS,       /* 主视图歌词窗格 */
    UI_MOUSE_TARGET_INFO,         /* 主视图信息窗格 */
    UI_MOUSE_TARGET_VISUAL,       /* 主视图频谱/黑胶窗格（两块矩形，同一归属） */
    UI_MOUSE_TARGET_SIDEBAR,      /* 子视图左侧菜单 */
    UI_MOUSE_TARGET_CONTENT,      /* 子视图内容区 */
    UI_MOUSE_TARGET_CONTEXT_MENU, /* 右键上下文菜单（任意视图，独占 overlay） */
    UI_MOUSE_TARGET_COUNT
} UIMouseTarget;

/**
 * @brief 区域表：由 ncurses 边缘层现场量出，交给纯逻辑层判定。
 *
 * 标志位必须与 `view` 一致：例如控制区弹层只在 `VIEW_MAIN` 有效，
 * 设置/编辑器的弹层只在 `VIEW_SETTINGS` 有效——判定函数按视图 gate，
 * 因此别处残留的"展开"标志不可能偷走事件。
 */
typedef struct {
    ViewMode view;
    int input_exclusive;      /* 模态输入/快捷键捕获：吞掉一切 */
    int overlay_expanded;     /* 有弹层展开（overlay 字段即其矩形） */
    UIMouseTarget overlay;    /* 展开的是哪一个弹层 */
    UIRect overlay_rect;

    int editor_active;        /* 布局编辑器激活（非模态菜单时按窗格处理） */
    int library_browser;      /* 曲库浏览器占用播放列表窗格 */

    UIRect hint_bar;          /* 底部提示条（仅点击有意义） */
    UIRect playlist;          /* 主视图窗格 */
    UIRect controls;
    UIRect lyrics;
    UIRect info;
    UIRect visual;            /* 频谱窗格 */
    UIRect vinyl;             /* 黑胶窗格：与频谱同一归属（都无鼠标动作） */

    UIRect sidebar;           /* 子视图侧栏（y=2 起） */
    UIRect content;           /* 子视图内容区（menu_content_area） */
} UIMouseRegions;

/** @brief 判定结果。 */
typedef struct {
    UIMouseTarget target;     /* 归属者；NONE = 无人认领 */
    int swallow;              /* 1 = 事件已消费，不得再派发给其它处理 */
    int inside_overlay;       /* 弹层展开时：本次命中是否在弹层内 */
    int row;                  /* 目标区域本地行（0 起）；不适用为 -1 */
    int col;                  /* 目标区域本地列（0 起）；不适用为 -1 */
    int wheel;                /* 归一化滚轮增量（-1/0/+1） */
    int click_count;          /* 1/2/3，仅诊断；所有点击都执行同一套“选中+执行” */
    int right_click;          /* 1 = 本次事件是右键按下 */
} UIMouseDecision;

/**
 * @brief 纯逻辑归属判定（`ui/mouse_routing.c`，无 ncurses 依赖）。
 *
 * @param r  区域表
 * @param ev 归一化事件
 * @return 判定结果；永不失败
 */
UIMouseDecision ui_mouse_resolve(const UIMouseRegions *r, const UIMouseEvent *ev);

/* 本头文件**不含 ncurses 类型**：`MEVENT → UIMouseEvent` 的归一化与
 * `handle_global_mouse_event()` 门面声明在 `ui/ui.h`（那边已 include ncurses），
 * 这样 `ui/mouse_routing.c` 与它的单测可以只 include 本文件。 */

#endif /* UI_MOUSE_H */
