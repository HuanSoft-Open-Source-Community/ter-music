#include "../include/defs.h"
#include "../include/media_session.h"
#include "../include/lyrics.h"
#include "../include/menu_views.h"
#include "../include/library.h"
#include "../include/library_browser.h"
#include "../include/braille_art.h"
#include "../include/search.h"
#include "../include/scrollbar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/remote.h"
#include <ncursesw/ncurses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>

// 前向声明
void audio_backend_shutdown(void);
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;
extern WINDOW *win_lyrics;

extern const char *control_labels[];

// 全局窗口变量
WINDOW *win_playlist;
WINDOW *win_controls;
WINDOW *win_lyrics;

// 控件标签文本
const char *control_labels[] = {"上一曲", "播放/暂停", "下一曲", "停止", "循环", "倍速", "音量", "进度"};
static const char *control_labels_en[] = {"Prev", "Play/Pause", "Next", "Stop", "Loop", "Speed", "Vol", "Prog"};
int g_control_count = sizeof(control_labels) / sizeof(control_labels[0]);

// 歌词光标操作模式全局变量
int g_lyric_cursor_mode = 0;
int g_lyric_cursor_index = -1;

static pthread_mutex_t g_ui_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_pending_ui_mask = 0;
static char g_controls_status_message[256] = "";
static time_t g_controls_status_time = 0;
static int g_ascii_fallback_ui = 0;

int g_rainbow_mode_enabled = 0;
ColorTheme g_saved_theme;
int g_konami_input_pos = 0;
uint64_t g_konami_last_time = 0;
int g_rainbow_color_offset = 0;

char g_braille_art_buffer[8192] = "";
int g_album_cover_size = 0;
static int g_album_cover_enabled = 1;

#define KONAMI_SEQ_LENGTH 12
static const int konami_expected[KONAMI_SEQ_LENGTH] = {
    KEY_UP, KEY_UP,
    KEY_DOWN, KEY_DOWN,
    KEY_LEFT, KEY_RIGHT,
    KEY_LEFT, KEY_RIGHT,
    'B', 'A', 'B', 'A'
};

#define SEEK_STEP_SECONDS 5
#define VOLUME_STEP_PERCENT 5
#define UI_INPUT_TIMEOUT_MS 40
#define UI_PROGRESS_REFRESH_MS 80

enum {
    CONTROL_IDX_PREV = 0,
    CONTROL_IDX_PLAY_PAUSE = 1,
    CONTROL_IDX_NEXT = 2,
    CONTROL_IDX_STOP = 3,
    CONTROL_IDX_LOOP = 4,
    CONTROL_IDX_SPEED = 5,
    CONTROL_IDX_VOLUME = 6,
    CONTROL_IDX_PROGRESS = 7
};

void render_playlist_content(void);
void render_controls(void);
static int get_corner_spectrum_height(int h);
static int calculate_lyrics_content_top(int h, int w);
static void activate_current_control(void);
static uint64_t get_current_track_duration_ms(void);
static void seek_to_position_ms(uint64_t pos_ms);
static int get_playlist_scroll_offset(void);
static int handle_main_view_mouse_event(const MEVENT *event);
int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                      char *buffer, size_t buffer_size, int trim_whitespace,
                      int password_mode, int prefill);

enum {
    INPUT_ESCAPE_NONE = 0,
    INPUT_ESCAPE_ESC,
    INPUT_ESCAPE_CSI,
    INPUT_ESCAPE_SS3,
    INPUT_ESCAPE_OSC,
    INPUT_ESCAPE_OSC_ESC
};

static uint64_t get_ui_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

int use_ascii_fallback_ui(void) {
    return g_ascii_fallback_ui;
}

int use_english_ui(void) {
    return g_ascii_fallback_ui || g_app_config.ui_language == UI_LANG_EN;
}

static const char *ui_text(const char *utf8, const char *ascii) {
    return use_english_ui() ? ascii : utf8;
}

static void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad);

static void trim_input_whitespace(char *buffer) {
    if (!buffer) {
        return;
    }

    char *start = buffer;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != buffer) {
        memmove(buffer, start, strlen(start) + 1);
    }

    size_t len = strlen(buffer);
    while (len > 0 && isspace((unsigned char)buffer[len - 1])) {
        buffer[--len] = '\0';
    }
}

static void pop_last_utf8_char(char *buffer) {
    if (!buffer) {
        return;
    }

    size_t len = strlen(buffer);
    if (len == 0) {
        return;
    }

    size_t new_len = len - 1;
    while (new_len > 0 && (((unsigned char)buffer[new_len] & 0xC0) == 0x80)) {
        new_len--;
    }
    buffer[new_len] = '\0';
}

static void redraw_text_input(WINDOW *win, int row, int col, const char *prompt,
                              const char *buffer, int password_mode) {
    if (!win || !prompt || !buffer) {
        return;
    }

    int h, w;
    getmaxyx(win, h, w);
    if (row < 0 || row >= h || col < 0 || col >= w) {
        return;
    }

    int prompt_width = utf8_str_width(prompt);
    int available_width = w - col - 1;
    if (available_width < 1) {
        available_width = 1;
    }

    int input_width = available_width - prompt_width;
    if (input_width < 1) {
        input_width = 1;
    }

    char visible_input[MAX_PATH_LEN * 2];
    visible_input[0] = '\0';

    const char *display_src = buffer;
    char masked[MAX_PATH_LEN * 2];
    if (password_mode) {
        int blen = utf8_str_width(buffer);
        for (int i = 0; i < blen && i < (int)sizeof(masked) - 1; i++) {
            masked[i] = '*';
        }
        masked[blen < (int)sizeof(masked) ? blen : sizeof(masked) - 1] = '\0';
        display_src = masked;
    }

    int total_width = utf8_str_width(display_src);
    if (total_width > input_width) {
        utf8_str_substring(visible_input, display_src, total_width - input_width, input_width);
    } else {
        snprintf(visible_input, sizeof(visible_input), "%s", display_src);
    }

    mvwprintw(win, row, col, "%s%s", prompt, visible_input);
    wclrtoeol(win);
    wmove(win, row, col + prompt_width + utf8_str_width(visible_input));
    wrefresh(win);
}

static int consume_input_escape_sequence(int *escape_state, wint_t ch) {
    if (!escape_state) {
        return 0;
    }

    switch (*escape_state) {
        case INPUT_ESCAPE_NONE:
            if (ch == 27) {
                *escape_state = INPUT_ESCAPE_ESC;
                return 1;
            }
            return 0;
        case INPUT_ESCAPE_ESC:
            if (ch == L'[') {
                *escape_state = INPUT_ESCAPE_CSI;
                return 1;
            }
            if (ch == L'O') {
                *escape_state = INPUT_ESCAPE_SS3;
                return 1;
            }
            if (ch == L']') {
                *escape_state = INPUT_ESCAPE_OSC;
                return 1;
            }
            *escape_state = INPUT_ESCAPE_NONE;
            return 1;
        case INPUT_ESCAPE_CSI:
            if (ch >= 0x40 && ch <= 0x7E) {
                *escape_state = INPUT_ESCAPE_NONE;
            }
            return 1;
        case INPUT_ESCAPE_SS3:
            *escape_state = INPUT_ESCAPE_NONE;
            return 1;
        case INPUT_ESCAPE_OSC:
            if (ch == 7) {
                *escape_state = INPUT_ESCAPE_NONE;
            } else if (ch == 27) {
                *escape_state = INPUT_ESCAPE_OSC_ESC;
            }
            return 1;
        case INPUT_ESCAPE_OSC_ESC:
            *escape_state = (ch == L'\\') ? INPUT_ESCAPE_NONE : INPUT_ESCAPE_OSC;
            return 1;
        default:
            *escape_state = INPUT_ESCAPE_NONE;
            return 0;
    }
}

int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                      char *buffer, size_t buffer_size, int trim_whitespace,
                      int password_mode, int prefill) {
    if (!win || !prompt || !buffer || buffer_size == 0) {
        return -1;
    }

    if (!prefill) {
        buffer[0] = '\0';
    }
    redraw_text_input(win, row, col, prompt, buffer, password_mode);
    flushinp();

    int escape_state = 0;

    while (1) {
        wint_t wch = 0;
        int rc = get_wch(&wch);
        if (rc == ERR) {
            escape_state = INPUT_ESCAPE_NONE;
            media_session_tick();
            continue;
        }

        if (rc == KEY_CODE_YES) {
            escape_state = INPUT_ESCAPE_NONE;
            if (wch == KEY_ENTER) {
                break;
            }
            if (wch == KEY_BACKSPACE) {
                pop_last_utf8_char(buffer);
                redraw_text_input(win, row, col, prompt, buffer, password_mode);
            }
            continue;
        }

        if (consume_input_escape_sequence(&escape_state, wch)) {
            continue;
        }

        if (wch == L'\n' || wch == L'\r') {
            break;
        }

        if (wch == 127 || wch == 8) {
            pop_last_utf8_char(buffer);
            redraw_text_input(win, row, col, prompt, buffer, password_mode);
            continue;
        }

        if (!iswprint(wch)) {
            continue;
        }

        char encoded[MB_CUR_MAX + 1];
        mbstate_t state;
        memset(&state, 0, sizeof(state));
        size_t written = wcrtomb(encoded, wch, &state);
        if (written == (size_t)-1 || written == 0) {
            continue;
        }

        size_t current_len = strlen(buffer);
        if (current_len + written >= buffer_size) {
            continue;
        }

        memcpy(buffer + current_len, encoded, written);
        buffer[current_len + written] = '\0';
        redraw_text_input(win, row, col, prompt, buffer, password_mode);
    }

    if (trim_whitespace) {
        trim_input_whitespace(buffer);
    }

    return 0;
}

static const char *get_playlist_source_label(void) {
    static char folder_path[MAX_PATH_LEN];

    if (playlist_has_multiple_sources()) {
        return ui_text("多目录队列", "Mixed queue");
    }

    playlist_copy_folder_path(folder_path, sizeof(folder_path));
    if (folder_path[0] != '\0') {
        return folder_path;
    }

    return ui_text("(无)", "(none)");
}

static int get_controls_progress_row(int height) {
    if (height >= 9) {
        return 1;
    }

    int row = height / 2 - 2;
    if (row < 1) {
        row = 1;
    }
    return row;
}

static int get_controls_button_row(int height) {
    if (height >= 9) {
        return 2;
    }

    int row = height / 2;
    if (row < 2) {
        row = 2;
    }
    return row;
}

static int get_controls_visualizer_top(int height) {
    if (height >= 9) {
        return 4;
    }
    return get_controls_button_row(height) + 1;
}

static int get_controls_visualizer_bottom(int height) {
    return height - 3;
}

static const char *get_control_label(int index) {
    if (index < 0 || index >= CONTROL_COUNT) {
        return "";
    }
    return use_english_ui() ? control_labels_en[index] : control_labels[index];
}

