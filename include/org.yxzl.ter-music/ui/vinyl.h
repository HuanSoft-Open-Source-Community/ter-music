/**
 * @file vinyl.h
 * @brief 黑胶碟片窗格（前端渲染）
 *
 * 黑胶/粒子动画原本内嵌在歌词窗格的无歌词分支；独立 vinyl 窗格与
 * 歌词回退共用同一套绘制函数。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_VINYL_H
#define UI_VINYL_H

#include <ncursesw/ncurses.h>

/**
 * @brief 在指定窗口内容区绘制黑胶/粒子动画。
 *
 * 颜色可用时绘制彩色半块碟面、旋转的封面圆形中心标签与彩色频谱柱；
 * 无颜色/ASCII/色对预算不足时逐字回退到原字符动画。
 *
 * @param win           目标窗口（边框/标题由调用方或 render_vinyl_pane 画）
 * @param show_messages 1 = 无歌词时显示“无歌词”两行文案
 * @param body_attr     窗格正文属性（ui_pane_body_attr），用于标签掩码/文案
 */
void render_vinyl_disc(WINDOW *win, int show_messages, int body_attr);

/** @brief 绘制独立 vinyl 窗格（含边框/标题）。 */
void render_vinyl_pane(void);

/** @brief 100ms 节拍：播放/暂停时推进黑胶动画。 */
void vinyl_tick(void);

#endif /* UI_VINYL_H */
