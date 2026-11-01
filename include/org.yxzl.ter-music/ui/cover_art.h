/**
 * @file cover_art.h
 * @brief TUI 字符封面：配色单元格缓存、颜色对绑定、绘制
 *
 * 信息窗格与控制区共用一个模块、两个独立槽位（各自独立的 ncurses pair
 * 区间与缓存），避免一个窗格重建颜色对时改到另一个窗格已绘制的单元。
 * 黑胶中心标签另占一套标签槽 pair 区间与缓存，按角度旋转圆形裁剪。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_COVER_ART_H
#define UI_COVER_ART_H

#include <ncursesw/ncurses.h>

#include "ui/cover_art_cells.h"

#define COVER_ART_SLOT_INFO     0
#define COVER_ART_SLOT_CONTROLS 1
#define COVER_ART_SLOT_COUNT    2

/**
 * @brief 取指定槽位的封面网格（带缓存）。
 *
 * @param slot        COVER_ART_SLOT_*
 * @param image_path 已解码出的封面图片路径（png/jpeg）
 * @param cols        字符列数
 * @param rows        字符行数
 * @return 网格指针（下一次调用/重置前有效）；失败返回 NULL
 */
const CoverArtGrid *cover_art_get(int slot, const char *image_path,
                                  int cols, int rows);

/**
 * @brief 把网格画到窗口内；越界单元跳过，结束后恢复 body_attr。
 *
 * @param body_attr 窗格正文属性（ui_pane_body_attr）
 */
void cover_art_draw(WINDOW *win, int x, int y,
                    const CoverArtGrid *grid, int body_attr);

/**
 * @brief 取黑胶中心标签的旋转圆形网格（带缓存）。
 *
 * 与信息/控制区封面使用同一套色阶、调色板量化与色对预算逻辑；角度按
 * 2π/64 分桶缓存，角度未变时直接复用上一帧网格。颜色不可用或绑定失败
 * 返回 NULL，调用方应回退为无标签的纯色碟心（整个彩色黑胶不可用时才由
 * vinyl 层退回原字符动画）。
 *
 * @param image_path 已解码出的封面图片路径（png/jpeg）
 * @param cols       标签字符列数（3..COVER_ART_MAX_COLS）
 * @param rows       标签字符行数（3..COVER_ART_MAX_ROWS）
 * @param angle      旋转角（弧度）
 * @return 网格指针（下一次调用前有效）；失败返回 NULL
 */
const CoverArtGrid *cover_art_get_label(const char *image_path,
                                        int cols, int rows, double angle);

/**
 * @brief 黑胶彩色块使用的固定色对范围与色阶。
 *
 * @param tier       输出 8 / 256
 * @param pair_base  输出固定色对起始 pair
 * @param pair_budget 输出固定色对数量（COVER_ART_VINYL_FIXED_PAIRS）
 * @return 1 可用；0 应回退字符动画
 */
int cover_art_vinyl_colors(int *tier, int *pair_base, int *pair_budget);

#endif /* UI_COVER_ART_H */