static const char *get_spectrum_glyph(int units) {
    static const char *glyphs[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    static const char ascii_glyphs[] = {' ', '.', ':', '-', '=', '+', '*', '#', '#'};

    if (units < 0) {
        units = 0;
    }
    if (units > 8) {
        units = 8;
    }

    if (use_ascii_fallback_ui()) {
        static char glyph_buf[2];
        glyph_buf[0] = ascii_glyphs[units];
        glyph_buf[1] = '\0';
        return glyph_buf;
    }

    return glyphs[units];
}

static void render_wave_particle_visualizer(int start_col, int graph_width) {
    if (!win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h < 7 || w < 24) {
        return;
    }

    int button_row = get_controls_button_row(h);
    int viz_top = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);
    int viz_height = viz_bottom - viz_top + 1;
    if (viz_height < 2) {
        return;
    }

    if (graph_width <= 0) {
        graph_width = w - 4;
    }
    if (start_col <= 0) {
        start_col = 2;
    }

    if (graph_width < 12) {
        return;
    }

    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    get_visualizer_snapshot(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    (void)peaks;

    uint64_t now_ms = get_ui_time_ms();
    int inactive_decay = 0;
    int is_visualizer_active = 0;
    if (last_update_ms > 0 && now_ms > last_update_ms) {
        inactive_decay = (int)((now_ms - last_update_ms) / 90ULL);
        if ((now_ms - last_update_ms) < 250ULL &&
            (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED)) {
            is_visualizer_active = 1;
        }
    }

    static int *column_units = NULL;
    static int column_units_cap = 0;
    if (graph_width > column_units_cap) {
        int *new_buf = realloc(column_units, (size_t)graph_width * sizeof(int));
        if (!new_buf) return;
        column_units = new_buf;
        column_units_cap = graph_width;
    }
    for (int col = 0; col < graph_width; col++) {
        double normalized = (graph_width <= 1)
            ? 0.0
            : ((double)col * (double)(VISUALIZER_BAND_COUNT - 1)) / (double)(graph_width - 1);
        int left = (int)normalized;
        int right = left + 1;
        if (right >= VISUALIZER_BAND_COUNT) {
            right = VISUALIZER_BAND_COUNT - 1;
        }
        double frac = normalized - (double)left;
        int blended_level = (int)lround(((double)levels[left] * (1.0 - frac)) + ((double)levels[right] * frac));

        int level = blended_level - inactive_decay * 7;
        if (level < 0) {
            level = 0;
        }

        if (is_visualizer_active && level > 0 && level < 3) {
            level = 3;
        }

        int units = (level * viz_height * 8 + 99) / 100;
        if (level > 0 && units == 0) {
            units = 1;
        }
        column_units[col] = units;
    }

    if (graph_width >= 3) {
        static int *smoothed_units = NULL;
        static int smoothed_units_cap = 0;
        if (graph_width > smoothed_units_cap) {
            int *new_buf = realloc(smoothed_units, (size_t)graph_width * sizeof(int));
            if (!new_buf) return;
            smoothed_units = new_buf;
            smoothed_units_cap = graph_width;
        }
        smoothed_units[0] = (column_units[0] * 3 + column_units[1]) / 4;
        for (int col = 1; col < graph_width - 1; col++) {
            smoothed_units[col] = (column_units[col - 1] + column_units[col] * 2 + column_units[col + 1]) / 4;
        }
        smoothed_units[graph_width - 1] = (column_units[graph_width - 2] + column_units[graph_width - 1] * 3) / 4;

        for (int col = 0; col < graph_width; col++) {
            column_units[col] = smoothed_units[col];
        }
    }

    for (int col = 0; col < graph_width; col++) {
        for (int row = viz_bottom; row >= viz_top; row--) {
            int row_from_bottom = viz_bottom - row;
            int units = column_units[col] - row_from_bottom * 8;
            if (units < 0) {
                units = 0;
            }
            if (units > 8) {
                units = 8;
            }
            mvwaddstr(win_controls, row, start_col + col, get_spectrum_glyph(units));
        }
    }
}

static void render_album_cover(void) {
    if (!win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    if (!g_album_cover_enabled || !g_app_config.show_album_cover) {
        return;
    }

    int h, w;
    getmaxyx(win_controls, h, w);

    int viz_top = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);
    int viz_height = viz_bottom - viz_top + 1;

    if (viz_height < BRAILLE_MIN_SIZE * BRAILLE_CELL_H) {
        return;
    }

    int optimal_size = calculate_optimal_cover_size(h);
    if (optimal_size < BRAILLE_MIN_SIZE) {
        return;
    }

    int cover_char_width = optimal_size * 2;
    int min_spectrum_width = 20;

    if (w - cover_char_width - 4 < min_spectrum_width) {
        return;
    }

    char cover_path[MAX_PATH_LEN];
    if (get_current_album_cover_path(cover_path, sizeof(cover_path)) != 0) {
        return;
    }

    if (g_album_cover_size != optimal_size || g_braille_art_buffer[0] == '\0') {
        g_album_cover_size = optimal_size;
        if (generate_braille_art_dynamic(cover_path, BRAILLE_DEFAULT_THRESHOLD,
                                          cover_char_width, optimal_size,
                                          g_braille_art_buffer, sizeof(g_braille_art_buffer)) != 0) {
            g_braille_art_buffer[0] = '\0';
            return;
        }
    }

    char *lines[BRAILLE_MAX_SIZE];
    int line_count = get_braille_art_lines(g_braille_art_buffer, lines, BRAILLE_MAX_SIZE);

    int start_row = viz_top;
    int start_col = 2;

    for (int i = 0; i < line_count && i < optimal_size; i++) {
        if (start_row + i < viz_bottom) {
            mvwprintw(win_controls, start_row + i, start_col, "%s", lines[i]);
        }
        free(lines[i]);
    }
}

static void clear_visualizer_area(void) {
    if (!win_controls) {
        return;
    }

    int h, w;
    getmaxyx(win_controls, h, w);

    int button_row = get_controls_button_row(h);
    int viz_top = get_controls_visualizer_top(h);
    int viz_bottom = get_controls_visualizer_bottom(h);

    int separator_row = viz_top - 1;
    if (separator_row > button_row && separator_row < h - 1) {
        mvwhline(win_controls, separator_row, 1, ACS_HLINE, w - 2);
        mvwaddch(win_controls, separator_row, 0, ACS_VLINE);
        mvwaddch(win_controls, separator_row, w - 1, ACS_VLINE);
    }

    for (int row = viz_top; row <= viz_bottom; row++) {
        mvwhline(win_controls, row, 1, ' ', w - 2);
    }
}

static void render_visualizer_with_album_cover(void) {
    if (!win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    int h, w;
    getmaxyx(win_controls, h, w);

    // 当高度较小时，按钮可能换行，为避免重叠，禁用可视化效果
    if (h < 9) {
        return;
    }

    clear_visualizer_area();

    if (g_app_config.show_album_cover) {
        render_album_cover();
    }

    render_wave_particle_visualizer(2, w - 4);
}

static void build_control_label(int index, char *dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (index < 0 || index >= CONTROL_COUNT) {
        return;
    }

    if (index == CONTROL_IDX_LOOP) {
        snprintf(dest, dest_size, "%s:%s", get_control_label(index), get_loop_mode_str());
        return;
    }

    if (index == CONTROL_IDX_SPEED) {
        snprintf(dest, dest_size, "%s:%.2fx", get_control_label(index), g_playback_speed);
        return;
    }

    if (index == CONTROL_IDX_VOLUME) {
        snprintf(dest, dest_size, "%s:%d%%", get_control_label(index), get_volume_percent());
        return;
    }

    utf8_str_truncate(dest, get_control_label(index), (int)dest_size - 1);
}

static int is_primary_mouse_click(const MEVENT *event) {
    if (!event) {
        return 0;
    }

    if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED)) {
        return 1;
    }

    return (event->bstate & BUTTON1_RELEASED) != 0 &&
           (event->bstate & BUTTON1_PRESSED) == 0;
}

static int translate_screen_to_window(WINDOW *win, int screen_y, int screen_x, int *window_y, int *window_x) {
    if (!win) {
        return 0;
    }

    int beg_y, beg_x;
    int height, width;
    getbegyx(win, beg_y, beg_x);
    getmaxyx(win, height, width);

    if (screen_y < beg_y || screen_y >= beg_y + height ||
        screen_x < beg_x || screen_x >= beg_x + width) {
        return 0;
    }

    if (window_y) {
        *window_y = screen_y - beg_y;
    }
    if (window_x) {
        *window_x = screen_x - beg_x;
    }
    return 1;
}

static int get_playlist_index_from_window_row(int window_y, int *display_index, int *actual_index) {
    if (!win_playlist) {
        return 0;
    }

    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;

    int content_height = h - 2;
    int visible_lines = content_height - 6;
    if (visible_lines <= 0 || window_y < 1 || window_y >= 1 + visible_lines) {
        return 0;
    }

    int clicked_display_index = get_playlist_scroll_offset() + (window_y - 1);

    if (g_search_state.active || g_search_state.in_progress) {
        pthread_mutex_lock(&g_search_mutex);
        int snap_count = g_search_state.result_count;
        int idx = (snap_count > 0 && clicked_display_index >= 0 && clicked_display_index < snap_count)
                  ? g_search_state.result_indices[clicked_display_index] : -1;
        pthread_mutex_unlock(&g_search_mutex);
        if (idx < 0) {
            return 0;
        }
        if (display_index) {
            *display_index = clicked_display_index;
        }
        if (actual_index) {
            *actual_index = idx;
        }
        return 1;
    }

    if (g_sort_state.active) {
        if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) {
            return 0;
        }
        if (display_index) {
            *display_index = clicked_display_index;
        }
        if (actual_index) {
            *actual_index = g_sort_state.sorted_indices[clicked_display_index];
        }
        return 1;
    }

    if (clicked_display_index < 0 || clicked_display_index >= playlist_count()) {
        return 0;
    }

    if (display_index) {
        *display_index = clicked_display_index;
    }
    if (actual_index) {
        *actual_index = clicked_display_index;
    }
    return 1;
}

static int get_control_index_from_window_point(int window_y, int window_x) {
    if (!win_controls) {
        return -1;
    }

    int h, w;
    getmaxyx(win_controls, h, w);

    int button_start_row = get_controls_button_row(h);
    int button_end_row = h - 2;

    if (window_y < button_start_row || window_y > button_end_row) {
        return -1;
    }

    int available_width = w - 2;
    int current_col = 0;
    int is_first_in_row = 1;
    int current_row = button_start_row;

    for (int i = 0; i < CONTROL_COUNT - 1; i++) {
        char display_label[32];
        build_control_label(i, display_label, sizeof(display_label));
        int button_width = utf8_str_width(display_label) + 4;

        if (is_first_in_row) {
            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT-1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;

                if (row_width + temp_button_width > available_width && row_button_count > 0) {
                    break;
                }
                row_width += temp_button_width;
                row_button_count++;
            }

            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
            is_first_in_row = 0;
        }

        if (!is_first_in_row && current_col + button_width > w - 1) {
            current_row++;
            if (current_row > button_end_row) {
                break;
            }

            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT-1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;

                if (row_width + temp_button_width > available_width && row_button_count > 0) {
                    break;
                }
                row_width += temp_button_width;
                row_button_count++;
            }

            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
        }

        if (window_y == current_row && window_x >= current_col && window_x < current_col + button_width) {
            return i;
        }

        current_col += button_width;
    }

    return -1;
}

static int get_lyric_index_from_window_row(int window_y, int *lyric_index, double *timestamp) {
    if (!win_lyrics || !g_app_config.show_lyrics_panel) {
        return 0;
    }

    int h, w;
    getmaxyx(win_lyrics, h, w);

    pthread_mutex_lock(&g_lyrics.lock);

    if (!g_lyrics.has_lyrics || g_lyrics.count == 0 || g_lyrics.current_index < 0) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int content_top = calculate_lyrics_content_top(h, w);
    int visible_lines = h - content_top - 1;
    if (visible_lines <= 0 || window_y < content_top || window_y >= content_top + visible_lines) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    int current_center_idx = (g_lyric_cursor_mode && g_lyrics.cursor_index >= 0)
        ? g_lyrics.cursor_index
        : g_lyrics.current_index;

    int start_idx = current_center_idx - (visible_lines / 2);
    if (start_idx < 0) {
        start_idx = 0;
    }
    if (start_idx + visible_lines > g_lyrics.count) {
        start_idx = g_lyrics.count - visible_lines;
    }
    if (start_idx < 0) {
        start_idx = 0;
    }

    int clicked_lyric_index = start_idx + (window_y - content_top);
    if (clicked_lyric_index < 0 || clicked_lyric_index >= g_lyrics.count) {
        pthread_mutex_unlock(&g_lyrics.lock);
        return 0;
    }

    if (lyric_index) {
        *lyric_index = clicked_lyric_index;
    }
    if (timestamp) {
        *timestamp = g_lyrics.lines[clicked_lyric_index].timestamp;
    }

    pthread_mutex_unlock(&g_lyrics.lock);
    return 1;
}

int get_menu_hint_fkey_from_column(int screen_x) {
    static const char *menu_labels_zh[] = {
        "F1:主页", "F2:设置", "F3:历史", "F4:歌单",
        "F5:收藏", "F6:信息", "F7:中/EN", "F8:帮助", "F9:退出"
    };
    static const char *menu_labels_en[] = {
        "F1:Home", "F2:Settings", "F3:History", "F4:Playlists",
        "F5:Favorites", "F6:Info", "F7:Lang", "F8:Help", "F9:Quit"
    };

    const char **labels = use_english_ui() ? menu_labels_en : menu_labels_zh;
    int col = 2;

    for (int i = 0; i < 9; i++) {
        int width = utf8_str_width(labels[i]);
        if (screen_x >= col && screen_x < col + width) {
            return KEY_F(i + 1);
        }
        col += width + 2;
    }

    return 0;
}

