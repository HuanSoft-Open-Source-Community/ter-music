/**
 * @file dialog.c — Text input prompts and folder dialogs
 * Extracted from the original ui.c monolith.
 */
#include "types.h"
#include <ncursesw/ncurses.h>
#include "ui/dialog.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include "playlist/playlist.h"
#include "audio/audio.h"
#include "config/config.h"
#include "logger/logger.h"
#include "media/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* win_controls is defined in ui.c */
extern WINDOW *win_controls;
#include <wchar.h>
#include <wctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* Escape sequence parser states (shared with text input helpers) */
enum {
    INPUT_ESCAPE_NONE = 0,
    INPUT_ESCAPE_ESC,
    INPUT_ESCAPE_CSI,
    INPUT_ESCAPE_SS3,
    INPUT_ESCAPE_OSC,
    INPUT_ESCAPE_OSC_ESC
};

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

static void prompt_folder_input(int append_mode) {
    // 空指针检查：避免win_controls未初始化时崩溃
    if (!win_controls) {
        return;
    }
    noecho();
    curs_set(1);

    const char *folder_prompt = append_mode
        ? i18n_get("dialog.append_folder")
        : i18n_get("dialog.folder_path");
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
                             i18n_get("status.appended"),
                             count,
                             i18n_get("status.tracks_unit"));
                    update_controls_status(msg);
                } else {
                    update_controls_status(i18n_get("status.folder_loaded"));
                }
                render_playlist_content();
            } else {
                if (append_mode) {
                    update_controls_status(i18n_get("dialog.no_new_audio"));
                } else {
                    update_controls_status(i18n_get("dialog.no_audio_found"));
                    reset_playlist_state();
                }
                render_playlist_content();
            }
        } else {
            update_controls_status(i18n_get("dialog.path_invalid"));
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

void prompt_append_folder() {
    prompt_folder_input(1);
}


/**
 * 主事件循环
 * 处理用户输入、焦点切换和功能调用
 */
