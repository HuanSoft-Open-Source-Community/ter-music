/**
 * @file playlist_render.c
 * @brief 播放列表内容渲染
 *
 * 从 ui.c 拆分，负责播放列表窗口的内容绘制，包括歌曲列表、
 * 搜索结果显示，以及独立的底部信息窗格与专辑封面。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "player/player.h"
#include "ui/ui.h"
#include "info/info.h"
#include "i18n/i18n.h"
#include "ui/menu_internal.h"
#include "ui/scrollbar.h"
#include "ui/info_bar_layout.h"
#include "ui/cover_art.h"
#include "ui/layout.h"
#include "playlist/playlist.h"
#include "search/search.h"
#include "config/config.h"
#include "logger/logger.h"
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
extern WINDOW *win_info;

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
    if (width < 0) {
        width = 0;
    }

    /* 有界截断：既遵守终端列宽，也绝不超过 dest 的字节容量。
     * util/utf8.c 的 utf8_str_truncate/utf8_str_pad 只按列宽工作、
     * 不知道 dest 大小；窗格可以很宽，这里必须自己守容量。 */
    size_t write = 0;
    int cols = 0;
    const char *cursor = input;
    while (*cursor && cols < width) {
        int char_width = 1;
        size_t char_len = utf8_next_char(cursor, NULL, &char_width);
        if (char_len == 0 || char_width < 0) {
            break;
        }
        if (cols + char_width > width) {
            break;
        }
        if (write + char_len + 1 > dest_size) {
            break;
        }
        unsigned char first = (unsigned char)cursor[0];
        if (char_len == 1 && (first < 0x20 || first == 0x7f)) {
            dest[write++] = ' ';
        } else {
            memcpy(dest + write, cursor, char_len);
            write += char_len;
        }
        cols += char_width;
        cursor += char_len;
    }

    if (*cursor != '\0' && cols + 3 <= width && write + 3 + 1 <= dest_size) {
        memcpy(dest + write, "...", 3);
        write += 3;
    }
    dest[write] = '\0';

    if (pad) {
        int current_width = utf8_str_width(dest);
        while (current_width < width && write + 1 < dest_size) {
            dest[write++] = ' ';
            current_width++;
        }
        dest[write] = '\0';
    }
}

/* 信息栏单行文本的容量。需同时容纳 label + 元数据值，并为 UTF-8
 * 裁剪和省略号留余量；实际可显示列数还会被 info_bar_layout 的列宽限制。 */
#define INFO_BAR_LINE_MAX 1024

/* 元数据可能含控制字符（例如标签里的换行）；信息栏是单行布局，
 * 统一替换为空格，避免写穿到下一行/边框。 */
static void info_bar_sanitize_controls(char *text)
{
    if (!text) return;
    for (char *p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            *p = ' ';
        }
    }
}

/* 在信息栏的一个分区矩形内输出一行文本：
 *   - 先按矩形宽度裁剪，再走 ui_pane_clip_print() 做窗口级兜底，
 *     任何情况下不触碰右边框；
 *   - line 是分区内相对行号，必须落在 [0, region->h)；
 *   - 分区矩形由 info_bar_layout_compute_rows() 给出，已通过 validate。 */
static void info_draw_line(const InfoBarRect *region, int line,
                           const char *text)
{
    if (!win_info || !region || region->w <= 0 ||
        line < 0 || line >= region->h) {
        return;
    }

    int row = region->y + line;
    int x = region->x;
    int width = region->w;

    int h, w;
    getmaxyx(win_info, h, w);
    if (row < 1 || row >= h - 1) return;
    if (x < 1 || x >= w - 1) return;

    /* 最后一个可写列是 w-2；把区间右端严格限制在 w-1 之前。 */
    if (x + width > w - 1) {
        width = w - 1 - x;
    }
    if (width <= 0) return;

    /* utf8_str_truncate 的列宽参数以字形列计，最坏每列 4 字节。 */
    int max_columns = (int)((INFO_BAR_LINE_MAX - 8) / 4);
    if (width > max_columns) {
        width = max_columns;
    }

    char line_text[INFO_BAR_LINE_MAX];
    snprintf(line_text, sizeof(line_text), "%s", text ? text : "");
    info_bar_sanitize_controls(line_text);

    char clipped[INFO_BAR_LINE_MAX];
    format_display_text(clipped, sizeof(clipped), line_text, width, 0);
    ui_pane_clip_print(win_info, row, x, clipped);
}

