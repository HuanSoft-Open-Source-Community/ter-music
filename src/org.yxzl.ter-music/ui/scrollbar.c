#include "ui/scrollbar.h"

int scrollbar_draw(WINDOW *win, int top, int height,
                   int total, int visible, int offset, int col) {
    if (!win || total <= visible || height <= 0) {
        return 0;
    }

    // 先清除整个滚动条轨道，防止其他内容写到该列后形成视觉残留
    for (int i = 0; i < height; i++) {
        mvwaddch(win, top + i, col, ' ');
    }

    // 计算滑块大小（比例 = 可见行数 / 总行数）
    double thumb_ratio = (double)visible / total;
    int thumb_h = (int)(thumb_ratio * height);
    if (thumb_h < 1) thumb_h = 1;

    // 计算滑块位置
    int max_offset = total - visible;
    double pos = (max_offset > 0) ? (double)offset / max_offset : 0;
    int thumb_y = (int)(pos * (height - thumb_h));
    if (thumb_y + thumb_h > height)
        thumb_y = height - thumb_h;
    if (thumb_y < 0) thumb_y = 0;

    // 绘制滑块（█）
    for (int i = thumb_y; i < thumb_y + thumb_h; i++) {
        mvwaddch(win, top + i, col, ACS_BLOCK);
    }
    return 1;
}
