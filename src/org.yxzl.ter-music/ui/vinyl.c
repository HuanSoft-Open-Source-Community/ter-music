/**
 * @file vinyl.c
 * @brief 独立黑胶碟片窗格与歌词窗格的无歌词回退动画
 *
 * 绘制逻辑从 ui/lyrics.c 迁出，改为接受任意 WINDOW*，使独立 vinyl 窗格
 * 与歌词窗格的无歌词回退共用同一套实现。颜色可用时绘制彩色半块碟面、
 * 旋转的封面圆形中心标签与彩色频谱柱；无颜色/ASCII/色对预算不足时逐字
 * 回退到原字符动画。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "types.h"
#include "audio/visualizer.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "player/player.h"
#include "ui/cover_art.h"
#include "ui/layout.h"
#include "ui/ui.h"
#include "ui/vinyl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 终端字符宽高比：横向 1 列约等于纵向 0.56 行的视觉长度。 */
#define VINYL_X_SCALE 1.8

static uint64_t vinyl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int vinyl_glyph_width(const char *glyph)
{
    int width = utf8_str_width(glyph);
    return width > 0 ? width : 1;
}

static void draw_vinyl_glyph(WINDOW *win, int row, int col,
                             const char *glyph, int attrs)
{
    if (!win || !glyph || glyph[0] == '\0') {
        return;
    }

    int h, w;
    getmaxyx(win, h, w);

    int glyph_width = vinyl_glyph_width(glyph);
    if (row <= 0 || row >= h - 1 || col <= 1 || col + glyph_width > w - 1) {
        return;
    }

    if (attrs != 0) {
        wattron(win, attrs);
    }
    mvwprintw(win, row, col, "%s", glyph);
    if (attrs != 0) {
        wattroff(win, attrs);
    }
}