int handle_menu_hint_bar_click(const MEVENT *event) {
    if (!event || !is_primary_mouse_click(event)) {
        return 0;
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    if (event->y == max_y - 1) {
        int fkey = get_menu_hint_fkey_from_column(event->x);
        if (fkey != 0) {
            handle_function_keys(fkey);
            return 1;
        }
    }

    return 0;
}

static int handle_main_view_mouse_event(const MEVENT *event) {
    if (!event || !is_primary_mouse_click(event)) {
        return 0;
    }

    int window_y, window_x;
    if (translate_screen_to_window(win_playlist, event->y, event->x, &window_y, &window_x)) {
        int display_index = -1;
        int actual_index = -1;
        (void)window_x;

        if (!get_playlist_index_from_window_row(window_y, &display_index, &actual_index)) {
            return 0;
        }

        g_control_focus = 0;
        if (g_search_state.active || g_search_state.in_progress) {
            if (g_search_state.in_progress) {
                search_async_cancel();
                pthread_mutex_lock(&g_search_mutex);
                g_search_state.in_progress = 0;
                pthread_mutex_unlock(&g_search_mutex);
            }
            g_search_state.selected_index = display_index;
            play_audio(actual_index);
            g_search_state.active = 0;
            g_selected_index = g_sort_state.active ? 0 : actual_index;
            render_playlist_content();
        } else {
            g_selected_index = display_index;
            play_audio(actual_index);
        }
        render_controls();
        return 1;
    }

    if (translate_screen_to_window(win_controls, event->y, event->x, &window_y, &window_x)) {
        int control_index = get_control_index_from_window_point(window_y, window_x);
        if (control_index < 0) {
            return 0;
        }

        g_control_focus = 1;
        g_current_control_idx = control_index;

        if (control_index != CONTROL_IDX_VOLUME && control_index != CONTROL_IDX_PROGRESS) {
            activate_current_control();
            render_playlist_content();
        }
        render_controls();
        return 1;
    }

    if (translate_screen_to_window(win_lyrics, event->y, event->x, &window_y, &window_x)) {
        int lyric_index = -1;
        double target_timestamp = 0.0;
        (void)window_x;

        if (!get_lyric_index_from_window_row(window_y, &lyric_index, &target_timestamp)) {
            return 0;
        }

        pthread_mutex_lock(&g_lyrics.lock);
        g_lyrics.cursor_index = lyric_index;
        g_lyric_cursor_index = lyric_index;
        pthread_mutex_unlock(&g_lyrics.lock);

        seek_audio(target_timestamp);
        render_lyrics();
        return 1;
    }

    return 0;
}

static int locale_uses_utf8(void) {
    const char *codeset = nl_langinfo(CODESET);
    if (!codeset) {
        return 0;
    }

    return strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0;
}

static void ensure_utf8_locale(void) {
    const char *fallbacks[] = {"C.UTF-8", "zh_CN.UTF-8", "en_US.UTF-8", NULL};

    setlocale(LC_ALL, "");
    if (locale_uses_utf8()) {
        return;
    }

    for (int i = 0; fallbacks[i] != NULL; i++) {
        if (setlocale(LC_ALL, fallbacks[i]) && locale_uses_utf8()) {
            return;
        }
    }

    setlocale(LC_CTYPE, "");
}

static int locale_supports_cjk_width(void) {
    static const char *sample = "中";
    mbstate_t state;
    memset(&state, 0, sizeof(state));

    wchar_t wc = 0;
    size_t len = mbrtowc(&wc, sample, strlen(sample), &state);
    if (len == (size_t)-1 || len == (size_t)-2 || len == 0) {
        return 0;
    }

    return wcwidth(wc) == 2;
}

static int terminal_needs_ascii_fallback(void) {
    const char *term = getenv("TERM");
    const char *force_ascii = getenv("TER_MUSIC_FORCE_ASCII");
    const char *force_utf8 = getenv("TER_MUSIC_FORCE_UTF8");

    if (force_utf8 && strcmp(force_utf8, "1") == 0) {
        return 0;
    }
    if (force_ascii && strcmp(force_ascii, "1") == 0) {
        return 1;
    }
    if (!locale_uses_utf8()) {
        return 1;
    }
    if (MB_CUR_MAX <= 1 || !locale_supports_cjk_width()) {
        return 1;
    }
    if (!term || term[0] == '\0') {
        return 0;
    }
    if (strcmp(term, "dumb") == 0) {
        return 1;
    }

    return 0;
}

static size_t utf8_next_char(const char *src, wchar_t *wc_out, int *width_out) {
    mbstate_t state;
    memset(&state, 0, sizeof(state));

    wchar_t wc = 0;
    size_t char_len = mbrtowc(&wc, src, MB_CUR_MAX, &state);
    if (char_len == (size_t)-1 || char_len == (size_t)-2) {
        wc = (unsigned char)src[0];
        char_len = 1;
    } else if (char_len == 0) {
        if (wc_out) {
            *wc_out = L'\0';
        }
        if (width_out) {
            *width_out = 0;
        }
        return 0;
    }

    int width = wcwidth(wc);
    if (width < 0) {
        width = 1;
    }

    if (wc_out) {
        *wc_out = wc;
    }
    if (width_out) {
        *width_out = width;
    }
    return char_len;
}

static void sanitize_ascii_text(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }

    size_t write = 0;
    int prev_space = 1;
    int saw_non_ascii = 0;

    for (size_t read = 0; src[read] != '\0' && write + 1 < dest_size; ) {
        unsigned char c = (unsigned char)src[read];

        if (c < 0x80) {
            if (isprint(c)) {
                if (isspace(c)) {
                    if (!prev_space) {
                        dest[write++] = ' ';
                        prev_space = 1;
                    }
                } else {
                    dest[write++] = (char)c;
                    prev_space = 0;
                }
            }
            read++;
            continue;
        }

        saw_non_ascii = 1;
        int char_width = 1;
        size_t char_len = utf8_next_char(src + read, NULL, &char_width);
        if (char_len == 0) {
            break;
        }
        if (!prev_space && write + 1 < dest_size) {
            dest[write++] = ' ';
            prev_space = 1;
        }
        read += char_len;
    }

    while (write > 0 && dest[write - 1] == ' ') {
        write--;
    }
    dest[write] = '\0';

    if (write == 0 && saw_non_ascii) {
        snprintf(dest, dest_size, "[non-ASCII]");
    }
}

static void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad) {
    if (!dest || dest_size == 0) {
        return;
    }

    const char *input = src ? src : "";
    char ascii_buf[MAX_PATH_LEN];
    if (g_ascii_fallback_ui) {
        sanitize_ascii_text(ascii_buf, sizeof(ascii_buf), input);
        input = ascii_buf;
    }

    if (pad) {
        utf8_str_pad(dest, dest_size, input, width);
    } else {
        utf8_str_truncate(dest, input, width);
    }
}

static void render_controls_status_line(void) {
    if (!win_controls) {
        return;
    }

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h <= 2 || w <= 3) {
        return;
    }

    char status_msg[sizeof(g_controls_status_message)];
    time_t status_time = 0;

    pthread_mutex_lock(&g_ui_request_mutex);
    snprintf(status_msg, sizeof(status_msg), "%s", g_controls_status_message);
    status_time = g_controls_status_time;
    pthread_mutex_unlock(&g_ui_request_mutex);

    int status_row = h - 2;
    mvwhline(win_controls, status_row, 1, ' ', w - 2);

    if (status_msg[0] != '\0' && (time(NULL) - status_time) < 3) {
        char display_msg[sizeof(g_controls_status_message)];
        format_display_text(display_msg, sizeof(display_msg), status_msg, w - 4, 0);
        mvwprintw(win_controls, status_row, 2, "%s", display_msg);
    }
}

void request_ui_refresh(int dirty_mask) {
    if (dirty_mask == 0) {
        return;
    }

    pthread_mutex_lock(&g_ui_request_mutex);
    g_pending_ui_mask |= dirty_mask;
    pthread_mutex_unlock(&g_ui_request_mutex);
}

void process_pending_ui_refresh(void) {
    int dirty_mask = 0;

    pthread_mutex_lock(&g_ui_request_mutex);
    dirty_mask = g_pending_ui_mask;
    g_pending_ui_mask = 0;
    pthread_mutex_unlock(&g_ui_request_mutex);

    if (dirty_mask == 0 || g_current_view != VIEW_MAIN) {
        return;
    }

    if (dirty_mask & UI_DIRTY_PLAYLIST) {
        render_playlist_content();
    }
    if (dirty_mask & UI_DIRTY_CONTROLS) {
        render_controls();
    }
    if (dirty_mask & UI_DIRTY_LYRICS) {
        render_lyrics();
    }
}

/**
 * 初始化ncurses环境
 * 设置本地化、终端模式和颜色对
 */
void init_ncurses() {
    log_info("ui", "Initializing ncurses");
    ensure_utf8_locale();
    g_ascii_fallback_ui = terminal_needs_ascii_fallback();

    if (isatty(STDOUT_FILENO)) {
        printf("\033%%G");
        fflush(stdout);
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED | BUTTON1_RELEASED, NULL);
    curs_set(0);
    clear();

    if (has_colors()) {
        start_color();
        init_pair(COLOR_PAIR_BORDER, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_PAIR_PLAYLIST, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_PAIR_CONTROLS, COLOR_YELLOW, COLOR_BLACK);
        init_pair(COLOR_PAIR_LYRICS, COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_PAIR_SIDEBAR, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK, COLOR_WHITE);
    }
}

/**
 * UTF-8字符串截断函数
 * 根据显示列数而非字节数截断字符串，正确处理多字节字符
 */
int utf8_str_truncate(char *dest, const char *src, int max_cols) {
    if (!dest || !src || max_cols <= 0) {
        if (dest) *dest = '\0';
        return 0;
    }

    int cols = 0;
    char *d = dest;
    const char *s = src;

    while (*s && cols < max_cols) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0 || cols + char_width > max_cols) {
            break;
        }

        memcpy(d, s, char_len);
        d += char_len;
        s += char_len;
        cols += char_width;
    }

    if (*s && cols + 3 <= max_cols) {
        memcpy(d, "...", 4);
    } else {
        *d = '\0';
    }

    return cols;
}

/**
 * 计算 UTF-8 字符串的显示宽度（列数）
 */
int utf8_str_width(const char *src) {
    if (!src) return 0;
    int cols = 0;
    const char *s = src;

    while (*s) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0) {
            break;
        }

        s += char_len;
        cols += char_width;
    }
    return cols;
}

/**
 * UTF-8 字符串从指定偏移开始截取
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param start_col 起始列偏移（从 0 开始）
 * @param max_cols 最大列数
 * @return 实际占用的列数
 */
int utf8_str_substring(char *dest, const char *src, int start_col, int max_cols) {
    if (!dest || !src || max_cols <= 0) {
        if (dest) *dest = '\0';
        return 0;
    }

    int cols = 0;
    char *d = dest;
    const char *s = src;
    int result_cols = 0;
    int leading_padding = 0;

    while (*s && cols < start_col) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0) {
            break;
        }

        if (cols + char_width > start_col) {
            leading_padding = cols + char_width - start_col;
            s += char_len;
            break;
        }

        s += char_len;
        cols += char_width;
    }

    while (leading_padding > 0 && result_cols < max_cols) {
        *d++ = ' ';
        leading_padding--;
        result_cols++;
    }

    while (*s && result_cols < max_cols) {
        int char_width = 1;
        size_t char_len = utf8_next_char(s, NULL, &char_width);
        if (char_len == 0 || result_cols + char_width > max_cols) {
            break;
        }

        memcpy(d, s, char_len);
        d += char_len;
        s += char_len;
        result_cols += char_width;
    }

    *d = '\0';

    return result_cols;
}

int utf8_str_pad(char *dest, size_t dest_size, const char *src, int width) {
    if (!dest || dest_size == 0 || width <= 0) {
        if (dest && dest_size > 0) {
            dest[0] = '\0';
        }
        return 0;
    }

    utf8_str_truncate(dest, src ? src : "", width);

    int current_width = utf8_str_width(dest);
    size_t len = strlen(dest);
    // 使用条件表达式避免 GCC 的 -Wstringop-overflow 误报
    while (current_width < width && len < dest_size - 1) {
        if (len < dest_size - 1) {
            dest[len] = ' ';
            len++;
        }
        current_width++;
    }
    if (len < dest_size) {
        dest[len] = '\0';
    }
    return current_width;
}

