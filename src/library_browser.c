/**
 * @file library_browser.c
 * @brief Library browsing UI implementation
 *
 * Provides artist/album/genre browsing and track playback from the
 * SQLite-backed music library. Activated by pressing 'M' in the main view.
 *
 * Navigation state machine:
 *   LIBRARY_HOME → LIBRARY_ARTISTS → LIBRARY_ARTIST_ALBUMS → LIBRARY_ALBUM_TRACKS
 *   LIBRARY_HOME → LIBRARY_ALBUMS → LIBRARY_ALBUM_TRACKS
 *   LIBRARY_HOME → LIBRARY_GENRES → LIBRARY_GENRE_TRACKS
 *   LIBRARY_HOME → LIBRARY_ALL_TRACKS
 *   LIBRARY_HOME → LIBRARY_SEARCH_RESULTS
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#include "../include/library_browser.h"
#include "../include/library.h"
#include "../include/search.h"
#include "../include/logger.h"
#include <ncursesw/ncurses.h>
#include <string.h>
#include <stdlib.h>

/* ── Global state ─────────────────────────────────────────────────── */
LibraryState g_library_state = {0};

/* ── Forward declarations ─────────────────────────────────────────── */
static void render_library_home(WINDOW *win, int content_h, int content_w);
static void render_artist_list(WINDOW *win, int content_h, int content_w);
static void render_album_list(WINDOW *win, int content_h, int content_w,
                               const char *filter_artist);
static void render_genre_list(WINDOW *win, int content_h, int content_w);
static void render_track_list(WINDOW *win, int content_h, int content_w,
                               const int *rowids, int count, const char *title);

static void handle_home_input(int ch);
static void handle_artist_input(int ch);
static void handle_album_input(int ch);
static void handle_genre_input(int ch);
static void handle_track_list_input(int ch, const int *rowids, int count);

static int  ensure_scroll_visible(int selected, int offset, int visible);
static void truncate_text(char *buf, size_t buf_size, const char *text, int max_cols);

/* ── Public API ───────────────────────────────────────────────────── */

void library_browser_toggle(void) {
    if (!g_library_state.active) {
        if (!g_library_state.available) return;
        library_browser_enter();
    } else {
        library_browser_exit();
    }
}

void library_browser_enter(void) {
    g_library_state.active = 1;
    g_library_state.view = LIBRARY_HOME;
    g_library_state.selected_index = 0;
    g_library_state.scroll_offset = 0;
    g_library_state.item_count = 4; /* home menu: artists, albums, genres, all tracks */
    g_library_state.filter_artist[0] = '\0';
    g_library_state.filter_album[0] = '\0';
    g_library_state.filter_genre[0] = '\0';
    g_library_state.available = library_is_available();
    search_clear();
    request_ui_refresh(UI_DIRTY_PLAYLIST);
}

void library_browser_exit(void) {
    g_library_state.active = 0;
    g_library_state.view = LIBRARY_HOME;
    g_library_state.selected_index = 0;
    g_library_state.scroll_offset = 0;
    g_library_state.item_count = 0;
    request_ui_refresh(UI_DIRTY_PLAYLIST);
}

/* ── Render dispatch ─────────────────────────────────────────────── */

