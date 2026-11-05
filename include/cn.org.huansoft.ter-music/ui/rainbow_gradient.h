/**
 * @file rainbow_gradient.h
 * @brief 科乐美彩蛋的纯数学前景色色板与相位计算（无 ncurses 依赖）
 *
 * 彩蛋只在界面前景上做斜向渐变：背景由 UI 层统一设为透明（或黑回退），
 * 本模块只负责“某一相位应该用哪一种前景色”。色板排除纯黑、纯白和
 * **中性灰**（黑↔白过渡轴），但保留带色相的暗灰/浅灰，让彩黑、彩白
 * 过渡更连贯：
 *   - 256 色终端：在 xterm 6×6×6 色立方里挑亮度适中的色阶；
 *   - 8 色终端：只用 ANSI 1..6 六个彩色基色。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_RAINBOW_GRADIENT_H
#define UI_RAINBOW_GRADIENT_H

/* 一次渐变的档数上限；256 色终端使用全套，低色终端按色对预算缩减。 */
#define RAINBOW_MAX_STEPS 128

/**
 * @brief 终端色数 → 建议的渐变档数。
 *
 * 256 色及以上 → RAINBOW_MAX_STEPS；8..255 色 → 6（8 色档，16/88 色
 * 也按这一档处理）；其它（黑白/无颜色）→ 0。档数只是上限，调用方还要
 * 按可用色对缩减。
 */
int rainbow_default_steps(int colors);

/**
 * @brief 生成 steps 档前景色（终端调色板下标）。
 *
 * 256 色终端走一条首尾相接的三维 Lissajous 色路，再映射到允许色；
 * 8 色档走 6 段色环并映射到 ANSI 1..6。任何一档都不会是黑(0)、白(7)、
 * 灰阶（r=g=b）或近中性灰，但允许带色相的暗灰/浅灰。
 *
 * @param steps  1..RAINBOW_MAX_STEPS
 * @param colors 终端 COLORS（>= 8）
 * @param fg_out 长度至少 steps 的数组
 * @return 0 成功；-1 参数非法。
 */
int rainbow_build_swatches(int steps, int colors, int *fg_out);

/**
 * @brief 为某个前景色挑选反色高亮用的对比文字色。
 *
 * 返回 0（黑）或 7（白）：背景越亮越用黑，越暗越用白。它只用于
 * A_REVERSE 高亮格的文字，不属于流光色板本身。
 */
int rainbow_contrast_fg(int fg);

/**
 * @brief 从当前相位附近的色板中挑出“暗 / 中 / 亮”三档。
 *
 * 用于黑胶碟面：最多取 8 个邻近色板项，按调色板下标去重后按亮度排序，
 * 取最暗、中位、最亮写入 out[0..2]。in[2] 最亮，供旋转高光使用。
 *
 * @param fg     色板（rainbow_build_swatches 的输出）
 * @param steps  色板档数
 * @param offset 当前相位
 * @param out    输出 3 个调色板下标
 */
void rainbow_pick_shades(const int *fg, int steps, int offset, int out[3]);

/**
 * @brief 屏幕坐标 + 相位 → 渐变档下标。
 *
 * 斜率固定为 x + 2y：终端字符约 2:1，这样在视觉上接近 45°。
 * 返回 0..steps-1；offset 可为负，内部做非负取模。
 */
int rainbow_phase(int x, int y, int offset, int steps);

#endif