int utf8_backspace(char *buffer, int pos, WINDOW *win) {
    if (!buffer || pos <= 0) {
        return pos;
    }

    WINDOW *target_win = win ? win : stdscr;
    int cx = getcurx(target_win);
    int cy = getcury(target_win);
    unsigned char last_c = (unsigned char)buffer[pos - 1];
    int bytes_to_remove = 1;

    if (last_c >= 0x80) {
        if ((last_c & 0xE0) == 0xC0) bytes_to_remove = 2;
        else if ((last_c & 0xF0) == 0xE0) bytes_to_remove = 3;
        else if ((last_c & 0xF8) == 0xF0) bytes_to_remove = 4;
        else if ((last_c & 0xC0) == 0x80) {
            bytes_to_remove = 2;
            while (pos - bytes_to_remove >= 0 &&
                   (unsigned char)buffer[pos - bytes_to_remove] >= 0x80 &&
                   (unsigned char)buffer[pos - bytes_to_remove] < 0xC0) {
                bytes_to_remove++;
            }
        }
        if (bytes_to_remove > pos) bytes_to_remove = pos;
        pos -= bytes_to_remove;

        if ((last_c & 0xF0) == 0xE0 || (last_c & 0xE0) == 0xC0) {
            wmove(target_win, cy, cx - 2);
        } else {
            wmove(target_win, cy, cx - 1);
        }
    } else {
        pos--;
        wmove(target_win, cy, cx - 1);
    }

    wclrtoeol(target_win);
    wrefresh(target_win);
    return pos;
}

/**
 * 创建和调整窗口布局
 * 设置播放列表、控制栏和歌词窗口的大小和位置
 */
void create_layout() {
    log_debug("ui", "create_layout() called");
    // 初始化视图状态（仅在第一次调用时）
    static int initialized = 0;
    if (!initialized) {
        g_current_view = VIEW_MAIN;
        g_menu_selected_idx = 0;
        initialized = 1;
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    // 边界检查：确保最小尺寸，防止负数尺寸导致崩溃
    if (max_y < 8) max_y = 8;
    if (max_x < 20) max_x = 20;

    int lyrics_width;
    if (max_x >= 160) {
        lyrics_width = max_x / 3;
    } else if (max_x >= 120) {
        lyrics_width = (max_x * 3) / 8;
    } else {
        lyrics_width = (max_x * 2) / 5;
    }

    if (lyrics_width < 28) {
        lyrics_width = 28;
    }
    if (lyrics_width > max_x - 44) {
        lyrics_width = max_x - 44;
    }
    if (lyrics_width < 10) lyrics_width = 10;
    int main_width;
    if (g_app_config.show_lyrics_panel) {
        main_width = max_x - lyrics_width - 1;
    } else {
        lyrics_width = 0;
        main_width = max_x - 1;
    }
    if (main_width < 10) main_width = 10;

    // 预留边框空间和底部提示条（预留1行给菜单提示条）
    // 使左侧总高度与右侧歌词区高度对齐：歌词区高度是 max_y - 3
    int total_inner_height = max_y - 3;
    if (total_inner_height < 3) total_inner_height = 3;

    // 控制区域高度计算（优先保证最小4行）
    int controls_height;
    if (max_y >= 34) {
        controls_height = 7;
    } else if (max_y >= 24) {
        controls_height = 6;
    } else {
        controls_height = 5;
    }

    // 播放列表信息栏需要2行：边框1行 + 标题1行
    int min_controls = 4;        // 控制区域最小4行
    int min_playlist_info = 2;   // 播放列表信息栏最小2行
    int min_playlist = 1;        // 播放列表最小1行（极端情况）
    
    // 逐步压缩逻辑（优先级从高到低）：
    // 1. 控制区域（最小4行）
    // 2. 播放列表信息栏（最小2行）
    // 3. 播放列表内容区域（首先被压缩）
    
    // 计算初始播放列表高度
    int playlist_height = total_inner_height - controls_height;
    
    // 步骤1：保证播放列表信息栏至少2行
    if (playlist_height < min_playlist_info) {
        playlist_height = min_playlist_info;
        controls_height = total_inner_height - playlist_height;
    }
    
    // 步骤2：保证控制区域至少4行
    if (controls_height < min_controls) {
        controls_height = min_controls;
        playlist_height = total_inner_height - controls_height;
    }
    
    // 步骤3：如果播放列表被压缩到小于1行，调整
    if (playlist_height < min_playlist) {
        playlist_height = min_playlist;
        controls_height = total_inner_height - playlist_height;
    }
    
    // 步骤4：最终保护，确保没有负数高度
    if (controls_height < 1) controls_height = 1;
    if (playlist_height < 1) playlist_height = 1;
    
    // 重新平衡：如果总和超过 total_inner_height，压缩播放列表
    int total_used = controls_height + playlist_height;
    if (total_used > total_inner_height) {
        playlist_height = total_inner_height - controls_height;
        if (playlist_height < 1) {
            playlist_height = 1;
            controls_height = total_inner_height - playlist_height;
        }
    }

    // 1. 创建播放列表窗口 (左上)
    win_playlist = newwin(playlist_height, main_width, 1, 1);
    box(win_playlist, 0, 0);
    mvwprintw(win_playlist, 0, 2, "%s", ui_text(" 播放列表 ", " Playlist "));
    wbkgd(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    wrefresh(win_playlist);

    // 2. 创建控制栏窗口 (左下)
    win_controls = newwin(controls_height, main_width, 1 + playlist_height, 1);
    box(win_controls, 0, 0);
    const char *focus_hint = g_control_focus ? ui_text("[控件焦点]", "[Ctrl Focus]") : ui_text("[列表焦点]", "[List Focus]");
    const char *lyric_hint = g_lyric_cursor_mode ? ui_text("[D:退出定位]", "[D:Exit Seek]") : ui_text("[D:歌词定位]", "[D:Lyric Seek]");
    char controls_header[160];
    snprintf(controls_header, sizeof(controls_header), "%s %s %s %s %s",
             ui_text("控制区", "Controls"),
             ui_text("[空格:执行]", "[Space:Run]"),
             ui_text("[C:控件]", "[C:Ctrl]"),
             ui_text("[L:列表]", "[L:List]"),
             focus_hint);
    mvwprintw(win_controls, 0, 2, " %s %s", controls_header, lyric_hint);
    wbkgd(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));
    
    wrefresh(win_controls);

    // 3. 创建歌词侧栏窗口 (右侧) - 高度减1为底部提示条预留空间
    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        int lyrics_height = max_y - 3;
        if (lyrics_height < 3) lyrics_height = 3;
        win_lyrics = newwin(lyrics_height, lyrics_width, 1, 1 + main_width);
        box(win_lyrics, 0, 0);
        mvwprintw(win_lyrics, 0, 2, "%s", ui_text(" 歌词 ", " Lyrics "));
        wbkgd(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));
        
        mvwprintw(win_lyrics, 2, 2, "%s", ui_text("未加载歌词。", "No lyrics loaded."));
        wrefresh(win_lyrics);
    } else {
        win_lyrics = NULL;
    }

    // --- 新增：绘制分隔线 ---

    // 绘制左侧区域与右侧歌词区之间的垂直分隔线（仅当显示歌词面板时）
    // 起点：(1, 1 + main_width), 长度：max_y - 3（给底部提示条预留空间）
    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        int vline_len = max_y - 3;
        if (vline_len < 1) vline_len = 1;
        mvvline(1, 1 + main_width, ACS_VLINE, vline_len);
    }

    // 绘制播放列表与控制栏之间的水平分隔线
    // 起点：(1 + playlist_height, 1), 长度：main_width
    int hline_len = main_width;
    if (hline_len < 1) hline_len = 1;
    mvhline(1 + playlist_height, 1, ACS_HLINE, hline_len);
    
    // 绘制交叉点字符，使分隔线连接更自然（仅当显示歌词面板时）
    if (g_app_config.show_lyrics_panel && lyrics_width > 0) {
        mvaddch(1 + playlist_height, 1 + main_width, ACS_PLUS);
    }

    // 刷新标准屏以显示分隔线
    refresh();
    
    // 渲染底部菜单栏提示条
    render_menu_hint_bar();
}

/**
 * 渲染播放列表内容
 * 包括歌曲列表和底部状态栏
 */
