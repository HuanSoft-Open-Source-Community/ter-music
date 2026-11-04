#ifndef UI_H
#define UI_H

#include "types.h"
#include "keymap/actions.h"
#include "util/utf8.h"
#include <ncursesw/ncurses.h>

/* ── Extern globals ── */
extern ViewMode g_current_view;
extern int g_menu_selected_idx;
extern int g_content_selected_idx;
extern FocusArea g_focus_area;
extern int g_control_focus;
extern int g_current_control_idx;
extern int g_control_count;
extern int g_current_position;
extern int g_total_duration;
extern int g_initial_seek_position;
extern pthread_mutex_t g_seek_mutex;
extern int g_lyric_cursor_mode;
extern int g_lyric_cursor_index;

extern int g_debug_enabled;

extern WINDOW *win_playlist;
extern WINDOW *win_info;
extern WINDOW *win_controls;
extern WINDOW *win_spectrum;
extern WINDOW *win_lyrics;
extern WINDOW *win_vinyl;
extern int g_ascii_fallback_ui;

/* ── Control index constants ── */
enum {
    CONTROL_IDX_PREV       = 0,
    CONTROL_IDX_PLAY_PAUSE = 1,
    CONTROL_IDX_NEXT       = 2,
    CONTROL_IDX_STOP       = 3,
    CONTROL_IDX_LOOP       = 4,
    CONTROL_IDX_SPEED      = 5,
    CONTROL_IDX_VOLUME     = 6,
    CONTROL_IDX_PROGRESS   = 7
};

/* ── Function prototypes ── */
void init_ncurses(void);
void cleanup(void);
/* 恢复被 init_ncurses() 守卫改道的 stderr（幂等）；
 * 退出路径与崩溃处理器在向真实终端打印错误前调用。 */
void ui_restore_stdio(void);
void run_event_loop(void);

void create_layout(void);
void destroy_main_windows(void);
void render_playlist_content(void);
void render_info_bar(void);
void render_controls(void);
void controls_reset_popup(void);
/* 控制区弹层的当前窗口；未激活或创建失败时为 NULL。
 * 流光彩蛋用它把弹层纳入整屏合成，避免刷新底层窗口时把弹层擦掉。 */
WINDOW *controls_popup_window(void);
/* 主视图窗格统一刷新入口：彩蛋激活时先把前景合成成当前流光色再刷新，
 * 避免基础色帧先被刷到终端造成闪动。右键菜单展开且与窗口相交时改走
 * "wnoutrefresh + 菜单 backing 压回 + 单次 doupdate" 的合成刷新，
 * 保证终端不会输出菜单矩形内的窗格像素；其余情况等价 wrefresh。 */
int ui_refresh_window(WINDOW *win);
/* 彩蛋色对的背景：终端支持默认色时返回 -1（透明），否则返回黑(0)。 */
int ui_transparent_background(void);
void render_spectrum_pane(void);
void spectrum_tick(void);
void lyrics_animation_tick(void);

void update_progress_bar(void);
void update_controls_status(const char *msg);
void request_ui_refresh(int dirty_mask);
void process_pending_ui_refresh(void);
/* reap_finished_playback_thread() / process_pending_playback_action() 属音频层，
 * 声明已移至 audio/audio.h */
/* 可视化采样属音频层，声明已移至 audio/visualizer.h */

void apply_color_theme(void);
int use_ascii_fallback_ui(void);
const char *ui_text(const char *key, const char *unused);

/* 统一的内容下标播放入口：内容下标→路径→队列位（失配先重推队列再播）。
 * 各界面播放入口都应经它下发，避免把内容下标当队列位置盲发 PlayAt。 */
void ui_play_track_index(int track_index);

/* ── Internal UI sub-module functions (used across split modules) ── */
void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad);
void sanitize_ascii_text(char *dest, size_t dest_size, const char *src);
void render_visualizer_with_album_cover(void);
void build_control_label(int index, char *dest, size_t dest_size);
void activate_current_control(void);
void render_controls_status_line(void);
void controls_status_tick(void);

/* 主视图动作分发（键盘与右键菜单共用）。
 * 返回 1 = 已处理；2 = 请求退出事件循环。 */
int handle_main_action(ActionId action);

/* 列表窗格的文件/队列模式切换（键盘 view.tab_switch 与右键菜单共用）。
 * 只切列表模式；歌词来源切换仍由 view.tab_switch 的键盘分支先判。
 * 返回 1 = 已切换；0 = 列表未加载。 */
int ui_toggle_playlist_tab_mode(void);

/* 控制区内容坐标。渲染、进度增量刷新、鼠标命中和可视化共用同一份
 * 结果，避免各自换行/居中导致画面与点击错位。 */
typedef struct {
    int progress_row;                 /* -1 = 不显示 */
    int status_row;
    int button_row[CONTROL_COUNT];    /* -1 = 未显示 */
    int button_col[CONTROL_COUNT];
    int button_width[CONTROL_COUNT];
    int visualizer_top;               /* -1 = 无空间 */
    int visualizer_bottom;
} ControlsLayout;

void controls_compute_layout(int height, int width, ControlsLayout *out);

/* 键盘与鼠标滚轮共用的步进量（原定义在 ui.c，控制区滚轮也要用，故上移）。
 * 数值不变：快进/快退 5 秒、音量 5%。 */
#define UI_SEEK_STEP_SECONDS    5
#define UI_VOLUME_STEP_PERCENT  5

int  get_playlist_scroll_offset(void);
uint64_t get_ui_time_ms(void);
void seek_relative_seconds(int delta_seconds);

/**
 * @brief 移动"当前列表"的选中项一格（键盘上下键与鼠标滚轮共用）。
 *
 * 覆盖四种列表形态：搜索命中、播放队列、文件树、普通/排序播放列表；
 * 边界由各自的数量钳制，越界不做事。
 *
 * @param delta               -1 上/前一格，+1 下/后一格
 * @param guard_lyric_cursor  非 0 时保留 j/k 键在歌词跳转模式下的既有护栏
 *                            （该模式下上下键归歌词，不移动列表）。鼠标滚轮
 *                            传 0：滚轮是窗格本地操作，与键位约定无关。
 */
void ui_move_main_list_selection(int delta, int guard_lyric_cursor);

/**
 * @brief 歌词跳转光标移动一格并跳到该行时间戳（上一句/下一句）。
 *
 * 键盘（Ctrl+L 后的上下键）与歌词窗格滚轮共用；不改变跳转模式开关。
 */
void ui_lyric_cursor_step(int delta);

/**
 * @brief 全局鼠标事件处理：归一化 MEVENT → 量区域 → 归属判定 → 派发到属主模块。
 *
 * 归属规则见 ui/mouse.h 头部注释（弹层展开时弹层拥有本次事件，
 * 弹层外的点击收起弹层并吞掉，绝不穿透到背后窗格）。
 * @return 1 = 已消费；0 = 无人认领
 */
int  handle_global_mouse_event(const MEVENT *event);

int  handle_popup_action(ActionId action);
int  handle_popup_input(int ch);
int  is_primary_mouse_click(const MEVENT *event);
int  translate_screen_to_window(WINDOW *win, int screen_y, int screen_x, int *window_y, int *window_x);
int  get_playlist_index_from_window_row(int window_y, int *display_index, int *actual_index);
int  get_control_index_from_window_point(int window_y, int window_x);
int  get_lyric_index_from_window_row(int window_y, int *lyric_index, double *timestamp);
int  get_menu_hint_action_from_column(int screen_x, ActionId *action);

/* ── UTF-8 utility functions (declared in util/utf8.h, defined in util/utf8.c) ── */

#endif
