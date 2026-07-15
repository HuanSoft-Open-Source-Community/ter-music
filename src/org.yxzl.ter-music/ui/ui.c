/**
 * @file ui.c
 * @brief UI 框架层 — 初始化、事件循环、资源清理
 *
 * 各渲染模块已拆分到独立文件：
 *   - layout.c        : 窗口布局创建
 *   - visualizer.c    : 频谱可视化/专辑封面
 *   - controls.c      : 控制栏按钮/激活
 *   - progress_ui.c   : 进度条更新
 *   - playlist_render.c : 播放列表内容渲染
 *   - mouse.c         : 鼠标事件处理
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/dialog.h"
#include "audio/audio.h"
#include "playlist/playlist.h"
#include "config/config.h"
#include "media/session.h"
#include "ui/lyrics.h"
#include "ui/menus.h"
#include "library/library.h"
#include "library/browser/browser.h"
#include "ui/braille/braille_art.h"
#include "search/search.h"
#include "ui/scrollbar.h"
#include "remote/remote.h"
#include "logger/logger.h"
#include "audio/progress/progress.h"
#include "audio/play_queue.h"
#include "ui/menu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

/* ── Forward declarations from split modules ── */
extern void render_playlist_content(void);
extern void render_controls(void);
extern void create_layout(void);
extern void render_visualizer_with_album_cover(void);
extern void seek_relative_seconds(int delta_seconds);
extern int  handle_main_view_mouse_event(const MEVENT *event);
extern void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad);
extern void sanitize_ascii_text(char *dest, size_t dest_size, const char *src);
extern int g_playlist_tab_mode;
extern int g_queue_selected_index;
extern int g_saved_browser_index;

/* audio backend shutdown (defined in audio.c) */
void audio_backend_shutdown(void);

uint64_t get_ui_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ============================================================
 * Global window variables
 * ============================================================ */

WINDOW *win_playlist;
WINDOW *win_controls;
WINDOW *win_lyrics;

/* ── Control count ── */
int g_control_count = CONTROL_COUNT;

/* ── Lyric cursor mode ── */
int g_lyric_cursor_mode = 0;
int g_lyric_cursor_index = -1;

/* ── UI refresh state ── */
static pthread_mutex_t g_ui_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_pending_ui_mask = 0;

/* ── ASCII fallback flag ── */
int g_ascii_fallback_ui = 0;

/* ── Rainbow / Konami state ── */
int g_rainbow_mode_enabled = 0;
ColorTheme g_saved_theme;
int g_konami_input_pos = 0;
uint64_t g_konami_last_time = 0;
int g_rainbow_color_offset = 0;

/* ── Braille album art buffer ── */
char g_braille_art_buffer[8192] = "";
int g_album_cover_size = 0;

/* ── Konami sequence ── */
#define KONAMI_SEQ_LENGTH 12
static const int konami_expected[KONAMI_SEQ_LENGTH] = {
    KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN,
    KEY_LEFT, KEY_RIGHT, KEY_LEFT, KEY_RIGHT,
    'B', 'A', 'B', 'A'
};

/* ── Const values ── */
#define SEEK_STEP_SECONDS 5
#define VOLUME_STEP_PERCENT 5
#define UI_INPUT_TIMEOUT_MS 40
#define UI_PROGRESS_REFRESH_MS 80
#define ESC_TIMEOUT_MS 3000
#define RAINBOW_UPDATE_MS 100

/* ============================================================
 * Locale / UTF-8 helpers
 * ============================================================ */

static int locale_uses_utf8(void)
{
    const char *codeset = nl_langinfo(CODESET);
    if (!codeset) return 0;
    return strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0;
}

static void ensure_utf8_locale(void)
{
    const char *fallbacks[] = {"C.UTF-8", "zh_CN.UTF-8", "en_US.UTF-8", NULL};
    setlocale(LC_ALL, "");
    if (locale_uses_utf8()) return;
    for (int i = 0; fallbacks[i] != NULL; i++) {
        if (setlocale(LC_ALL, fallbacks[i]) && locale_uses_utf8()) return;
    }

    setlocale(LC_CTYPE, "");
}