void render_library_content(void) {
    extern WINDOW *win_playlist;
    WINDOW *win = win_playlist;
    if (!win) return;

    if (!g_library_state.available) {
        mvwprintw(win, 2, 2, "Library unavailable — no database.");
        mvwprintw(win, 3, 2, "Press M to return.");
        wrefresh(win);
        return;
    }

    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    int content_h = max_y - 2;  /* reserve 1 line for title, 1 for hint */
    int content_w = max_x - 2;

    switch (g_library_state.view) {
    case LIBRARY_HOME:
        render_library_home(win, content_h, content_w);
        break;
    case LIBRARY_ARTISTS:
    case LIBRARY_ARTIST_ALBUMS:
        render_artist_list(win, content_h, content_w);
        break;
    case LIBRARY_ALBUMS:
        render_album_list(win, content_h, content_w, NULL);
        break;
    case LIBRARY_GENRES:
        render_genre_list(win, content_h, content_w);
        break;
    case LIBRARY_ALL_TRACKS:
    case LIBRARY_ALBUM_TRACKS:
    case LIBRARY_GENRE_TRACKS:
    case LIBRARY_SEARCH_RESULTS:
        /* track lists are rendered through the static helper with cached data */
        /* For simplicity, re-query on each render (library data is small) */
        {
            int rowids[1000];
            int count = 0;
            char title[64] = "";
            switch (g_library_state.view) {
            case LIBRARY_ALL_TRACKS:
                count = library_get_all_track_rowids(rowids, 1000);
                snprintf(title, sizeof(title), "All Tracks");
                break;
            case LIBRARY_ALBUM_TRACKS:
                count = library_get_album_track_rowids(
                    g_library_state.filter_artist,
                    g_library_state.filter_album, rowids, 1000);
                snprintf(title, sizeof(title), "%s", g_library_state.filter_album);
                break;
            case LIBRARY_GENRE_TRACKS:
                count = library_get_genre_track_rowids(
                    g_library_state.filter_genre, rowids, 1000);
                snprintf(title, sizeof(title), "%s", g_library_state.filter_genre);
                break;
            case LIBRARY_SEARCH_RESULTS:
                /* search results handled separately */
                break;
            default:
                break;
            }
            render_track_list(win, content_h, content_w, rowids, count, title);
        }
        break;
    }
    wrefresh(win);
}

/* ── Sub-renderers ───────────────────────────────────────────────── */

static void render_library_home(WINDOW *win, int content_h, int content_w) {
    (void)content_h;
    const char *items[] = {
        "Artists",
        "Albums",
        "Genres",
        "All Tracks",
    };
    int count = 4;

    /* Title */
    mvwprintw(win, 0, 1, "♪ Music Library");

    /* Separator */
    for (int x = 1; x < content_w; x++)
        mvwaddch(win, 1, x, '-');

    /* Items */
    int start = g_library_state.scroll_offset;
    int visible = content_h - 3;
    for (int i = 0; i < visible && (start + i) < count; i++) {
        int idx = start + i;
        char buf[512];
        if (idx < count) {
            snprintf(buf, sizeof(buf), "  %d. %s", idx + 1, items[idx]);
        }
        if (idx == g_library_state.selected_index) {
            wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
            wattroff(win, A_REVERSE);
        } else {
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
        }
    }

    /* Actions */
    if (start + visible >= count) {
        int line = count + 2;
        if (line < content_h) {
            for (int x = 1; x < content_w; x++)
                mvwaddch(win, line, x, '-');
            line++;
            mvwprintw(win, line, 2, "[S] Scan Directory");
            line++;
            mvwprintw(win, line, 2, "[R] Refresh Library");
        }
    }

    /* Hint bar */
    mvwprintw(win, content_h, 1, "Enter:browse  ESC:exit");
}