static const char *get_playlist_source_label(void)
{
    static char folder_path[MAX_PATH_LEN];
    if (playlist_has_multiple_sources()) {
        return i18n_get("player.mixed_queue");
    }
    playlist_copy_folder_path(folder_path, sizeof(folder_path));
    if (folder_path[0] != '\0') return folder_path;
    return i18n_get("general.none");
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
 * Playlist scroll offset
 * ============================================================ */

/* 计算滚动起点并钳制：选中索引保持在 [0, total_tracks-1]，
 * 起点保持在 [0, max(0, total_tracks - visible_lines)]，
 * 避免越界索引在列表尾部留下空白行。 */
static int clamp_playlist_start(int total_tracks, int current_selected, int visible_lines)
{
    if (visible_lines <= 0) return 0;
    if (current_selected < 0) current_selected = 0;
    if (total_tracks > 0 && current_selected >= total_tracks)
        current_selected = total_tracks - 1;

    int start = current_selected >= visible_lines
                ? current_selected - visible_lines + 1
                : 0;
    int max_start = total_tracks - visible_lines;
    if (max_start < 0) max_start = 0;
    if (start > max_start) start = max_start;
    return start;
}

int get_playlist_scroll_offset(void)
{
    if (!win_playlist) return 0;

    int h, w;
    getmaxyx(win_playlist, h, w);
    (void)w;

    int content_height = h - 2;
    int visible_lines = content_height;

    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        return clamp_playlist_start(player_queue_count(), g_queue_selected_index, visible_lines);
    } else if (g_search_state.active || g_search_state.in_progress) {
        int total = g_search_state.result_count;
        if (total < 0) total = 0;
        return clamp_playlist_start(total, g_search_state.selected_index, visible_lines);
    } else {
        int total = playlist_tree_is_active() ? playlist_visible_count() : playlist_count();
        return clamp_playlist_start(total, g_selected_index, visible_lines);
    }
}

/* ============================================================
 * Playlist content rendering
 * ============================================================ */

/* 把播放门面快照与“当前选中曲目”合并成信息窗格使用的轨道快照。
 * 播放中优先当前出声曲目；未播放时回退当前选中行，保持原有行为。 */
static void info_pane_track_snapshot(InfoTrack *out, int have_track,
                                     int selected_index, const Track *selected)
{
    memset(out, 0, sizeof(*out));

    const InfoTrack *playing = player_track();
    if (playing && playing->valid) {
        *out = *playing;
    } else if (have_track && selected) {
        out->valid = 1;
        out->index = selected_index;
        snprintf(out->path, sizeof(out->path), "%s", selected->path);
        snprintf(out->title, sizeof(out->title), "%s", selected->title);
        snprintf(out->artist, sizeof(out->artist), "%s", selected->artist);
        snprintf(out->album, sizeof(out->album), "%s", selected->album);
    }

    out->playlist_total = playlist_count();
    out->queue_count = player_queue_count();
    out->queue_position = player_queue_position();
}

