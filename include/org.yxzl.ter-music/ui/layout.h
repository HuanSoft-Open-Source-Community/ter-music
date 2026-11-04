/**
 * @file layout.h
 * @brief TUI 主界面运行时布局管理
 *
 * 负责加载布局目录、读取/保存前端本地选中状态、展开强制平铺分栏树、
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

/**
 * @brief 最近一次 init/reload 中自动适配（旧几何被改为强制平铺）的布局数。
 *
 * 用于设置页重载提示；解析层不再因几何问题拒绝布局。
 */
int ui_layout_last_adjusted(void);

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

/**
 * @brief 当前布局某窗格字段的生效颜色。
 * @param field       LAYOUT_COLOUR_FG / LAYOUT_COLOUR_BG。
 * @param from_layout 可为 NULL；1 = 布局显式覆盖，0 = 继承角色基色。
 * @return 颜色值（-1 终端默认 / 0..255 调色板）。
 */
int ui_layout_pane_colour(LayoutPaneKind kind, int field, int *from_layout);

/** @brief 该窗格种类是否出现在当前布局的分栏树里（不看窗口是否已建）。 */
int ui_layout_pane_enabled(LayoutPaneKind kind);

/**
 * @brief 当前布局同时含歌词与黑胶两格、且当前曲目没有歌词 → 歌词格临时让位。
 *
 * 判定只读 `player_lyrics()` 的常驻快照（两个后端都由 pump 维护），
 * 不触发整篇歌词拉取、不阻塞、无退避。
 *
 * 让位只影响运行时的几何：布局本身、颜色设置与用户布局文件都不变，
 * 换到有歌词的曲目时歌词格自动回来。
 */
int ui_layout_lyrics_pane_suppressed(void);

/**
 * @brief 最近一次 create_layout() 实际生效的"歌词格让位"值。
 *
 * 用于把期望值与实际生效几何对账：界面只在两者不一致时重建一次，
 * 避免"期望值被当成了生效值"导致歌词格永久不回来。
 * 只读；不改动布局。
 * @return -1 = 尚未建过布局；0 = 歌词格在几何里；1 = 已让位（歌词格被摘掉）。
 */
int ui_layout_lyrics_pane_applied(void);

/**
 * @brief 修改当前布局某窗格的前景/背景色并落盘（设置页窗格色行用）。
 *
 * 提交语义为"先写文件，成功才改内存"，失败时界面与布局都保持原样：
 * - 内置预设与用户布局：写用户文件的**同 id 覆盖**，每个预设至多多一条记录；
 * - 来源为 TER_MUSIC_LAYOUTS：同 id 会被 env 目录再盖掉，改用自增副本
 *   `<id>-2`/`<id>-3`… 并切换过去；
 * - 用户文件已满（LAYOUT_MAX_CATALOG）：返回 -1，不做任何修改。
 * @param value LAYOUT_COLOR_INHERIT 表示恢复"跟随主题"。
 * @param out_id 可为 NULL；返回最终生效的布局 id。
 * @return 0 成功；-1 失败（err 给出原因）。
 */
int ui_layout_set_pane_colour(LayoutPaneKind kind, int field, int value,
                              char *out_id, size_t out_id_size,
                              char *err, size_t err_size);

/** @brief 窗格的本地化显示名（布局编辑器与窗格标题共用）。 */
const char *ui_layout_pane_title(LayoutPaneKind kind);

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