static void render_artist_list(WINDOW *win, int content_h, int content_w) {
    char **artists = NULL;
    int count = 0;

    if (g_library_state.view == LIBRARY_ARTISTS) {
        artists = library_get_artists(&count);
        mvwprintw(win, 0, 1, "♪ Artists");
    } else {
        /* LIBRARY_ARTIST_ALBUMS: show albums for selected artist */
        LibraryAlbum *albums = library_get_albums(g_library_state.filter_artist, &count);
        mvwprintw(win, 0, 1, "♪ %s", g_library_state.filter_artist);

        /* Render album list in the same area */
        for (int x = 1; x < content_w; x++)
            mvwaddch(win, 1, x, '-');

        int start = g_library_state.scroll_offset;
        int visible = content_h - 3;
        for (int i = 0; i < visible && (start + i) < count; i++) {
            int idx = start + i;
            char buf[512];
            if (idx < count) {
                snprintf(buf, sizeof(buf), "  %s  (%d tracks)",
                         albums[idx].album, albums[idx].track_count);
            }
            if (idx == g_library_state.selected_index) {
                wattron(win, A_REVERSE);
                mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
                wattroff(win, A_REVERSE);
            } else {
                mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
            }
        }

        mvwprintw(win, content_h, 1, "Enter:browse album  ESC:back");
        library_free_albums(albums, count);
        return;
    }

    /* Separator */
    for (int x = 1; x < content_w; x++)
        mvwaddch(win, 1, x, '-');

    /* Render artists */
    int start = g_library_state.scroll_offset;
    int visible = content_h - 3;
    for (int i = 0; i < visible && (start + i) < count; i++) {
        int idx = start + i;
        char buf[512];
        if (idx < count) {
            snprintf(buf, sizeof(buf), "  %s", artists[idx]);
        }
        if (idx == g_library_state.selected_index) {
            wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
            wattroff(win, A_REVERSE);
        } else {
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
        }
    }

    mvwprintw(win, content_h, 1, "Enter:browse albums  ESC:back");
    library_free_artists(artists, count);
}

static void render_album_list(WINDOW *win, int content_h, int content_w,
                               const char *filter_artist) {
    (void)filter_artist;
    LibraryAlbum *albums = library_get_albums(NULL, &g_library_state.item_count);
    mvwprintw(win, 0, 1, "♪ Albums");

    for (int x = 1; x < content_w; x++)
        mvwaddch(win, 1, x, '-');

    int start = g_library_state.scroll_offset;
    int visible = content_h - 3;
    for (int i = 0; i < visible && (start + i) < g_library_state.item_count; i++) {
        int idx = start + i;
        char buf[512];
        if (idx < g_library_state.item_count) {
            snprintf(buf, sizeof(buf), "  %s — %s  (%d tracks)",
                     albums[idx].artist, albums[idx].album,
                     albums[idx].track_count);
        }
        if (idx == g_library_state.selected_index) {
            wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
            wattroff(win, A_REVERSE);
        } else {
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
        }
    }

    mvwprintw(win, content_h, 1, "Enter:browse album  ESC:back");
    library_free_albums(albums, g_library_state.item_count);
}

static void render_genre_list(WINDOW *win, int content_h, int content_w) {
    char **genres = library_get_genres(&g_library_state.item_count);
    mvwprintw(win, 0, 1, "♪ Genres");

    for (int x = 1; x < content_w; x++)
        mvwaddch(win, 1, x, '-');

    int start = g_library_state.scroll_offset;
    int visible = content_h - 3;
    for (int i = 0; i < visible && (start + i) < g_library_state.item_count; i++) {
        int idx = start + i;
        char buf[512];
        if (idx < g_library_state.item_count) {
            snprintf(buf, sizeof(buf), "  %s", genres[idx]);
        }
        if (idx == g_library_state.selected_index) {
            wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
            wattroff(win, A_REVERSE);
        } else {
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, buf);
        }
    }

    mvwprintw(win, content_h, 1, "Enter:browse tracks  ESC:back");
    library_free_genres(genres, g_library_state.item_count);
}

static void render_track_list(WINDOW *win, int content_h, int content_w,
                               const int *rowids, int count, const char *title) {
    g_library_state.item_count = count;
    mvwprintw(win, 0, 1, "♪ %s  (%d)", title, count);

    for (int x = 1; x < content_w; x++)
        mvwaddch(win, 1, x, '-');

    int start = g_library_state.scroll_offset;
    int visible = content_h - 3;
    for (int i = 0; i < visible && (start + i) < count; i++) {
        int idx = start + i;
        Track t;
        char header[80];
        char line[512];

        if (library_get_track_metadata(rowids[idx], &t) == 0) {
            truncate_text(header, sizeof(header), t.title, content_w - 20);
            snprintf(line, sizeof(line), "  %s  —  %s", header, t.artist);
        } else {
            snprintf(line, sizeof(line), "  Track %d", idx + 1);
        }

        if (idx == g_library_state.selected_index) {
            wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, line);
            wattroff(win, A_REVERSE);
        } else {
            mvwprintw(win, i + 2, 1, "%-*s", content_w - 2, line);
        }
    }

    mvwprintw(win, content_h, 1, "Enter:play  A:append  ESC:back");
}

