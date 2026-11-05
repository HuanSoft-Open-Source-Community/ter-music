#ifndef MENU_INTERNAL_H
#define MENU_INTERNAL_H

#include "types.h"
#include "ui/mouse.h"
#include "ui/context_menu.h"
#include <ncursesw/ncurses.h>

/*
 * Internal declarations shared across menu/view modules
 * (menus.c, util.c, settings.c, history.c, favorites.c,
 *  playlist_view.c, info_view.c, help_view.c)
 */

/* ============================================================
 * Sidebar item arrays (defined in util.c)
 * ============================================================ */

extern const char *settings_sidebar_items[];
extern const char *history_sidebar_items[];
extern const char *playlist_sidebar_items[];
extern const char *favorites_sidebar_items[];
extern const char *info_sidebar_items[];
extern const char *help_sidebar_items[];
extern const char *language_sidebar_items[];

extern const int SETTINGS_ITEM_COUNT;
extern const int HISTORY_ITEM_COUNT;
extern const int PLAYLIST_ITEM_COUNT;
extern const int FAVORITES_ITEM_COUNT;
extern const int INFO_ITEM_COUNT;
extern const int HELP_ITEM_COUNT;
extern const int LANGUAGE_ITEM_COUNT;

/* ============================================================
 * Text helper functions (defined in util.c)
 * ============================================================ */

const char *menu_bool_text(int enabled);
const char *menu_color_name(int color_value);
void sanitize_ascii_menu_text(char *dest, size_t dest_size, const char *src);
const char **resolve_sidebar_items(const char **items);

/* ============================================================
 * Status message (defined in util.c)
 * ============================================================ */

void show_status_message(const char *msg);
const char *get_status_message(void);
time_t get_status_message_time(void);
void repaint_status_message(void);

void rounded_box(WINDOW *win);

/* ============================================================
 * Unified non-main content area (defined in util.c)
 *
 * content_x/content_width reserve the scrollbar column (max_x-2)
 * and the right border column (max_x-1) so clearing text never
 * erases the frame.
 * ============================================================ */

typedef struct {
    int max_y, max_x;
    int menu_width;      /* max_x / 4 */
    int content_x;       /* menu_width + 2 */
    int content_width;   /* max_x - content_x - 2 */
} MenuContentArea;

void menu_content_area(MenuContentArea *out);
void menu_content_clear_rows(const MenuContentArea *area, int top, int bottom);
void menu_content_clear_row(const MenuContentArea *area, int row);
void menu_content_puts(const MenuContentArea *area, int row, const char *text);
void rounded_box_open_bottom(WINDOW *win);

/* ============================================================
 * Global variable declarations (defined in util.c)
 * ============================================================ */

extern ViewMode g_current_view;
extern int g_menu_selected_idx;
extern PlayHistory g_play_history;
extern Favorites g_favorites;
extern DirHistory g_dir_history;
extern int g_content_selected_idx;
extern FocusArea g_focus_area;

/* ============================================================
 * Per-view scroll / state variables (defined in util.c)
 * ============================================================ */

extern int g_history_content_offset;
extern int g_favorites_content_offset;

/* Playlist view state lives in playlist_view.c; reset through reset_playlist_view(). */

/* ============================================================
 * Config helpers (defined in menus.c)
 * ============================================================ */

void ensure_config_dir_exists(void);
void init_default_config(void);
void apply_color_theme(void);
void try_migrate_from_json(void);
void init_all_persistent_data(int readonly_mirror);
int  load_temp_playlist(void);
void save_temp_playlist(void);
void cleanup_temp_playlist(void);

/* ============================================================
 * Data management (defined in menus.c)
 * ============================================================ */

void load_history(void);
void load_favorites(void);
void load_dir_history(void);
void load_all_playlists(void);
void add_dir_history_entry(const char *path);
void clear_dir_history(void);
/* add_history_entry 是跨模块（ui.c 换曲钩子）调用的公开入口，声明归 menus.h。 */
int  add_to_favorites(Track *track);
int  remove_from_favorites(int index);
int  create_user_playlist(const char *name);
int  delete_user_playlist(int index);
int  rename_user_playlist(int index, const char *new_name);
int  add_track_to_playlist(int playlist_idx, Track *track);
int  remove_track_from_playlist(int playlist_idx, int track_idx);

/* ============================================================
 * View input handler declarations (defined in respective view .c)
 * ============================================================ */

int handle_settings_input(int ch);
/* 快捷键页是否处于按键捕获模式（Esc 应交给设置页取消捕获）。 */
int settings_hotkey_capture_active(void);
/* 设置页选项/颜色选择弹层的窗口；未激活时 NULL。
 * 流光彩蛋用它把弹层纳入整屏合成，避免刷新底层窗口时把弹层擦掉。 */