static void info_pane_playback_snapshot(InfoPlayback *out)
{
    const InfoPlayback *snapshot = player_playback();
    if (snapshot) {
        *out = *snapshot;
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = (PlayState)player_play_state();
    out->play_mode = (PlayMode)player_play_mode();
    out->volume_percent = player_volume_percent();
    out->speed = player_speed();
}

/* 信息窗格元数据行：只使用 info_fields 中窗格可显示的字段。
 * 队列页且队列字段可用时，最后一行显示队列位置；否则回退专辑。 */
static int info_pane_fill_metadata(const InfoTrack *track,
                                   const InfoPlayback *playback,
                                   char rows[][INFO_BAR_LINE_MAX], int max_rows)
{
    const int mask = g_app_config.info_fields_mask;
    int count = 0;

#define ADD_PANE_FIELD(bit)                                                   \
    do {                                                                      \
        if (count < max_rows && (mask & (bit)) &&                             \
            info_render_field_line((bit), track, playback, rows[count],       \
                                   INFO_BAR_LINE_MAX)) {                      \
            count++;                                                          \
        }                                                                     \
    } while (0)

    ADD_PANE_FIELD(INFO_FIELD_STATE);
    ADD_PANE_FIELD(INFO_FIELD_MODE);
    ADD_PANE_FIELD(INFO_FIELD_TITLE);
    ADD_PANE_FIELD(INFO_FIELD_ARTIST);

    int tail_filled = 0;
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE &&
        (mask & INFO_FIELD_QUEUE) && count < max_rows) {
        if (info_render_field_line(INFO_FIELD_QUEUE, track, playback,
                                   rows[count], INFO_BAR_LINE_MAX)) {
            count++;
            tail_filled = 1;
        }
    }
    if (!tail_filled) {
        ADD_PANE_FIELD(INFO_FIELD_ALBUM);
    }

#undef ADD_PANE_FIELD
    return count;
}

/* 信息窗格音频行：format 字段控制整块 4 行（采样率/位深/比特率/编码）。 */
static int info_pane_fill_audio(const InfoTrack *track,
                                char rows[][INFO_BAR_LINE_MAX], int max_rows)
{
    if (!(g_app_config.info_fields_mask & INFO_FIELD_FORMAT) || max_rows < 4) {
        return 0;
    }

    char rate[32], depth[32], bitrate[32], codec[32];
    info_format_audio_fields_from(track ? track->sample_rate : 0,
                                  track ? track->bit_depth : 0,
                                  track ? track->bit_rate : 0,
                                  track ? track->codec : "",
                                  rate, sizeof(rate),
                                  depth, sizeof(depth),
                                  bitrate, sizeof(bitrate),
                                  codec, sizeof(codec));
    snprintf(rows[0], INFO_BAR_LINE_MAX, "%s%s",
             i18n_get("player.sample_rate"), rate);
    snprintf(rows[1], INFO_BAR_LINE_MAX, "%s%s",
             i18n_get("player.bit_depth"), depth);
    snprintf(rows[2], INFO_BAR_LINE_MAX, "%s%s",
             i18n_get("player.bitrate"), bitrate);
    snprintf(rows[3], INFO_BAR_LINE_MAX, "%s%s",
             i18n_get("player.codec"), codec);
    return 4;
}

