#ifndef SCROLLBAR_H
#define SCROLLBAR_H

#include <ncursesw/ncurses.h>

// 在窗格右侧绘制滚动条
// win:      ncurses 窗口
// top:      内容区域的起始行号（在 win 内）
// height:   内容区域高度（行数）
// total:    内容总行数
// visible:  可见行数
// offset:   当前滚动偏移（首个可见行索引）
// col:      滚动条所在列号（w-2 或 max_x-2）
// 返回值：1=已绘制滚动条，0=无需滚动条
int scrollbar_draw(WINDOW *win, int top, int height,
                   int total, int visible, int offset, int col);

#endif