static const char *pick_disc_glyph(double diff, double angle, int pulse_phase)
{
    static const char *ascii_idle[] = {".", "o", "x", "+", "a", "m", "n", "z"};
    static const char *ascii_hot[] = {"*", "#", "@", "%", "M", "U", "S", "I", "C"};
    static const char *unicode_idle[] = {"·", "•", "◦", "○", "◇", "A", "M", "N", "Z", "♪"};
    static const char *unicode_hot[] = {"✦", "✧", "✶", "✹", "♪", "♬", "♩", "M", "U", "S", "I", "C"};

    const char **idle = use_ascii_fallback_ui() ? ascii_idle : unicode_idle;
    const char **hot = use_ascii_fallback_ui() ? ascii_hot : unicode_hot;
    int idle_count = use_ascii_fallback_ui()
        ? (int)(sizeof(ascii_idle) / sizeof(ascii_idle[0]))
        : (int)(sizeof(unicode_idle) / sizeof(unicode_idle[0]));
    int hot_count = use_ascii_fallback_ui()
        ? (int)(sizeof(ascii_hot) / sizeof(ascii_hot[0]))
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

static const char *pick_bar_glyph(double angle, int step, int extent,
                                  int pulse_phase)
{
    static const char *ascii_body[] = {"|", "/", "\\", ":", "!", "+", "="};
    static const char *ascii_tip[] = {"*", "#", "@", "M", "U", "S", "I", "C"};
    static const char *unicode_body[] = {"·", "•", "╎", "╏", "│", "┆", "┊", "♪"};
    static const char *unicode_tip[] = {"✦", "✧", "✶", "✹", "♪", "♬", "♩", "♫", "M", "U", "S", "I", "C"};

    const char **body = use_ascii_fallback_ui() ? ascii_body : unicode_body;
    const char **tip = use_ascii_fallback_ui() ? ascii_tip : unicode_tip;
    int body_count = use_ascii_fallback_ui()
        ? (int)(sizeof(ascii_body) / sizeof(ascii_body[0]))
        : (int)(sizeof(unicode_body) / sizeof(unicode_body[0]));
    int tip_count = use_ascii_fallback_ui()
        ? (int)(sizeof(ascii_tip) / sizeof(ascii_tip[0]))
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

static int use_emoji_no_lyrics_title(void)
{
    return !use_ascii_fallback_ui() && utf8_str_width("🎵") >= 2;
}

/* ============================================================
 * 彩色块模式：固定色对与几何/动画快照
 * ============================================================ */

/* 0 = 未探测；1 = 彩色块可用；-1 = 不可用（本进程永久回退字符动画）。 */
static int g_vinyl_color_state;
static int g_vinyl_disc_pair[3][3];
static int g_vinyl_bar_pair[3];

/* 探测并初始化黑胶固定色对：3×3 碟面上下半 + 3 个频谱柱颜色。
 * 任何一步失败都退回字符动画，不做半套彩色。 */
static int vinyl_color_configure(void)
{
    if (g_vinyl_color_state != 0) {
        return g_vinyl_color_state > 0;
    }
    g_vinyl_color_state = -1;

    if (!has_colors() || use_ascii_fallback_ui()) {
        return 0;
    }

    int tier = 0;
    int pair_base = 0;
    int pair_budget = 0;
    if (!cover_art_vinyl_colors(&tier, &pair_base, &pair_budget) ||
        (tier != 8 && tier != 256)) {
        return 0;
    }
    if (pair_budget < COVER_ART_VINYL_FIXED_PAIRS) {
        return 0;
    }
    if (pair_base < 0 ||
        pair_base + COVER_ART_VINYL_FIXED_PAIRS > (int)COLOR_PAIRS ||
        pair_base + COVER_ART_VINYL_FIXED_PAIRS > 256) {
        return 0;
    }

    static const int shades_256[3] = {234, 240, 255};
    static const int bars_256[3] = {46, 226, 196};
    static const int shades_8[3] = {0, 4, 7};
    static const int bars_8[3] = {2, 3, 1};
    const int *shades = (tier == 256) ? shades_256 : shades_8;
    const int *bars = (tier == 256) ? bars_256 : bars_8;

    for (int i = 0; i < 3; i++) {
        if (shades[i] < 0 || shades[i] >= (int)COLORS ||
            bars[i] < 0 || bars[i] >= (int)COLORS) {
            return 0;
        }
    }

    for (int top = 0; top < 3; top++) {
        for (int bottom = 0; bottom < 3; bottom++) {
            int pair = pair_base + top * 3 + bottom;
            if (init_pair((short)pair, (short)shades[top],
                          (short)shades[bottom]) != OK) {
                return 0;
            }
            g_vinyl_disc_pair[top][bottom] = pair;
        }
    }
    for (int i = 0; i < 3; i++) {
        int pair = pair_base + 9 + i;
        if (init_pair((short)pair, (short)bars[i], (short)bars[i]) != OK) {
            return 0;
        }
        g_vinyl_bar_pair[i] = pair;
    }

    g_vinyl_color_state = 1;
    return 1;
}

typedef struct {
    int h;
    int w;
    int center_x;
    int center_y;
    int disc_outer;
    int disc_inner;
    int max_bar_len;
    int step;
    int label_rx;
    int label_ry;
} VinylGeometry;

static void vinyl_compute_geometry(WINDOW *win, VinylGeometry *geo)
{
    memset(geo, 0, sizeof(*geo));
    getmaxyx(win, geo->h, geo->w);

    geo->center_y = geo->h / 2 - 1;
    geo->center_x = geo->w / 2;

    int ring_radius = (geo->h - 8) / 2;
    int width_radius = (geo->w - 10) / 4;
    if (width_radius < ring_radius) {
        ring_radius = width_radius;
    }
    if (ring_radius < 4) {
        ring_radius = 4;
    }

    geo->disc_outer = ring_radius - 1;
    geo->disc_inner = geo->disc_outer / 2;
    geo->max_bar_len = ring_radius / 2;
    if (geo->max_bar_len < 2) {
        geo->max_bar_len = 2;
    }
    geo->step = (geo->h > 30) ? 2 : 1;

    /* 中心标签占满原来的空心碟心；太小就不画封面，保持纯深色碟面。 */
    double label_radius = (double)geo->disc_inner - 0.4;
    int rx = (int)lround(label_radius * VINYL_X_SCALE);
    int ry = (int)lround(label_radius);
    int max_rx = (COVER_ART_MAX_COLS - 1) / 2;
    int max_ry = (COVER_ART_MAX_ROWS - 1) / 2;
    if (rx > max_rx) {
        rx = max_rx;
    }
    if (ry > max_ry) {
        ry = max_ry;
    }
    if (rx >= 2 && ry >= 2) {
        geo->label_rx = rx;
        geo->label_ry = ry;
    }
}

typedef struct {
    uint64_t now_ms;
    double spin;
    double highlight_angle;
    int pulse_phase;
    int levels[VISUALIZER_BAND_COUNT];
    int peaks[VISUALIZER_BAND_COUNT];
    uint64_t last_update_ms;
    int is_idle;
    int stopped;
} VinylAnimation;

static void vinyl_prepare_animation(VinylAnimation *anim)
{
    memset(anim, 0, sizeof(*anim));
    anim->now_ms = vinyl_now_ms();
    player_visualizer(anim->levels, anim->peaks, VISUALIZER_BAND_COUNT,
                      &anim->last_update_ms);

    anim->spin = (double)(anim->now_ms % 5000ULL) / 5000.0;
    anim->highlight_angle = (anim->spin * M_PI * 2.0) - (M_PI / 2.0);
    anim->pulse_phase = (int)((anim->now_ms / 180ULL) % 24ULL);
    anim->stopped = (player_play_state() == PLAY_STATE_STOPPED);
    anim->is_idle = (anim->last_update_ms == 0) ||
                    (anim->now_ms > anim->last_update_ms + 260ULL) ||
                    anim->stopped;
}

/* 两个角度之间的最短距离，结果范围为 [0, π]。
 * highlight_angle 会超过 π，而 atan2 的结果在 [-π, π]；若直接相减后只做
 * 一次 2π - diff，差值超过 2π 时会得到负数，左上象限的分界处就会被
 * 误判为高光。先取模再归一化可避免这块虚假的扇形。 */
static double vinyl_angle_distance(double a, double b)
{
    double diff = fmod(a - b, M_PI * 2.0);
    if (diff < 0.0) {
        diff += M_PI * 2.0;
    }
    if (diff > M_PI) {
        diff = (M_PI * 2.0) - diff;
    }
    return diff;
}

static int vinyl_band_count(const VinylGeometry *geo)
{
    int band_count = VISUALIZER_BAND_COUNT;
    int max_band_count = (int)(geo->disc_outer * 5.0);
    if (band_count > max_band_count) {
        band_count = max_band_count;
    }
    if (band_count < 16) {
        band_count = 16;
    }
    return band_count;
}

static int vinyl_bar_level(const VinylAnimation *anim, int i, int band_count)
{
    int src = (i * VISUALIZER_BAND_COUNT) / band_count;
    if (src >= VISUALIZER_BAND_COUNT) {
        src = VISUALIZER_BAND_COUNT - 1;
    }

    int level = anim->levels[src];
    if (anim->is_idle) {
        double wave = sin((anim->spin * M_PI * 6.0) + (double)i * 0.42);
        level = 16 + (int)(14.0 * (wave + 1.0));
        if (anim->stopped) {
            level /= 2;
        }
    }
    return level;
}

static double vinyl_bar_angle(int i, int band_count)
{
    return (-M_PI / 2.0) +
           ((2.0 * M_PI * (double)i) / (double)band_count);
}

static int vinyl_bar_extent(const VinylGeometry *geo, int level)
{
    int extent = 1 + (level * geo->max_bar_len) / 100;
    if (extent < 1) {
        extent = 1;
    }
    return extent;
}

static void vinyl_bar_cell(const VinylGeometry *geo, double angle,
                           int bar_step, int *row, int *col)
{
    double radius = (double)geo->disc_outer + 1.0 + (double)bar_step;
    *row = (int)lround((double)geo->center_y + sin(angle) * radius);
    *col = (int)lround((double)geo->center_x +
                       cos(angle) * radius * VINYL_X_SCALE);
}

static int vinyl_bar_color(int level)
{
    if (level < 35) {
        return 0;
    }
    if (level < 70) {
        return 1;
    }
    return 2;
}

/* 碟面三档明暗：同心沟槽 + 内圈暗分隔 + 外圈亮边 + 旋转高光。 */
static int vinyl_disc_shade(double dx, double dy, const VinylGeometry *geo,
                            double highlight_diff)
{
    double dist = sqrt(dx * dx + dy * dy);
    double inner = (double)geo->disc_inner;
    double outer = (double)geo->disc_outer;

    if (inner < 0.0) {
        inner = 0.0;
    }
    if (dist < inner) {
        dist = inner;
    }
    if (dist > outer) {
        dist = outer;
    }

    double span = outer - inner;
    double t = span > 1e-6 ? (dist - inner) / span : 0.0;

    int shade;
    if (t < 0.08) {
        shade = 0;
    } else if (t > 0.90) {
        shade = 2;
    } else {
        shade = ((int)(t * 7.0) & 1) ? 1 : 0;
    }

    if (highlight_diff < 0.45) {
        if (shade < 1) {
            shade = 1;
        }
    }
    if (highlight_diff < 0.16) {
        shade = 2;
    }
    return shade;
}

static const CoverArtGrid *vinyl_get_label(const VinylGeometry *geo,
                                           const VinylAnimation *anim,
                                           int *rx, int *ry)
{
    *rx = 0;
    *ry = 0;
    if (geo->label_rx < 2 || geo->label_ry < 2) {
        return NULL;
    }

    char cover_path[MAX_PATH_LEN];
    if (player_cover_path(cover_path, sizeof(cover_path)) != 0 ||
        cover_path[0] == '\0') {
        return NULL;
    }

    const CoverArtGrid *grid = cover_art_get_label(
        cover_path, 2 * geo->label_rx + 1, 2 * geo->label_ry + 1,
        anim->spin * 2.0 * M_PI);
    if (!grid) {
        return NULL;
    }

    *rx = geo->label_rx;
    *ry = geo->label_ry;
    return grid;
}

static void vinyl_draw_color_disc(WINDOW *win, const VinylGeometry *geo,
                                  const VinylAnimation *anim,
                                  int label_rx, int label_ry)
{
    for (int row = 1; row < geo->h - 1; row++) {
        for (int col = 2; col < geo->w - 2; col++) {
            double dx = ((double)col - (double)geo->center_x) / VINYL_X_SCALE;
            double dy = (double)row - (double)geo->center_y;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist > (double)geo->disc_outer) {
                continue;
            }

            /* 标签椭圆内由封面覆盖；掩码四角在椭圆外，随后由碟面重绘。 */
            if (label_rx > 0 && label_ry > 0) {
                double lx = ((double)col - (double)geo->center_x) /
                            (double)label_rx;
                double ly = ((double)row - (double)geo->center_y) /
                            (double)label_ry;
                if (lx * lx + ly * ly <= 1.0 + 1e-9) {
                    continue;
                }
            }

            double angle = atan2(dy, dx);
            double diff = vinyl_angle_distance(angle, anim->highlight_angle);

            double top_dy = ((double)row - 0.25) - (double)geo->center_y;
            double bottom_dy = ((double)row + 0.25) - (double)geo->center_y;
            int top = vinyl_disc_shade(dx, top_dy, geo, diff);
            int bottom = vinyl_disc_shade(dx, bottom_dy, geo, diff);

            wattrset(win, COLOR_PAIR(g_vinyl_disc_pair[top][bottom]));
            mvwaddstr(win, row, col, "\xe2\x96\x80"); /* ▀ */
        }
    }
}

static void vinyl_draw_color_bars(WINDOW *win, const VinylGeometry *geo,
                                  const VinylAnimation *anim)
{
    int band_count = vinyl_band_count(geo);
    for (int i = 0; i < band_count; i++) {
        int level = vinyl_bar_level(anim, i, band_count);
        int extent = vinyl_bar_extent(geo, level);
        double angle = vinyl_bar_angle(i, band_count);
        int pair = g_vinyl_bar_pair[vinyl_bar_color(level)];

        for (int step = 0; step < extent; step++) {
            int row, col;
            vinyl_bar_cell(geo, angle, step, &row, &col);
            if (row <= 0 || row >= geo->h - 1 ||
                col <= 1 || col >= geo->w - 1) {
                continue;
            }
            wattrset(win, COLOR_PAIR(pair));
            mvwaddstr(win, row, col, "\xe2\x96\x80"); /* ▀ */
        }
    }
}

static void draw_vinyl_messages(WINDOW *win, int show_messages,
                                int body_attr, const VinylGeometry *geo)
{
    if (!show_messages) {
        return;
    }

    wattrset(win, body_attr);

    const char *title = use_emoji_no_lyrics_title()
        ? i18n_get("lyrics.no_lyrics_emoji")
        : i18n_get("lyrics.no_lyrics");
    const char *subtitle = use_emoji_no_lyrics_title()
        ? i18n_get("lyrics.particle_emoji")
        : i18n_get("lyrics.particle_text");
    int title_col = geo->center_x - utf8_str_width(title) / 2;
    int subtitle_col = geo->center_x - utf8_str_width(subtitle) / 2;
    int text_row = geo->center_y + geo->disc_outer + geo->max_bar_len + 1;

    if (text_row < geo->h - 2) {
        if (title_col < 2) {
            title_col = 2;
        }
        ui_pane_clip_printf(win, text_row, title_col, "%s", title);
    }
    if (text_row + 1 < geo->h - 1) {
        if (subtitle_col < 2) {
            subtitle_col = 2;
        }
        ui_pane_clip_printf(win, text_row + 1, subtitle_col, "%s", subtitle);
    }
}

/* 彩色块模式：标签先画，碟面后画（盖掉标签矩形的掩码四角），最后频谱柱。 */
static void render_vinyl_disc_color(WINDOW *win, int show_messages,
                                    int body_attr, int draw_bars)
{
    VinylGeometry geo;
    VinylAnimation anim;
    vinyl_compute_geometry(win, &geo);
    vinyl_prepare_animation(&anim);

    int rx = 0;
    int ry = 0;
    const CoverArtGrid *label = vinyl_get_label(&geo, &anim, &rx, &ry);

    if (label) {
        cover_art_draw(win, geo.center_x - rx, geo.center_y - ry,
                       label, body_attr);
    }

    vinyl_draw_color_disc(win, &geo, &anim, rx, ry);
    if (draw_bars) {
        vinyl_draw_color_bars(win, &geo, &anim);
    }

    wattrset(win, body_attr);
    draw_vinyl_messages(win, show_messages, body_attr, &geo);
}

/* 原字符动画：颜色不可用时的逐字回退，行为保持升级前不变。 */
static void render_vinyl_disc_chars(WINDOW *win, int show_messages,
                                    int body_attr, int draw_bars)
{
    VinylGeometry geo;
    VinylAnimation anim;
    vinyl_compute_geometry(win, &geo);
    vinyl_prepare_animation(&anim);

    for (int row = 1; row < geo.h - 1; row += geo.step) {
        for (int col = 2; col < geo.w - 2; col += geo.step) {
            double dx = ((double)col - (double)geo.center_x) / VINYL_X_SCALE;
            double dy = (double)row - (double)geo.center_y;
            double dist = sqrt(dx * dx + dy * dy);

            if (dist > (double)geo.disc_inner &&
                dist <= (double)geo.disc_outer) {
                double angle = atan2(dy, dx);
                double diff = vinyl_angle_distance(angle, anim.highlight_angle);
                draw_vinyl_glyph(win, row, col,
                                 pick_disc_glyph(diff, angle, anim.pulse_phase),
                                 diff < 0.18 ? A_BOLD : 0);
            } else if (dist <= (double)geo.disc_inner - 0.4) {
                mvwaddch(win, row, col, ' ');
            }
        }
    }

    if (draw_bars) {
        int band_count = vinyl_band_count(&geo);
        for (int i = 0; i < band_count; i++) {
            int level = vinyl_bar_level(&anim, i, band_count);
            int extent = vinyl_bar_extent(&geo, level);
            double angle = vinyl_bar_angle(i, band_count);

            for (int step = 0; step < extent; step++) {
                int row, col;
                vinyl_bar_cell(&geo, angle, step, &row, &col);
                if (row <= 0 || row >= geo.h - 1 ||
                    col <= 1 || col >= geo.w - 1) {
                    continue;
                }
                draw_vinyl_glyph(win, row, col,
                                 pick_bar_glyph(angle, step, extent,
                                                anim.pulse_phase + i),
                                 step == extent - 1 ? A_BOLD : 0);
            }
        }
    }

    wattrset(win, body_attr);
    draw_vinyl_messages(win, show_messages, body_attr, &geo);
}

void render_vinyl_disc(WINDOW *win, int show_messages, int body_attr,
                       int draw_bars)
{
    if (!win) {
        return;
    }

    int h, w;
    getmaxyx(win, h, w);
    if (h < 12 || w < 24) {
        if (show_messages && h >= 1 && w >= 4) {
            wattrset(win, body_attr);
            ui_pane_clip_printf(win, h / 2, 2, "%s",
                                i18n_get("lyrics.not_available"));
        }
        return;
    }

    if (vinyl_color_configure()) {
        render_vinyl_disc_color(win, show_messages, body_attr, draw_bars);
    } else {
        render_vinyl_disc_chars(win, show_messages, body_attr, draw_bars);
    }
}

void render_vinyl_pane(void)
{
    if (!win_vinyl || g_current_view != VIEW_MAIN) {
        return;
    }

    int h, w;
    getmaxyx(win_vinyl, h, w);
    if (h < 1 || w < 1) {
        return;
    }

    ui_pane_begin(LAYOUT_PANE_VINYL, win_vinyl, i18n_get("controls.vinyl"));
    if (h < 3 || w < 6) {
        wrefresh(win_vinyl);
        return;
    }
    render_vinyl_disc(win_vinyl, player_lyrics_total() <= 0,
                      ui_pane_body_attr(LAYOUT_PANE_VINYL), 1);
    wrefresh(win_vinyl);
}

void vinyl_tick(void)
{
    if (!win_vinyl || g_current_view != VIEW_MAIN) {
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
    render_vinyl_pane();
}
