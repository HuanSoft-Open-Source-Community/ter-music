#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "ui/braille/braille_art.h"

int rgba_to_gray(const unsigned char *rgba, int w, int h, unsigned char *gray) {
    if (!rgba || !gray || w <= 0 || h <= 0) return -1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 4;
            float r = rgba[idx];
            float g = rgba[idx + 1];
            float b = rgba[idx + 2];
            gray[y * w + x] = (unsigned char)(0.299f * r + 0.587f * g + 0.114f * b + 0.5f);
        }
    }
    return 0;
}

int resize_gray(const unsigned char *src, int sw, int sh, unsigned char *dst, int dw, int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return -1;

    float scale_x = (float)sw / dw;
    float scale_y = (float)sh / dh;

    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            float src_x = dx * scale_x;
            float src_y = dy * scale_y;

            int x0 = (int)floorf(src_x);
            int y0 = (int)floorf(src_y);
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;

            float fx = src_x - x0;
            float fy = src_y - y0;

            float v00 = src[y0 * sw + x0];
            float v10 = src[y0 * sw + x1];
            float v01 = src[y1 * sw + x0];
            float v11 = src[y1 * sw + x1];

            float interp = (1 - fx) * (1 - fy) * v00 +
                          fx * (1 - fy) * v10 +
                          (1 - fx) * fy * v01 +
                          fx * fy * v11;

            dst[dy * dw + dx] = (unsigned char)(interp + 0.5f);
        }
    }
    return 0;
}

void threshold_gray(const unsigned char *gray, int w, int h, unsigned char *binary, uint8_t thresh) {
    if (!gray || !binary || w <= 0 || h <= 0) return;

    for (int i = 0; i < w * h; i++) {
        binary[i] = (gray[i] >= thresh) ? 1 : 0;
    }
}

int binary_to_braille(const unsigned char *binary, int w, int h, int cols, int rows, uint32_t *output) {
    if (!binary || !output || w <= 0 || h <= 0) return -1;
    if (w % BRAILLE_CELL_W != 0 || h % BRAILLE_CELL_H != 0) return -1;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            uint8_t bits = 0;

            for (int dy = 0; dy < BRAILLE_CELL_H; dy++) {
                for (int dx = 0; dx < BRAILLE_CELL_W; dx++) {
                    int px = col * BRAILLE_CELL_W + dx;
                    int py = row * BRAILLE_CELL_H + dy;

                    if (binary[py * w + px]) {
                        int dot;
                        if (dy < 3) {
                            dot = (dx == 0) ? dy + 1 : dy + 4;
                        } else {
                            dot = (dx == 0) ? 7 : 8;
                        }

                        bits |= (1 << (dot - 1));
                    }
                }
            }

            output[row * cols + col] = 0x2800 + bits;
        }
    }
    return 0;
}

int braille_to_utf8(uint32_t codepoint, char *utf8_buf) {
    if (!utf8_buf) return 0;
    if (codepoint < 0x800 || codepoint > 0xFFFF) return 0;

    utf8_buf[0] = (char)(0xE0 | (codepoint >> 12));
    utf8_buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    utf8_buf[2] = (char)(0x80 | (codepoint & 0x3F));
    utf8_buf[3] = '\0';
    return 3;
}

void print_braille_art(const uint32_t *braille, int rows, int cols) {
    if (!braille || rows <= 0 || cols <= 0) return;

    char utf8_buf[4];
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            braille_to_utf8(braille[row * cols + col], utf8_buf);
            fwrite(utf8_buf, 1, 3, stdout);
        }
        fputc('\n', stdout);
    }
}

