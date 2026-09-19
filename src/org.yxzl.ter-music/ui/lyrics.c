/**
 * @file lyrics.c
 * @brief 歌词面板渲染（前端）
 *
 * 歌词数据由后端持有（`lyrics/lyrics.c`）：本文件只负责把**当前状态画出来**
 * ——无歌词时的频谱动画、歌词行渲染、滚动条、光标模式下的跳转高亮。
 *
 * 后端状态经 `lyrics/lyrics.h` 的只读接口获取：
 *   - `g_lyrics`（行数据与高亮行，读时持有 `g_lyrics.lock`）；
 *   - `lyrics_tick()` 由 `core_tick()` 每轮推进，界面经 core 的状态监听重绘。
 *
 * 界面私有状态：`g_lyric_cursor_mode` / `g_lyric_cursor_index`（光标模式与
 * 跳转位置），后端不认识它们。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/visualizer.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "lyrics/lyrics.h"
#include "player/player.h"
#include "ui/ui.h"
#include "ui/lyrics.h"
#include "ui/menu_internal.h"
#include "ui/scrollbar.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 外部窗口变量声明
extern WINDOW *win_lyrics;

/* ASCII 回退渲染用的净化（实现见文件末尾附近） */
static void sanitize_ascii_lyric(char *dest, size_t dest_size, const char *src);

static uint64_t lyric_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int lyric_glyph_width(const char *glyph) {
    int width = utf8_str_width(glyph);
    return width > 0 ? width : 1;
}

static void draw_lyric_glyph(int row, int col, const char *glyph, int attrs) {
    if (!win_lyrics || !glyph || glyph[0] == '\0') {
        return;
    }

    int h, w;
    getmaxyx(win_lyrics, h, w);

    int glyph_width = lyric_glyph_width(glyph);
    if (row <= 0 || row >= h - 1 || col <= 1 || col + glyph_width > w - 1) {
        return;
    }

    if (attrs != 0) {
        wattron(win_lyrics, attrs);
    }
    mvwprintw(win_lyrics, row, col, "%s", glyph);
    if (attrs != 0) {
        wattroff(win_lyrics, attrs);
    }
}

static const char *pick_disc_glyph(double diff, double angle, int pulse_phase) {
    static const char *ascii_idle[] = {".", "o", "x", "+", "a", "m", "n", "z"};
    static const char *ascii_hot[] = {"*", "#", "@", "%", "M", "U", "S", "I", "C"};
    static const char *unicode_idle[] = {"·", "•", "◦", "○", "◇", "A", "M", "N", "Z", "♪"};
    static const char *unicode_hot[] = {"✦", "✧", "✶", "✹", "♪", "♬", "♩", "M", "U", "S", "I", "C"};

    const char **idle = use_ascii_fallback_ui() ? ascii_idle : unicode_idle;
    const char **hot = use_ascii_fallback_ui() ? ascii_hot : unicode_hot;
    int idle_count = use_ascii_fallback_ui() ? (int)(sizeof(ascii_idle) / sizeof(ascii_idle[0]))
                                             : (int)(sizeof(unicode_idle) / sizeof(unicode_idle[0]));
    int hot_count = use_ascii_fallback_ui() ? (int)(sizeof(ascii_hot) / sizeof(ascii_hot[0]))
                                            : (int)(sizeof(unicode_hot) / sizeof(unicode_hot[0]));

    int angle_bucket = (int)lround(((angle + M_PI) / (M_PI * 2.0)) * 16.0);
    if (diff < 0.18) {
        return hot[(pulse_phase + angle_bucket) % hot_count];
    }
    if (diff < 0.42) {
        return hot[(pulse_phase / 2 + angle_bucket / 2) % hot_count];
    }
    return idle[(pulse_phase / 3 + angle_bucket) % idle_count];
}

