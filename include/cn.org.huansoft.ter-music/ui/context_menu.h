/**
 * @file context_menu.h
 * @brief 右键上下文菜单：多层菜单栈、条目模型、渲染与输入分发
 *
 * 设计目标（与 docs 的右键章节一致）：
 *   - **在指针位置唤起**：根菜单以右击坐标为先验，贴边时上翻/左翻并钳制到
 *     屏幕内（末行提示条除外），子菜单随父菜单右侧/左侧放置；
 *   - **单 overlay**：同一时刻至多一个菜单/弹层；菜单打开时其它弹层已被
 *     取消，展开期间所有权归属由 ui/mouse_routing.c 统一裁决；
 *   - **复用现有动作路径**：菜单项携带 ActionId 或回调，执行时走各视图
 *     已有的 handle_*_action()/execute_global_action()，不合成按键；
 *   - **竞态安全**：执行前先销毁菜单窗口再分发；视图/resize/配置重载/
 *     布局重建同步关闭；行相关操作可携带快照做失配校验；
 *   - **不闪**：与菜单相交的窗格刷新先入虚拟屏，再把菜单各层 backing
 *     压回，最后只 doupdate 一次；终端不会看到菜单被动画擦掉的中间帧。
 *
 * 本头文件不 include ncurses：窗口访问器返回 void *（与 PopupState 同风格）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_CONTEXT_MENU_H
#define UI_CONTEXT_MENU_H

#include <stdint.h>

#include "types.h"
#include "keymap/actions.h"
#include "ui/mouse.h"

#define UI_CTX_MAX_ITEMS   20
#define UI_CTX_MAX_LEVELS  3
#define UI_CTX_LABEL_MAX   128

typedef struct UIContextMenuLevel UIContextMenuLevel;
typedef struct UIContextMenuContext UIContextMenuContext;

/** @brief 菜单区域（决定 builder 取哪套条目）。 */
typedef enum {
    UI_CTX_AREA_NONE = 0,
    UI_CTX_AREA_SIDEBAR,
    UI_CTX_AREA_CONTENT,
    UI_CTX_AREA_LIST,
    UI_CTX_AREA_LIBRARY,
    UI_CTX_AREA_INFO,
    UI_CTX_AREA_EDITOR
} UIContextMenuArea;

/**
 * @brief 打开菜单时捕获的上下文快照。
 *
 * 行相关动作在打开后如果底层列表变化，dispatcher 可用这些字段做失配校验；
 * 字段语义由各页面自己解释，菜单引擎只负责原样传递。
 */
struct UIContextMenuContext {
    ViewMode view;
    UIMouseTarget target;
    UIContextMenuArea area;
    int valid;              /* 指针命中具体行/条目 */
    int index;              /* 本页行号（视图语义） */
    int track_index;        /* 主列表解析出的内容曲目下标，-1 = 无 */
    int is_dir;
    int dir_expanded;
    int list_mode;          /* PLAYLIST_MODE_FILE_BROWSER / PLAY_QUEUE */
    int search_active;
    int queue_mode;
    uint64_t queue_revision;
    int library_view;       /* 曲库浏览器子页 */
    int library_count;
    int eq_band;
    int hotkey_index;
    char item_path[MAX_PATH_LEN];  /* 历史等按路径定位的行 */
};

typedef void (*UIContextMenuCallback)(const UIContextMenuContext *ctx,
                                      intptr_t value);
typedef void (*UIContextMenuBuilder)(UIContextMenuLevel *level,
                                     const UIContextMenuContext *ctx);
/** @return 1 = 已处理；0 = 交给 execute_global_action() 兜底。 */
typedef int (*UIContextMenuDispatch)(ActionId action,
                                     const UIContextMenuContext *ctx);

/** @brief 条目种类。 */
typedef enum {
    UI_CTX_ITEM_ACTION = 0,
    UI_CTX_ITEM_CALLBACK,
    UI_CTX_ITEM_SUBMENU,
    UI_CTX_ITEM_SEPARATOR
} UIContextMenuItemKind;

/** @brief 单个菜单条目（builder 通过 ui_ctx_menu_add_* 填充）。 */
typedef struct {
    UIContextMenuItemKind kind;
    char label[UI_CTX_LABEL_MAX];       /* 已本地化文本 */
    ActionId action;                    /* ACTION 条目 */
    UIContextMenuCallback callback;     /* CALLBACK 条目 */
    intptr_t value;                     /* 回调/子菜单自带的整数载荷 */
    UIContextMenuBuilder submenu;       /* SUBMENU 条目 */
    int enabled;
    int checked;
    const char *debug_name;             /* 日志用（可 NULL） */
} UIContextMenuItem;

