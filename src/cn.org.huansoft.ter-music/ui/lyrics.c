/**
 * @file lyrics.c
 * @brief 歌词面板渲染（前端）
 *
 * 歌词数据由后端持有（`lyrics/lyrics.c`）：本文件只负责把**当前状态画出来**
 * ——歌词行渲染、滚动条、光标模式下的跳转高亮。无歌词时的黑胶/粒子动画
 * 统一由 ui/vinyl.c 提供（独立 vinyl 窗格与这里的回退共用同一套绘制）。
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
#include "ui/layout.h"
#include "ui/lyrics.h"
#include "ui/menu_internal.h"
#include "ui/vinyl.h"
#include "ui/scrollbar.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 外部窗口变量声明
extern WINDOW *win_lyrics;

/* ASCII 回退渲染用的净化（实现见文件末尾附近） */
static void sanitize_ascii_lyric(char *dest, size_t dest_size, const char *src);

/* 返回 src 前 chars 个 UTF-8 字符的字节偏移（不足则到字符串结尾）。 */
static size_t ui_utf8_prefix_bytes(const char *src, int chars)
{
    if (!src || chars <= 0) {
        return 0;
    }
    size_t pos = 0;
    int count = 0;
    while (src[pos] != '\0' && count < chars) {
        unsigned char c = (unsigned char)src[pos];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        for (size_t i = 1; i < len; i++) {
            if (src[pos + i] == '\0' || ((unsigned char)src[pos + i] & 0xC0) != 0x80) {
                len = 1;
                break;
            }
        }
        pos += len;
        count++;
    }
    return pos;
}

/* 按后端当前高亮行重绘。推进由 lyrics_tick()（core_tick 每轮）完成，
 * 本函数只把结果画出来——这就是“后端推进、前端渲染”的分界。 */
/**
 * 渲染单行歌词
 * @param row 行号（窗口内坐标）
 * @param text 歌词文本
 * @param is_highlighted 是否高亮
 * @param show_marker 是否显示 ">" 标记
 * @param sung_chars 当前行已唱字数（仅主当前行有意义）
 * @param char_total 当前行总字数（0 = 无逐字信息，保持整行高亮）
 */
/* 歌词元数据可能夹带换行/制表符；单行窗格里必须压成空格，否则
 * ncurses 会写到下一行、越过窗格边框。 */
static void sanitize_lyric_controls(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    size_t write = 0;
    for (const unsigned char *cursor = (const unsigned char *)(src ? src : "");
         *cursor != '\0' && write + 1 < dest_size; cursor++) {
        dest[write++] = (*cursor < 0x20 || *cursor == 0x7f) ? ' ' : (char)*cursor;
    }
    dest[write] = '\0';
}