/* ── Input handling dispatch ─────────────────────────────────────── */

int handle_library_input(int ch) {
    if (!g_library_state.active) return 0;

    switch (g_library_state.view) {
    case LIBRARY_HOME:
        handle_home_input(ch);
        break;
    case LIBRARY_ARTISTS:
        handle_artist_input(ch);
        break;
    case LIBRARY_ARTIST_ALBUMS:
        handle_album_input(ch);
        break;
    case LIBRARY_ALBUMS:
        handle_album_input(ch);
        break;
    case LIBRARY_GENRES:
        handle_genre_input(ch);
        break;
    case LIBRARY_ALL_TRACKS:
    case LIBRARY_ALBUM_TRACKS:
    case LIBRARY_GENRE_TRACKS:
        {
            /* Re-query to get rowids for the current view */
            int rowids[1000];
            int count = 0;
            switch (g_library_state.view) {
            case LIBRARY_ALL_TRACKS:
                count = library_get_all_track_rowids(rowids, 1000);
                break;
            case LIBRARY_ALBUM_TRACKS:
                count = library_get_album_track_rowids(
                    g_library_state.filter_artist,
                    g_library_state.filter_album, rowids, 1000);
                break;
            case LIBRARY_GENRE_TRACKS:
                count = library_get_genre_track_rowids(
                    g_library_state.filter_genre, rowids, 1000);
                break;
            default:
                break;
            }
            handle_track_list_input(ch, rowids, count);
        }
        break;
    case LIBRARY_SEARCH_RESULTS:
        break;
    }

    request_ui_refresh(UI_DIRTY_PLAYLIST);
    return 1;
}

/* ── Home menu input ─────────────────────────────────────────────── */

static void navigate_to_subview(LibraryViewMode view) {
    g_library_state.view = view;
    g_library_state.selected_index = 0;
    g_library_state.scroll_offset = 0;
    g_library_state.item_count = 0;

    switch (view) {
    case LIBRARY_ARTISTS: {
        char **artists = library_get_artists(&g_library_state.item_count);
        library_free_artists(artists, g_library_state.item_count);
        break;
    }
    case LIBRARY_ALBUMS: {
        LibraryAlbum *albums = library_get_albums(NULL, &g_library_state.item_count);
        library_free_albums(albums, g_library_state.item_count);
        break;
    }
    case LIBRARY_GENRES: {
        char **genres = library_get_genres(&g_library_state.item_count);
        library_free_genres(genres, g_library_state.item_count);
        break;
    }
    case LIBRARY_ALL_TRACKS:
        g_library_state.item_count = library_get_track_count();
        break;
    default:
        break;
    }
}

static void handle_home_input(int ch) {
    int max_items = 4;

    switch (ch) {
    case KEY_UP:
        if (g_library_state.selected_index > 0)
            g_library_state.selected_index--;
        break;
    case KEY_DOWN:
        if (g_library_state.selected_index < max_items - 1)
            g_library_state.selected_index++;
        break;
    case '\n':
    case ' ':
    case KEY_ENTER:
        switch (g_library_state.selected_index) {
        case 0: navigate_to_subview(LIBRARY_ARTISTS); break;
        case 1: navigate_to_subview(LIBRARY_ALBUMS);  break;
        case 2: navigate_to_subview(LIBRARY_GENRES);  break;
        case 3: navigate_to_subview(LIBRARY_ALL_TRACKS); break;
        }
        break;
    case 's':
    case 'S':
        /* Scan directory — for now, scan from current working directory */
        library_scan_directory(".");
        request_ui_refresh(UI_DIRTY_PLAYLIST);
        break;
    case 'r':
    case 'R':
        library_scan_all_roots();
        request_ui_refresh(UI_DIRTY_PLAYLIST);
        break;
    case 27:  /* ESC */
        library_browser_exit();
        break;
    }
}

