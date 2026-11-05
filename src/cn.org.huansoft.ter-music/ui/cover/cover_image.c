/**
 * @file cover_image.c
 * @brief 封面图片解码与像素缓冲工具（实现）
 *
 * 拆自旧的 `ui/braille/`：`load_png_image` / `load_jpeg_image` /
 * `load_image` 原在 `image_loader.c`，`rgba_to_gray` / `resize_gray` /
 * `threshold_gray` 原在 `braille_art.c`——三者都是与字符集无关的像素
 * 运算，TUI 半块封面与文本封面字符集共用，故归入本文件。
 * 函数体逐字搬迁，无行为改动。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <jpeglib.h>
#include "ui/cover/cover_image.h"

int load_png_image(const char *filepath, unsigned char **rgba_out, int *w, int *h) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return -1;
    }

    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) {
        fclose(fp);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);

    /* Reject decompression bombs before libpng allocates internally */
    png_set_user_limits(png, MAX_COVER_IMAGE_DIM, MAX_COVER_IMAGE_DIM);

    png_read_info(png, info);

    *w = png_get_image_width(png, info);
    *h = png_get_image_height(png, info);

    if (*w <= 0 || *h <= 0 || *w > MAX_COVER_IMAGE_DIM || *h > MAX_COVER_IMAGE_DIM) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type & PNG_COLOR_MASK_ALPHA) {
    } else {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);

    *w = png_get_image_width(png, info);
    *h = png_get_image_height(png, info);
    int row_bytes = png_get_rowbytes(png, info);

    png_bytep *row_pointers = malloc(sizeof(png_bytep) * (*h));
    if (!row_pointers) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    for (int i = 0; i < *h; i++) {
        row_pointers[i] = malloc(row_bytes);
        if (!row_pointers[i]) {
            for (int j = 0; j < i; j++) free(row_pointers[j]);
            free(row_pointers);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return -1;
        }
    }

    png_read_image(png, row_pointers);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *rgba_out = malloc((*w) * (*h) * 4);
    if (!*rgba_out) {
        for (int i = 0; i < *h; i++) free(row_pointers[i]);
        free(row_pointers);
        return -1;
    }

    for (int y = 0; y < *h; y++) {
        memcpy(*rgba_out + y * (*w) * 4, row_pointers[y], (*w) * 4);
        free(row_pointers[y]);
    }
    free(row_pointers);

    return 0;
}

int load_jpeg_image(const char *filepath, unsigned char **rgb_out, int *w, int *h) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return -1;
    }

    unsigned char jpeg_sig[2];
    if (fread(jpeg_sig, 1, 2, fp) != 2 || jpeg_sig[0] != 0xFF || jpeg_sig[1] != 0xD8) {
        fclose(fp);
        return -1;
    }
    fseek(fp, 0, SEEK_SET);

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    /* Reject decompression bombs before decompression allocates memory */
    if (cinfo.image_width <= 0 || cinfo.image_height <= 0 ||
        cinfo.image_width > MAX_COVER_IMAGE_DIM ||
        cinfo.image_height > MAX_COVER_IMAGE_DIM) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    jpeg_start_decompress(&cinfo);

    *w = cinfo.output_width;
    *h = cinfo.output_height;
    int num_channels = cinfo.output_components;

    if (num_channels != 1 && num_channels != 3) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    *rgb_out = malloc((*w) * (*h) * 3);
    if (!*rgb_out) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    unsigned char *row_buffer = malloc((*w) * num_channels);
    if (!row_buffer) {
        free(*rgb_out);
        *rgb_out = NULL;
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < *h; y++) {
        jpeg_read_scanlines(&cinfo, &row_buffer, 1);
        if (num_channels == 1) {
            for (int x = 0; x < *w; x++) {
                (*rgb_out)[(y * (*w) + x) * 3] = row_buffer[x];
                (*rgb_out)[(y * (*w) + x) * 3 + 1] = row_buffer[x];
                (*rgb_out)[(y * (*w) + x) * 3 + 2] = row_buffer[x];
            }
        } else {
            memcpy(*rgb_out + y * (*w) * 3, row_buffer, (*w) * 3);
        }
    }

    free(row_buffer);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    return 0;
}

int load_image(const char *filepath, unsigned char **rgba_out, int *w, int *h) {
    if (!filepath || !rgba_out || !w || !h) return -1;

    *rgba_out = NULL;
    unsigned char *rgb = NULL;

    if (load_jpeg_image(filepath, &rgb, w, h) == 0) {
        *rgba_out = malloc((*w) * (*h) * 4);
        if (!*rgba_out) {
            free(rgb);
            return -1;
        }
        for (int i = 0; i < (*w) * (*h); i++) {
            (*rgba_out)[i * 4] = rgb[i * 3];
            (*rgba_out)[i * 4 + 1] = rgb[i * 3 + 1];
            (*rgba_out)[i * 4 + 2] = rgb[i * 3 + 2];
            (*rgba_out)[i * 4 + 3] = 255;
        }
        free(rgb);
        return 0;
    }

    if (load_png_image(filepath, rgba_out, w, h) == 0) {
        return 0;
    }

    return -1;
}

/* ── 像素缓冲工具（原 braille_art.c:7-63，逐字搬迁） ── */

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