static int locale_supports_cjk_width(void)
{
    static const char *sample = "中";
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t wc = 0;
    size_t len = mbrtowc(&wc, sample, strlen(sample), &state);
    if (len == (size_t)-1 || len == (size_t)-2 || len == 0) return 0;
    return wcwidth(wc) == 2;
}

static int terminal_needs_ascii_fallback(void)
{
    const char *term = getenv("TERM");
    const char *force_ascii = getenv("TER_MUSIC_FORCE_ASCII");
    const char *force_utf8  = getenv("TER_MUSIC_FORCE_UTF8");

    if (force_utf8  && strcmp(force_utf8,  "1") == 0) return 0;
    if (force_ascii && strcmp(force_ascii, "1") == 0) return 1;
    if (!locale_uses_utf8()) return 1;
    if (MB_CUR_MAX <= 1 || !locale_supports_cjk_width()) return 1;
    if (!term || term[0] == '\0') return 0;
    if (strcmp(term, "dumb") == 0) return 1;
    return 0;
}

/* ============================================================
 * UTF-8 next character helper
 * ============================================================ */

size_t utf8_next_char(const char *src, wchar_t *wc_out, int *width_out)
{
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t wc = 0;
    size_t char_len = mbrtowc(&wc, src, MB_CUR_MAX, &state);
    if (char_len == (size_t)-1 || char_len == (size_t)-2) {
        wc = (unsigned char)src[0];
        char_len = 1;
    } else if (char_len == 0) {
        if (wc_out)   *wc_out = L'\0';
        if (width_out) *width_out = 0;
        return 0;
    }

    int width = wcwidth(wc);
    if (width < 0) width = 1;
    if (wc_out)   *wc_out = wc;
    if (width_out) *width_out = width;
    return char_len;
}

/* ============================================================
 * ui_text: bilingual text helper
 * ============================================================ */

const char *ui_text(const char *key, const char *unused)
{
    (void)unused;
    return i18n_get(key);
}

/* ============================================================
 * Refresh system
 * ============================================================ */

void request_ui_refresh(int dirty_mask)
{
    if (dirty_mask == 0) return;
    pthread_mutex_lock(&g_ui_request_mutex);
    g_pending_ui_mask |= dirty_mask;
    pthread_mutex_unlock(&g_ui_request_mutex);
}

void process_pending_ui_refresh(void)
{
    int dirty_mask = 0;
    pthread_mutex_lock(&g_ui_request_mutex);
    dirty_mask = g_pending_ui_mask;
    g_pending_ui_mask = 0;
    pthread_mutex_unlock(&g_ui_request_mutex);

    if (dirty_mask == 0 || g_current_view != VIEW_MAIN) return;

    if (dirty_mask & UI_DIRTY_PLAYLIST) render_playlist_content();
    if (dirty_mask & UI_DIRTY_CONTROLS) render_controls();
    if (dirty_mask & UI_DIRTY_LYRICS)   render_lyrics();
}

/* ============================================================
 * ncurses initialization
 * ============================================================ */

void init_ncurses(void)
{
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
        use_default_colors();  /* enable -1 (COLOR_DEFAULT) for transparent bg support */
        assume_default_colors(-1, -1);  /* pair 0 also uses terminal defaults */
        init_pair(COLOR_PAIR_BORDER,    COLOR_CYAN,   -1);  /* transparent bg */
        init_pair(COLOR_PAIR_PLAYLIST,  COLOR_WHITE,  -1);
        init_pair(COLOR_PAIR_CONTROLS,  COLOR_YELLOW, -1);
        init_pair(COLOR_PAIR_LYRICS,    COLOR_GREEN,  -1);
        init_pair(COLOR_PAIR_SIDEBAR,   COLOR_CYAN,   -1);
    init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK,  COLOR_WHITE);
    }

        bkgdset(' ' | COLOR_PAIR(0));  /* transparent bg via border pair */
        clear();
}

/* ============================================================
 * Event loop
 * ============================================================ */

