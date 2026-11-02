/**
 * @file layout.h
 * @brief TUI 主界面运行时布局管理
 *
 * 负责加载布局目录、读取/保存前端本地选中状态、把布局解析为窗口矩形、
 * 建窗、按窗格分配颜色，并提供统一的窗格边框/标题/背景绘制入口。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "ui/layout_geometry.h"
#include "ui/layout_spec.h"

#include <ncursesw/ncurses.h>

/**
 * @brief 加载布局目录并解析选中布局。可安全重复调用。
 * @return 0 成功（含回退到内置 classic）。
 */
int ui_layout_init(void);

/**
 * @brief 重新读取布局文件与本地选中状态。
 * @return 重新加载后的布局数量；失败时保留旧目录并返回 -1。
 */
int ui_layout_reload(void);

int ui_layout_count(void);
const char *ui_layout_id_at(int index);
const char *ui_layout_name_at(int index);
int ui_layout_current_index(void);
const char *ui_layout_current_id(void);
const char *ui_layout_current_name(void);

/**
 * @brief 拷贝当前布局规范到 out（布局编辑器打开时用）。
 * @return 0 成功；-1 参数错误或布局系统未初始化。
 */
int ui_layout_current_spec(LayoutSpec *out);

/**
 * @brief 切换当前布局并写本地状态。
 * @return 0 成功；-1 未找到或状态写失败（内存中仍已应用）。
 */
int ui_layout_select(const char *id);

/* ============================================================
 * 用户布局文件（~/.config/ter-music/layouts.json，供布局编辑器）
 * ============================================================ */

/** @brief 用户布局文件完整路径；失败返回 -1。 */
int layout_user_file_path(char *path, size_t path_size);

/**
 * @brief 把一份布局写入用户布局文件（同 id 覆盖）。
 *
 * 读现有用户文件 → upsert → 原子写回；文件里其它用户布局保持不变。
 * 用户文件损坏时按空目录处理并重建（写入会覆盖旧内容）。
 * @return 0 成功；-1 失败（err 给出原因）。
 */
int layout_user_save_layout(const LayoutSpec *spec, char *err, size_t err_size);

/**
 * @brief 从用户布局文件删除一份布局（用于恢复内置预设原貌）。
 *
 * 删空后直接移除文件。文件不存在或 id 不存在视为成功。
 * @return 0 成功；-1 失败（err 给出原因）。
 */
int layout_user_remove_layout(const char *id, char *err, size_t err_size);

/** @brief 按当前主题/彩虹状态重新初始化窗格颜色 pair。 */
void ui_layout_apply_colors(void);

int ui_pane_is_visible(LayoutPaneKind kind);
int ui_pane_body_attr(LayoutPaneKind kind);
int ui_pane_border_attr(LayoutPaneKind kind);

/**
 * @brief 统一的窗格起始绘制：填背景、画边框、画标题。
 *
 * 即使窗口只有 1×1，也会用背景色填满该单元格（只跳过边框/标题），
 * 保证严格平铺布局在极小终端上也不留空白。
 */
void ui_pane_begin(LayoutPaneKind kind, WINDOW *win, const char *title);

/**
 * @brief 在窗格内容区（避开上下左右边框）内裁剪并输出文本。
 *
 * 行号必须落在 1..h-2，列号必须落在 1..w-2；超出直接丢弃。文本按剩余
 * 内容列宽裁剪，因此动态元数据/状态文案不会越过右边框或底边框。
 */
void ui_pane_clip_print(WINDOW *win, int row, int col, const char *text);
void ui_pane_clip_printf(WINDOW *win, int row, int col,
                         const char *fmt, ...);

/**
 * @brief 在内容绘制之后重绘窗格左右竖边与底边（保留第 0 行标题）。
 *
 * 作为最后一道视觉保险：即使某个渲染器（例如字符封面）意外越过内容区，
 * 边框仍由本函数最后覆盖，不会被内容盖住。不重画顶边/标题行。
 */
void ui_pane_repaint_border(LayoutPaneKind kind, WINDOW *win);

#endif /* UI_LAYOUT_H */