/* ── Artist list input ───────────────────────────────────────────── */

static void handle_artist_input(int ch) {
    switch (ch) {
    case KEY_UP:
        if (g_library_state.selected_index > 0)
            g_library_state.selected_index--;
        break;
    case KEY_DOWN:
        if (g_library_state.selected_index < g_library_state.item_count - 1)
            g_library_state.selected_index++;
        break;
    case '\n':
    case ' ':
    case KEY_ENTER: {
        char **artists = library_get_artists(&g_library_state.item_count);
        if (g_library_state.selected_index < g_library_state.item_count && artists) {
            strncpy(g_library_state.filter_artist,
                    artists[g_library_state.selected_index],
                    sizeof(g_library_state.filter_artist) - 1);
            g_library_state.filter_artist[sizeof(g_library_state.filter_artist) - 1] = '\0';
            g_library_state.view = LIBRARY_ARTIST_ALBUMS;
            g_library_state.selected_index = 0;
            g_library_state.scroll_offset = 0;
            /* Count albums for this artist */
            LibraryAlbum *albums = library_get_albums(
                g_library_state.filter_artist, &g_library_state.item_count);
            library_free_albums(albums, g_library_state.item_count);
        }
        library_free_artists(artists, g_library_state.item_count);
        break;
    }
    case 27:  /* ESC */
        g_library_state.view = LIBRARY_HOME;
        g_library_state.selected_index = 0;
        g_library_state.scroll_offset = 0;
        g_library_state.item_count = 4;
        break;
    }
}

/* ── Album list input (shared: albums view + artist→albums) ───────── */

static void handle_album_input(int ch) {
    switch (ch) {
    case KEY_UP:
        if (g_library_state.selected_index > 0)
            g_library_state.selected_index--;
        break;
    case KEY_DOWN:
        if (g_library_state.selected_index < g_library_state.item_count - 1)
            g_library_state.selected_index++;
        break;
    case '\n':
    case ' ':
    case KEY_ENTER: {
        LibraryAlbum *albums = library_get_albums(
            (g_library_state.view == LIBRARY_ARTIST_ALBUMS)
                ? g_library_state.filter_artist : NULL,
            &g_library_state.item_count);
        if (g_library_state.selected_index < g_library_state.item_count && albums) {
            strncpy(g_library_state.filter_artist,
                    albums[g_library_state.selected_index].artist,
                    sizeof(g_library_state.filter_artist) - 1);
            strncpy(g_library_state.filter_album,
                    albums[g_library_state.selected_index].album,
                    sizeof(g_library_state.filter_album) - 1);
            g_library_state.view = LIBRARY_ALBUM_TRACKS;
            g_library_state.selected_index = 0;
            g_library_state.scroll_offset = 0;
            g_library_state.item_count = library_get_album_track_rowids(
                g_library_state.filter_artist,
                g_library_state.filter_album, NULL, 0);
        }
        library_free_albums(albums, g_library_state.item_count);
        break;
    }
    case 27:  /* ESC */
        if (g_library_state.view == LIBRARY_ARTIST_ALBUMS) {
            g_library_state.view = LIBRARY_ARTISTS;
        } else {
            g_library_state.view = LIBRARY_HOME;
        }
        g_library_state.selected_index = 0;
        g_library_state.scroll_offset = 0;
        g_library_state.filter_artist[0] = '\0';
        g_library_state.filter_album[0] = '\0';
        break;
    }
}

/* ── Genre list input ────────────────────────────────────────────── */

