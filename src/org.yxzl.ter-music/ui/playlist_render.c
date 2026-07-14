/**
 * @file playlist_render.c
 * @brief 播放列表内容渲染
 *
 * 从 ui.c 拆分，负责播放列表窗口的内容绘制，包括歌曲列表、
 * 搜索结果显示、底部状态信息栏和专辑封面。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "ui/menu_internal.h"
#include "ui/scrollbar.h"
#include "ui/braille/braille_art.h"
#include "playlist/playlist.h"
#include "search/search.h"
#include "config/config.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "library/browser/browser.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>

extern WINDOW *win_playlist;
extern char g_braille_art_buffer[8192];
extern int g_album_cover_size;

/* ── 主视图播放列表标签模式（文件浏览 vs 播放队列） ── */
int g_playlist_tab_mode = PLAYLIST_MODE_FILE_BROWSER;
int g_queue_selected_index = 0;   /* cursor in queue view */
int g_saved_browser_index = -1;  /* saved g_selected_index when switching to queue */

/* ============================================================
 * Text formatting
 * ============================================================ */

void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad)
{
    if (!dest || dest_size == 0) return;

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

static const char *get_playlist_source_label(void)
{
    static char folder_path[MAX_PATH_LEN];
    if (playlist_has_multiple_sources()) {
        return ui_text("多目录队列", "Mixed queue");
    }
    playlist_copy_folder_path(folder_path, sizeof(folder_path));
    if (folder_path[0] != '\0') return folder_path;
    return ui_text("(无)", "(none)");
}

/* ============================================================
 * Sanitize ASCII text (also used by format_display_text)
 * ============================================================ */

void sanitize_ascii_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) return;

    dest[0] = '\0';
    if (!src || src[0] == '\0') return;

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
        if (char_len == 0) break;
        if (!prev_space && write + 1 < dest_size) {
            dest[write++] = ' ';
            prev_space = 1;
        }
        read += char_len;
    }

    while (write > 0 && dest[write - 1] == ' ') write--;
    dest[write] = '\0';

    if (write == 0 && saw_non_ascii) {
        snprintf(dest, dest_size, "[non-ASCII]");
    }
}

/* ============================================================
 * Playlist content rendering
 * ============================================================ */

