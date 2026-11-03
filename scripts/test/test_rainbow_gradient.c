/**
 * @file test_rainbow_gradient.c
 * @brief 彩蛋前景色色板/相位的纯函数单测
 */

#include "ui/layout_spec.h"
#include "ui/rainbow_gradient.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static double luminance(int index)
{
    int r = 0;
    int g = 0;
    int b = 0;
    layout_color_rgb(index, &r, &g, &b);
    return (0.2126 * (double)r + 0.7152 * (double)g + 0.0722 * (double)b) /
           255.0;
}

static void test_rgb_palette(void)
{
    static const struct {
        int index;
        int r;
        int g;
        int b;
    } cases[] = {
        {0, 0, 0, 0},
        {7, 229, 229, 229},
        {16, 0, 0, 0},
        {231, 255, 255, 255},
        {232, 8, 8, 8},
        {255, 238, 238, 238},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int r = -1;
        int g = -1;
        int b = -1;
        layout_color_rgb(cases[i].index, &r, &g, &b);
        CHECK(r == cases[i].r && g == cases[i].g && b == cases[i].b,
              "layout_color_rgb expected value");
    }

    int r = -1;
    int g = -1;
    int b = -1;
    layout_color_rgb(-1, &r, &g, &b);
    CHECK(r == 0 && g == 0 && b == 0, "layout_color_rgb clamps low index");

    layout_color_rgb(300, &r, &g, &b);
    CHECK(r == 238 && g == 238 && b == 238,
          "layout_color_rgb clamps high index");
}

static void test_default_steps(void)
{
    CHECK(rainbow_default_steps(256) == RAINBOW_MAX_STEPS, "256 colours");
    CHECK(rainbow_default_steps(88) == 6, "88 colours use 8-colour tier");
    CHECK(rainbow_default_steps(16) == 6, "16 colours use 8-colour tier");
    CHECK(rainbow_default_steps(8) == 6, "8 colours");
    CHECK(rainbow_default_steps(7) == 0, "7 colours unsupported");
    CHECK(rainbow_default_steps(0) == 0, "0 colours unsupported");
}