static void handle_genre_input(int ch) {
    switch (ch) {
    case KEY_UP:
        if (g_library_state.selected_index > 0)
            g_library_state.selected_index--;
        break;
    case KEY_DOWN:
        if (g_library_state.selected_index < g_library_state.item_count - 1)
            g_library_state.selected_index++;
        break;
    case '\n':
    case ' ':
    case KEY_ENTER: {
        char **genres = library_get_genres(&g_library_state.item_count);
        if (g_library_state.selected_index < g_library_state.item_count && genres) {
            strncpy(g_library_state.filter_genre,
                    genres[g_library_state.selected_index],
                    sizeof(g_library_state.filter_genre) - 1);
            g_library_state.filter_genre[sizeof(g_library_state.filter_genre) - 1] = '\0';
            g_library_state.view = LIBRARY_GENRE_TRACKS;
            g_library_state.selected_index = 0;
            g_library_state.scroll_offset = 0;
            g_library_state.item_count = library_get_genre_track_rowids(
                g_library_state.filter_genre, NULL, 0);
        }
        library_free_genres(genres, g_library_state.item_count);
        break;
    }
    case 27:  /* ESC */
        g_library_state.view = LIBRARY_HOME;
        g_library_state.selected_index = 0;
        g_library_state.scroll_offset = 0;
        g_library_state.item_count = 4;
        break;
    }
}

/* ── Track list input ────────────────────────────────────────────── */

static void handle_track_list_input(int ch, const int *rowids, int count) {
    switch (ch) {
    case KEY_UP:
        if (g_library_state.selected_index > 0)
            g_library_state.selected_index--;
        break;
    case KEY_DOWN:
        if (g_library_state.selected_index < count - 1)
            g_library_state.selected_index++;
        break;
    case '\n':
    case ' ':
    case KEY_ENTER:
        /* Play the selected track */
        if (g_library_state.selected_index < count) {
            library_load_into_playlist(&rowids[g_library_state.selected_index], 1, 0);
            /* Auto-play doesn't happen with library_load_into_playlist alone.
             * We need to set the current play index and start playback. */
            /* The existing event loop handles this if we set state properly */
            search_clear();
            g_selected_index = 0;
            play_audio(0);
            library_browser_exit();
        }
        break;
    case 'a':
    case 'A':
        /* Append to current playlist */
        if (g_library_state.selected_index < count) {
            library_load_into_playlist(&rowids[g_library_state.selected_index], 1, 1);
        }
        break;
    case 27:  /* ESC */
        /* Go back to previous view */
        switch (g_library_state.view) {
        case LIBRARY_ALL_TRACKS:
            g_library_state.view = LIBRARY_HOME;
            g_library_state.item_count = 4;
            break;
        case LIBRARY_ALBUM_TRACKS:
            if (g_library_state.filter_artist[0]) {
                g_library_state.view = LIBRARY_ARTIST_ALBUMS;
            } else {
                g_library_state.view = LIBRARY_ALBUMS;
            }
            break;
        case LIBRARY_GENRE_TRACKS:
            g_library_state.view = LIBRARY_GENRES;
            break;
        default:
            g_library_state.view = LIBRARY_HOME;
            g_library_state.item_count = 4;
            break;
        }
        g_library_state.selected_index = 0;
        g_library_state.scroll_offset = 0;
        break;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static int ensure_scroll_visible(int selected, int offset, int visible) {
    if (selected < offset)
        return selected;
    if (selected >= offset + visible)
        return selected - visible + 1;
    return offset;
}

static void truncate_text(char *buf, size_t buf_size, const char *text, int max_cols) {
    if (!text || !text[0]) {
        buf[0] = '\0';
        return;
    }
    int width = utf8_str_width(text);
    if (width <= max_cols) {
        strncpy(buf, text, buf_size - 1);
        buf[buf_size - 1] = '\0';
    } else {
        utf8_str_substring(buf, text, 0, max_cols - 1);
        size_t len = strlen(buf);
        if (len + 3 < buf_size) {
            buf[len] = '.';
            buf[len + 1] = '.';
            buf[len + 2] = '.';
            buf[len + 3] = '\0';
        }
    }
}
