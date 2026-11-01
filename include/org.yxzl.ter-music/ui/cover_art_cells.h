/**
 * @file cover_art_cells.h
 * @brief TUI 字符封面的纯配色/字形计算（无 ncurses、无图片库依赖）
 *
 * 彩色模式统一使用半块字符 U+2580（▀）作为“两层合成”：
 *   - 上半格 = 前景色 fg（上层）
 *   - 下半格 = 背景色 bg（下层）
 * 上下颜色相同时视觉上就是全块；因此彩色模式不需要 U+2588（█），
 * 也不存在“给 █ 定义两种颜色”的问题。
 *
 * 单色回退使用 █/▀/▄/空格 表达 2 位灰度；ASCII 回退用 #/空格。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_COVER_ART_CELLS_H
#define UI_COVER_ART_CELLS_H

#include <stddef.h>

#define COVER_ART_MAX_COLS 32
#define COVER_ART_MAX_ROWS 16

/* 主题 pair 1..6、布局 pair 10..25 已占用；封面从 32 起。
 * 上限 224 使 32+224=256，兼容 ncurses 的 8 位 pair 字段。 */
#define COVER_ART_PAIR_BASE 32
#define COVER_ART_PAIR_CAP  224

#define COVER_ART_MONO_THRESHOLD 128

/* 彩色黑胶的固定色对：3×3 碟面上下半色组合 + 3 个频谱柱颜色。 */
#define COVER_ART_VINYL_FIXED_PAIRS 12

/* 启用彩色黑胶所需的最小可用色对数。低于此值时不强行挤占封面预算，
 * 黑胶回退到原字符动画，信息/控制区封面保持原有 2:1 预算。 */
#define COVER_ART_VINYL_MIN_TOTAL 64

typedef struct {
    int color_vinyl;       /* 1 = 标签槽与固定色对可用 */
    int info_base;
    int info_budget;
    int controls_base;
    int controls_budget;
    int label_base;
    int label_budget;
    int fixed_base;
    int fixed_budget;
} CoverArtPairPlan;

typedef enum {
    COVER_GLYPH_SPACE = 0,  /* ' ' */
    COVER_GLYPH_UPPER = 1,  /* ▀：上=fg，下=bg */
    COVER_GLYPH_LOWER = 2,  /* ▄：下=fg，上=bg（单色灰度用） */
    COVER_GLYPH_FULL  = 3,  /* █：fg 单色（单色灰度用） */
    COVER_GLYPH_ASCII = 4   /* '#'（ASCII 回退用） */
} CoverGlyph;

typedef struct {
    unsigned char glyph;   /* CoverGlyph */
    short fg;              /* 调色板颜色索引；单色为 -1 */
    short bg;              /* 调色板颜色索引；单色为 -1 */
    int pair;              /* ncurses pair 号；由 UI 层绑定，单色为 0 */
} CoverArtCell;

typedef struct {
    int cols;
    int rows;
    int colorized;         /* 1 = 彩色（单元格用 ▀ + pair） */
    int pair_count;        /* 本网格实际使用的 (fg,bg) 组合数 */
    CoverArtCell cells[COVER_ART_MAX_ROWS][COVER_ART_MAX_COLS];
} CoverArtGrid;

/**
 * @brief 终端能力 → 色阶。
 *
 * @param has_colors  has_colors() 的结果（1/0）
 * @param colors      COLORS（无颜色时传 0）
 * @param unicode     是否允许 UTF-8 半块字符（ASCII 回退时传 0）
 * @return 0=单色，8=8 色，256=256 色
 */
int cover_art_tier_from_caps(int has_colors, int colors, int unicode);

/**
 * @brief 按 COLOR_PAIRS 拆分信息窗格 / 控制区 / 黑胶标签与固定色对的预算。
 *
 * 可用色对充足（>= COVER_ART_VINYL_MIN_TOTAL）时：
 *   固定色对取 COVER_ART_VINYL_FIXED_PAIRS，剩余按 info:controls:label = 2:1:1；
 * 否则 color_vinyl=0，label/fixed 预算为 0，info/controls 维持 2/3 与 1/3
 * 的旧比例，避免小终端为黑胶牺牲封面质量。总预算受 COVER_ART_PAIR_CAP 限制。
 */
void cover_art_pair_plan(int color_pairs, CoverArtPairPlan *out);

/**
 * @brief 从方形封面源图构建“旋转圆形标签”网格。
 *
 * 标签把源图的内切圆映射到 cols×rows 的字符矩形（cols/rows 通常为奇数，
 * 使圆心落在格心）；格心落在圆外的单元是掩码空格，不占用颜色对。
 * 选色统计按未旋转的圆形裁剪做一次，旋转只改变位置，避免颜色随角度闪烁。
 * 颜色不可用（tier=0、ASCII、预算<4）或 2 色仍放不下预算时，输出
 * colorized=0；调用方不应绘制单色块（黑胶层此时回退为无标签的彩色碟面，
 * 整个彩色黑胶都不可用时才退回原字符动画）。
 *
 * @param src_rgb  src_w × src_h 像素、每像素 3 字节、行优先
 * @param cols     标签字符列数（3..COVER_ART_MAX_COLS）
 * @param rows     标签字符行数（3..COVER_ART_MAX_ROWS）
 * @param angle    旋转角（弧度）；源图内切圆随角度旋转
 * @param tier     0 / 8 / 256
 * @param pair_budget 本槽可用的颜色对数量（<4 时 colorized=0）
 * @param unicode  1=允许半块字符；0=colorized=0
 * @param out      输出网格；失败时清零
 *
 * @return 0 成功（含 colorized=0 的“不可着色”）；-1 参数非法
 */
int cover_art_cells_build_label(const unsigned char *src_rgb,
                                int src_w, int src_h,
                                int cols, int rows, double angle,
                                int tier, int pair_budget, int unicode,
                                CoverArtGrid *out);

/**
 * @brief 从目标分辨率 RGB 缓冲构建单元格网格。
 *
 * @param rgb     cols × rows*2 像素、每像素 3 字节、行优先（上像素行、
 *                下像素行交替）
 * @param cols    字符列数（1..COVER_ART_MAX_COLS）
 * @param rows    字符行数（1..COVER_ART_MAX_ROWS）
 * @param tier    0 / 8 / 256
 * @param pair_budget 本槽可用的颜色对数量（<4 时回退单色）
 * @param unicode 1=允许半块字符；0=ASCII 单色
 * @param out     输出网格；失败时清零
 *
 * @return 0 成功；-1 参数非法
 */
int cover_art_cells_build(const unsigned char *rgb, int cols, int rows,
                          int tier, int pair_budget, int unicode,
                          CoverArtGrid *out);

#endif /* UI_COVER_ART_CELLS_H */
