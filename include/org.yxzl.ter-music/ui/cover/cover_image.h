/**
 * @file cover_image.h
 * @brief 封面图片解码（PNG/JPEG）与像素缓冲工具
 *
 * 从旧的 `ui/braille/braille_art.h` 拆出：这里只有"把图片读进内存、
 * 缩放、转灰、二值化"这些与字符集无关的像素运算。TUI 字符封面
 * （`ui/cover_art.c`，▀ 半块 + 调色板）与 CLI/D-Bus 文本封面
 * （`ui/cover/text_cover.c`，盲文/ASCII/半块三种字符集）共用同一份。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef COVER_IMAGE_H
#define COVER_IMAGE_H

#include <stdint.h>
#include <stddef.h>

/* Maximum dimensions for album cover images before scaling.
 * Anything beyond 4096×4096 wastes memory and may be a
 * decompression-bomb attack. */
#define MAX_COVER_IMAGE_DIM 4096

/**
 * @brief 解码 PNG 为 RGBA 缓冲（调用方 free）。
 * @return 0 成功；-1 失败（读不了、不是 PNG、尺寸越界）
 */
int load_png_image(const char *filepath, unsigned char **rgba_out, int *w, int *h);

/**
 * @brief 解码 JPEG 为 RGB 缓冲（调用方 free）。
 * @return 0 成功；-1 失败
 */
int load_jpeg_image(const char *filepath, unsigned char **rgb_out, int *w, int *h);

/**
 * @brief 按扩展名内容嗅探解码为 RGBA 缓冲（JPEG 补齐 alpha=255）。
 * @return 0 成功；-1 失败
 */
int load_image(const char *filepath, unsigned char **rgba_out, int *w, int *h);

/** @brief RGB(A) 缓冲转亮度缓冲（BT.601，四舍五入）。0 成功；-1 参数错误 */
int rgba_to_gray(const unsigned char *rgba, int w, int h, unsigned char *gray);

/** @brief 亮度缓冲双线性缩放。0 成功；-1 参数错误 */
int resize_gray(const unsigned char *src, int sw, int sh,
                unsigned char *dst, int dw, int dh);

/** @brief 亮度缓冲按阈值二值化（1=亮，0=暗） */
void threshold_gray(const unsigned char *gray, int w, int h,
                    unsigned char *binary, uint8_t thresh);

#endif /* COVER_IMAGE_H */
