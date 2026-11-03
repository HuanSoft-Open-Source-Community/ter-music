/**
 * @file rainbow.h
 * @brief 科乐美彩蛋：整屏斜向渐变流光（屏幕合成层）
 *
 * 与主题/布局配色解耦：普通格子只替换前景色，背景统一透明（终端不支持
 * 默认色时回退黑）；反色/高亮选中格用“对比文字 + 流光背景”专用色对，
 * 中心封面与频谱柱保持原色，黑胶碟面由 vinyl_rainbow_apply() 重染。
 * 不修改 AppConfig.theme，也不写布局/配置文件。关闭时重绘即恢复原色。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_RAINBOW_H
#define UI_RAINBOW_H

#include <ncursesw/ncurses.h>

/**
 * @brief 观察一个按键是否构成 Konami 序列；命中后切换彩蛋。
 *
 * 序列为 ↑↑↓↓←→←→ B A B A，任意两键间隔超过 3 秒则重置。
 * 调用方只负责把当前视图的按键喂进来，不消费按键。
 */
void check_konami_input(int ch);

/** @brief 切换彩蛋；黑白终端静默忽略，色对不足时提示且不激活。 */
void toggle_rainbow_mode(void);

/** @brief 彩蛋当前是否正在做前景合成。 */
int ui_rainbow_active(void);

/**
 * @brief 对当前可见界面做一次整屏合成并刷新。
 *
 * 未激活、无颜色或色对未就绪时立即返回；激活时按时间推进相位，
 * 依次处理 stdscr、主视图窗格、控制区/设置弹层，最后 doupdate。
 * 可在事件循环每轮调用（相位未变时不会产生终端输出）。
 */
void ui_rainbow_paint(void);

#endif