static const char *pick_bar_glyph(double angle, int step, int extent, int pulse_phase) {
    static const char *ascii_body[] = {"|", "/", "\\", ":", "!", "+", "="};
    static const char *ascii_tip[] = {"*", "#", "@", "M", "U", "S", "I", "C"};
    static const char *unicode_body[] = {"·", "•", "╎", "╏", "│", "┆", "┊", "♪"};
    static const char *unicode_tip[] = {"✦", "✧", "✶", "✹", "♪", "♬", "♩", "♫", "M", "U", "S", "I", "C"};

    const char **body = use_ascii_fallback_ui() ? ascii_body : unicode_body;
    const char **tip = use_ascii_fallback_ui() ? ascii_tip : unicode_tip;
    int body_count = use_ascii_fallback_ui() ? (int)(sizeof(ascii_body) / sizeof(ascii_body[0]))
                                             : (int)(sizeof(unicode_body) / sizeof(unicode_body[0]));
    int tip_count = use_ascii_fallback_ui() ? (int)(sizeof(ascii_tip) / sizeof(ascii_tip[0]))
                                            : (int)(sizeof(unicode_tip) / sizeof(unicode_tip[0]));

    double normalized = fmod(angle + (M_PI * 2.0), M_PI * 2.0);
    if (normalized < 0) {
        normalized += M_PI * 2.0;
    }

    int angle_bucket = (int)lround((normalized / (M_PI * 2.0)) * 12.0);
    if (step == extent - 1) {
        return tip[(pulse_phase + angle_bucket) % tip_count];
    }
    return body[(pulse_phase / 2 + angle_bucket + step) % body_count];
}

static int use_emoji_no_lyrics_title(void) {
    return !use_ascii_fallback_ui() && utf8_str_width("🎵") >= 2;
}