void run_event_loop(void)
{
    int ch;
    log_info("ui", "Event loop started");

    render_playlist_content();
    render_controls();
    render_lyrics();

    timeout(UI_INPUT_TIMEOUT_MS);

    int esc_pending = 0;
    uint64_t esc_pending_time = 0;
    static uint64_t last_rainbow_update_ms = 0;

    while (1) {
        reap_finished_playback_thread();
        process_pending_playback_action();
        process_pending_ui_refresh();
        media_session_tick();

        if (g_config_reload_requested) {
            g_config_reload_requested = 0;
            reload_config();
        }

        ch = getch();

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

        if (ch == ERR) {
            if (esc_pending) {
                uint64_t now = get_ui_time_ms();
                if (now - esc_pending_time > ESC_TIMEOUT_MS) esc_pending = 0;
            }
            continue;
        }

        // ESC prefix handling
        if (esc_pending) {
            uint64_t now = get_ui_time_ms();
            if (now - esc_pending_time > ESC_TIMEOUT_MS) {
                esc_pending = 0;
            } else if (ch >= '1' && ch <= '9') {
                handle_function_keys(KEY_F(1 + (ch - '1')));
                esc_pending = 0;
                continue;
            }
            esc_pending = 0;
            if (ch == 27) {
                if (g_current_view != VIEW_MAIN) {
                    esc_pending = 0;
                } else {
                    esc_pending = 1;
                    esc_pending_time = get_ui_time_ms();
                    continue;
                }
            }
        }

        if (ch == 27) {
            if (g_current_view == VIEW_MAIN) {
                /* When a popup is active, ESC must dismiss it immediately.
                 * Do NOT enter the esc_pending chain so the byte falls through
                 * to handle_popup_input() at line ~461. */
                if (g_popup.active && g_control_focus == 1) {
                    /* fall through — popup handler will catch the ESC */
                } else {
                    esc_pending = 1;
                    esc_pending_time = get_ui_time_ms();
                    continue;
                }
            }
        }

        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                log_debug("ui", "Mouse event at (%d,%d)", event.x, event.y);
                if (handle_menu_hint_bar_click(&event)) continue;
                if (g_current_view == VIEW_MAIN) {
                    handle_main_view_mouse_event(&event);
                }
            }
            continue;
        }

        if (ch >= KEY_F(1) && ch <= KEY_F(9)) {
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

        if (g_current_view != VIEW_MAIN) {
            handle_menu_input(ch);
            continue;
        }

        // Main view input handling
        if (!playlist_is_loaded() && g_control_focus == 0) {
            if (ch == 'O' || ch == 'o') { prompt_open_folder(); render_playlist_content(); continue; }
            if (ch == 'I' || ch == 'i') { prompt_append_folder(); render_playlist_content(); continue; }
        }

        if (ch == 'C') {
            g_control_focus = 1;
            g_current_control_idx = 1;
            render_playlist_content();
            render_controls();
            continue;
        }

        if (ch == 'L') {
            g_control_focus = 0;
            render_controls();
            render_playlist_content();
            continue;
        }

        if (ch == 'M') {
            library_browser_toggle();
            render_playlist_content();
            render_controls();
            continue;
        }

        if (ch == 12 && g_current_view == VIEW_MAIN) {  /* Ctrl+L */
            pthread_mutex_lock(&g_lyrics.lock);
            if (g_lyrics.has_lyrics && g_lyrics.count > 0 && g_lyrics.current_index >= 0) {
                g_lyric_cursor_mode = !g_lyric_cursor_mode;
                if (g_lyric_cursor_mode) {
                    g_lyrics.cursor_index = g_lyrics.current_index;
                    g_lyric_cursor_index = g_lyrics.cursor_index;
                    update_controls_status(i18n_get("lyrics.seek_enabled"));
                } else {
                    update_controls_status(i18n_get("lyrics.seek_disabled"));
                }
                pthread_mutex_unlock(&g_lyrics.lock);
                render_controls();
                render_lyrics();
                continue;
            } else {
                pthread_mutex_unlock(&g_lyrics.lock);
                if (!g_lyrics.has_lyrics)
                    update_controls_status(i18n_get("lyrics.no_position"));
                continue;
            }
        }

        // Control focus mode
        if (g_control_focus == 1) {
            /* Intercept popup input first */
            if (g_popup.active && handle_popup_input(ch))
                continue;

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
                case ',': seek_relative_seconds(-SEEK_STEP_SECONDS); break;
                case '.': seek_relative_seconds(SEEK_STEP_SECONDS); break;
                case ' ':
                    activate_current_control();
                    render_playlist_content();
                    render_controls();
                    break;
            }
        } else {
            // List focus mode
            if (!playlist_is_loaded()) continue;

            switch (ch) {
                case KEY_UP:
                    if (g_lyric_cursor_mode && g_lyrics.has_lyrics) {
                        pthread_mutex_lock(&g_lyrics.lock);
                        if (g_lyrics.cursor_index > 0) {
                            g_lyrics.cursor_index--;
                            g_lyric_cursor_index = g_lyrics.cursor_index;
                            double t = g_lyrics.lines[g_lyrics.cursor_index].timestamp;
                            pthread_mutex_unlock(&g_lyrics.lock);
                            render_lyrics();
                            if (g_play_state != PLAY_STATE_STOPPED && progress_tracker_is_ready())
                                seek_audio(t);
                        } else {
                            pthread_mutex_unlock(&g_lyrics.lock);
                        }
                    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                        if (g_queue_selected_index > 0) { g_queue_selected_index--; render_playlist_content(); }
                    } else if (g_search_state.active || g_search_state.in_progress) {
                        if (g_search_state.selected_index > 0) {
                            g_search_state.selected_index--;
                            render_playlist_content();
                        }
                    } else {
                        if (g_selected_index > 0) { g_selected_index--; render_playlist_content(); }
                    }
                    break;
                case KEY_DOWN:
                    if (g_lyric_cursor_mode && g_lyrics.has_lyrics) {
                        pthread_mutex_lock(&g_lyrics.lock);
                        if (g_lyrics.cursor_index < g_lyrics.count - 1) {
                            g_lyrics.cursor_index++;
                            g_lyric_cursor_index = g_lyrics.cursor_index;
                            double t = g_lyrics.lines[g_lyrics.cursor_index].timestamp;
                            pthread_mutex_unlock(&g_lyrics.lock);
                            render_lyrics();
                            if (g_play_state != PLAY_STATE_STOPPED && progress_tracker_is_ready())
                                seek_audio(t);
                        } else {
                            pthread_mutex_unlock(&g_lyrics.lock);
                        }
                    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                        if (g_queue_selected_index < g_play_queue.count - 1) { g_queue_selected_index++; render_playlist_content(); }
                    } else if (g_search_state.active || g_search_state.in_progress) {
                        if (g_search_state.selected_index < g_search_state.result_count - 1) {
                            g_search_state.selected_index++;
                            render_playlist_content();
                        }
                    } else {
                        int max_count = playlist_tree_is_active() ? playlist_visible_count() : playlist_count();
                        if (g_selected_index < max_count - 1) {
                            g_selected_index++;
                            render_playlist_content();
                        }
                    }
                    break;
                case 'j':
                    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                        if (g_queue_selected_index < g_play_queue.count - 1)
                            g_queue_selected_index++;
                        render_playlist_content();
                    } else if (!g_lyric_cursor_mode) {
                        int max_count = playlist_tree_is_active() ? playlist_visible_count() : playlist_count();
                        if (g_selected_index < max_count - 1)
                            g_selected_index++;
                        render_playlist_content();
                    }
                    break;
                case 'k':
                    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                        if (g_queue_selected_index > 0)
                            g_queue_selected_index--;
                        render_playlist_content();
                    } else if (!g_lyric_cursor_mode) {
                        if (g_selected_index > 0)
                            g_selected_index--;
                        render_playlist_content();
                    }
                    break;
                case ' ':
                case 10:
                    if (g_search_state.in_progress) {
                        update_controls_status(i18n_get("status.search_wait"));
                        break;
                    }
                    if (g_search_state.active && g_search_state.result_count > 0) {
                        int original_index = g_search_state.result_indices[g_search_state.selected_index];
                        play_audio(original_index);
                        g_search_state.active = 0;
                        g_selected_index = g_sort_state.active ? 0 : original_index;
                        render_playlist_content();
                    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE && g_play_queue.count > 0) {
                        int queue_pos = g_queue_selected_index >= 0 && g_queue_selected_index < g_play_queue.count
                            ? g_queue_selected_index : 0;
                        int play_idx = g_play_queue.indices[queue_pos];
                        g_play_queue.current_position = queue_pos;
                        play_audio(play_idx);
                        g_selected_index = play_idx;
                    } else {
                        /* Tree mode: toggle directory expand, or play file */
                        if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
                            && !g_search_state.active) {
                            int node_type = get_visible_node_type(g_selected_index);
                            if (node_type == TREE_NODE_DIRECTORY) {
                                int tree_idx = get_visible_node_tree_index(g_selected_index);
                                playlist_toggle_directory_expand(tree_idx);
                                if (g_selected_index >= playlist_visible_count())
                                    g_selected_index = playlist_visible_count() - 1;
                                if (g_selected_index < 0) g_selected_index = 0;
                                render_playlist_content();
                                break;
                            }
                            /* File node: get real track index */
                            int real_idx = get_visible_node_track_index(g_selected_index);
                            if (real_idx >= 0)
                                play_audio(real_idx);
                            break;
                        }
                        int play_idx = g_selected_index;
                        if (g_sort_state.active) play_idx = g_sort_state.sorted_indices[g_selected_index];
                        play_audio(play_idx);
                    }
                    break;
                case 'O': case 'o': prompt_open_folder(); render_playlist_content(); break;
                case 'i':
                    if (g_play_queue.count > 0 && g_current_play_index >= 0) {
                        int track_idx = g_selected_index;
                        if (g_search_state.active)
                            track_idx = g_search_state.result_indices[g_search_state.selected_index];
                        else if (g_sort_state.active)
                            track_idx = g_sort_state.sorted_indices[g_selected_index];
                        else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                            track_idx = get_visible_node_track_index(g_selected_index);
                            if (track_idx < 0) break;
                        }
                        play_queue_insert_after(&g_play_queue, track_idx);
                        update_controls_status(i18n_get("status.inserted_next"));
                    }
                    break;
                case 'I': prompt_append_folder(); render_playlist_content(); break;
                case 'f': case 'F':
                    if (playlist_count() > 0) {
                        Track t;
                        int track_idx = g_selected_index;
                        if (g_search_state.active) {
                            pthread_mutex_lock(&g_search_mutex);
                            track_idx = g_search_state.result_indices[g_search_state.selected_index];
                            pthread_mutex_unlock(&g_search_mutex);
                        } else if (g_sort_state.active) {
                            track_idx = g_sort_state.sorted_indices[g_selected_index];
                        } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                            track_idx = get_visible_node_track_index(g_selected_index);
                            if (track_idx < 0) break;
                        }
                        get_track_metadata(track_idx, &t);
                        int result = add_to_favorites(&t);
                        update_controls_status(result == 0
                            ? i18n_get("favorites.added")
                            : i18n_get("favorites.exists_or_full"));
                    }
                    break;
                case '/':
                case 'S':
                    if (g_current_view == VIEW_MAIN) {
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
                        update_controls_status(i18n_get("status.search_cancelled"));
                        continue;
                    }
                    break;
                case 9:   /* Tab */
                case KEY_BTAB:  /* Shift+Tab (KEY_BTAB = 353) */
                    if (playlist_is_loaded()) {
                        /* Save browser cursor position before switching to queue */
                        if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER)
                            g_saved_browser_index = g_selected_index;

                        g_playlist_tab_mode = !g_playlist_tab_mode;

                        /* Restore browser cursor when switching back from queue */
                        if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                            if (g_saved_browser_index >= 0)
                                g_selected_index = g_saved_browser_index;
                            g_saved_browser_index = -1;
                        }

                        /* Auto-fill queue on first switch if empty */
                        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE &&
                            g_play_queue.count == 0 && playlist_count() > 0) {
                            int anchor = g_current_play_index >= 0 ? g_current_play_index : 0;
                            play_queue_rebuild(&g_play_queue, &g_playlist, g_play_mode, anchor);
                            g_queue_selected_index = 0;
                            for (int i = 0; i < g_play_queue.count; i++) {
                                if (g_play_queue.indices[i] == anchor) {
                                    g_queue_selected_index = i;
                                    break;
                                }
                            }
                        }
                        render_playlist_content();
                        render_controls();
                        update_controls_status(
                            g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE
                                ? i18n_get("status.switched_queue")
                                : i18n_get("status.switched_browser"));
                    }
                    break;
                case 'a':
                    if (playlist_count() > 0) {
                        int track_idx = g_selected_index;
                        if (g_search_state.active) {
                            pthread_mutex_lock(&g_search_mutex);
                            track_idx = g_search_state.result_indices[g_search_state.selected_index];
                            pthread_mutex_unlock(&g_search_mutex);
                        } else if (g_sort_state.active) {
                            track_idx = g_sort_state.sorted_indices[g_selected_index];
                        } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                            track_idx = get_visible_node_track_index(g_selected_index);
                            if (track_idx < 0) break;
                        }
                        play_queue_append(&g_play_queue, track_idx);
                        update_controls_status(i18n_get("status.added_to_queue"));
                    }
                    break;
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
                        } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                            track_idx = get_visible_node_track_index(g_selected_index);
                            if (track_idx < 0) break;
                        }
                        get_track_metadata(track_idx, &t);

                        int max_y, max_x;
                        getmaxyx(stdscr, max_y, max_x);
                        WINDOW *win_win = newwin(max_y - 4, max_x - 4, 2, 2);
                        rounded_box(win_win);
                        mvwprintw(win_win, 0, 2, "%s", i18n_get("playlist_mgr.select_playlist"));
                        wbkgd(win_win, COLOR_PAIR(COLOR_PAIR_PLAYLIST));

                        int start_y = 2;
                        int visible_lines = max_y - 8;
                        int selected = 0, offset = 0;

                        while (1) {
                            for (int i = 0; i < visible_lines && (offset + i) < g_playlist_manager.count; i++) {
                                int idx = offset + i;
                                UserPlaylist *pl = &g_playlist_manager.playlists[idx];
                                char playlist_name[MAX_PLAYLIST_NAME_LEN + 8];
                                format_display_text(playlist_name, sizeof(playlist_name), pl->name, 30, 1);
                                if (idx == selected) {
                                    wattron(win_win, A_REVERSE);
                                    mvwprintw(win_win, start_y + i, 2,
                                              i18n_get("playlist_mgr.track_count_fmt"),
                                              playlist_name, pl->track_count);
                                    wattroff(win_win, A_REVERSE);
                                } else {
                                    mvwprintw(win_win, start_y + i, 2,
                                              i18n_get("playlist_mgr.track_count_fmt"),
                                              playlist_name, pl->track_count);
                                }
                            }

                            mvwprintw(win_win, max_y - 6, 2, "%s",
                                      ui_text("↑/↓: 选择 | ENTER: 确认 | ESC: 取消",
                                              "Up/Down: Select | Enter: OK | Esc: Cancel"));
                            wrefresh(win_win);
                            wtimeout(win_win, UI_INPUT_TIMEOUT_MS);
                            int c = wgetch(win_win);
                            if (c == ERR) { media_session_tick(); continue; }
                            if (c == 27) break;
                            if (c == KEY_UP && selected > 0) {
                                selected--;
                                if (selected < offset) offset = selected;
                            }
                            if (c == KEY_DOWN && selected < g_playlist_manager.count - 1) {
                                selected++;
                                if (selected >= offset + visible_lines) offset = selected - visible_lines + 1;
                            }
                            if (c == 10 || c == ' ') {
                                int r = add_track_to_playlist(selected, &t);
                                if (r == 0)       update_controls_status(i18n_get("playlist_mgr.added_to_playlist"));
                                else if (r == -3) update_controls_status(i18n_get("playlist_mgr.playlist_full"));
                                else              update_controls_status(i18n_get("playlist_mgr.already_in_playlist"));
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
                case 'd':
                    if (g_play_queue.count > 0) {
                        int target_pos;
                        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                            target_pos = g_queue_selected_index;
                        } else {
                            int track_idx = g_selected_index;
                            if (g_search_state.active) {
                                pthread_mutex_lock(&g_search_mutex);
                                track_idx = g_search_state.result_indices[g_search_state.selected_index];
                                pthread_mutex_unlock(&g_search_mutex);
                            } else if (g_sort_state.active) {
                                track_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            target_pos = -1;
                            for (int i = 0; i < g_play_queue.count; i++) {
                                if (g_play_queue.indices[i] == track_idx) { target_pos = i; break; }
                            }
                        }
                        if (target_pos >= 0) {
                            play_queue_remove_at(&g_play_queue, target_pos);
                            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                                if (g_queue_selected_index >= g_play_queue.count)
                                    g_queue_selected_index = g_play_queue.count > 0 ? g_play_queue.count - 1 : 0;
                            }
                            render_playlist_content();
                        }
                    }
                    break;
                case 'D':
                    if (g_play_queue.count > 0) {
                        play_queue_clear(&g_play_queue);
                        g_queue_selected_index = 0;
                        render_playlist_content();
                        update_controls_status(i18n_get("status.queue_cleared"));
                    }
                    break;
                case 'J':
                    if (g_play_queue.count > 1) {
                        int target_pos;
                        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                            target_pos = g_queue_selected_index;
                        } else {
                            int track_idx = g_selected_index;
                            if (g_search_state.active) {
                                pthread_mutex_lock(&g_search_mutex);
                                track_idx = g_search_state.result_indices[g_search_state.selected_index];
                                pthread_mutex_unlock(&g_search_mutex);
                            } else if (g_sort_state.active) {
                                track_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            target_pos = -1;
                            for (int i = 0; i < g_play_queue.count; i++) {
                                if (g_play_queue.indices[i] == track_idx) { target_pos = i; break; }
                            }
                        }
                        if (target_pos >= 0 && target_pos < g_play_queue.count - 1) {
                            play_queue_move_down(&g_play_queue, target_pos);
                            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                                g_queue_selected_index++;
                            render_playlist_content();
                        }
                    }
                    break;
                case 'K':
                    if (g_play_queue.count > 1) {
                        int target_pos;
                        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                            target_pos = g_queue_selected_index;
                        } else {
                            int track_idx = g_selected_index;
                            if (g_search_state.active) {
                                pthread_mutex_lock(&g_search_mutex);
                                track_idx = g_search_state.result_indices[g_search_state.selected_index];
                                pthread_mutex_unlock(&g_search_mutex);
                            } else if (g_sort_state.active) {
                                track_idx = g_sort_state.sorted_indices[g_selected_index];
                            }
                            target_pos = -1;
                            for (int i = 0; i < g_play_queue.count; i++) {
                                if (g_play_queue.indices[i] == track_idx) { target_pos = i; break; }
                            }
                        }
                        if (target_pos > 0) {
                            play_queue_move_up(&g_play_queue, target_pos);
                            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                                g_queue_selected_index--;
                            render_playlist_content();
                        }
                    }
                    break;
                case '1': set_play_mode(PLAY_MODE_SEQUENTIAL); render_controls(); break;
                case '2': set_play_mode(PLAY_MODE_SINGLE_REPEAT); render_controls(); break;
                case '3': set_play_mode(PLAY_MODE_LIST_REPEAT); render_controls(); break;
                case '4': set_play_mode(PLAY_MODE_SHUFFLE_REPEAT); render_controls(); break;
                case '5': set_play_mode(PLAY_MODE_FOLDER_SEQUENTIAL); render_controls(); break;
            }
        }

        /* Global list-mode keys that don't need switch */
        if (g_current_view == VIEW_MAIN && g_control_focus == 0 && g_lyric_cursor_mode == 0) {
            if (ch == 'n') {
                next_track();
                continue;
            }
            if (ch == 'p') {
                prev_track();
                continue;
            }
            if (ch == 'h' && g_play_history.count > 0) {
                /* Simple inline history: show last 10 entries as a popup window */
                int my, mx;
                getmaxyx(stdscr, my, mx);
                int n = g_play_history.count > 10 ? 10 : g_play_history.count;
                int pw = 60, ph = n + 4;
                int px = (mx - pw) / 2, py = (my - ph) / 2;
                WINDOW *w = newwin(ph, pw, py, px);
                rounded_box(w);
                mvwprintw(w, 0, 2, "%s", i18n_get("play_history.title"));
                int sel = 0, off = 0;
                while (1) {
                    for (int i = 0; i < n && i < ph - 3; i++) {
                        int idx = off + i;
                        if (idx >= g_play_history.count) break;
                        const HistoryEntry *e = &g_play_history.entries[idx];
                        char label[80];
                        if (e->title[0])
                            snprintf(label, sizeof(label), "%s - %s", e->title, e->artist);
                        else
                            snprintf(label, sizeof(label), "%s", e->path);
                        if (idx == sel) {
                            wattron(w, A_REVERSE);
                            mvwprintw(w, i + 1, 2, " %s ", label);
                            wattroff(w, A_REVERSE);
                        } else {
                            mvwprintw(w, i + 1, 2, " %s", label);
                        }
                    }
                    mvwprintw(w, ph - 2, 2, "%s", i18n_get("play_history.hint"));
                    wrefresh(w);
                    wtimeout(w, UI_INPUT_TIMEOUT_MS);
                    int c = wgetch(w);
                    if (c == ERR) { media_session_tick(); continue; }
                    if (c == 27) break;
                    if (c == KEY_UP && sel > 0) sel--;
                    if (c == KEY_DOWN && sel < g_play_history.count - 1) sel++;
                    if (c == 10 || c == ' ') {
                        int found = playlist_find_track_index_by_path(g_play_history.entries[sel].path);
                        if (found >= 0) { play_audio(found); g_selected_index = found; }
                        break;
                    }
                }
                delwin(w);
                render_playlist_content();
                render_controls();
                continue;
            }
        }
    }

}