void render_info_bar(void)
{
    if (!win_info || g_current_view != VIEW_MAIN) {
        return;
    }

    int h, w;
    getmaxyx(win_info, h, w);
    if (h < 1 || w < 1) {
        return;
    }
    ui_pane_begin(LAYOUT_PANE_INFO, win_info, i18n_get("hint.info"));
    if (h < 3 || w < 6) {
        ui_refresh_window(win_info);
        return;
    }

    int playlist_total = playlist_count();
    int snap_active = g_search_state.active;
    int snap_in_progress = g_search_state.in_progress;
    int tree_active = playlist_tree_is_active() &&
                      g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER &&
                      !snap_active && !snap_in_progress;

    Track selected_track;
    int have_track = 0;
    int selected_index = 0;
    memset(&selected_track, 0, sizeof(selected_track));
    if (playlist_total > 0) {
        int index = player_track_index() >= 0 ? player_track_index() : g_selected_index;
        if (g_sort_state.active && !snap_active && player_track_index() < 0) {
            index = g_sort_state.sorted_indices[g_selected_index];
        }
        if (tree_active && player_track_index() < 0) {
            int ti = get_visible_node_track_index(g_selected_index);
            if (ti >= 0) index = ti;
        }
        if (index < 0) index = 0;
        if (index >= playlist_total) index = playlist_total - 1;
        get_track_metadata(index, &selected_track);
        selected_index = index;
        have_track = 1;
    }

    InfoTrack track;
    info_pane_track_snapshot(&track, have_track, selected_index, &selected_track);
    InfoPlayback playback;
    info_pane_playback_snapshot(&playback);

    char metadata_lines[INFO_BAR_METADATA_LINES][INFO_BAR_LINE_MAX];
    char audio_lines[INFO_BAR_AUDIO_LINES][INFO_BAR_LINE_MAX];
    int metadata_count = info_pane_fill_metadata(&track, &playback,
                                                 metadata_lines,
                                                 INFO_BAR_METADATA_LINES);
    int audio_count = info_pane_fill_audio(&track, audio_lines,
                                           INFO_BAR_AUDIO_LINES);

    /* 封面：播放设置的总开关 + 信息显示的分开关同时开启才请求。
     * 字符封面的尺寸/字符集仍由 CLI/D-Bus 路径使用，TUI 用彩色半块。 */
    int cover_requested = g_app_config.info_show_cover &&
                          g_app_config.show_album_cover;
    char cover_path[MAX_PATH_LEN];
    int want_cover = 0;
    if (have_track && cover_requested &&
        player_cover_path(cover_path, sizeof(cover_path)) == 0) {
        want_cover = 1;
    }

    /* 自适应分区：横向（元数据 | 音频 | 封面）或纵向（封面在上、元数据/
     * 音频在下）。窗口太小时模型 valid=0，只保留边框/标题。 */
    InfoBarLayout layout;
    info_bar_layout_compute_rows(w, h, want_cover, metadata_count, audio_count,
                                 &layout);
    if (!layout.valid || !info_bar_layout_validate(w, h, &layout)) {
        log_error("info_bar", "Invalid info-pane geometry (%dx%d); skipping content",
                  w, h);
        ui_refresh_window(win_info);
        return;
    }

    const CoverArtGrid *cover_grid = NULL;
    if (want_cover && layout.cover.w > 0) {
        cover_grid = cover_art_get(COVER_ART_SLOT_INFO, cover_path,
                                   layout.cover.w, layout.cover.h);
        if (!cover_grid) {
            /* 解码失败：本帧退化为无封面布局，把封面空间还给文字。 */
            info_bar_layout_compute_rows(w, h, 0, metadata_count, audio_count,
                                         &layout);
            if (!layout.valid || !info_bar_layout_validate(w, h, &layout)) {
                ui_refresh_window(win_info);
                return;
            }
        }
    }

    for (int i = 0; i < layout.metadata_rows && i < metadata_count; i++) {
        info_draw_line(&layout.metadata, i, metadata_lines[i]);
    }
    for (int i = 0; i < layout.audio_rows && i < audio_count; i++) {
        info_draw_line(&layout.audio, i, audio_lines[i]);
    }

    // 专辑封面（彩色半块）：逐单元设置前景/背景颜色，绘制范围严格限制
    // 在模型给出的封面矩形内，随后由边框重绘兜底。
    if (cover_grid) {
        cover_art_draw(win_info, layout.cover.x, layout.cover.y,
                       cover_grid, ui_pane_body_attr(LAYOUT_PANE_INFO));
    }

    log_debug("info_bar",
              "mask=0x%04x metadata=%d audio=%d cover_requested=%d cover_drawn=%d",
              g_app_config.info_fields_mask, layout.metadata_rows,
              layout.audio_rows, cover_requested, cover_grid ? 1 : 0);

    /* 内容画完后最后补一遍边框，防止任何字形溢出盖住边框。 */
    ui_pane_repaint_border(LAYOUT_PANE_INFO, win_info);
    ui_refresh_window(win_info);
}

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

    int h, w;
    getmaxyx(win_playlist, h, w);
    if (h < 1 || w < 1) {
        return;
    }

    char title_buf[128];
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        snprintf(title_buf, sizeof(title_buf), " %s [%s] ",
                 i18n_get("player.play_queue"),
                 player_play_mode_name(0));
    } else if (snap_in_progress) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d/%d)",
                 i18n_get("playlist.search_progress"), snap_progress, playlist_count());
    } else if (snap_active) {
        snprintf(title_buf, sizeof(title_buf), "%s (%d %s)",
                 i18n_get("playlist.search_title"), snap_count,
                 i18n_get("playlist.results_unit"));
    } else {
        snprintf(title_buf, sizeof(title_buf), "%s", i18n_get("controls.playlist"));
    }
    ui_pane_begin(LAYOUT_PANE_PLAYLIST, win_playlist, title_buf);
    if (h < 3 || w < 12) {
        ui_refresh_window(win_playlist);
        return;
    }
    int content_height = h - 2;
    int playlist_total = playlist_count();
    int playlist_loaded = playlist_is_loaded();

    int tree_active = playlist_tree_is_active() &&
                      g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER &&
                      !snap_active && !snap_in_progress;

    int total_tracks;
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        total_tracks = player_queue_count();
    } else if ((snap_active || snap_in_progress) && snap_count > 0) {
        total_tracks = snap_count;
    } else if (snap_in_progress) {
        total_tracks = 0;
    } else {
        if (tree_active) {
            total_tracks = playlist_visible_count();
        } else {
            total_tracks = playlist_total;
        }
    }

    if (!playlist_loaded) {
        char display_path[MAX_PATH_LEN];
        format_display_text(display_path, sizeof(display_path), get_playlist_source_label(), w - 10, 0);
        ui_pane_clip_printf(win_playlist, h/2 - 1, 2, "%s", i18n_get("playlist.empty"));
        ui_pane_clip_printf(win_playlist, h/2, 2, "%s",
                            i18n_get("playlist.press_o_i"));
        ui_pane_clip_printf(win_playlist, h/2 + 1, 2, "%s%s", i18n_get("player.path"), display_path);
    } else {
        int start_idx = 0;
        int visible_lines = content_height;

        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
            if (total_tracks > 0 && g_queue_selected_index >= total_tracks)
                g_queue_selected_index = total_tracks - 1;
            if (g_queue_selected_index < 0) g_queue_selected_index = 0;
            start_idx = clamp_playlist_start(total_tracks, g_queue_selected_index, visible_lines);
        } else if (snap_active || snap_in_progress) {
            if (total_tracks > 0 && snap_selected >= total_tracks)
                snap_selected = total_tracks - 1;
            if (snap_selected < 0) snap_selected = 0;
            start_idx = clamp_playlist_start(total_tracks, snap_selected, visible_lines);
        } else {
            if (total_tracks > 0 && g_selected_index >= total_tracks)
                g_selected_index = total_tracks - 1;
            if (g_selected_index < 0) g_selected_index = 0;
            start_idx = clamp_playlist_start(total_tracks, g_selected_index, visible_lines);
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

        /* 队列视图：行数据直接取核心队列页（player_queue_page），保证
         * Shuffle/InsertAfter 之后界面行与核心实际持有的条目一致，而不是
         * 再绕回内容列表按物理下标猜。仅服务当前可见区间，越界行回退到
         * queue_index_at + get_track_metadata。 */
        BackendQueueEntry *queue_page = NULL;
        int queue_page_count = 0;
        if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE &&
            total_tracks > 0 && visible_lines > 0 && start_idx < total_tracks) {
            int want = visible_lines;
            if (start_idx + want > total_tracks) {
                want = total_tracks - start_idx;
            }
            queue_page = malloc(sizeof(BackendQueueEntry) * (size_t)want);
            if (queue_page) {
                queue_page_count = player_queue_page(start_idx, want, queue_page, want);
            }
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
                    ui_pane_clip_printf(win_playlist, i + 1, 2, "%s", truncated);
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
                    int is_now_playing = (actual_idx == player_track_index() &&
                                          player_play_state() != PLAY_STATE_STOPPED);
                    int attrs = A_NORMAL;
                    if (is_selected) attrs |= A_REVERSE;
                    if (is_now_playing) attrs |= A_BOLD;
                    if (attrs != A_NORMAL) wattron(win_playlist, attrs);
                    ui_pane_clip_printf(win_playlist, i + 1, 2, "%s %s %s",
                              indent, truncated_title, truncated_artist);
                    if (attrs != A_NORMAL) wattroff(win_playlist, attrs);
                }
            } else {
                /* ── Flat mode rendering ── */
                int prefix_width = 0;
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                    prefix_width = 5;

            if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                int row = idx - start_idx;
                if (queue_page && row >= 0 && row < queue_page_count) {
                    /* 直接用核心队列页的条目渲染这一行 */
                    memset(&t, 0, sizeof(t));
                    snprintf(t.title, sizeof(t.title), "%s", queue_page[row].title);
                    snprintf(t.artist, sizeof(t.artist), "%s", queue_page[row].artist);
                    snprintf(t.album, sizeof(t.album), "%s", queue_page[row].album);
                    snprintf(t.path, sizeof(t.path), "%s", queue_page[row].path);
                    if (queue_page[row].is_cue) {
                        /* 让下面的 [CUE] 前缀判定成立（页缓存不单独带 cue_offset） */
                        t.cue_offset = 1;
                        t.cue_track_number = 1;
                    }
                } else {
                    actual_idx = player_queue_index_at(idx);
                    get_track_metadata(actual_idx, &t);
                }
            } else if (snap_active || snap_in_progress) {
                actual_idx = snap_indices[idx];
                get_track_metadata(actual_idx, &t);
            } else if (g_sort_state.active) {
                actual_idx = g_sort_state.sorted_indices[idx];
                get_track_metadata(actual_idx, &t);
            } else {
                get_track_metadata(actual_idx, &t);
            }

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
                is_now_playing = (idx == player_queue_position() &&
                                  player_play_state() != PLAY_STATE_STOPPED);
            } else if (snap_active || snap_in_progress) {
                is_selected = (idx == snap_selected);
            } else {
                is_selected = (idx == g_selected_index && g_control_focus == 0);
                is_now_playing = (actual_idx == player_track_index() &&
                                  player_play_state() != PLAY_STATE_STOPPED);
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
                ui_pane_clip_printf(win_playlist, i + 1, 1, " %s %s %s ", prefix,
                          truncated_title, truncated_artist);
            } else {
                if (is_selected || attrs != A_NORMAL)
                    ui_pane_clip_printf(win_playlist, i + 1, 1, " %s %s ",
                              truncated_title, truncated_artist);
                else
                    ui_pane_clip_printf(win_playlist, i + 1, 2, "%s %s",
                              truncated_title, truncated_artist);
            }
            if (attrs != A_NORMAL) wattroff(win_playlist, attrs);
            } /* end flat mode */
        }

        free(queue_page);
        queue_page = NULL;
        if (total_tracks == 0) {
            if (snap_in_progress) {
                ui_pane_clip_printf(win_playlist, 1, 2, "%s", i18n_get("playlist.searching"));
            } else if (snap_active) {
                ui_pane_clip_printf(win_playlist, 1, 2, "%s", i18n_get("playlist.no_match"));
            } else {
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                    ui_pane_clip_printf(win_playlist, 1, 2, "%s", i18n_get("playlist.queue_empty"));
                else
                    ui_pane_clip_printf(win_playlist, 1, 2, "%s", i18n_get("playlist.no_audio"));
            }
        }

        // Scrollbar
        {
            int sb_col = w - 2;
            scrollbar_draw(win_playlist, 1, visible_lines, total_tracks, visible_lines, start_idx, sb_col);
        }

    }
    ui_refresh_window(win_playlist);
    render_info_bar();
}