int calculate_optimal_cover_size(int controls_height) {
    if (controls_height < 10) {
        return BRAILLE_MIN_SIZE;
    }

    int available_height = controls_height - 5;
    if (available_height < BRAILLE_MIN_SIZE * BRAILLE_CELL_H) {
        return BRAILLE_MIN_SIZE;
    }

    int target_height = (int)(available_height * 0.7);
    int size = target_height / BRAILLE_CELL_H;

    if (size < BRAILLE_MIN_SIZE) {
        size = BRAILLE_MIN_SIZE;
    } else if (size > BRAILLE_MAX_SIZE) {
        size = BRAILLE_MAX_SIZE;
    }

    return size;
}

int generate_braille_art_dynamic(const char *image_path,
                                  uint8_t threshold,
                                  int target_width,
                                  int target_height,
                                  char *output,
                                  size_t output_size) {
    if (!image_path || !output || output_size == 0) return -1;
    if (target_width <= 0 || target_height <= 0) return -1;

    unsigned char *rgba = NULL;
    int w, h;

    if (load_image(image_path, &rgba, &w, &h) != 0) {
        return -1;
    }

    /* Defence in depth: reject unexpected huge dimensions even if the
     * image loader somehow let them through (e.g. future codec changes). */
    if (w <= 0 || h <= 0 || w > MAX_COVER_IMAGE_DIM || h > MAX_COVER_IMAGE_DIM) {
        free(rgba);
        return -1;
    }

    int pixel_width = target_width * BRAILLE_CELL_W;
    int pixel_height = target_height * BRAILLE_CELL_H;

    /* Use size_t multiplication to prevent 32-bit overflow on the
     * allocation size (belt-and-suspenders: loaders already cap dims). */
    unsigned char *gray = malloc((size_t)w * (size_t)h);
    unsigned char *resized = malloc((size_t)pixel_width * (size_t)pixel_height);
    unsigned char *binary = malloc((size_t)pixel_width * (size_t)pixel_height);
    uint32_t *braille = malloc((size_t)target_width * (size_t)target_height * sizeof(uint32_t));

    if (!gray || !resized || !binary || !braille) {
        free(rgba); free(gray); free(resized); free(binary); free(braille);
        return -1;
    }

    rgba_to_gray(rgba, w, h, gray);
    free(rgba);
    rgba = NULL;

    resize_gray(gray, w, h, resized, pixel_width, pixel_height);
    free(gray);
    gray = NULL;

    threshold_gray(resized, pixel_width, pixel_height, binary, threshold);
    free(resized);
    resized = NULL;

    if (binary_to_braille(binary, pixel_width, pixel_height, target_width, target_height, braille) != 0) {
        free(binary); free(braille);
        return -1;
    }
    free(binary);
    binary = NULL;

    size_t offset = 0;
    char utf8_buf[4];
    for (int row = 0; row < target_height; row++) {
        for (int col = 0; col < target_width; col++) {
            int len = braille_to_utf8(braille[row * target_width + col], utf8_buf);
            if (offset + len + 1 < output_size) {
                memcpy(output + offset, utf8_buf, len);
                offset += len;
            }
        }
        if (offset + 1 < output_size) {
            output[offset] = '\n';
            offset++;
        }
    }
    output[offset] = '\0';

    free(braille);
    return 0;
}

int get_braille_art_lines(const char *braille_art, char **lines, int max_lines) {
    if (!braille_art || !lines || max_lines <= 0) return 0;

    int line_count = 0;
    const char *p = braille_art;
    const char *line_start = p;

    while (*p && line_count < max_lines) {
        if (*p == '\n') {
            size_t len = p - line_start;
            if (len > 0) {
                lines[line_count] = malloc(len + 1);
                if (lines[line_count]) {
                    memcpy(lines[line_count], line_start, len);
                    lines[line_count][len] = '\0';
                    line_count++;
                }
            }
            line_start = p + 1;
        }
        p++;
    }

    if (line_start < p && line_count < max_lines) {
        size_t len = p - line_start;
        if (len > 0) {
            lines[line_count] = malloc(len + 1);
            if (lines[line_count]) {
                memcpy(lines[line_count], line_start, len);
                lines[line_count][len] = '\0';
                line_count++;
            }
        }
    }

    return line_count;
}