static void render_lyric_line(int row, const char *text, int is_highlighted,
                              int show_marker, int sung_chars, int char_total) {
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
    char clean_text[MAX_LYRIC_TEXT_LEN];
    sanitize_lyric_controls(clean_text, sizeof(clean_text), display_src);
    display_src = clean_text;

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

    /* 逐字高亮：整行放得下时才拆分（超长行仍走截断/滚动 + 整行反显，
     * 避免可见窗口与字符时间映射互相干扰）；ASCII 回退无法按码点映射，
     * 也退回整行高亮。 */
    int use_karaoke = is_highlighted && char_total > 0 &&
                      !use_ascii_fallback_ui() && text_width <= max_width;
    if (use_karaoke) {
        if (sung_chars < 0) {
            sung_chars = 0;
        }
        if (sung_chars > char_total) {
            sung_chars = char_total;
        }

        size_t prefix_len = ui_utf8_prefix_bytes(display_text, sung_chars);
        char prefix[MAX_LYRIC_TEXT_LEN];
        if (prefix_len >= sizeof(prefix)) {
            prefix_len = sizeof(prefix) - 1;
        }
        memcpy(prefix, display_text, prefix_len);
        prefix[prefix_len] = '\0';
        const char *suffix = display_text + prefix_len;

        int text_col = (show_marker ? 4 : 3) + align_offset;
        if (show_marker) {
            ui_pane_clip_printf(win_lyrics, row, 2, ">");
        }
        if (prefix_len > 0) {
            wattron(win_lyrics, A_REVERSE | A_BOLD);
            ui_pane_clip_printf(win_lyrics, row, text_col, "%s", prefix);
            wattroff(win_lyrics, A_REVERSE | A_BOLD);
        }
        if (*suffix != '\0') {
            wattron(win_lyrics, A_BOLD);
            ui_pane_clip_printf(win_lyrics, row, text_col + utf8_str_width(prefix), "%s", suffix);
            wattroff(win_lyrics, A_BOLD);
        }
        return;
    }

    // 应用高亮并显示
    if (is_highlighted) {
        wattron(win_lyrics, A_REVERSE);
        if (show_marker) {
            ui_pane_clip_printf(win_lyrics, row, 2, ">");
            ui_pane_clip_printf(win_lyrics, row, 4 + align_offset, "%s", display_text);
        } else {
            // 第二行高亮，不显示标记，缩进对齐
            ui_pane_clip_printf(win_lyrics, row, 3 + align_offset, "%s", display_text);
        }
        wattroff(win_lyrics, A_REVERSE);
    } else {
        // 普通行，使用默认颜色对
        ui_pane_clip_printf(win_lyrics, row, 3 + align_offset, "%s", display_text);
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
    if (!win_lyrics) {
        log_debug("lyrics",
                  "render_lyrics: lyrics pane window is not built "
                  "(layout has no lyrics leaf for this track)");
        return;
    }

    int h, w;
    getmaxyx(win_lyrics, h, w);
    const char *title = g_lyric_cursor_mode
        ? i18n_get("controls.lyrics_seek")
        : i18n_get("controls.lyrics");
    ui_pane_begin(LAYOUT_PANE_LYRICS, win_lyrics, title);
    if (h < 3 || w < 8) {
        ui_refresh_window(win_lyrics);
        return;
    }

    /* 歌词数据归后端：这里只取当前高亮行与总行数，逐行内容按需取。
     * 光标（跳转模式）是界面私有状态，后端不认识。 */
    int highlight = -1;
    int has_lyrics = player_lyrics_highlight(&highlight, NULL, NULL);
    int total = player_lyrics_total();

    if (!has_lyrics || total <= 0) {
        /* 布局已有独立 spectrum 窗格时回退动画不再画频谱柱，
         * 避免同一屏出现两份频谱（碟面与文案保留）。 */
        int draw_bars = !ui_pane_is_visible(LAYOUT_PANE_SPECTRUM);
        render_vinyl_disc(win_lyrics, 1,
                          ui_pane_body_attr(LAYOUT_PANE_LYRICS), draw_bars);
        ui_refresh_window(win_lyrics);
        return;
    }

    /* 频谱已拆到独立窗格；歌词内容从边框内第 1 行开始。 */
    int content_top = 1;

    if (highlight < 0) {
        int message_row = content_top + ((h - content_top - 1) / 2);
        if (message_row >= h - 1) {
            message_row = h - 2;
        }
        ui_pane_clip_printf(win_lyrics, message_row, 2, "%s", i18n_get("lyrics.playing"));
        ui_refresh_window(win_lyrics);
        return;
    }

    int visible_lines = h - content_top - 1;
    if (visible_lines <= 0) {
        ui_refresh_window(win_lyrics);
        return;
    }

    int current_center_idx = (g_lyric_cursor_mode && g_lyric_cursor_index >= 0)
        ? g_lyric_cursor_index : highlight;

    int char_sung = 0;
    int char_total = 0;
    int has_char_progress = player_lyrics_char_progress(&char_sung, &char_total);

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

        /* 逐字进度只用于“主当前行”；光标模式与同时间戳的第二行仍整行高亮。 */
        int line_sung = 0;
        int line_total = 0;
        if (!g_lyric_cursor_mode && has_char_progress && lyric_idx == highlight) {
            line_sung = char_sung;
            line_total = char_total;
        }

        render_lyric_line(row, line.text, is_highlighted, show_marker,
                          line_sung, line_total);
    }

    scrollbar_draw(win_lyrics, content_top, visible_lines,
                   total, visible_lines, start_idx, w - 2);

    ui_refresh_window(win_lyrics);
}

/* 无歌词时歌词窗格仍保留黑胶/粒子动画。它不再搭歌词/频谱的 150ms
 * 刷新便车，而是由主循环以 100ms 的独立节拍驱动；有歌词时不做任何事，
 * 歌词高亮仍由后端修订号触发。 */
void lyrics_animation_tick(void)
{
    if (!win_lyrics || g_current_view != VIEW_MAIN) {
        return;
    }
    if (player_lyrics_total() > 0) {
        return;
    }
    PlayState state = player_play_state();
    if (state != PLAY_STATE_PLAYING && state != PLAY_STATE_PAUSED) {
        return;
    }

    static uint64_t last_tick_ms = 0;
    uint64_t now_ms = get_ui_time_ms();
    if (last_tick_ms != 0 && now_ms - last_tick_ms < 100ULL) {
        return;
    }
    last_tick_ms = now_ms;
    render_lyrics();
}
