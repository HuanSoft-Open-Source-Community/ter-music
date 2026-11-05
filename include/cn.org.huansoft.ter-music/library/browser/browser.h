/**
 * @file library_browser.h
 * @brief Library browsing UI — artists, albums, genres, all tracks
 *
 * Renders inside the main playlist window when g_library_state.active is set.
 * Keyboard-driven navigation: Up/Down to select, Enter to drill down,
 * ESC/Backspace to go back, M to toggle library mode.
 * 鼠标：这块窗格被浏览器占用时，点击与滚轮都归浏览器（见
 * library_handle_mouse），不能再按播放列表的行号解释。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#ifndef LIBRARY_BROWSER_H
#define LIBRARY_BROWSER_H

#include "types.h"
#include "ui/mouse.h"

/* ── Extern globals ── */
extern LibraryState g_library_state;

/**
 * Toggle library browsing mode on/off.
 * Called when the user presses the library keybind ('M') in the main view.
 */
void library_browser_toggle(void);

/**
 * Render the library browser content into win_playlist.
 * Dispatches to the correct sub-view renderer based on g_library_state.view.
 */
void render_library_content(void);

/**
 * Handle keyboard input when in library browsing mode.
 * Returns 1 if the input was consumed, 0 if it should fall through
 * to the main view input handler.
 */
#include "keymap/actions.h"

/** 按动作处理曲库浏览器输入；已处理返回 1。 */
int handle_library_action(ActionId action);

int handle_library_input(int ch);

/**
 * 处理落在本窗格上的鼠标事件（仅在 g_library_state.active 时被调用）。
 *
 * d->row 是窗格本地行（区域矩形由 getbegyx(win_playlist) 量得），
 * 行→条目映射与 render_library_home 等渲染函数同源。
 * 单击=选中并执行（下钻）；滚轮一格=上下选择一项。
 * @return 1 = 已消费（事件归浏览器，不再交给播放列表）
 */
int library_handle_mouse(const UIMouseEvent *ev, const UIMouseDecision *d);

/**
 * Called when entering library mode (sets up initial state).
 */
void library_browser_enter(void);

/**
 * Called when exiting library mode (cleans up state).
 */
void library_browser_exit(void);

#endif /* LIBRARY_BROWSER_H */
