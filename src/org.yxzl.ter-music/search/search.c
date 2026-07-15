#include "search/search.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "playlist/playlist.h"
#include "logger/logger.h"
#include "media/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <ctype.h>

extern WINDOW *win_playlist;
extern WINDOW *win_controls;

extern void render_playlist_content(void);
extern void update_controls_status(const char *msg);

SearchState g_search_state = {0};
pthread_mutex_t g_search_mutex = PTHREAD_MUTEX_INITIALIZER;

#define SEARCH_BATCH_PUBLISH_SIZE 15

static void* search_thread_func(void *arg) {
    char *query = (char *)arg;
    int playlist_total = playlist_count();
    int local_results[MAX_SEARCH_RESULTS];
    int local_count = 0;
    int last_published_count = 0;

    for (int i = 0; i < playlist_total && local_count < MAX_SEARCH_RESULTS; i++) {
        // 检查取消
        pthread_mutex_lock(&g_search_mutex);
        int should_cancel = g_search_state.cancel;
        pthread_mutex_unlock(&g_search_mutex);
        if (should_cancel) {
            break;
        }

        // 匹配检查（可能因 FFmpeg I/O 阻塞）
        if (track_matches_query(i, query)) {
            local_results[local_count++] = i;
        }

        // 批量发布：每 BATCH_SIZE 个匹配或循环结束时
        if ((local_count - last_published_count) >= SEARCH_BATCH_PUBLISH_SIZE ||
            i == playlist_total - 1) {
            pthread_mutex_lock(&g_search_mutex);
            memcpy(g_search_state.result_indices, local_results,
                   local_count * sizeof(int));
            g_search_state.result_count = local_count;
            g_search_state.progress = i + 1;
            pthread_mutex_unlock(&g_search_mutex);
            last_published_count = local_count;
            request_ui_refresh(UI_DIRTY_PLAYLIST);
        }
    }

    // 最终发布
    pthread_mutex_lock(&g_search_mutex);
    memcpy(g_search_state.result_indices, local_results,
           local_count * sizeof(int));
    g_search_state.result_count = local_count;
    g_search_state.progress = playlist_total;
    g_search_state.active = (local_count > 0 && !g_search_state.cancel) ? 1 : 0;
    g_search_state.in_progress = 0;
    g_search_state.cancel = 0;
    g_search_state.selected_index = 0;
    g_search_state.result_offset = 0;
    snprintf(g_search_state.query, sizeof(g_search_state.query), "%s",
             query ? query : "");
    pthread_mutex_unlock(&g_search_mutex);

    request_ui_refresh(UI_DIRTY_PLAYLIST);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %d %s",
             i18n_get("search.completed"),
             local_count,
             i18n_get("search.results_unit"));
    update_controls_status(msg);
    log_info("search", "Search '%s': %d results out of %d tracks",
             query, local_count, playlist_total);

    free(query);
    return NULL;
}

void search_async_start(const char *query) {
    if (!query || query[0] == '\0') {
        return;
    }

    search_async_cancel();

    pthread_mutex_lock(&g_search_mutex);
    pthread_t old_thread = g_search_state.thread;
    int had_running = g_search_state.in_progress;
    pthread_mutex_unlock(&g_search_mutex);

    if (had_running) {
        pthread_join(old_thread, NULL);
    }

    pthread_mutex_lock(&g_search_mutex);
    memset(g_search_state.result_indices, 0,
           sizeof(g_search_state.result_indices));
    g_search_state.result_count = 0;
    g_search_state.selected_index = 0;
    g_search_state.result_offset = 0;
    g_search_state.active = 0;
    g_search_state.progress = 0;
    g_search_state.cancel = 0;
    g_search_state.in_progress = 1;
    snprintf(g_search_state.query, sizeof(g_search_state.query), "%s", query);
    pthread_mutex_unlock(&g_search_mutex);

    char *query_copy = strdup(query);
    if (pthread_create(&g_search_state.thread, NULL,
                       search_thread_func, query_copy) != 0) {
        free(query_copy);
        pthread_mutex_lock(&g_search_mutex);
        g_search_state.in_progress = 0;
        g_search_state.active = 0;
        pthread_mutex_unlock(&g_search_mutex);
        update_controls_status(i18n_get("search.failed"));
        return;
    }

    request_ui_refresh(UI_DIRTY_PLAYLIST);
    update_controls_status(i18n_get("search.searching"));
}