WINDOW *settings_sel_window(void);
int handle_history_input(int ch);
int handle_playlist_input(int ch);
int handle_favorites_input(int ch);
int handle_info_input(int ch);
int handle_help_input(int ch);
int handle_language_input(int ch);

/* ============================================================
 * Per-view mouse handlers (defined in respective view .c)
 *
 * 由 ui/mouse.c 的全局分发调用：归属（侧栏还是内容区、在不在弹层里）
 * 已经由 ui/mouse_routing.c 判定完毕，视图只负责把 d->row/d->col 解释成
 * 自己的条目，并复用键盘那条动作路径。
 * 约定（与 docs 里的鼠标章节一致）：
 *   滚轮一格 = 在鼠标所在的这一列上移动选中项（不切换其它窗格）；
 *   单击 = 选中并执行（等同回车；双击/三击也只算一次单击）；
 *   行号→条目用各视图渲染期记下的几何，不在这里抄第二遍公式。
 * 返回值：1 = 已消费。无人认领时分发不会调用它们。
 * ============================================================ */

int settings_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int history_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int favorites_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int playlist_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int info_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int help_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);
int language_view_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);

/* ============================================================
 * View reset functions (called from menus.c switch_to_view)
 * ============================================================ */

void reset_settings_view(void);
void reset_playlist_view(void);
void reset_help_view(void);
void reset_language_view(void);

/* ============================================================
 * Help view lifecycle (defined in help_view.c)
 * ============================================================ */

void help_free_lines(void);

/* ============================================================
 * Popup state (defined in controls.c)
 * ============================================================ */

extern PopupState g_popup;
int popup_handle_mouse_click(int screen_y, int screen_x);

/* 弹层展开状态与屏幕矩形：全局鼠标分发用它决定"这次事件归谁"。
 * rect 取自 PopupState 的 start_row/start_col/height/width（弹层画在 stdscr 上，
 * 本身就是屏幕坐标），未展开时返回 0。 */
int controls_popup_expanded(void);
int controls_popup_rect(UIRect *out);
/* 弹层展开时的滚轮：一格移动一个候选项（复用键盘的 popup.up/down 动作）。 */
int controls_popup_handle_wheel(int delta);
/* 控制区窗格上的滚轮：control_index 由 get_control_index_from_window_point 得出，
 * delta = -1 上滚 / +1 下滚；语义见 controls.c 注释（进度/音量/倍速档位循环）。 */
int controls_handle_wheel_control(int control_index, int delta);

/* ============================================================
 * Remote settings (defined in settings.c)
 * ============================================================ */

void remote_enter_list_mode(void);

/* ============================================================
 * Right-click context menus (defined in each view / context_menu_main.c)
 *
 * 各页 builder 只填条目；dispatcher 把 ActionId 交回该页既有的
 * handle_*_action()（返回 0 时菜单引擎回退 execute_global_action）。
 * ============================================================ */

void main_list_context_menu_build(UIContextMenuLevel *level,
                                  const UIContextMenuContext *ctx);
int  main_list_context_dispatch(ActionId action,
                                const UIContextMenuContext *ctx);

void settings_context_menu_build(UIContextMenuLevel *level,
                                 const UIContextMenuContext *ctx);
int  settings_context_dispatch(ActionId action,
                               const UIContextMenuContext *ctx);

void history_context_menu_build(UIContextMenuLevel *level,
                                const UIContextMenuContext *ctx);
int  history_context_dispatch(ActionId action,
                              const UIContextMenuContext *ctx);

void playlist_view_context_menu_build(UIContextMenuLevel *level,
                                      const UIContextMenuContext *ctx);
int  playlist_view_context_dispatch(ActionId action,
                                    const UIContextMenuContext *ctx);

void favorites_context_menu_build(UIContextMenuLevel *level,
                                  const UIContextMenuContext *ctx);
int  favorites_context_dispatch(ActionId action,
                                const UIContextMenuContext *ctx);

void help_context_menu_build(UIContextMenuLevel *level,
                             const UIContextMenuContext *ctx);
int  help_context_dispatch(ActionId action,
                           const UIContextMenuContext *ctx);

void language_context_menu_build(UIContextMenuLevel *level,
                                 const UIContextMenuContext *ctx);
int  language_context_dispatch(ActionId action,
                               const UIContextMenuContext *ctx);

/* F6 信息页的右键菜单在 info_view.c 内私有实现；
 * 主视图信息窗格没有右键菜单。 */

/* 右键取消既有弹层（由 ui/mouse.c 在 overlay 分支调用）。 */
int controls_popup_cancel(void);
int settings_cancel_sel_menu(void);

#endif /* MENU_INTERNAL_H */