/* ============================================================
 * Cleanup
 * ============================================================ */

void cleanup(void)
{
    log_info("ui", "cleanup() called");

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

    if (win_playlist) { delwin(win_playlist); win_playlist = NULL; }
    if (win_controls) { delwin(win_controls); win_controls = NULL; }
    if (win_lyrics)   { delwin(win_lyrics);   win_lyrics   = NULL; }

    endwin();
    audio_backend_shutdown();
    remote_cleanup();
}

/* ============================================================
 * Rainbow / Konami
 * ============================================================ */

void update_rainbow_colors(void)
{
    if (!g_rainbow_mode_enabled || !has_colors()) return;

    static int rainbow_colors[7] = {
        COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
        COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
    };

    g_rainbow_color_offset = (g_rainbow_color_offset + 1) % 7;

    g_app_config.theme.border_fg     = rainbow_colors[(0 + g_rainbow_color_offset) % 7];
    g_app_config.theme.playlist_fg   = rainbow_colors[(1 + g_rainbow_color_offset) % 7];
    g_app_config.theme.controls_fg   = rainbow_colors[(2 + g_rainbow_color_offset) % 7];
    g_app_config.theme.lyrics_fg     = rainbow_colors[(3 + g_rainbow_color_offset) % 7];
    g_app_config.theme.sidebar_fg    = rainbow_colors[(4 + g_rainbow_color_offset) % 7];
    g_app_config.theme.highlight_fg  = rainbow_colors[(5 + g_rainbow_color_offset) % 7];
    g_app_config.theme.highlight_bg  = rainbow_colors[(6 + g_rainbow_color_offset) % 7];

    apply_color_theme();
    if (g_current_view == VIEW_MAIN)
        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
}

