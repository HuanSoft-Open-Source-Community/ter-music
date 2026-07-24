#ifndef UI_H
#define UI_H

#include "types.h"
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

extern int g_rainbow_mode_enabled;
extern ColorTheme g_saved_theme;
extern int g_debug_enabled;
extern int g_konami_input_pos;
extern uint64_t g_konami_last_time;
extern int g_rainbow_color_offset;

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;
extern int g_ascii_fallback_ui;
extern char g_braille_art_buffer[8192];
extern int g_album_cover_size;

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
void run_event_loop(void);

void create_layout(void);
void render_playlist_content(void);
void render_controls(void);

void update_progress_bar(void);
void update_controls_status(const char *msg);
void request_ui_refresh(int dirty_mask);
void process_pending_ui_refresh(void);
void reap_finished_playback_thread(void);
void process_pending_playback_action(void);
void reset_visualizer_state(void);
void push_visualizer_samples(const int32_t *samples, int frame_count, int channels);
void get_visualizer_snapshot(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);

void apply_color_theme(void);
int use_ascii_fallback_ui(void);
const char *ui_text(const char *key, const char *unused);
void check_konami_input(int ch);
void toggle_rainbow_mode(void);
void update_rainbow_colors(void);

/* ── Internal UI sub-module functions (used across split modules) ── */
void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad);
void sanitize_ascii_text(char *dest, size_t dest_size, const char *src);
void render_visualizer_with_album_cover(void);
void build_control_label(int index, char *dest, size_t dest_size);
void activate_current_control(void);
void render_controls_status_line(void);
int  get_playlist_scroll_offset(void);
int  get_controls_progress_row(int height);
int  get_controls_button_row(int height);
int  get_controls_visualizer_top(int height);
int  get_controls_visualizer_bottom(int height);
uint64_t get_ui_time_ms(void);
void seek_relative_seconds(int delta_seconds);

int  handle_main_view_mouse_event(const MEVENT *event);
int  handle_menu_hint_bar_click(const MEVENT *event);
int  is_primary_mouse_click(const MEVENT *event);
int  translate_screen_to_window(WINDOW *win, int screen_y, int screen_x, int *window_y, int *window_x);
int  get_playlist_index_from_window_row(int window_y, int *display_index, int *actual_index);
int  get_control_index_from_window_point(int window_y, int window_x);
int  get_lyric_index_from_window_row(int window_y, int *lyric_index, double *timestamp);
int  get_menu_hint_fkey_from_column(int screen_x);

/* ── UTF-8 utility functions (defined in ui/utf8.c) ── */
int  utf8_str_truncate(char *dest, const char *src, int max_cols);
int  utf8_str_width(const char *src);
int  utf8_str_substring(char *dest, const char *src, int start_col, int max_cols);
int  utf8_str_pad(char *dest, size_t dest_size, const char *src, int width);
size_t utf8_next_char(const char *src, wchar_t *wc_out, int *width_out);
#endif
