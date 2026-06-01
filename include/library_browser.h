/**
 * @file library_browser.h
 * @brief Library browsing UI — artists, albums, genres, all tracks
 *
 * Renders inside the main playlist window when g_library_state.active is set.
 * Keyboard-driven navigation: Up/Down to select, Enter to drill down,
 * ESC/Backspace to go back, M to toggle library mode.
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#ifndef LIBRARY_BROWSER_H
#define LIBRARY_BROWSER_H

#include "defs.h"

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
int handle_library_input(int ch);

/**
 * Called when entering library mode (sets up initial state).
 */
void library_browser_enter(void);

/**
 * Called when exiting library mode (cleans up state).
 */
void library_browser_exit(void);

#endif /* LIBRARY_BROWSER_H */