void render_playlist_content(void)
{
    if (!win_playlist) return;

    // Library browser mode
    if (g_library_state.active) {
        extern void render_library_content(void);
        render_library_content();
        return;
    }

    // Search state snapshot
    int snap_active, snap_in_progress, snap_count, snap_selected, snap_progress;
    int snap_indices[MAX_SEARCH_RESULTS];
    pthread_mutex_lock(&g_search_mutex);
    snap_active = g_search_state.active;
    snap_in_progress = g_search_state.in_progress;
    snap_count = g_search_state.result_count;
    snap_selected = g_search_state.selected_index;
    snap_progress = g_search_state.progress;
    if (snap_count > 0) {
        memcpy(snap_indices, g_search_state.result_indices, snap_count * sizeof(int));
    }
    pthread_mutex_unlock(&g_search_mutex);

    werase(win_playlist);
    wattron(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    rounded_box(win_playlist);

    char title_buf[128];
    if (snap_in_progress) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d/%d) ",
                 ui_text(" 搜索中 ", " Searching "), snap_progress, playlist_count());
        mvwprintw(win_playlist, 0, 2, "%s", title_buf);
    } else if (snap_active) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d %s) ",
                 ui_text(" 搜索结果 ", " Search Results "), snap_count,
                 ui_text("个", "found"));
        mvwprintw(win_playlist, 0, 2, "%s", title_buf);
    } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        snprintf(title_buf, sizeof(title_buf), " %s [%s] ",
                 ui_text("播放队列", "Play Queue"),
                 get_play_mode_str());
        mvwprintw(win_playlist, 0, 2, "%s", title_buf);
    } else {
        mvwprintw(win_playlist, 0, 2, "%s", ui_text(" 播放列表 ", " Playlist "));
    }
    wattroff(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));
    wbkgd(win_playlist, COLOR_PAIR(COLOR_PAIR_PLAYLIST));

    int h, w;
    getmaxyx(win_playlist, h, w);
    int content_height = h - 2;
    int playlist_total = playlist_count();
    int playlist_loaded = playlist_is_loaded();

    int tree_active = playlist_tree_is_active() &&
                      g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER &&
                      !snap_active && !snap_in_progress;

    int total_tracks, current_selected;
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        total_tracks = g_play_queue.count;
        current_selected = g_queue_selected_index;
    } else if ((snap_active || snap_in_progress) && snap_count > 0) {
        total_tracks = snap_count;
        current_selected = snap_selected;
    } else if (snap_in_progress) {
        total_tracks = 0;
        current_selected = 0;
    } else {
        if (tree_active) {
            total_tracks = playlist_visible_count();
        } else {
            total_tracks = playlist_total;
        }
        current_selected = g_selected_index;
    }

    if (!playlist_loaded) {
        char display_path[MAX_PATH_LEN];
        format_display_text(display_path, sizeof(display_path), get_playlist_source_label(), w - 10, 0);
        mvwprintw(win_playlist, h/2 - 1, 2, "%s", ui_text("播放列表为空。", "Playlist is empty."));
        mvwprintw(win_playlist, h/2, 2, "%s",
                  ui_text("按 'O' 打开目录，按 'I' 追加目录。",
                          "Press 'O' to open a folder, 'I' to append one."));
        mvwprintw(win_playlist, h/2 + 1, 2, "%s%s", ui_text("当前路径：", "Path: "), display_path);
    } else {
        int start_idx = 0;
        int visible_lines = content_height - 6;

        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
            if (g_queue_selected_index >= visible_lines)
                start_idx = g_queue_selected_index - visible_lines + 1;
        } else if (snap_active || snap_in_progress) {
            if (snap_selected >= visible_lines)
                start_idx = snap_selected - visible_lines + 1;
        } else {
            if (g_selected_index >= visible_lines)
                start_idx = g_selected_index - visible_lines + 1;
        }

        if (tree_active) {
            /* Preload metadata for file nodes in the visible tree range */
            int loaded = 0;
            Track tmp;
            for (int vi = start_idx; vi < start_idx + visible_lines && vi < total_tracks && loaded < 2; vi++) {
                int track_idx = get_visible_node_track_index(vi);
                if (track_idx >= 0) {
                    get_track_metadata(track_idx, &tmp);
                    loaded++;
                }
            }
        } else if (g_sort_state.active && !snap_active) {
            int loaded = 0;
            Track tmp;
            for (int vi = start_idx; vi < start_idx + visible_lines && vi < total_tracks && loaded < 2; vi++) {
                get_track_metadata(g_sort_state.sorted_indices[vi], &tmp);
                loaded++;
            }
        } else if (!tree_active) {
            preload_visible_tracks(start_idx, start_idx + visible_lines - 1);
        }

        for (int i = 0; i < visible_lines && (start_idx + i) < total_tracks; i++) {
            int idx = start_idx + i;
            int actual_idx = idx;
            Track t;

            if (tree_active) {
                /* ── Tree mode rendering ── */
                int tree_idx = get_visible_node_tree_index(idx);
                if (tree_idx < 0) continue;

                int node_type = get_visible_node_type(idx);
                int node_depth = get_tree_node_depth(tree_idx);
                const char *node_name = get_tree_node_name(tree_idx);

                /* Build indent string */
                char indent[64];
                int indent_len = node_depth * 2;
                if (indent_len > 60) indent_len = 60;
                memset(indent, ' ', indent_len);
                indent[indent_len] = '\0';

                if (node_type == TREE_NODE_DIRECTORY) {
                    /* Directory node */
                    int expanded = 0;
                    playlist_lock();
                    if (tree_idx >= 0 && tree_idx < g_playlist.tree_node_count)
                        expanded = g_playlist.tree_nodes[tree_idx].expanded;
                    playlist_unlock();

                    char dir_line[MAX_PATH_LEN + 64];
                    snprintf(dir_line, sizeof(dir_line), "%s%s %s/",
                             indent, expanded ? "[-]" : "[+]", node_name);

                    char truncated[MAX_PATH_LEN + 64];
                    format_display_text(truncated, sizeof(truncated), dir_line, w - 4, 0);

                    int is_selected = (idx == g_selected_index && g_control_focus == 0);
                    int attrs = A_NORMAL;
                    if (is_selected) attrs |= A_REVERSE;
                    if (attrs != A_NORMAL) wattron(win_playlist, attrs);
                    mvwprintw(win_playlist, i + 1, 2, "%s", truncated);
                    if (attrs != A_NORMAL) wattroff(win_playlist, attrs);
                } else {
                    /* File node */
                    actual_idx = get_visible_node_track_index(idx);
                    if (actual_idx < 0) continue;
                    get_track_metadata(actual_idx, &t);

                    char display_title[MAX_META_LEN];
                    if (t.cue_offset > 0 && t.cue_track_number > 0)
                        snprintf(display_title, sizeof(display_title), "[CUE] %s", t.title);
                    else
                        snprintf(display_title, sizeof(display_title), "%s", t.title);

                    int avail = w - 4 - indent_len;
                    int title_width  = avail * 3 / 5;
                    int artist_width = avail * 2 / 5;
                    if (title_width  < 2) title_width  = 2;
                    if (artist_width < 2) artist_width = 2;

                    char truncated_title[MAX_META_LEN];
                    char truncated_artist[MAX_META_LEN];
                    format_display_text(truncated_title, sizeof(truncated_title), display_title, title_width - 1, 1);
                    format_display_text(truncated_artist, sizeof(truncated_artist), t.artist, artist_width - 1, 1);

                    int is_selected = (idx == g_selected_index && g_control_focus == 0);
                    int is_now_playing = (actual_idx == g_current_play_index &&
                                          g_play_state != PLAY_STATE_STOPPED);
                    int attrs = A_NORMAL;
                    if (is_selected) attrs |= A_REVERSE;
                    if (is_now_playing) attrs |= A_BOLD;
                    if (attrs != A_NORMAL) wattron(win_playlist, attrs);
                    mvwprintw(win_playlist, i + 1, 2, "%s %s %s",
                              indent, truncated_title, truncated_artist);
                    if (attrs != A_NORMAL) wattroff(win_playlist, attrs);
                }
            } else {
                /* ── Flat mode rendering ── */
                int prefix_width = 0;
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                    prefix_width = 5;

            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                actual_idx = g_play_queue.indices[idx];
            } else if (snap_active || snap_in_progress) {
                actual_idx = snap_indices[idx];
            } else if (g_sort_state.active) {
                actual_idx = g_sort_state.sorted_indices[idx];
            }

            get_track_metadata(actual_idx, &t);

            int title_width  = (w - 4 - prefix_width) * 3 / 5;
            int artist_width = (w - 4 - prefix_width) * 2 / 5;
            if (title_width  < 2) title_width  = 2;
            if (artist_width < 2) artist_width = 2;

            /* For CUE sub-tracks, prepend track number and add a visible marker */
            char display_title[MAX_META_LEN];
            if (t.cue_offset > 0 && t.cue_track_number > 0) {
                snprintf(display_title, sizeof(display_title), "[CUE] %s", t.title);
            } else {
                snprintf(display_title, sizeof(display_title), "%s", t.title);
            }

            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            format_display_text(truncated_title, sizeof(truncated_title), display_title, title_width - 1, 1);
            format_display_text(truncated_artist, sizeof(truncated_artist), t.artist, artist_width - 1, 1);

            int is_selected, is_now_playing = 0;
            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                is_selected = (idx == g_queue_selected_index && g_control_focus == 0);
                is_now_playing = (idx == g_play_queue.current_position &&
                                  g_play_state != PLAY_STATE_STOPPED);
            } else if (snap_active || snap_in_progress) {
                is_selected = (idx == snap_selected);
            } else {
                is_selected = (idx == g_selected_index && g_control_focus == 0);
                is_now_playing = (actual_idx == g_current_play_index &&
                                  g_play_state != PLAY_STATE_STOPPED);
            }

            /* Build sequence number prefix for queue mode */
            char prefix[8] = "";
            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                snprintf(prefix, sizeof(prefix), "%4d.", idx + 1);

            /* Determine attributes: selected → A_REVERSE, now-playing → A_BOLD */
            int attrs = A_NORMAL;
            if (is_selected) attrs |= A_REVERSE;
            if (is_now_playing) attrs |= A_BOLD;

            if (attrs != A_NORMAL) wattron(win_playlist, attrs);
            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                mvwprintw(win_playlist, i + 1, 1, " %s %s %s ", prefix,
                          truncated_title, truncated_artist);
            } else {
                if (is_selected || attrs != A_NORMAL)
                    mvwprintw(win_playlist, i + 1, 1, " %s %s ",
                              truncated_title, truncated_artist);
                else
                    mvwprintw(win_playlist, i + 1, 2, "%s %s",
                              truncated_title, truncated_artist);
            }
            if (attrs != A_NORMAL) wattroff(win_playlist, attrs);
            } /* end flat mode */
        }

        if (total_tracks == 0) {
            if (snap_in_progress) {
                mvwprintw(win_playlist, 1, 2, "%s", ui_text("正在搜索...", "Searching..."));
            } else if (snap_active) {
                mvwprintw(win_playlist, 1, 2, "%s", ui_text("没有找到匹配的歌曲。", "No matching tracks found."));
            } else {
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                    mvwprintw(win_playlist, 1, 2, "%s", ui_text("队列为空。按 'a' 从浏览视图添加曲目。", "Queue empty. Press 'a' to add from browser."));
                else
                    mvwprintw(win_playlist, 1, 2, "%s", ui_text("当前目录下没有音频文件。", "No audio files found here."));
            }
        }

        // Scrollbar
        {
            int sb_col = w - 2;
            scrollbar_draw(win_playlist, 1, visible_lines, total_tracks, visible_lines, start_idx, sb_col);
        }

        // Status bar
        int status_line = h - 7;
        for (int x = 1; x < w - 1; x++)
            mvwaddstr(win_playlist, status_line, x, "\xe2\x94\x80"); /* ─ */

        char status_msg[MAX_META_LEN];
        switch (g_play_state) {
            case PLAY_STATE_PLAYING: snprintf(status_msg, sizeof(status_msg), "%s", ui_text("播放中", "Playing")); break;
            case PLAY_STATE_PAUSED:  snprintf(status_msg, sizeof(status_msg), "%s", ui_text("已暂停", "Paused")); break;
            case PLAY_STATE_STOPPED:
            default:                 snprintf(status_msg, sizeof(status_msg), "%s", ui_text("已停止", "Stopped")); break;
        }

        if (playlist_total > 0) {
            Track t;
            int index = g_current_play_index >= 0 ? g_current_play_index : g_selected_index;
            if (g_sort_state.active && !snap_active && g_current_play_index < 0) {
                index = g_sort_state.sorted_indices[g_selected_index];
            }
            /* Tree mode: translate visible index to track index */
            if (tree_active && g_current_play_index < 0) {
                int ti = get_visible_node_track_index(g_selected_index);
                if (ti >= 0) index = ti;
            }
            if (index < 0) index = 0;
            if (index >= playlist_total) index = playlist_total - 1;
            get_track_metadata(index, &t);

            int cover_start_col = w - 2;
            int cover_char_width = 0;
            int cover_lines = 0;
            int show_cover = 0;

            char cover_path[MAX_PATH_LEN];
            static int g_album_cover_enabled = 1;
            if (g_album_cover_enabled && g_app_config.show_album_cover &&
                get_current_album_cover_path(cover_path, sizeof(cover_path)) == 0) {
                int min_info_width = 52;
                int max_cover_size = 12;
                int min_cover_size = 5;
                int available_width  = w - min_info_width - 4;
                int available_height = 5;

                int max_cover_width  = available_width / 2;
                int max_cover_height = available_height;
                int cover_size = max_cover_width < max_cover_height ? max_cover_width : max_cover_height;
                if (cover_size > max_cover_size) cover_size = max_cover_size;
                if (cover_size < min_cover_size) cover_size = min_cover_size;

                cover_char_width = cover_size * 2;
                cover_lines = cover_size;

                if (w - cover_char_width - 4 >= min_info_width) {
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
                        cover_start_col = w - cover_char_width - 3;
                    }
                }
            }

            int info_width = show_cover ? cover_start_col - 4 : w - 4;
            int col_width = info_width / 2;
            int left_col_x   = 2;
            int center_col_x = 2 + col_width;

            char truncated_title[MAX_META_LEN];
            char truncated_artist[MAX_META_LEN];
            char truncated_album[MAX_META_LEN];
            format_display_text(truncated_title, sizeof(truncated_title), t.title, col_width - 1, 0);
            format_display_text(truncated_artist, sizeof(truncated_artist), t.artist, col_width - 1, 0);
            format_display_text(truncated_album, sizeof(truncated_album), t.album, col_width - 1, 0);

            // Left column: metadata
            mvwprintw(win_playlist, status_line + 1, left_col_x, "%s%s", ui_text("状态：", "State: "), status_msg);
            mvwprintw(win_playlist, status_line + 2, left_col_x, "%s%s", ui_text("模式：", "Mode: "), get_play_mode_str());
            mvwprintw(win_playlist, status_line + 3, left_col_x, "%s%s", ui_text("标题：", "Title: "), truncated_title);
            mvwprintw(win_playlist, status_line + 4, left_col_x, "%s%s", ui_text("艺术家：", "Artist: "), truncated_artist);
            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE && g_play_queue.count > 0) {
                char qbuf[64];
                snprintf(qbuf, sizeof(qbuf), "%s %d/%d",
                         menu_text("队列：", "Queue: "),
                         g_play_queue.current_position + 1, g_play_queue.count);
                mvwprintw(win_playlist, status_line + 5, left_col_x, "%s", qbuf);
            } else {
                mvwprintw(win_playlist, status_line + 5, left_col_x, "%s%s", ui_text("专辑：", "Album: "), truncated_album);
            }

            // Center column: audio technical info
            char rate_str[32] = "--", depth_str[32] = "--", bitrate_str[32] = "--", codec_display[32] = "--";
            if (g_audio_sample_rate > 0) snprintf(rate_str, sizeof(rate_str), "%dHz", g_audio_sample_rate);
            if (g_audio_bit_depth > 0)   snprintf(depth_str, sizeof(depth_str), "%dbit", g_audio_bit_depth);
            if (g_audio_bit_rate > 0)    snprintf(bitrate_str, sizeof(bitrate_str), "%dkbps", g_audio_bit_rate / 1000);
            if (g_audio_codec_name[0] != '\0') {
                char upper[32];
                int ci;
                for (ci = 0; g_audio_codec_name[ci] && ci < (int)sizeof(upper) - 1; ci++)
                    upper[ci] = (ci == 0) ? toupper((unsigned char)g_audio_codec_name[ci]) : g_audio_codec_name[ci];
                upper[ci] = '\0';
                snprintf(codec_display, sizeof(codec_display), "%s", upper);
            }

            mvwprintw(win_playlist, status_line + 1, center_col_x, "%s%s", ui_text("采样率：", "Sample Rate: "), rate_str);
            mvwprintw(win_playlist, status_line + 2, center_col_x, "%s%s", ui_text("位深：", "Bit Depth: "), depth_str);
            mvwprintw(win_playlist, status_line + 3, center_col_x, "%s%s", ui_text("比特率：", "Bitrate: "), bitrate_str);
            mvwprintw(win_playlist, status_line + 4, center_col_x, "%s%s", ui_text("编码：", "Codec: "), codec_display);

            // Album cover (braille art)
            if (show_cover) {
                char *lines[BRAILLE_MAX_SIZE];
                int line_count = get_braille_art_lines(g_braille_art_buffer, lines, BRAILLE_MAX_SIZE);
                int start_row = status_line + 1;
                for (int i = 0; i < line_count && i < cover_lines; i++) {
                    if (start_row + i < h - 1)
                        mvwprintw(win_playlist, start_row + i, cover_start_col, "%s", lines[i]);
                    free(lines[i]);
                }
            }
        } else {
            int col_width = (w - 4) / 2;
            int center_col_x = 2 + col_width;
            mvwprintw(win_playlist, status_line + 1, 2, "%s%s", ui_text("状态：", "State: "), status_msg);
            mvwprintw(win_playlist, status_line + 2, 2, "%s%s", ui_text("模式：", "Mode: "), get_play_mode_str());
            mvwprintw(win_playlist, status_line + 3, 2, "%s--", ui_text("标题：", "Title: "));
            mvwprintw(win_playlist, status_line + 4, 2, "%s--", ui_text("艺术家：", "Artist: "));
            mvwprintw(win_playlist, status_line + 5, 2, "%s--", ui_text("专辑：", "Album: "));
            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE && g_play_queue.count > 0) {
                char qbuf[64];
                snprintf(qbuf, sizeof(qbuf), "%s %d/%d",
                         menu_text("队列：", "Queue: "),
                         g_play_queue.current_position + 1, g_play_queue.count);
                mvwprintw(win_playlist, status_line + 5, 2, "%s", qbuf);
            }
            mvwprintw(win_playlist, status_line + 1, center_col_x, "%s--", ui_text("采样率：", "Sample Rate: "));
            mvwprintw(win_playlist, status_line + 2, center_col_x, "%s--", ui_text("位深：", "Bit Depth: "));
            mvwprintw(win_playlist, status_line + 3, center_col_x, "%s--", ui_text("比特率：", "Bitrate: "));
            mvwprintw(win_playlist, status_line + 4, center_col_x, "%s--", ui_text("编码：", "Codec: "));
        }
    }
    wrefresh(win_playlist);
}
