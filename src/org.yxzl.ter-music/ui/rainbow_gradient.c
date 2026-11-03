/**
 * @file rainbow_gradient.c
 * @brief 流光彩蛋的前景色色板与相位（纯数学，无 ncurses 依赖）
 *
 * 256 色终端不能直接用连续 RGB，只能在 xterm 6x6x6 色立方里取色。
 * 固定亮度的 HSV 色环在色立方上只会落到少数几档（实测 128 步只有 6 种
 * 颜色），会形成很宽的色带。这里改走一条三维 Lissajous 色路，并且只在
 * 亮度适中、不是中性灰的色立方格子里取最近色——纯黑/纯白和黑↔白轴上
 * 的中性灰排除，但带色相的暗灰/浅灰保留，以改善彩黑、彩白过渡。
 *
 * 8 色档没有色立方可用，只取 ANSI 1..6 六个彩色基色。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/rainbow_gradient.h"

#include "ui/layout_spec.h"

#include <math.h>

/* 8 色档的 HSV 色环：足够饱和，但仍在 ANSI 基本色的可映射范围内。 */
#define RAINBOW_SATURATION 0.85
#define RAINBOW_VALUE      0.85

/* 256 色档的三维 Lissajous 色路。 */
#define RAINBOW_RGB_MID 110.0
#define RAINBOW_RGB_AMP 90.0
#define RAINBOW_TAU 6.283185307179586476925286766559

/* 终端字符通常高约为宽的两倍，x + 2y 在视觉上接近 45°。 */
#define RAINBOW_SLOPE_X 1
#define RAINBOW_SLOPE_Y 2

/*
 * 过滤门槛：只排除纯黑/纯白和“黑↔白轴上的中性灰”。
 * chroma 在 24..63 的带色相灰（彩黑/彩白过渡）保留，让相邻色阶更连贯。
 */
#define RAINBOW_MIN_CHROMA 24
#define RAINBOW_MIN_MAX_RGB 40
#define RAINBOW_MAX_MIN_RGB 230
#define RAINBOW_MIN_LUMA 0.10
#define RAINBOW_MAX_LUMA 0.90

/* 反色高亮文字色阈值：背景亮度高于它就用黑字，否则用白字。 */
#define RAINBOW_CONTRAST_THRESHOLD 0.55

/* HSV → RGB；h 为角度，s/v 为 0..1。 */
static void hsv_to_rgb(double h, double s, double v,
                       int *r, int *g, int *b)
{
    h = fmod(h, 360.0);
    if (h < 0.0) {
        h += 360.0;
    }

    double c = v * s;
    double hp = h / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r1 = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;

    switch ((int)hp) {
        case 0: r1 = c; g1 = x; break;
        case 1: r1 = x; g1 = c; break;
        case 2: g1 = c; b1 = x; break;
        case 3: g1 = x; b1 = c; break;
        case 4: r1 = x; b1 = c; break;
        default: r1 = c; b1 = x; break;
    }

    double m = v - c;
    *r = (int)lround((r1 + m) * 255.0);
    *g = (int)lround((g1 + m) * 255.0);
    *b = (int)lround((b1 + m) * 255.0);
}

static long colour_distance(int r, int g, int b,
                            int pr, int pg, int pb)
{
    long dr = r - pr;
    long dg = g - pg;
    long db = b - pb;
    return dr * dr + dg * dg + db * db;
}

static double colour_luminance(int index)
{
    int r = 0;
    int g = 0;
    int b = 0;
    layout_color_rgb(index, &r, &g, &b);
    return (0.2126 * (double)r + 0.7152 * (double)g + 0.0722 * (double)b) /
           255.0;
}

/* 只接受亮度适中、非中性灰的色立方格子；彩黑/彩白过渡色保留。 */
static int palette_index_is_chromatic(int index)
{
    int r = 0;
    int g = 0;
    int b = 0;
    layout_color_rgb(index, &r, &g, &b);

    int max_rgb = r > g ? r : g;
    if (b > max_rgb) {
        max_rgb = b;
    }
    int min_rgb = r < g ? r : g;
    if (b < min_rgb) {
        min_rgb = b;
    }

    if (max_rgb - min_rgb < RAINBOW_MIN_CHROMA) {
        return 0;
    }
    if (max_rgb < RAINBOW_MIN_MAX_RGB || min_rgb > RAINBOW_MAX_MIN_RGB) {
        return 0;
    }

    double luma = colour_luminance(index);
    return luma >= RAINBOW_MIN_LUMA && luma <= RAINBOW_MAX_LUMA;
}