static void render_no_lyrics_spectrum(int h, int w) {
    if (h < 12 || w < 24) {
        mvwprintw(win_lyrics, h / 2, 2, "%s", i18n_get("lyrics.not_available"));
        return;
    }

    // 自身节流：距上次调用不足 100ms 则跳过（外层已有节流，双保险）
    static uint64_t last_render_ms = 0;
    uint64_t now_ms = lyric_now_ms();
    if (now_ms - last_render_ms < 100) return;
    last_render_ms = now_ms;

    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    player_visualizer(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);

    double spin = (double)(now_ms % 5000ULL) / 5000.0;
    double highlight_angle = (spin * M_PI * 2.0) - (M_PI / 2.0);
    int pulse_phase = (int)((now_ms / 180ULL) % 24ULL);

    int center_y = h / 2 - 1;
    int center_x = w / 2;
    double x_scale = 1.8;
    int ring_radius = (h - 8) / 2;
    int width_radius = (w - 10) / 4;
    if (width_radius < ring_radius) {
        ring_radius = width_radius;
    }
    if (ring_radius < 4) {
        ring_radius = 4;
    }

    int disc_outer = ring_radius - 1;
    int disc_inner = disc_outer / 2;
    int max_bar_len = ring_radius / 2;
    if (max_bar_len < 2) {
        max_bar_len = 2;
    }

    // 大窗口下隔行渲染以降低 CPU 负载
    int step = (h > 30) ? 2 : 1;
    for (int row = 1; row < h - 1; row += step) {
        for (int col = 2; col < w - 2; col += step) {
            double dx = ((double)col - (double)center_x) / x_scale;
            double dy = (double)row - (double)center_y;
            double dist = sqrt(dx * dx + dy * dy);

            if (dist > disc_inner && dist <= disc_outer) {
                double angle = atan2(dy, dx);
                double diff = fabs(angle - highlight_angle);
                if (diff > M_PI) {
                    diff = (2.0 * M_PI) - diff;
                }
                draw_lyric_glyph(row, col, pick_disc_glyph(diff, angle, pulse_phase), diff < 0.18 ? A_BOLD : 0);
            } else if (dist <= disc_inner - 0.4) {
                mvwaddch(win_lyrics, row, col, ' ');
            }
        }
    }

    int band_count = VISUALIZER_BAND_COUNT;
    int max_band_count = (int)(disc_outer * 5.0);
    if (band_count > max_band_count) {
        band_count = max_band_count;
    }
    if (band_count < 16) {
        band_count = 16;
    }

    int is_idle = (last_update_ms == 0) || (now_ms > last_update_ms + 260ULL);
    if (player_play_state() == PLAY_STATE_STOPPED) {
        is_idle = 1;
    }

    for (int i = 0; i < band_count; i++) {
        int src = (i * VISUALIZER_BAND_COUNT) / band_count;
        if (src >= VISUALIZER_BAND_COUNT) {
            src = VISUALIZER_BAND_COUNT - 1;
        }

        int level = levels[src];
        if (is_idle) {
            double wave = sin((spin * M_PI * 6.0) + (double)i * 0.42);
            level = 16 + (int)(14.0 * (wave + 1.0));
            if (player_play_state() == PLAY_STATE_STOPPED) {
                level /= 2;
            }
        }

        int extent = 1 + (level * max_bar_len) / 100;
        if (extent < 1) {
            extent = 1;
        }

        double angle = (-M_PI / 2.0) + ((2.0 * M_PI * (double)i) / (double)band_count);
        const char *bar_glyph = pick_bar_glyph(angle, 0, extent, pulse_phase + i);

        for (int step = 0; step < extent; step++) {
            double radius = (double)disc_outer + 1.0 + (double)step;
            int row = (int)lround((double)center_y + sin(angle) * radius);
            int col = (int)lround((double)center_x + cos(angle) * radius * x_scale);
            if (row <= 0 || row >= h - 1 || col <= 1 || col >= w - 1) {
                continue;
            }
            bar_glyph = pick_bar_glyph(angle, step, extent, pulse_phase + i);
            draw_lyric_glyph(row, col, bar_glyph, step == extent - 1 ? A_BOLD : 0);
        }
    }

    const char *title = use_emoji_no_lyrics_title()
        ? i18n_get("lyrics.no_lyrics_emoji")
        : i18n_get("lyrics.no_lyrics");
    const char *subtitle = use_emoji_no_lyrics_title()
        ? i18n_get("lyrics.particle_emoji")
        : i18n_get("lyrics.particle_text");
    int title_col = center_x - utf8_str_width(title) / 2;
    int subtitle_col = center_x - utf8_str_width(subtitle) / 2;
    int text_row = center_y + disc_outer + max_bar_len + 1;

    if (text_row < h - 2) {
        if (title_col < 2) {
            title_col = 2;
        }
        mvwprintw(win_lyrics, text_row, title_col, "%s", title);
    }
    if (text_row + 1 < h - 1) {
        if (subtitle_col < 2) {
            subtitle_col = 2;
        }
        mvwprintw(win_lyrics, text_row + 1, subtitle_col, "%s", subtitle);
    }
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

static const char *lyric_spectrum_glyph(int units) {
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

static int render_corner_spectrum(int h, int w) {
    int spectrum_height = get_corner_spectrum_height(h);
    int graph_top = 1;
    int graph_bottom = graph_top + spectrum_height - 1;
    if (graph_bottom >= h - 2 || w < 16) {
        return 1;
    }

    int graph_width = w - 4;
    int levels[VISUALIZER_BAND_COUNT] = {0};
    int peaks[VISUALIZER_BAND_COUNT] = {0};
    uint64_t last_update_ms = 0;
    player_visualizer(levels, peaks, VISUALIZER_BAND_COUNT, &last_update_ms);
    (void)peaks;

    uint64_t now_ms = lyric_now_ms();
    int inactive_decay = 0;
    int is_visualizer_active = 0;
    if (last_update_ms > 0 && now_ms > last_update_ms) {
        inactive_decay = (int)((now_ms - last_update_ms) / 90ULL);
        if ((now_ms - last_update_ms) < 250ULL &&
            (player_play_state() == PLAY_STATE_PLAYING || player_play_state() == PLAY_STATE_PAUSED)) {
            is_visualizer_active = 1;
        }
    }

    static int *column_units = NULL;
    static int column_units_cap = 0;
    if (graph_width > column_units_cap) {
        int *new_buf = realloc(column_units, (size_t)graph_width * sizeof(int));
        if (!new_buf) return 1;
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

        int units = (level * spectrum_height * 8 + 99) / 100;
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
            if (!new_buf) return 1;
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

    for (int row = graph_top; row <= graph_bottom; row++) {
        mvwhline(win_lyrics, row, 1, ' ', w - 2);
    }

    for (int col = 0; col < graph_width; col++) {
        for (int row = graph_bottom; row >= graph_top; row--) {
            int row_from_bottom = graph_bottom - row;
            int units = column_units[col] - row_from_bottom * 8;
            if (units < 0) {
                units = 0;
            }
            if (units > 8) {
                units = 8;
            }
            mvwaddstr(win_lyrics, row, 2 + col, lyric_spectrum_glyph(units));
        }
    }

    int separator_row = graph_bottom + 1;
    if (separator_row < h - 1) {
        for (int x = 1; x < w - 1; x++)
            mvwaddstr(win_lyrics, separator_row, x, "\xe2\x94\x80"); /* ─ */
        mvwaddstr(win_lyrics, separator_row, 0, "\xe2\x94\x82");     /* │ */
        mvwaddstr(win_lyrics, separator_row, w - 1, "\xe2\x94\x82"); /* │ */
        return separator_row + 1;
    }

    return graph_bottom + 1;
}

/* 按后端当前高亮行重绘。推进由 lyrics_tick()（core_tick 每轮）完成，
 * 本函数只把结果画出来——这就是“后端推进、前端渲染”的分界。 */
/**
 * 渲染单行歌词
 * @param row 行号（窗口内坐标）
 * @param text 歌词文本
 * @param is_highlighted 是否高亮
 * @param show_marker 是否显示 ">" 标记
 */
static void render_lyric_line(int row, const char *text, int is_highlighted, int show_marker) {
    int h, w;
    getmaxyx(win_lyrics, h, w);

    // 检查窗口尺寸是否有效
    if (h <= 2 || w <= 4) {
        return;
    }

    // 检查行号是否有效
    if (row < 1 || row >= h - 1) {
        return;
    }

    // 计算最大可用宽度（减去边框和缩进，再减1列为滚动条预留空间）
    int max_width = w - 5;
    if (max_width <= 0) {
        return;
    }
    if (show_marker) {
        max_width -= 2;  // 为 "> " 预留空间
        if (max_width <= 0) {
            return;
        }
    }
    
    char ascii_text[MAX_LYRIC_TEXT_LEN];
    const char *display_src = text ? text : "";
    if (use_ascii_fallback_ui()) {
        sanitize_ascii_lyric(ascii_text, sizeof(ascii_text), display_src);
        display_src = ascii_text;
    }

    // 计算文本实际宽度
    int text_width = utf8_str_width(display_src);
    
    // 确定起始列偏移（用于水平滚动）
    static time_t last_scroll_time = 0;
    static int scroll_offset = 0;
    time_t now = time(NULL);
    
    // 只对高亮且超长的文本启用滚动
    int use_scrolling = is_highlighted && text_width > max_width;
    char display_text[MAX_LYRIC_TEXT_LEN];
    
    if (use_scrolling) {
        // 每 1 秒更新一次偏移量
        if (now != last_scroll_time) {
            scroll_offset++;
            // 当完全滚出后重置
            if (scroll_offset > text_width - max_width + 3) {  // +3 为了显示省略号
                scroll_offset = 0;
            }
            last_scroll_time = now;
        }
        
        // 使用偏移量截取文本
        utf8_str_substring(display_text, display_src, scroll_offset, max_width);
    } else {
        // 非高亮或文本不超长，使用普通截断
        utf8_str_truncate(display_text, display_src, max_width);
    }
    
    // 计算对齐偏移（仅在非滚动模式下生效）
    int align_offset = 0;
    if (!use_scrolling && g_app_config.lyrics_alignment != 0) {
        int display_width = utf8_str_width(display_text);
        int padding = max_width - display_width;
        if (padding > 0) {
            if (g_app_config.lyrics_alignment == 1) {         // 居中
                align_offset = padding / 2;
            } else if (g_app_config.lyrics_alignment == 2) {  // 居右
                align_offset = padding;
            }
        }
    }

    // 应用高亮并显示
    if (is_highlighted) {
        wattron(win_lyrics, A_REVERSE);
        if (show_marker) {
            mvwprintw(win_lyrics, row, 2, ">");
            mvwprintw(win_lyrics, row, 4 + align_offset, "%s", display_text);
        } else {
            // 第二行高亮，不显示标记，缩进对齐
            mvwprintw(win_lyrics, row, 3 + align_offset, "%s", display_text);
        }
        wattroff(win_lyrics, A_REVERSE);
    } else {
        // 普通行，使用默认颜色对
        mvwprintw(win_lyrics, row, 3 + align_offset, "%s", display_text);
    }
}

/* ── Embedded lyrics extraction ── */

/**
 * @brief Extract lyrics embedded in audio file metadata.
 *
 * Searches FFmpeg AVDictionary first, then APE tags.
 * FFmpeg maps:
 *   - MP3 ID3v2 USLT    → key "lyrics"
 *   - FLAC Vorbis Comment → key "LYRICS" / "lyrics"
 *   - M4A/MP4 iTunes     → key "\xa9lyr" / "lyrics"
 * APE tag:
 *   - APE/WV/MP3 etc     → key "LYRICS"
 *
 * @param audio_path  Path to the audio file.
 * @return 0 on success (lyrics found), -1 on failure.
 */
void update_lyrics_display(void) {
    if (g_current_view != VIEW_MAIN) {
        return;
    }

    /* 纯文本歌词（无时间戳）没有“推进”，但需要一次首帧渲染 */
    int has_timestamps = 0;
    int has_lyrics = lyrics_highlight(NULL, NULL, &has_timestamps, NULL);
    if (!has_lyrics) {
        return;
    }
    if (!has_timestamps) {
        render_lyrics();
    }
}

static void sanitize_ascii_lyric(char *dest, size_t dest_size, const char *src) {
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

    for (size_t read = 0; src[read] != '\0' && write + 1 < dest_size; read++) {
        unsigned char c = (unsigned char)src[read];

        if (c < 0x80) {
            if (isspace(c)) {
                if (!prev_space) {
                    dest[write++] = ' ';
                    prev_space = 1;
                }
            } else if (isprint(c)) {
                dest[write++] = (char)c;
                prev_space = 0;
            }
        } else {
            saw_non_ascii = 1;
            if (!prev_space && write + 1 < dest_size) {
                dest[write++] = ' ';
                prev_space = 1;
            }
        }
    }

    while (write > 0 && dest[write - 1] == ' ') {
        write--;
    }
    dest[write] = '\0';

    if (write == 0 && saw_non_ascii) {
        snprintf(dest, dest_size, "[non-ASCII]");
    }
}


void render_lyrics(void) {
    if (!win_lyrics || !g_app_config.show_lyrics_panel) {
        return;
    }

    int h, w;
    getmaxyx(win_lyrics, h, w);

    werase(win_lyrics);
    wattron(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));

    rounded_box(win_lyrics);
    if (g_lyric_cursor_mode) {
        mvwprintw(win_lyrics, 0, 2, "%s", i18n_get("controls.lyrics_seek"));
    } else {
        mvwprintw(win_lyrics, 0, 2, "%s", i18n_get("controls.lyrics"));
    }

    wattroff(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));
    wbkgd(win_lyrics, COLOR_PAIR(COLOR_PAIR_LYRICS));

    /* 歌词数据归后端：这里只取当前高亮行与总行数，逐行内容按需取。
     * 光标（跳转模式）是界面私有状态，后端不认识。 */
    int highlight = -1;
    int has_lyrics = player_lyrics_highlight(&highlight, NULL, NULL);
    int total = player_lyrics_total();

    if (!has_lyrics || total <= 0) {
        render_no_lyrics_spectrum(h, w);
        wrefresh(win_lyrics);
        return;
    }

    int content_top = render_corner_spectrum(h, w);

    if (highlight < 0) {
        int message_row = content_top + ((h - content_top - 1) / 2);
        if (message_row >= h - 1) {
            message_row = h - 2;
        }
        mvwprintw(win_lyrics, message_row, 2, "%s", i18n_get("lyrics.playing"));
        wrefresh(win_lyrics);
        return;
    }

    int visible_lines = h - content_top - 1;
    if (visible_lines <= 0) {
        wrefresh(win_lyrics);
        return;
    }

    int current_center_idx = (g_lyric_cursor_mode && g_lyric_cursor_index >= 0)
        ? g_lyric_cursor_index : highlight;

    int start_idx = current_center_idx - (visible_lines / 2);
    if (start_idx < 0) start_idx = 0;
    if (start_idx + visible_lines > total) {
        start_idx = total - visible_lines;
    }
    if (start_idx < 0) start_idx = 0;

    for (int i = 0; i < visible_lines && (start_idx + i) < total; i++) {
        int lyric_idx = start_idx + i;
        int row = content_top + i;

        LyricLine line;
        if (player_lyrics_line_at(lyric_idx, &line) != 0) {
            break;
        }

        int is_highlighted;
        int show_marker;

        if (g_lyric_cursor_mode) {
            is_highlighted = (lyric_idx == g_lyric_cursor_index);
            show_marker = (lyric_idx == g_lyric_cursor_index);
        } else {
            int highlight_count = player_lyrics_highlight_count();
            if (highlight_count < 1) highlight_count = 1;
            is_highlighted = (lyric_idx >= highlight &&
                              lyric_idx < highlight + highlight_count);
            show_marker = (lyric_idx == highlight);
        }

        render_lyric_line(row, line.text, is_highlighted, show_marker);
    }

    scrollbar_draw(win_lyrics, content_top, visible_lines,
                   total, visible_lines, start_idx, w - 2);

    wrefresh(win_lyrics);
}