void render_playlist_content() {
    // 空指针检查：避免win_playlist未初始化时崩溃
    if (!win_playlist) {
        return;
    }

    // 图书馆浏览模式：完全替代播放列表渲染
    if (g_library_state.active) {
        render_library_content();
        return;
    }

    // 拍摄搜索状态快照，保证渲染一致性
    int snap_active, snap_in_progress, snap_count, snap_selected, snap_progress;
    int snap_indices[MAX_SEARCH_RESULTS];
    pthread_mutex_lock(&g_search_mutex);
    snap_active = g_search_state.active;
    snap_in_progress = g_search_state.in_progress;
    snap_count = g_search_state.result_count;
    snap_selected = g_search_state.selected_index;
    snap_progress = g_search_state.progress;
    if (snap_count > 0) {
        memcpy(snap_indices, g_search_state.result_indices,
               snap_count * sizeof(int));
    }
    pthread_mutex_unlock(&g_search_mutex);

    werase(win_playlist); // 清空窗口内容
    box(win_playlist, 0, 0);
    char title_buf[128];
    if (snap_in_progress) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d/%d) ",
                 ui_text(" 搜索中 ", " Searching "),
                 snap_progress,
                 playlist_count());
        mvwprintw(win_playlist, 0, 2, "%s", title_buf);
    } else if (snap_active) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d %s) ",
                 ui_text(" 搜索结果 ", " Search Results "),
                 snap_count,
                 ui_text("个", "found"));
        mvwprintw(win_playlist, 0, 2, "%s", title_buf);
    } else {
        mvwprintw(win_playlist, 0, 2, "%s", ui_text(" 播放列表 ", " Playlist "));
    }
    wbkgd(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    int h, w;
    getmaxyx(win_playlist, h, w);
    int content_height = h - 2; // 可用行数
    int playlist_total = playlist_count();
    int playlist_loaded = playlist_is_loaded();
    
    int total_tracks;
    int current_selected;
    
    if ((snap_active || snap_in_progress) && snap_count > 0) {
        total_tracks = snap_count;
        current_selected = snap_selected;
    } else if (snap_in_progress) {
        // 搜索进行中但尚无结果
        total_tracks = 0;
        current_selected = 0;
    } else {
        total_tracks = playlist_total;
        current_selected = g_selected_index;
    }
    
    // 如果未加载，显示提示信息
    if (!playlist_loaded) {
        char display_path[MAX_PATH_LEN];
        format_display_text(display_path, sizeof(display_path),
                            get_playlist_source_label(),
                            w - 10, 0);
        mvwprintw(win_playlist, h/2 - 1, 2, "%s", ui_text("播放列表为空。", "Playlist is empty."));
        mvwprintw(win_playlist, h/2, 2, "%s",
                  ui_text("按 'O' 打开目录，按 'I' 追加目录。",
                          "Press 'O' to open a folder, 'I' to append one."));
        mvwprintw(win_playlist, h/2 + 1, 2, "%s%s", ui_text("当前路径：", "Path: "), display_path);
    } else {
        int start_idx = 0;
        int visible_lines = content_height - 6; // 预留 6 行给底部状态栏(1分隔线+5信息行)

        if (snap_active || snap_in_progress) {
            if (snap_selected >= visible_lines) {
                start_idx = snap_selected - visible_lines + 1;
            }
        } else {
            if (g_selected_index >= visible_lines) {
                start_idx = g_selected_index - visible_lines + 1;
            }
        }

        if (g_sort_state.active && !snap_active) {
            // 排序激活时，按排序后的实际索引预加载元数据缓存
            int loaded = 0;
            Track tmp;
            for (int vi = start_idx; vi < start_idx + visible_lines && vi < total_tracks && loaded < 2; vi++) {
                get_track_metadata(g_sort_state.sorted_indices[vi], &tmp);
                loaded++;
            }
        } else {
            preload_visible_tracks(start_idx, start_idx + visible_lines - 1);
        }

        for (int i = 0; i < visible_lines && (start_idx + i) < total_tracks; i++) {
            int idx = start_idx + i;
            int actual_idx = idx;
            Track t;

            if (snap_active || snap_in_progress) {
                actual_idx = snap_indices[idx];
            } else if (g_sort_state.active) {
                actual_idx = g_sort_state.sorted_indices[idx];
            }
            
            get_track_metadata(actual_idx, &t);
            
            // 计算可用宽度，为不同字段分配空间
            int title_width = (w - 4) * 3 / 5;  // 标题占3/5
            int artist_width = (w - 4) * 2 / 5; // 艺术家占2/5
            
            // 截断过长的字符串
            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            
            format_display_text(truncated_title, sizeof(truncated_title), t.title, title_width - 1, 1);
            format_display_text(truncated_artist, sizeof(truncated_artist), t.artist, artist_width - 1, 1);

            int is_selected;
            if (snap_active || snap_in_progress) {
                is_selected = (idx == snap_selected);
            } else {
                is_selected = (idx == g_selected_index && g_control_focus == 0);
            }
            
            if (is_selected) {
                wattron(win_playlist, A_REVERSE);
                mvwprintw(win_playlist, i + 1, 1, " %s %s ", truncated_title, truncated_artist);
                wattroff(win_playlist, A_REVERSE);
            } else {
                mvwprintw(win_playlist, i + 1, 2, "%s %s", truncated_title, truncated_artist);
            }
        }
        
        if (total_tracks == 0) {
            if (snap_in_progress) {
                mvwprintw(win_playlist, 1, 2, "%s",
                         ui_text("正在搜索...", "Searching..."));
            } else if (snap_active) {
                mvwprintw(win_playlist, 1, 2, "%s",
                         ui_text("没有找到匹配的歌曲。", "No matching tracks found."));
            } else {
                mvwprintw(win_playlist, 1, 2, "%s",
                         ui_text("当前目录下没有音频文件。", "No audio files found here."));
            }
        }

        // 绘制滚动条
        {
            int sb_col = w - 2;
            scrollbar_draw(win_playlist, 1, visible_lines,
                           total_tracks, visible_lines, start_idx, sb_col);
        }

        // --- 新增：在播放列表底部绘制状态栏 ---
        int status_line = h - 7;  // 上移一行，为5行信息栏留出空间
        mvwhline(win_playlist, status_line, 1, ACS_HLINE, w - 2);
        
        // 根据全局播放状态更新状态信息
        char status_msg[MAX_META_LEN];
        switch (g_play_state) {
            case PLAY_STATE_PLAYING:
                snprintf(status_msg, sizeof(status_msg), "%s", ui_text("播放中", "Playing"));
                break;
            case PLAY_STATE_PAUSED:
                snprintf(status_msg, sizeof(status_msg), "%s", ui_text("已暂停", "Paused"));
                break;
            case PLAY_STATE_STOPPED:
            default:
                snprintf(status_msg, sizeof(status_msg), "%s", ui_text("已停止", "Stopped"));
                break;
        }
        
        if (playlist_total > 0) {
            Track t;
            int index = g_current_play_index >= 0 ? g_current_play_index : g_selected_index;
            if (g_sort_state.active && !snap_active && g_current_play_index < 0) {
                index = g_sort_state.sorted_indices[g_selected_index];
            }
            if (index < 0) {
                index = 0;
            }
            if (index >= playlist_total) {
                index = playlist_total - 1;
            }
            get_track_metadata(index, &t);

            // 计算专辑封面显示区域
            int cover_start_col = w - 2;
            int cover_char_width = 0;
            int cover_lines = 0;
            int show_cover = 0;

            // 检查是否需要重新生成字符画
            char cover_path[MAX_PATH_LEN];
            if (g_album_cover_enabled && g_app_config.show_album_cover &&
                get_current_album_cover_path(cover_path, sizeof(cover_path)) == 0) {
                // 根据窗口宽度计算合适的封面大小
                int min_info_width = 52;  // 左侧信息栏最小宽度（两列各约26字符）
                int max_cover_size = 12;  // 最大封面大小（字符高度）
                int min_cover_size = 5;   // 最小封面大小

                // 计算可用空间（5行信息：状态、循环、标题、艺术家、专辑）
                int available_width = w - min_info_width - 4;  // 减去边框和间距
                int available_height = 5;  // 信息栏有5行可用

                // 计算封面大小（每个盲文字符2字符宽x4像素高，封面宽高比应为1:2字符）
                // 为了正方效果，封面高度（字符行数）= 封面宽度（字符数）/ 2
                int max_cover_width = available_width / 2;
                int max_cover_height = available_height;  // 高度等于行数

                // 让封面接近方形：宽度（字符数）= 高度（行数）* 2
                int cover_size = max_cover_width < max_cover_height ? max_cover_width : max_cover_height;
                if (cover_size > max_cover_size) cover_size = max_cover_size;
                if (cover_size < min_cover_size) cover_size = min_cover_size;

                cover_char_width = cover_size * 2;
                cover_lines = cover_size;

                // 检查空间是否足够
                if (w - cover_char_width - 4 >= min_info_width) {
                    // 重新生成字符画（如果大小变化或缓存为空）
                    if (g_album_cover_size != cover_size || g_braille_art_buffer[0] == '\0') {
                        g_album_cover_size = cover_size;
                        if (generate_braille_art_dynamic(cover_path, BRAILLE_DEFAULT_THRESHOLD,
                                                          cover_char_width, cover_size,
                                                          g_braille_art_buffer, sizeof(g_braille_art_buffer)) != 0) {
                            g_braille_art_buffer[0] = '\0';
                        }
                    }

                    if (g_braille_art_buffer[0] != '\0') {
                        show_cover = 1;
                        cover_start_col = w - cover_char_width - 2;
                    }
                }
            }

            // 计算左侧两栏宽度（左栏=元数据，中栏=音频技术信息，右栏=专辑封面）
            int info_width = show_cover ? cover_start_col - 4 : w - 4;
            int col_width = info_width / 2;       // 左栏和中栏等宽
            int left_col_x = 2;                    // 左栏起始列
            int center_col_x = 2 + col_width;     // 中栏起始列

            // 截断左栏文本（元数据）
            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            char truncated_album[MAX_META_LEN];

            format_display_text(truncated_title, sizeof(truncated_title), t.title, col_width - 1, 0);
            format_display_text(truncated_artist, sizeof(truncated_artist), t.artist, col_width - 1, 0);
            format_display_text(truncated_album, sizeof(truncated_album), t.album, col_width - 1, 0);

            // 左栏：元数据（5行）
            mvwprintw(win_playlist, status_line + 1, left_col_x, "%s%s",
                      ui_text("状态：", "State: "), status_msg);
            mvwprintw(win_playlist, status_line + 2, left_col_x, "%s%s",
                      ui_text("循环：", "Loop: "), get_loop_mode_str());
            mvwprintw(win_playlist, status_line + 3, left_col_x, "%s%s",
                      ui_text("标题：", "Title: "), truncated_title);
            mvwprintw(win_playlist, status_line + 4, left_col_x, "%s%s",
                      ui_text("艺术家：", "Artist: "), truncated_artist);
            mvwprintw(win_playlist, status_line + 5, left_col_x, "%s%s",
                      ui_text("专辑：", "Album: "), truncated_album);

            // 中栏：音频技术信息（5行）
            char rate_str[32] = "--";
            char depth_str[32] = "--";
            char bitrate_str[32] = "--";
            char codec_display[32] = "--";

            if (g_audio_sample_rate > 0) {
                snprintf(rate_str, sizeof(rate_str), "%dHz", g_audio_sample_rate);
            }
            if (g_audio_bit_depth > 0) {
                snprintf(depth_str, sizeof(depth_str), "%dbit", g_audio_bit_depth);
            }
            if (g_audio_bit_rate > 0) {
                snprintf(bitrate_str, sizeof(bitrate_str), "%dkbps", g_audio_bit_rate / 1000);
            }
            if (g_audio_codec_name[0] != '\0') {
                // 转大写首字母以显示更友好的名称
                char upper[32];
                int ci = 0;
                for (; g_audio_codec_name[ci] && ci < (int)sizeof(upper) - 1; ci++) {
                    upper[ci] = (ci == 0) ? toupper((unsigned char)g_audio_codec_name[ci])
                                          : g_audio_codec_name[ci];
                }
                upper[ci] = '\0';
                snprintf(codec_display, sizeof(codec_display), "%s", upper);
            }

            mvwprintw(win_playlist, status_line + 1, center_col_x, "%s%s",
                      ui_text("采样率：", "Sample Rate: "), rate_str);
            mvwprintw(win_playlist, status_line + 2, center_col_x, "%s%s",
                      ui_text("位深：", "Bit Depth: "), depth_str);
            mvwprintw(win_playlist, status_line + 3, center_col_x, "%s%s",
                      ui_text("比特率：", "Bitrate: "), bitrate_str);
            mvwprintw(win_playlist, status_line + 4, center_col_x, "%s%s",
                      ui_text("编码：", "Codec: "), codec_display);
            // 第5行留空

            // 绘制专辑封面字符画
            if (show_cover) {
                char *lines[BRAILLE_MAX_SIZE];
                int line_count = get_braille_art_lines(g_braille_art_buffer, lines, BRAILLE_MAX_SIZE);

                // 从状态行开始绘制，覆盖5行（状态+循环+标题+艺术家+专辑）
                int start_row = status_line + 1;
                for (int i = 0; i < line_count && i < cover_lines; i++) {
                    if (start_row + i < h - 1) {
                        mvwprintw(win_playlist, start_row + i, cover_start_col, "%s", lines[i]);
                    }
                    free(lines[i]);
                }
            }
        } else {
             int col_width = (w - 4) / 2;
             int center_col_x = 2 + col_width;
             // 左栏：元数据
             mvwprintw(win_playlist, status_line + 1, 2, "%s%s",
                       ui_text("状态：", "State: "), status_msg);
             mvwprintw(win_playlist, status_line + 2, 2, "%s%s",
                       ui_text("循环：", "Loop: "), get_loop_mode_str());
             mvwprintw(win_playlist, status_line + 3, 2, "%s--", ui_text("标题：", "Title: "));
             mvwprintw(win_playlist, status_line + 4, 2, "%s--", ui_text("艺术家：", "Artist: "));
             mvwprintw(win_playlist, status_line + 5, 2, "%s--", ui_text("专辑：", "Album: "));
             // 中栏：技术信息占位
             mvwprintw(win_playlist, status_line + 1, center_col_x, "%s--", ui_text("采样率：", "Sample Rate: "));
             mvwprintw(win_playlist, status_line + 2, center_col_x, "%s--", ui_text("位深：", "Bit Depth: "));
             mvwprintw(win_playlist, status_line + 3, center_col_x, "%s--", ui_text("比特率：", "Bitrate: "));
             mvwprintw(win_playlist, status_line + 4, center_col_x, "%s--", ui_text("编码：", "Codec: "));
        }
    }
    wrefresh(win_playlist);
}

/**
 * 渲染控制栏按钮
 * 显示播放控制按钮并高亮当前选中的控件
 */