void search_async_cancel(void) {
    pthread_mutex_lock(&g_search_mutex);
    if (g_search_state.in_progress) {
        g_search_state.cancel = 1;
    }
    pthread_mutex_unlock(&g_search_mutex);
}

int search_async_is_running(void) {
    int running;
    pthread_mutex_lock(&g_search_mutex);
    running = g_search_state.in_progress;
    pthread_mutex_unlock(&g_search_mutex);
    return running;
}

void search_clear(void) {
    search_async_cancel();

    pthread_mutex_lock(&g_search_mutex);
    pthread_t old_thread = g_search_state.thread;
    int was_running = g_search_state.in_progress;
    pthread_mutex_unlock(&g_search_mutex);

    if (was_running) {
        pthread_join(old_thread, NULL);
    }

    pthread_mutex_lock(&g_search_mutex);
    memset(&g_search_state, 0, sizeof(g_search_state));
    pthread_mutex_unlock(&g_search_mutex);
}

void search_prompt(void) {
    if (!win_controls || !playlist_is_loaded() || playlist_count() == 0) {
        if (!playlist_is_loaded() || playlist_count() == 0) {
            update_controls_status(i18n_get("search.playlist_empty"));
        }
        return;
    }

    curs_set(1);

    int max_y, max_x;
    getmaxyx(win_controls, max_y, max_x);

    mvwprintw(win_controls, 4, 2, "%s",
              i18n_get("search.prompt"));
    int input_start_col = getcurx(win_controls);
    wclrtoeol(win_controls);
    wrefresh(win_controls);

    char input[MAX_META_LEN];
    memset(input, 0, sizeof(input));
    int ch;

    flushinp();

    while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != 27 && strlen(input) < MAX_META_LEN - 1) {
        if (ch == ERR) {
            media_session_tick();
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (strlen(input) > 0) {
                size_t len = strlen(input) - 1;
                while (len > 0 && ((unsigned char)input[len] & 0xC0) == 0x80)
                    len--;
                input[len] = '\0';
                wmove(win_controls, 4, input_start_col);
                wclrtoeol(win_controls);
                wprintw(win_controls, "%s", input);
                wrefresh(win_controls);
            }
        } else if (ch >= 32) {
            size_t len = strlen(input);
            input[len] = (char)ch;
            input[len + 1] = '\0';
            wmove(win_controls, 4, input_start_col);
            wclrtoeol(win_controls);
            wprintw(win_controls, "%s", input);
            wrefresh(win_controls);
        }
    }

    flushinp();

    curs_set(0);

    mvwprintw(win_controls, 4, 2, "                    ");
    wclrtoeol(win_controls);
    wrefresh(win_controls);

    if (ch == 27) {
        update_controls_status(i18n_get("search.cancelled"));
        return;
    }

    if (strlen(input) > 0) {
        search_async_start(input);
    } else {
        search_async_cancel();
        pthread_mutex_lock(&g_search_mutex);
        g_search_state.active = 0;
        g_search_state.in_progress = 0;
        pthread_mutex_unlock(&g_search_mutex);
        render_playlist_content();
        update_controls_status(i18n_get("search.cancelled"));
    }
}

int search_lines(const char **lines, int line_count, const char *query,
                 int *results, int max_results) {
    if (!lines || !query || !results || max_results <= 0) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < line_count && count < max_results; i++) {
        if (strcasestr(lines[i], query)) {
            results[count++] = i;
        }
    }

    return count;
}
