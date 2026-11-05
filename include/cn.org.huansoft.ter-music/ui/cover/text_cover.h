/**
 * @file text_cover.h
 * @brief CLI / D-Bus 文本封面：盲文、ASCII、半块三种字符集的字符画
 *
 * 从旧的 `ui/braille/braille_art.h` 拆出。**注意职责边界**：TUI 里的专辑
 * 封面不再使用盲文点阵——它由 `ui/cover_art.c` + `ui/cover_art_cells.c`
 * 用 ▀ 半块单元 + 终端调色板绘制。本模块只服务 `ter-music cover/show`
 * 与 D-Bus `Info.GetCoverArt` 的 `--charset braille|ascii|half` 输出，
 * 其中 braille 分支仍逐字生成盲文点阵（故 `binary_to_braille` 等函数
 * 名保留 braille 字样）。
 *
 * 三种字符集的字符长宽比一致（盲文 2×4 像素/字符、ASCII 2×1、半块 1×2），
 * 因此在等宽字体下占用相同版面；阈值与缩放由 `ui/cover/cover_image.h`
 * 的像素工具完成。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef TEXT_COVER_H
#define TEXT_COVER_H

#include <stdint.h>
#include <stddef.h>

/* 盲文单元几何：1 字符 = 2×4 像素（仅 braille 字符集使用） */
#define BRAILLE_CELL_W 2
#define BRAILLE_CELL_H 4

/* 文本封面亮度阈值：三种字符集共用（原 BRAILLE_DEFAULT_THRESHOLD） */
#define TEXT_COVER_DEFAULT_THRESHOLD 128

/**
 * @brief 二值图按盲文单元打包成盲文码位（0x2800 + 8 点）。
 * @return 0 成功；-1 参数错误或像素尺寸不是单元的整数倍
 */
int binary_to_braille(const unsigned char *binary, int w, int h,
                      int cols, int rows, uint32_t *output);

/** @brief 盲文码位写入 3 字节 UTF-8 + '\0'；返回写入字节数（非盲文返回 0） */
int braille_to_utf8(uint32_t codepoint, char *utf8_buf);

/**
 * @brief 生成盲文点阵文本封面（output 为多行文本，UTF-8）。
 * @return 0 成功；-1 失败（读图、尺寸、内存）
 */
int generate_braille_art_dynamic(const char *image_path,
                                 uint8_t threshold,
                                 int target_width,
                                 int target_height,
                                 char *output,
                                 size_t output_size);

/** @brief 生成 ASCII 字符封面（每字符 2×1 像素，亮点 '#'，暗点空格）。 */
int generate_ascii_art_dynamic(const char *image_path,
                               uint8_t threshold,
                               int target_width,
                               int target_height,
                               char *output,
                               size_t output_size);

/** @brief 生成半块字符封面（每字符 1×2 像素：▀ ▄ █ 与空格）。 */
int generate_halfblock_art_dynamic(const char *image_path,
                                   uint8_t threshold,
                                   int target_width,
                                   int target_height,
                                   char *output,
                                   size_t output_size);

#endif /* TEXT_COVER_H */