void check_konami_input(int ch)
{
    uint64_t now = get_ui_time_ms();
    if (g_konami_input_pos > 0 && (now - g_konami_last_time) > 3000)
        g_konami_input_pos = 0;

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

void toggle_rainbow_mode(void)
{
    if (!has_colors()) return;

    if (g_rainbow_mode_enabled) {
        memcpy(&g_app_config.theme, &g_saved_theme, sizeof(ColorTheme));
        apply_color_theme();
        g_rainbow_mode_enabled = 0;
        g_rainbow_color_offset = 0;
        update_controls_status(i18n_get("status.rainbow_off"));
    } else {
        memcpy(&g_saved_theme, &g_app_config.theme, sizeof(ColorTheme));
        g_app_config.theme.border_bg    = -1;
        g_app_config.theme.playlist_bg  = -1;
        g_app_config.theme.controls_bg  = -1;
        g_app_config.theme.lyrics_bg    = -1;
        g_app_config.theme.sidebar_bg   = -1;
        g_app_config.theme.highlight_bg = -1;
        g_rainbow_color_offset = 0;
        update_rainbow_colors();
        g_rainbow_mode_enabled = 1;
        update_controls_status(i18n_get("status.rainbow_on"));
    }


    if (g_current_view == VIEW_MAIN) {
        create_layout();
        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    }

}