/* 在 256 色立方 16..231 中找最近的饱和色；找不到时返回 -1。 */
static int nearest_chromatic_256(int r, int g, int b)
{
    int best = -1;
    long best_dist = -1;

    for (int i = 16; i <= 231; i++) {
        if (!palette_index_is_chromatic(i)) {
            continue;
        }
        int pr = 0;
        int pg = 0;
        int pb = 0;
        layout_color_rgb(i, &pr, &pg, &pb);
        long dist = colour_distance(r, g, b, pr, pg, pb);
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

/* 8 色档：只在 ANSI 1..6 中找最近色，绝不到 0/7/8。 */
static int nearest_chromatic_ansi(int r, int g, int b)
{
    static const int ansi[] = {1, 2, 3, 4, 5, 6};
    int best = ansi[0];
    long best_dist = -1;

    for (size_t i = 0; i < sizeof(ansi) / sizeof(ansi[0]); i++) {
        int pr = 0;
        int pg = 0;
        int pb = 0;
        layout_color_rgb(ansi[i], &pr, &pg, &pb);
        long dist = colour_distance(r, g, b, pr, pg, pb);
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best = ansi[i];
        }
    }
    return best;
}

int rainbow_default_steps(int colors)
{
    if (colors >= 256) {
        return RAINBOW_MAX_STEPS;
    }
    if (colors >= 8) {
        return 6;
    }
    return 0;
}

int rainbow_build_swatches(int steps, int colors, int *fg_out)
{
    if (!fg_out || steps < 1 || steps > RAINBOW_MAX_STEPS || colors < 8) {
        return -1;
    }

    for (int i = 0; i < steps; i++) {
        int r = 0;
        int g = 0;
        int b = 0;

        if (colors >= 256) {
            double theta = RAINBOW_TAU * (double)i / (double)steps;
            r = (int)lround(RAINBOW_RGB_MID + RAINBOW_RGB_AMP * sin(2.0 * theta));
            g = (int)lround(RAINBOW_RGB_MID +
                            RAINBOW_RGB_AMP * sin(3.0 * theta + 1.0));
            b = (int)lround(RAINBOW_RGB_MID +
                            RAINBOW_RGB_AMP * sin(5.0 * theta + 2.0));

            int fg = nearest_chromatic_256(r, g, b);
            if (fg < 0) {
                return -1;
            }
            fg_out[i] = fg;
        } else {
            double hue = 360.0 * (double)i / (double)steps;
            hsv_to_rgb(hue, RAINBOW_SATURATION, RAINBOW_VALUE, &r, &g, &b);
            fg_out[i] = nearest_chromatic_ansi(r, g, b);
        }
    }
    return 0;
}

int rainbow_contrast_fg(int fg)
{
    return colour_luminance(fg) > RAINBOW_CONTRAST_THRESHOLD ? 0 : 7;
}

void rainbow_pick_shades(const int *fg, int steps, int offset, int out[3])
{
    if (!out) {
        return;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    if (!fg || steps <= 0) {
        return;
    }

    int window = steps < 8 ? steps : 8;
    int unique[8];
    int count = 0;
    int base = offset % steps;
    if (base < 0) {
        base += steps;
    }

    for (int i = 0; i < window; i++) {
        int colour = fg[(base + i) % steps];
        int duplicate = 0;
        for (int j = 0; j < count; j++) {
            if (unique[j] == colour) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            unique[count++] = colour;
        }
    }
    if (count == 0) {
        return;
    }

    /* 按亮度升序排序：暗 → 中 → 亮，亮档给旋转高光。 */
    for (int i = 1; i < count; i++) {
        int key = unique[i];
        double key_luma = colour_luminance(key);
        int j = i - 1;
        while (j >= 0 && colour_luminance(unique[j]) > key_luma) {
            unique[j + 1] = unique[j];
            j--;
        }
        unique[j + 1] = key;
    }

    out[0] = unique[0];
    out[1] = unique[count / 2];
    out[2] = unique[count - 1];
}

int rainbow_phase(int x, int y, int offset, int steps)
{
    if (steps <= 0) {
        return 0;
    }

    long long v = (long long)x * RAINBOW_SLOPE_X +
                  (long long)y * RAINBOW_SLOPE_Y +
                  (long long)offset;
    v %= steps;
    if (v < 0) {
        v += steps;
    }
    return (int)v;
}