/** @brief 一层的条目与当前几何；builder 只通过 add API 填充。 */
struct UIContextMenuLevel {
    UIContextMenuItem items[UI_CTX_MAX_ITEMS];
    int count;
    int selected;
    int scroll;
    int visible;
    char title[UI_CTX_LABEL_MAX];
    UIRect rect;
};

/**
 * @brief 打开根菜单（会先关闭已有菜单）。
 *
 * @param build     根层 builder；必须至少添加一个非分隔条目
 * @param dispatch  动作分发器；返回 0 时引擎回退 execute_global_action()
 * @param ctx       调用方捕获的上下文快照（引擎深拷贝）
 * @param anchor_y  右击屏幕行
 * @param anchor_x  右击屏幕列
 * @return 1 = 已打开；0 = 拒绝（无条目/屏幕太小/创建窗口失败）
 */
int ui_context_menu_open(UIContextMenuBuilder build,
                         UIContextMenuDispatch dispatch,
                         const UIContextMenuContext *ctx,
                         int anchor_y, int anchor_x);

/**
 * @brief 关闭菜单（幂等）。
 *
 * @param repaint 1 = 关闭后立即重绘当前界面擦掉菜单像素；
 *                0 = 交给调用方（视图切换/resize/动作分发）重绘。
 */
void ui_context_menu_close(int repaint);

/** @brief 菜单是否处于可交互状态（视图匹配且布局编辑器未激活）。 */
int ui_context_menu_active(void);

/** @brief 最顶层菜单的屏幕矩形；未激活返回 0。 */
int ui_context_menu_rect(UIRect *out);

/** @brief 菜单键盘输入：所有键都被消费（未识别键 = 关闭并吞掉）。 */
int ui_context_menu_handle_key(int ch);

/** @brief 菜单鼠标输入：选中/执行/滚动/外点关闭/右键关闭。 */
int ui_context_menu_handle_mouse(const UIMouseEvent *ev,
                                 const UIMouseDecision *d);

/**
 * @brief 每轮事件循环把菜单重新合成到最上层。
 *
 * 底层动画/异步重绘会擦掉菜单像素；本函数在激活时逐个 ui_refresh_window()。
 * 发现视图不匹配会自愈关闭。
 */
void ui_context_menu_refresh(void);

/** @brief 菜单层数（0 = 未激活）；供流光合成与测试。 */
int ui_context_menu_level_count(void);

/** @brief 第 level 层的窗口（void * 即 WINDOW *）；越界/未激活返回 NULL。 */
void *ui_context_menu_window_at(int level);

/**
 * @brief win 是否是菜单自身的窗口（void * 即 WINDOW *）。
 *
 * 合成刷新用：菜单窗口必须走普通 wrefresh，绝不能递归 reassert。
 */
int ui_context_menu_owns_window(const void *win);

/**
 * @brief 屏幕矩形是否与任一层菜单相交（只接触边缘不算）。
 *
 * 用于 ui_refresh_window() 的局部刷新判定：相交的窗口先入虚拟屏，
 * 菜单 backing 压回后只 doupdate 一次；不相交窗口仍走 wrefresh。
 */
int ui_context_menu_intersects_rect(int top, int left, int rows, int cols);

/**
 * @brief 把菜单各层 backing 重新压回虚拟屏（不绘制、不 doupdate）。
 *
 * 每个窗口先 touchwin 再 wnoutrefresh；返回成功压回的层数。调用方负责
 * 随后唯一的 doupdate，保证终端不会看到菜单矩形内的窗格像素。
 */
int ui_context_menu_reassert(void);

/**
 * @brief 把当前界面状态不匹配的失配报告给用户（右键菜单专用）。
 *
 * 打 `[context_menu] stale action=...` debug，并在状态栏显示
 * `context.stale`（主视图走控制区状态行，子视图走页面状态）。
 */
void ui_context_menu_report_stale(const char *what);

/* ── builder 用：填充一层菜单 ─────────────────────────────────── */

void ui_ctx_menu_set_title(UIContextMenuLevel *level, const char *title);
void ui_ctx_menu_add_action(UIContextMenuLevel *level, ActionId action,
                            int enabled, int checked);
void ui_ctx_menu_add_action_label(UIContextMenuLevel *level, const char *label,
                                  ActionId action, int enabled, int checked);
void ui_ctx_menu_add_callback(UIContextMenuLevel *level, const char *label,
                              const char *debug_name,
                              UIContextMenuCallback callback, intptr_t value,
                              int enabled, int checked);
void ui_ctx_menu_add_submenu(UIContextMenuLevel *level, const char *label,
                             UIContextMenuBuilder build, int enabled);
void ui_ctx_menu_add_separator(UIContextMenuLevel *level);

/** @brief 公共"前往页面 ▸"子菜单（除信息页/布局编辑器外的页面复用）。 */
void ui_context_menu_add_pages_submenu(UIContextMenuLevel *level);

#endif /* UI_CONTEXT_MENU_H */