void render_controls() {
    // 检查窗口是否有效
    if (!win_controls) {
        return;
    }
    
    werase(win_controls);
    box(win_controls, 0, 0);
    
    const char *focus_hint = g_control_focus ? ui_text("[控件焦点]", "[Ctrl Focus]") : ui_text("[列表焦点]", "[List Focus]");
    const char *lyric_hint = g_lyric_cursor_mode ? ui_text("[D:退出定位]", "[D:Exit Seek]") : ui_text("[D:歌词定位]", "[D:Lyric Seek]");
    char controls_header[160];
    snprintf(controls_header, sizeof(controls_header), "%s %s %s %s %s",
             ui_text("控制区", "Controls"),
             ui_text("[空格:执行]", "[Space:Run]"),
             ui_text("[C:控件]", "[C:Ctrl]"),
             ui_text("[L:列表]", "[L:List]"),
             focus_hint);
    mvwprintw(win_controls, 0, 2, " %s %s", controls_header, lyric_hint);
    wbkgd(win_controls, COLOR_PAIR(COLOR_PAIR_CONTROLS));

    int h, w;
    getmaxyx(win_controls, h, w);
    
    // 绘制进度条（在控件上方）
    if (g_play_state != PLAY_STATE_STOPPED && g_total_duration > 0) {
        int progress_row = get_controls_progress_row(h);
        
        // 窗口尺寸校验
        if (h >= 5 && w >= 20) {
            // 安全获取位置
            int current_pos = g_current_position;
            if (current_pos < 0) current_pos = 0;
            if (current_pos > g_total_duration) current_pos = g_total_duration;
            
            // 计算进度百分比
            int progress_percent = (current_pos * 100) / g_total_duration;
            if (progress_percent > 100) progress_percent = 100;
            
            // 格式化时间显示 - 限制时间值范围
            int current_min = current_pos / 60;
            int current_sec = current_pos % 60;
            int total_min = g_total_duration / 60;
            int total_sec = g_total_duration % 60;
            
            current_min %= 100;
            total_min %= 100;
            
            // 检查是否选中进度条
            int is_progress_selected = (g_current_control_idx == CONTROL_IDX_PROGRESS && g_control_focus == 1);
            
            if (is_progress_selected) {
                wattron(win_controls, A_REVERSE | A_BOLD);
            }
            
            // 时间显示 - 固定格式确保不越界
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d", 
                     current_min, current_sec, total_min, total_sec);
            mvwprintw(win_controls, progress_row, 2, "%s", time_str);
            
            // 计算进度条安全宽度
            int time_width = 13;
            int percent_width = 4;
            int padding = 4;
            
            int progress_bar_width = w - time_width - percent_width - padding - 4;
            if (progress_bar_width < 10) progress_bar_width = 10;
            
            int progress_start_col = 2 + time_width + 1;
            
            // 绘制进度条边框
            mvwprintw(win_controls, progress_row, progress_start_col, "[");
            
            int filled_width = (progress_bar_width * progress_percent) / 100;
            if (filled_width > progress_bar_width) filled_width = progress_bar_width;
            
            // 使用循环绘制，避免格式化字符串溢出
            for (int i = 0; i < progress_bar_width && (progress_start_col + 1 + i) < w - 2; i++) {
                char c = '-';
                if (i < filled_width) c = '=';
                else if (i == filled_width && progress_percent < 100) c = '>';
                
                mvwaddch(win_controls, progress_row, progress_start_col + 1 + i, c);
            }
            
            mvwprintw(win_controls, progress_row, progress_start_col + 1 + progress_bar_width, "]");
            mvwprintw(win_controls, progress_row, progress_start_col + 2 + progress_bar_width, 
                      "%d%%", progress_percent);
            
            // 强制恢复边框
            mvwaddch(win_controls, progress_row, 0, ACS_VLINE);
            mvwaddch(win_controls, progress_row, w - 1, ACS_VLINE);
            
            if (is_progress_selected) {
                wattroff(win_controls, A_REVERSE | A_BOLD);
            }
        }
    }
    
    int button_start_row = get_controls_button_row(h);
    int current_row = button_start_row;

    int available_width = w - 2;
    int current_col = 0;
    int is_first_in_row = 1;

    for (int i = 0; i < CONTROL_COUNT-1; i++) {
        char display_label[32];
        build_control_label(i, display_label, sizeof(display_label));
        int len = utf8_str_width(display_label);
        int button_width = len + 4;

        if (is_first_in_row) {
            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT-1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;

                if (row_width + temp_button_width > available_width && row_button_count > 0) {
                    break;
                }
                row_width += temp_button_width;
                row_button_count++;
            }

            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
            is_first_in_row = 0;
        }

        if (!is_first_in_row && current_col + button_width > w - 1) {
            current_row++;

            if (current_row >= h - 2) {
                break;
            }

            int row_button_count = 0;
            int row_width = 0;
            for (int j = i; j < CONTROL_COUNT-1; j++) {
                char temp_label[32];
                build_control_label(j, temp_label, sizeof(temp_label));
                int temp_len = utf8_str_width(temp_label);
                int temp_button_width = temp_len + 4;

                if (row_width + temp_button_width > available_width && row_button_count > 0) {
                    break;
                }
                row_width += temp_button_width;
                row_button_count++;
            }

            current_col = (available_width - row_width) / 2 + 1;
            if (current_col < 1) current_col = 1;
        }

        if (i == g_current_control_idx && g_control_focus == 1) {
            wattron(win_controls, A_REVERSE | A_BOLD);
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
            wattroff(win_controls, A_REVERSE | A_BOLD);
        } else {
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
        }

        current_col += button_width;
    }

    render_controls_status_line();

    render_visualizer_with_album_cover();

    wrefresh(win_controls);
}

/**
 * 更新进度条（增量更新版本）
 * 直接计算百分比并只重绘进度条区域
 */
void update_progress_bar() {
    static uint64_t last_refresh_ms = 0;
    static int last_position = -1;
    static int last_duration = -1;
    static PlayState last_state = PLAY_STATE_STOPPED;

    // 前置条件检查
    if (g_play_state == PLAY_STATE_STOPPED || g_total_duration <= 0 || !win_controls || g_current_view != VIEW_MAIN) {
        return;
    }

    if (g_play_state == PLAY_STATE_PLAYING && progress_tracker_is_ready()) {
        int tracked_position = progress_tracker_get_position_seconds();
        if (tracked_position < 0) {
            tracked_position = 0;
        }
        if (g_total_duration > 0 && tracked_position > g_total_duration) {
            tracked_position = g_total_duration;
        }
        g_current_position = tracked_position;
    }
    
    int h, w;
    getmaxyx(win_controls, h, w);
    
    // 窗口尺寸校验
    if (h < 5 || w < 20) return;  // 最小有效尺寸
    
    // 安全获取位置
    int current_pos = g_current_position;
    if (current_pos < 0) current_pos = 0;
    if (current_pos > g_total_duration) current_pos = g_total_duration;

    uint64_t now_ms = get_ui_time_ms();
    int position_changed = (current_pos != last_position);
    int force_redraw = position_changed || g_total_duration != last_duration || g_play_state != last_state;
    if (!force_redraw && (now_ms - last_refresh_ms) < UI_PROGRESS_REFRESH_MS) {
        return;
    }
    
    // 计算进度百分比
    int progress_percent = (current_pos * 100) / g_total_duration;
    if (progress_percent > 100) progress_percent = 100;
    
    // 格式化时间显示 - 限制时间值范围，防止格式化溢出
    int current_min = current_pos / 60;
    int current_sec = current_pos % 60;
    int total_min = g_total_duration / 60;
    int total_sec = g_total_duration % 60;
    
    current_min %= 100;  // 限制最大显示 99:59
    total_min %= 100;
    
    // 检查是否选中进度条控件
    int is_progress_selected = (g_current_control_idx == CONTROL_IDX_PROGRESS && g_control_focus == 1);
    
    int progress_row = get_controls_progress_row(h);
    if (progress_row < 1 || progress_row >= h - 1) return;
    
    // 安全清除行 - 保留边框
    wmove(win_controls, progress_row, 1);
    for (int i = 1; i < w - 1 && i < 512; i++) {  // 限制最大清除宽度
        waddch(win_controls, ' ');
    }
    
    if (is_progress_selected) {
        wattron(win_controls, A_REVERSE | A_BOLD);
    }
    
    // 时间显示 - 固定格式确保不越界
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d", 
             current_min, current_sec, total_min, total_sec);
    mvwprintw(win_controls, progress_row, 2, "%s", time_str);
    
    // 计算进度条安全宽度
    int time_width = 13;  // "MM:SS / MM:SS" 的宽度
    int percent_width = 4;  // "100%" 的宽度
    int padding = 4;        // 左右括号和空格
    
    int progress_bar_width = w - time_width - percent_width - padding - 4;
    if (progress_bar_width < 10) progress_bar_width = 10;  // 最小进度条宽度
    
    int progress_start_col = 2 + time_width + 1;
    
    // 绘制进度条边框
    mvwprintw(win_controls, progress_row, progress_start_col, "[");
    
    int filled_width = (progress_bar_width * progress_percent) / 100;
    if (filled_width > progress_bar_width) filled_width = progress_bar_width;
    
    // 使用循环绘制，避免格式化字符串溢出
    for (int i = 0; i < progress_bar_width && (progress_start_col + 1 + i) < w - 2; i++) {
        char c = '-';
        if (i < filled_width) c = '=';
        else if (i == filled_width && progress_percent < 100) c = '>';
        
        mvwaddch(win_controls, progress_row, progress_start_col + 1 + i, c);
    }
    
    mvwprintw(win_controls, progress_row, progress_start_col + 1 + progress_bar_width, "]");
    mvwprintw(win_controls, progress_row, progress_start_col + 2 + progress_bar_width, 
              "%d%%", progress_percent);
    
    if (is_progress_selected) {
        wattroff(win_controls, A_REVERSE | A_BOLD);
    }
    
    // 恢复边框
    mvwaddch(win_controls, progress_row, 0, ACS_VLINE);
    mvwaddch(win_controls, progress_row, w - 1, ACS_VLINE);

    wrefresh(win_controls);

    if (position_changed) {
        update_lyrics_display();
    }

    // 频谱渲染统一节流到 150ms，通过脏掩码延迟渲染
    static uint64_t last_spectrum_refresh_ms = 0;
    if (g_current_view == VIEW_MAIN && (now_ms - last_spectrum_refresh_ms >= 150ULL)) {
        request_ui_refresh(UI_DIRTY_LYRICS);
        last_spectrum_refresh_ms = now_ms;
    }

    last_refresh_ms = now_ms;
    last_position = current_pos;
    last_duration = g_total_duration;
    last_state = g_play_state;
}

/**
 * 更新控制栏状态信息
 * 在控制栏底部显示临时消息
 */
void update_controls_status(const char *msg) {
    if (msg && msg[0]) {
        log_debug("ui", "Controls status: %s", msg);
    }
    pthread_mutex_lock(&g_ui_request_mutex);
    snprintf(g_controls_status_message, sizeof(g_controls_status_message), "%s", msg ? msg : "");
    g_controls_status_time = time(NULL);
    g_pending_ui_mask |= UI_DIRTY_CONTROLS;
    pthread_mutex_unlock(&g_ui_request_mutex);
}

static void seek_relative_seconds(int delta_seconds) {
    if (delta_seconds == 0 || g_play_state == PLAY_STATE_STOPPED || g_total_duration <= 0) {
        return;
    }

    int current_pos = g_current_position;
    int new_pos = current_pos + delta_seconds;
    if (new_pos < 0) {
        new_pos = 0;
    }
    if (new_pos > g_total_duration) {
        new_pos = g_total_duration;
    }

    if (new_pos != current_pos) {
        seek_audio((double)new_pos);
    }
}

static void prompt_folder_input(int append_mode) {
    // 空指针检查：避免win_controls未初始化时崩溃
    if (!win_controls) {
        return;
    }
    noecho();
    curs_set(1);

    const char *folder_prompt = append_mode
        ? ui_text("输入要追加的目录：", "Append folder: ")
        : ui_text("输入目录路径：", "Folder path: ");
    char input_path[MAX_PATH_LEN];
    prompt_text_input(win_controls, 4, 2, folder_prompt,
                      input_path, sizeof(input_path), 1, 0, 0);
    flushinp();
    
    noecho();
    curs_set(0);
    
    mvwprintw(win_controls, 4, 2, "                    "); 
    wclrtoeol(win_controls);
    wrefresh(win_controls);

    if (strlen(input_path) > 0) {
        char expanded_path[MAX_PATH_LEN];
        if (input_path[0] == '~') {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, input_path + 1);
            } else {
                snprintf(expanded_path, sizeof(expanded_path), "%s", input_path);
            }
        } else {
            snprintf(expanded_path, sizeof(expanded_path), "%s", input_path);
        }
        
        struct stat s;
        if (stat(expanded_path, &s) == 0 && S_ISDIR(s.st_mode)) {
            int had_existing_playlist = playlist_is_loaded() && playlist_count() > 0;
            if (!append_mode) {
                stop_audio();
            }
            int count = append_mode ? append_playlist(expanded_path) : load_playlist(expanded_path);
            if (count > 0) {
                add_dir_history_entry(expanded_path);
                
                if (g_app_config.remember_last_path) {
                    snprintf(g_app_config.last_opened_path, sizeof(g_app_config.last_opened_path), "%s", expanded_path);
                    save_config();
                }

                if (!append_mode || !had_existing_playlist) {
                    g_selected_index = 0;
                }

                if (append_mode && had_existing_playlist) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%s %d %s",
                             ui_text("已追加", "Appended"),
                             count,
                             ui_text("首歌曲", "tracks"));
                    update_controls_status(msg);
                } else {
                    update_controls_status(ui_text("目录加载成功", "Folder loaded"));
                }
                render_playlist_content();
            } else {
                if (append_mode) {
                    update_controls_status(ui_text("目录中没有新的音频文件", "No new audio files to append"));
                } else {
                    update_controls_status(ui_text("未找到音频文件", "No audio files found"));
                    reset_playlist_state();
                }
                render_playlist_content();
            }
        } else {
            update_controls_status(ui_text("路径无效", "Invalid path"));
            if (!append_mode) {
                stop_audio();
                reset_playlist_state();
            }
            render_playlist_content();
        }
    }
}

void prompt_open_folder() {
    log_info("ui", "Opening folder prompt triggered");
    prompt_folder_input(0);
}