static void test_swatches(void)
{
    int fg[RAINBOW_MAX_STEPS];
    int again[RAINBOW_MAX_STEPS];

    memset(fg, 0, sizeof(fg));
    CHECK(rainbow_build_swatches(RAINBOW_MAX_STEPS, 256, fg) == 0,
          "build 128-step 256-colour swatches");
    CHECK(rainbow_build_swatches(RAINBOW_MAX_STEPS, 256, again) == 0,
          "build swatches twice");
    CHECK(memcmp(fg, again, sizeof(fg)) == 0,
          "swatch generation is deterministic");

    int seen[256];
    memset(seen, 0, sizeof(seen));
    int unique = 0;
    int tinted_low_chroma = 0;
    for (int i = 0; i < RAINBOW_MAX_STEPS; i++) {
        CHECK(fg[i] >= 16 && fg[i] <= 231,
              "256-colour foreground is a cube colour");
        CHECK(fg[i] != 0 && fg[i] != 7 && fg[i] != 8,
              "foreground is not black/white/gray");

        int r = 0;
        int g = 0;
        int b = 0;
        layout_color_rgb(fg[i], &r, &g, &b);
        int max_rgb = r > g ? r : g;
        if (b > max_rgb) max_rgb = b;
        int min_rgb = r < g ? r : g;
        if (b < min_rgb) min_rgb = b;
        int chroma = max_rgb - min_rgb;

        /* 只踢中性灰；带色相的暗灰/浅灰（彩黑、彩白过渡）允许。 */
        CHECK(chroma >= 24, "foreground is not neutral gray");
        CHECK(max_rgb >= 40 && min_rgb <= 230,
              "foreground is not near-black/near-white");
        double luma = luminance(fg[i]);
        CHECK(luma >= 0.10 && luma <= 0.90,
              "foreground luminance is mid-range");
        CHECK(!(r == g && g == b), "foreground is not a gray");

        if (chroma >= 24 && chroma < 64) {
            tinted_low_chroma++;
        }
        if (!seen[fg[i]]) {
            seen[fg[i]] = 1;
            unique++;
        }
    }
    /* 防止色路退化回少数几档、形成宽色带。 */
    CHECK(unique >= 24, "256-colour palette keeps enough distinct steps");
    /* 回归：带色相的低饱和过渡色不能被一起踢干净。 */
    CHECK(tinted_low_chroma > 0,
          "palette keeps tinted low-chroma transition colours");

    /* 反色高亮文字色：亮背景黑字，暗背景白字。 */
    CHECK(rainbow_contrast_fg(226) == 0, "bright colour uses black text");
    CHECK(rainbow_contrast_fg(17) == 7, "dark colour uses white text");
    for (int i = 0; i < RAINBOW_MAX_STEPS; i++) {
        int expected = luminance(fg[i]) > 0.55 ? 0 : 7;
        CHECK(rainbow_contrast_fg(fg[i]) == expected,
              "contrast colour follows background luminance");
    }

    /* 黑胶碟面：邻近色板取暗/中/亮三档。 */
    for (int offset = 0; offset < RAINBOW_MAX_STEPS; offset += 17) {
        int shades[3];
        rainbow_pick_shades(fg, RAINBOW_MAX_STEPS, offset, shades);
        for (int k = 0; k < 3; k++) {
            CHECK(shades[k] >= 16 && shades[k] <= 231,
                  "shade is a palette colour");
        }
        CHECK(luminance(shades[0]) <= luminance(shades[1]) &&
              luminance(shades[1]) <= luminance(shades[2]),
              "shades are ordered dark/mid/bright");
        CHECK(shades[0] != shades[2] || shades[0] != shades[1],
              "shade triplet is not all identical");
    }

    int low[RAINBOW_MAX_STEPS];
    CHECK(rainbow_build_swatches(6, 8, low) == 0,
          "build 8-colour swatches");
    for (int i = 0; i < 6; i++) {
        CHECK(low[i] >= 1 && low[i] <= 6,
              "8-colour tier uses ANSI 1..6 only");
    }

    CHECK(rainbow_build_swatches(6, 16, low) == 0,
          "16 colours use 8-colour tier");
    for (int i = 0; i < 6; i++) {
        CHECK(low[i] >= 1 && low[i] <= 6,
              "16-colour tier uses ANSI 1..6 only");
    }

    CHECK(rainbow_build_swatches(6, 88, low) == 0,
          "88 colours use 8-colour tier");
    for (int i = 0; i < 6; i++) {
        CHECK(low[i] >= 1 && low[i] <= 6,
              "88-colour tier uses ANSI 1..6 only");
    }

    int low_shades[3];
    rainbow_pick_shades(low, 6, 0, low_shades);
    for (int i = 0; i < 3; i++) {
        CHECK(low_shades[i] >= 1 && low_shades[i] <= 6,
              "8-colour shade is ANSI 1..6");
    }
    CHECK(luminance(low_shades[0]) <= luminance(low_shades[1]) &&
          luminance(low_shades[1]) <= luminance(low_shades[2]),
          "8-colour shades are ordered dark/mid/bright");

    CHECK(rainbow_build_swatches(0, 256, fg) != 0,
          "reject zero steps");
    CHECK(rainbow_build_swatches(RAINBOW_MAX_STEPS + 1, 256, fg) != 0,
          "reject too many steps");
    CHECK(rainbow_build_swatches(6, 7, fg) != 0,
          "reject monochrome terminal");
}

static void test_phase(void)
{
    CHECK(rainbow_phase(3, 4, 5, 128) == (3 + 2 * 4 + 5) % 128,
          "phase follows x + 2y + offset");
    CHECK(rainbow_phase(0, 0, -1, 128) == 127,
          "negative offset wraps");
    CHECK(rainbow_phase(0, 0, 128, 128) == 0,
          "one full turn wraps");
    CHECK(rainbow_phase(7, 9, 11, 0) == 0,
          "zero steps is safe");

    for (int x = 0; x < 16; x++) {
        for (int y = 0; y < 16; y++) {
            for (int offset = -8; offset <= 8; offset++) {
                int base = rainbow_phase(x, y, offset, 128);
                CHECK(base >= 0 && base < 128,
                      "phase stays in range");
                CHECK(rainbow_phase(x + 1, y, offset - 1, 128) == base,
                      "phase is diagonal-shift invariant");
            }
        }
    }
}

int main(void)
{
    test_rgb_palette();
    test_default_steps();
    test_swatches();
    test_phase();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_rainbow_gradient\n");
    return 0;
}