static void prompt_append_folder() {
    prompt_folder_input(1);
}


/**
 * 主事件循环
 * 处理用户输入、焦点切换和功能调用
 */
void run_event_loop() {
    int ch;
    log_info("ui", "Event loop started");

    // 初始渲染
    render_playlist_content();
    render_controls(); // 初始绘制控件
    render_lyrics();
    
    // 降低空转刷新频率，实时进度由独立节流控制
    timeout(UI_INPUT_TIMEOUT_MS);
    
    int esc_pending = 0;
    uint64_t esc_pending_time = 0;
    #define ESC_TIMEOUT_MS 3000
    #define RAINBOW_UPDATE_MS 100
    
    static uint64_t last_rainbow_update_ms = 0;
    
    while (1) {
        reap_finished_playback_thread();
        process_pending_playback_action();
        process_pending_ui_refresh();
        media_session_tick();

        /* Check for SIGHUP-triggered config reload */
        if (g_config_reload_requested) {
            g_config_reload_requested = 0;
            reload_config();
        }

        ch = getch();

        // 每帧都更新进度条（当播放状态为播放或暂停时）
        if (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED) {
            update_progress_bar();
        }

        if (g_rainbow_mode_enabled) {
            uint64_t now = get_ui_time_ms();
            if (now - last_rainbow_update_ms >= RAINBOW_UPDATE_MS) {
                update_rainbow_colors();
                last_rainbow_update_ms = now;
            }
        }

        process_pending_ui_refresh();
        
        // 如果用户没有按键，继续循环以允许进度条和歌词更新
        if (ch == ERR) {
            if (esc_pending) {
                uint64_t now = get_ui_time_ms();
                if (now - esc_pending_time > ESC_TIMEOUT_MS) {
                    esc_pending = 0;
                }
            }
            continue;
        }

        // 处理 ESC 前缀（用于 ESC+数字 备用快捷键）
        if (esc_pending) {
            uint64_t now = get_ui_time_ms();
            if (now - esc_pending_time > ESC_TIMEOUT_MS) {
                esc_pending = 0;
            } else if (ch >= '1' && ch <= '9') {
                int fnum = ch - '1';
                handle_function_keys(KEY_F(1 + fnum));
                esc_pending = 0;
                continue;
            }
            // 超时或不是数字，重置 pending
            esc_pending = 0;
            // 如果是新的 ESC，重新开始
            if (ch == 27) {
                if (g_current_view != VIEW_MAIN) {
                    esc_pending = 0;  // 菜单视图中退出 pending 状态，让 ESC 继续传递到视图处理器
                } else {
                    esc_pending = 1;
                    esc_pending_time = get_ui_time_ms();
                    continue;
                }
            }
            // 否则 fall through 处理当前字符
        }
        
        if (ch == 27) {
            if (g_current_view != VIEW_MAIN) {
                /* 菜单视图中不拦截 ESC，让其落入下方 g_current_view != VIEW_MAIN 派发 */
            } else {
                esc_pending = 1;
                esc_pending_time = get_ui_time_ms();
                continue;
            }
        }

        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                log_debug("ui", "Mouse event at (%d,%d)", event.x, event.y);
                if (handle_menu_hint_bar_click(&event)) {
                    continue;
                }
                if (g_current_view == VIEW_MAIN) {
                    handle_main_view_mouse_event(&event);
                }
            }
            continue;
        }
        
        // 处理功能键（F1-F9）
        if (ch >= KEY_F(1) && ch <= KEY_F(9)) {
            log_debug("ui", "Function key F%d pressed", ch - KEY_F(0));
            handle_function_keys(ch);
            continue;
        }
        
        if (ch == 'q' || ch == 'Q') {
            log_info("ui", "Event loop exiting on 'q' key");
            break;
        }

        if (g_current_view == VIEW_MAIN) {
            if (ch == '+' || ch == '=') {
                adjust_volume(VOLUME_STEP_PERCENT);
                render_controls();
                continue;
            }
            if (ch == '-' || ch == '_') {
                adjust_volume(-VOLUME_STEP_PERCENT);
                render_controls();
                continue;
            }
        }
        
        // 窗口大小改变：先于视图派发处理，确保所有视图都能正确重置布局
        if (ch == KEY_RESIZE) {
            delwin(win_playlist);
            delwin(win_controls);
            delwin(win_lyrics);
            clear();
            create_layout();
            if (g_current_view != VIEW_MAIN) {
                rerender_active_view();
            } else {
                request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
            }
            continue;
        }

        // 新增：如果在菜单视图模式下，优先处理菜单输入
        if (g_current_view != VIEW_MAIN) {
            handle_menu_input(ch);
            continue;
        }
        
        if (!playlist_is_loaded() && g_control_focus == 0) {
            // 未加载文件夹且焦点在列表时，支持打开或追加目录
            if (ch == 'O' || ch == 'o') {
                prompt_open_folder();
                render_playlist_content();
                continue;
            }
            if (ch == 'I' || ch == 'i') {
                prompt_append_folder();
                render_playlist_content();
                continue;
            }
        }

        // 焦点切换
        if (ch == 'C' || ch == 'c') {
            // 切换到控制区焦点 (需求要求大写 C，这里兼容小写以防误触，也可严格限制)
            // 严格遵循需求：按下大写 C
            if (ch == 'C') {
                g_control_focus = 1;
                g_current_control_idx = 1; // 默认选中播放/暂停
                render_playlist_content(); // 重绘列表以取消高亮
                render_controls();         // 重绘控件以高亮
                continue;
            }
        }
        
        if (ch == 'L' || ch == 'l') {
            // 切换到列表区焦点 (需求要求大写 L)
            if (ch == 'L') {
                g_control_focus = 0;
                render_controls();         // 重绘控件以取消高亮
                render_playlist_content(); // 重绘列表以高亮选中项
                continue;
            }
        }

        // 音乐库浏览模式切换 (M键)
        if (ch == 'M') {
            library_browser_toggle();
            render_playlist_content();
            render_controls();
            continue;
        }

        // 歌词光标模式切换 (D键)
        if (ch == 'D' || ch == 'd') {
            // 严格遵循需求：按下大写D切换
            if (ch == 'D' && g_current_view == VIEW_MAIN) {
                pthread_mutex_lock(&g_lyrics.lock);
                if (g_lyrics.has_lyrics && g_lyrics.count > 0 && g_lyrics.current_index >= 0) {
                    g_lyric_cursor_mode = !g_lyric_cursor_mode;
                    if (g_lyric_cursor_mode) {
                        // 激活时，初始化光标位置为当前播放歌词行
                        g_lyrics.cursor_index = g_lyrics.current_index;
                        g_lyric_cursor_index = g_lyrics.cursor_index;
                        update_controls_status(ui_text("已进入歌词定位模式", "Lyric seek enabled"));
                    } else {
                        update_controls_status(ui_text("已退出歌词定位模式", "Lyric seek disabled"));
                    }
                    pthread_mutex_unlock(&g_lyrics.lock);
                    render_controls();
                    render_lyrics();
                    continue;
                } else {
                    pthread_mutex_unlock(&g_lyrics.lock);
                    if (!g_lyrics.has_lyrics) {
                        update_controls_status(ui_text("当前没有可定位的歌词", "No lyric position available"));
                    }
                    continue;
                }
            }
        }

        if (g_control_focus == 1) {
            // === 控制区模式 ===
            switch (ch) {
                case KEY_UP:
                    if (g_current_control_idx == CONTROL_IDX_VOLUME) {
                        adjust_volume(VOLUME_STEP_PERCENT);
                        render_controls();
                    }
                    break;
                case KEY_DOWN:
                    if (g_current_control_idx == CONTROL_IDX_VOLUME) {
                        adjust_volume(-VOLUME_STEP_PERCENT);
                        render_controls();
                    }
                    break;
                case KEY_LEFT:
                    g_current_control_idx--;
                    if (g_current_control_idx < 0) g_current_control_idx = CONTROL_COUNT - 1;
                    render_controls();
                    break;
                case KEY_RIGHT:
                    g_current_control_idx++;
                    if (g_current_control_idx >= CONTROL_COUNT) g_current_control_idx = 0;
                    render_controls();
                    break;
                case ',':
                    seek_relative_seconds(-SEEK_STEP_SECONDS);
                    break;

                case '.':
                    seek_relative_seconds(SEEK_STEP_SECONDS);
                    break;
                case ' ':
                    activate_current_control();
                    render_playlist_content();
                    render_controls();
                    break;
            }
        } else {
            if (playlist_is_loaded()) {
                switch (ch) {
                    case KEY_UP:
                        if (g_lyric_cursor_mode && g_lyrics.has_lyrics) {
                            // 歌词编辑模式下，方向键控制歌词光标
                            pthread_mutex_lock(&g_lyrics.lock);
                            if (g_lyrics.cursor_index > 0) {
                                g_lyrics.cursor_index--;
                                g_lyric_cursor_index = g_lyrics.cursor_index;
                                double target_timestamp = g_lyrics.lines[g_lyrics.cursor_index].timestamp;
                                pthread_mutex_unlock(&g_lyrics.lock);
                                render_lyrics();
                                if (g_play_state != PLAY_STATE_STOPPED && progress_tracker_is_ready()) {
                                    seek_audio(target_timestamp);
                                }
                            } else {
                                pthread_mutex_unlock(&g_lyrics.lock);
                            }
                        } else if (g_search_state.active || g_search_state.in_progress) {
                            // 搜索模式下，方向键控制搜索结果选中项
                            if (g_search_state.selected_index > 0) {
                                g_search_state.selected_index--;
                                render_playlist_content();
                            }
                        } else {
                            // 正常模式下，方向键控制播放列表选中项
                            if (g_selected_index > 0) {
                                g_selected_index--;
                                render_playlist_content();
                            }
                        }
                        break;
                    case KEY_DOWN:
                        if (g_lyric_cursor_mode && g_lyrics.has_lyrics) {
                            // 歌词编辑模式下，方向键控制歌词光标
                            pthread_mutex_lock(&g_lyrics.lock);
                            if (g_lyrics.cursor_index < g_lyrics.count - 1) {
                                g_lyrics.cursor_index++;
                                g_lyric_cursor_index = g_lyrics.cursor_index;
                                double target_timestamp = g_lyrics.lines[g_lyrics.cursor_index].timestamp;
                                pthread_mutex_unlock(&g_lyrics.lock);
                                render_lyrics();
                                if (g_play_state != PLAY_STATE_STOPPED && progress_tracker_is_ready()) {
                                    seek_audio(target_timestamp);
                                }
                            } else {
                                pthread_mutex_unlock(&g_lyrics.lock);
                            }
                        } else if (g_search_state.active || g_search_state.in_progress) {
                            // 搜索模式下，方向键控制搜索结果选中项
                            if (g_search_state.selected_index < g_search_state.result_count - 1) {
                                g_search_state.selected_index++;
                                render_playlist_content();
                            }
                        } else {
                            // 正常模式下，方向键控制播放列表选中项
                            if (g_selected_index < playlist_count() - 1) {
                                g_selected_index++;
                                render_playlist_content();
                            }
                        }
                        break;
                    case ' ':
                    case 10:
                        if (g_search_state.in_progress) {
                            update_controls_status(ui_text("搜索中，请稍候...", "Searching, please wait..."));
                            break;
                        }
                        if (g_search_state.active && g_search_state.result_count > 0) {
                            int original_index = g_search_state.result_indices[g_search_state.selected_index];
                            play_audio(original_index);
                            g_search_state.active = 0;
                            g_selected_index = g_sort_state.active ? 0 : original_index;
                            render_playlist_content();
                        } else {
                            int play_idx = g_selected_index;
                            if (g_sort_state.active) {
                                play_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            play_audio(play_idx);
                        }
                        break;
                    case 'O':
                    case 'o':
                        prompt_open_folder();
                        render_playlist_content();
                        break;
                    case 'I':
                    case 'i':
                        prompt_append_folder();
                        render_playlist_content();
                        break;
                    case 'f':
                    case 'F':
                        if (playlist_count() > 0) {
                            Track t;
                            int track_idx = g_selected_index;
                            if (g_search_state.active) {
                                pthread_mutex_lock(&g_search_mutex);
                                track_idx = g_search_state.result_indices[g_search_state.selected_index];
                                pthread_mutex_unlock(&g_search_mutex);
                            } else if (g_sort_state.active) {
                                track_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            get_track_metadata(track_idx, &t);
                            int result = add_to_favorites(&t);
                            if (result == 0) {
                                update_controls_status(ui_text("已添加到收藏", "Added to favorites"));
                            } else {
                                update_controls_status(ui_text("收藏已存在或已满", "Favorite exists or list is full"));
                            }
                        }
                        break;
                    case 's':
                    case 'S':
                        if (ch == 'S' && g_current_view == VIEW_MAIN) {
                            search_prompt();
                            continue;
                        }
                        break;
                    case 27:
                        if (g_search_state.active || g_search_state.in_progress) {
                            search_async_cancel();
                            pthread_mutex_lock(&g_search_mutex);
                            g_search_state.active = 0;
                            g_search_state.in_progress = 0;
                            pthread_mutex_unlock(&g_search_mutex);
                            render_playlist_content();
                            update_controls_status(ui_text("搜索已取消", "Search cancelled"));
                            continue;
                        }
                        break;
                    case 'a':
                    case 'A':
                        if (playlist_count() > 0 && g_playlist_manager.count > 0) {
                            Track t;
                            int track_idx = g_selected_index;
                            if (g_search_state.active) {
                                pthread_mutex_lock(&g_search_mutex);
                                track_idx = g_search_state.result_indices[g_search_state.selected_index];
                                pthread_mutex_unlock(&g_search_mutex);
                            } else if (g_sort_state.active) {
                                track_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            get_track_metadata(track_idx, &t);
                            Track *tp = &t;

                            int max_y, max_x;
                            getmaxyx(stdscr, max_y, max_x);

                            WINDOW *win_win = newwin(max_y - 4, max_x - 4, 2, 2);
                            box(win_win, 0, 0);
                            mvwprintw(win_win, 0, 2, "%s", ui_text(" 选择歌单 ", " Select Playlist "));
                            wbkgd(win_win, COLOR_PAIR(COLOR_PAIR_PLAYLIST));

                            int start_y = 2;
                            int visible_lines = max_y - 8;
                            int selected = 0;
                            int offset = 0;

                             while (1) {
                                for (int i = 0; i < visible_lines && (offset + i) < g_playlist_manager.count; i++) {
                                    int idx = offset + i;
                                    UserPlaylist *pl = &g_playlist_manager.playlists[idx];
                                    if (idx == selected) {
                                        wattron(win_win, A_REVERSE);
                                        char playlist_name[MAX_PLAYLIST_NAME_LEN + 8];
                                        format_display_text(playlist_name, sizeof(playlist_name), pl->name, 30, 1);
                                        mvwprintw(win_win, start_y + i, 2,
                                                  use_english_ui() ? " %s (%d tracks)" : " %s (%d 首)",
                                                  playlist_name, pl->track_count);
                                        wattroff(win_win, A_REVERSE);
                                    } else {
                                        char playlist_name[MAX_PLAYLIST_NAME_LEN + 8];
                                        format_display_text(playlist_name, sizeof(playlist_name), pl->name, 30, 1);
                                        mvwprintw(win_win, start_y + i, 2,
                                                  use_english_ui() ? " %s (%d tracks)" : " %s (%d 首)",
                                                  playlist_name, pl->track_count);
                                    }
                                }

                                mvwprintw(win_win, max_y - 6, 2, "%s",
                                          ui_text("↑/↓: 选择 | ENTER: 确认 | ESC: 取消",
                                                  "Up/Down: Select | Enter: OK | Esc: Cancel"));
                                wrefresh(win_win);

                                wtimeout(win_win, UI_INPUT_TIMEOUT_MS);
                                int c = wgetch(win_win);
                                if (c == ERR) {
                                    media_session_tick();
                                    continue;
                                } else if (c == 27) {
                                    break;
                                } else if (c == KEY_UP) {
                                    if (selected > 0) {
                                        selected--;
                                        if (selected < offset) {
                                            offset = selected;
                                        }
                                    }
                                } else if (c == KEY_DOWN) {
                                    if (selected < g_playlist_manager.count - 1) {
                                        selected++;
                                        if (selected >= offset + visible_lines) {
                                            offset = selected - visible_lines + 1;
                                        }
                                    }
                                } else if (c == 10 || c == ' ') {
                                    int result = add_track_to_playlist(selected, tp);
                                      if (result == 0) {
                                          update_controls_status(ui_text("已添加到歌单", "Added to playlist"));
                                      } else if (result == -3) {
                                          update_controls_status(ui_text("歌单已满", "Playlist is full"));
                                      } else {
                                          update_controls_status(ui_text("歌曲已在歌单中", "Track already in playlist"));
                                      }
                                    break;
                                }
                             }

                             delwin(win_win);
                             request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
                        } else if (g_playlist_manager.count == 0) {
                            update_controls_status(ui_text("还没有歌单，请先到 F4 新建",
                                                           "No playlist yet. Create one in F4"));
                        }
                        break;
                }
            }
        }
    }
}

/**
 * 清理ncurses资源
 * 释放窗口并结束ncurses模式
 */
void cleanup() {
    log_info("ui", "cleanup() called");

    // 取消并等待搜索线程
    search_async_cancel();
    pthread_mutex_lock(&g_search_mutex);
    if (g_search_state.in_progress) {
        pthread_t old_thread = g_search_state.thread;
        pthread_mutex_unlock(&g_search_mutex);
        pthread_join(old_thread, NULL);
    } else {
        pthread_mutex_unlock(&g_search_mutex);
    }

    persist_playback_session_state();
    stop_audio();
    wait_for_playback_thread_shutdown();
    media_session_shutdown();
    library_shutdown();

    if (win_playlist) {
        delwin(win_playlist);
        win_playlist = NULL;
    }
    if (win_controls) {
        delwin(win_controls);
        win_controls = NULL;
    }
    if (win_lyrics) {
        delwin(win_lyrics);
        win_lyrics = NULL;
    }
    endwin(); // 结束 ncurses 模式
    audio_backend_shutdown();
    remote_cleanup();
}

static void activate_current_control(void) {
    switch (g_current_control_idx) {
        case CONTROL_IDX_PREV:
            prev_track();
            break;
        case CONTROL_IDX_PLAY_PAUSE:
            {
                PlayState current_state = g_play_state;
                int is_thread_running = g_play_thread_running;

                if (current_state == PLAY_STATE_PLAYING && is_thread_running) {
                    pause_audio();
                } else if (current_state == PLAY_STATE_PAUSED && is_thread_running) {
                    resume_audio();
                } else if (current_state == PLAY_STATE_STOPPED) {
                    int playlist_total = playlist_count();
                    if (playlist_is_loaded() && playlist_total > 0) {
                        int target_index = (g_current_play_index >= 0)
                            ? g_current_play_index
                            : g_selected_index;
                        if (g_sort_state.active && g_current_play_index < 0) {
                            target_index = g_sort_state.sorted_indices[g_selected_index];
                        }
                        if (target_index >= 0 && target_index < playlist_total) {
                            play_audio(target_index);
                        }
                    }
                }
            }
            break;
        case CONTROL_IDX_NEXT:
            next_track();
            break;
        case CONTROL_IDX_STOP:
            stop_audio();
            break;
        case CONTROL_IDX_LOOP:
            toggle_loop_mode();
            break;
        case CONTROL_IDX_SPEED:
            toggle_playback_speed();
            break;
        case CONTROL_IDX_VOLUME:
            adjust_volume(10);
            break;
        case CONTROL_IDX_PROGRESS:
            break;
    }
}

static uint64_t get_current_track_duration_ms(void) {
    return (uint64_t)g_total_duration * 1000;
}

static void seek_to_position_ms(uint64_t pos_ms) {
    double pos_seconds = (double)pos_ms / 1000.0;
    seek_audio(pos_seconds);
}

static int get_corner_spectrum_height(int h) {
    if (h >= 28) {
        return 5;
    }
    if (h >= 22) {
        return 4;
    }
    if (h >= 17) {
        return 3;
    }
    if (h >= 13) {
        return 2;
    }
    return 1;
}

static int calculate_lyrics_content_top(int h, int w) {
    int spectrum_height = get_corner_spectrum_height(h);
    int graph_top = 1;
    int graph_bottom = graph_top + spectrum_height - 1;
    if (graph_bottom >= h - 2 || w < 16) {
        return 1;
    }
    return graph_bottom + 1;
}

/**
 * 获取播放列表当前的滚动偏移量
 * @return 滚动偏移（第一行可见歌曲的索引）
 */
static int get_playlist_scroll_offset(void) {
    if (!win_playlist) {
        return 0;
    }
    
    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;
    
    int content_height = h - 2;  // 减去边框
    int visible_lines = content_height - 6;  // 预留 6 行给底部状态栏(1分隔线+5信息行)

    if (visible_lines <= 0) {
        return 0;
    }

    if (g_search_state.active || g_search_state.in_progress) {
        int offset = 0;
        if (g_search_state.selected_index >= visible_lines) {
            offset = g_search_state.selected_index - visible_lines + 1;
        }
        return offset;
    } else {
        int offset = 0;
        if (g_selected_index >= visible_lines) {
            offset = g_selected_index - visible_lines + 1;
        }
        return offset;
    }
}

void update_rainbow_colors(void) {
    if (!g_rainbow_mode_enabled || !has_colors()) {
        return;
    }

    static int rainbow_colors[7] = {
        COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
        COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
    };

    g_rainbow_color_offset = (g_rainbow_color_offset + 1) % 7;

    g_app_config.theme.border_fg = rainbow_colors[(0 + g_rainbow_color_offset) % 7];
    g_app_config.theme.playlist_fg = rainbow_colors[(1 + g_rainbow_color_offset) % 7];
    g_app_config.theme.controls_fg = rainbow_colors[(2 + g_rainbow_color_offset) % 7];
    g_app_config.theme.lyrics_fg = rainbow_colors[(3 + g_rainbow_color_offset) % 7];
    g_app_config.theme.sidebar_fg = rainbow_colors[(4 + g_rainbow_color_offset) % 7];
    g_app_config.theme.highlight_fg = rainbow_colors[(5 + g_rainbow_color_offset) % 7];
    g_app_config.theme.highlight_bg = rainbow_colors[(6 + g_rainbow_color_offset) % 7];

    apply_color_theme();

    if (g_current_view == VIEW_MAIN) {
        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    }
}

void check_konami_input(int ch) {
    uint64_t now = get_ui_time_ms();
    
    if (g_konami_input_pos > 0 && (now - g_konami_last_time) > 3000) {
        g_konami_input_pos = 0;
    }
    
    int expected = konami_expected[g_konami_input_pos];
    int matched = 0;
    
    if (ch == expected) {
        matched = 1;
    } else if ((expected == 'B' && (ch == 'b' || ch == 'B')) ||
               (expected == 'A' && (ch == 'a' || ch == 'A'))) {
        matched = 1;
    }
    
    if (matched) {
        g_konami_input_pos++;
        g_konami_last_time = now;
        if (g_konami_input_pos == KONAMI_SEQ_LENGTH) {
            toggle_rainbow_mode();
            g_konami_input_pos = 0;
        }
    } else {
        g_konami_input_pos = 0;
        if (ch == konami_expected[0]) {
            g_konami_input_pos = 1;
            g_konami_last_time = now;
        }
    }
}

void toggle_rainbow_mode(void) {
    if (!has_colors()) {
        return;
    }
    
    if (g_rainbow_mode_enabled) {
        memcpy(&g_app_config.theme, &g_saved_theme, sizeof(ColorTheme));
        apply_color_theme();
        g_rainbow_mode_enabled = 0;
        g_rainbow_color_offset = 0;
        update_controls_status(use_english_ui() ? "Rainbow mode disabled" : "彩虹模式已关闭");
    } else {
        memcpy(&g_saved_theme, &g_app_config.theme, sizeof(ColorTheme));
        
        g_app_config.theme.border_bg = COLOR_BLACK;
        g_app_config.theme.playlist_bg = COLOR_BLACK;
        g_app_config.theme.controls_bg = COLOR_BLACK;
        g_app_config.theme.lyrics_bg = COLOR_BLACK;
        g_app_config.theme.sidebar_bg = COLOR_BLACK;
        g_app_config.theme.highlight_bg = COLOR_BLACK;
        
        g_rainbow_color_offset = 0;
        update_rainbow_colors();
        
        g_rainbow_mode_enabled = 1;
        update_controls_status(use_english_ui() ? "Konami code! Rainbow mode enabled" : "康娜米！彩虹模式已启用");
    }
    
    if (g_current_view == VIEW_MAIN) {
        create_layout();
        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    }
}
