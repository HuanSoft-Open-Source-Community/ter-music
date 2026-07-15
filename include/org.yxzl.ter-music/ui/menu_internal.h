#ifndef MENU_INTERNAL_H
#define MENU_INTERNAL_H

#include "types.h"

/*
 * Internal declarations shared across menu/view modules
 * (menus.c, util.c, settings.c, history.c, favorites.c,
 *  playlist_view.c, info_view.c, help_view.c)
 */

/* ============================================================
 * Sidebar item arrays (defined in util.c)
 * ============================================================ */

extern const char *settings_sidebar_items[];
extern const char *history_sidebar_items[];
extern const char *playlist_sidebar_items[];
extern const char *favorites_sidebar_items[];
extern const char *info_sidebar_items[];
extern const char *help_sidebar_items[];
extern const char *language_sidebar_items[];

extern const int SETTINGS_ITEM_COUNT;
extern const int HISTORY_ITEM_COUNT;
extern const int PLAYLIST_ITEM_COUNT;
extern const int FAVORITES_ITEM_COUNT;
extern const int INFO_ITEM_COUNT;
extern const int HELP_ITEM_COUNT;
extern const int LANGUAGE_ITEM_COUNT;

/* ============================================================
 * Text helper functions (defined in util.c)
 * ============================================================ */

const char *menu_text(const char *key, const char *unused);
const char *menu_bool_text(int enabled);
const char *menu_color_name(int color_value);
const char *menu_language_name(void);
void sanitize_ascii_menu_text(char *dest, size_t dest_size, const char *src);
const char **resolve_sidebar_items(const char **items);
const char *resolve_menu_title(const char *title);

/* ============================================================
 * Status message (defined in util.c)
 * ============================================================ */

void show_status_message(const char *msg);
const char *get_status_message(void);
time_t get_status_message_time(void);

void rounded_box(WINDOW *win);

/* ============================================================
 * Global variable declarations (defined in util.c)
 * ============================================================ */

extern ViewMode g_current_view;
extern int g_menu_selected_idx;
extern PlayHistory g_play_history;
extern Favorites g_favorites;
extern DirHistory g_dir_history;
extern int g_content_selected_idx;
extern FocusArea g_focus_area;

/* ============================================================
 * Per-view scroll / state variables (defined in util.c)
 * ============================================================ */

extern int g_history_content_offset;
extern int g_favorites_content_offset;
extern int g_playlist_content_offset;

/* ============================================================
 * Playlist view state (defined in playlist_view.c)
 * ============================================================ */

extern int g_playlist_view_mode;
extern int g_playlist_selected_playlist;
extern int g_playlist_track_offset;

/* ============================================================
 * Config helpers (defined in menus.c)
 * ============================================================ */

void ensure_config_dir_exists(void);
void init_default_config(void);
void apply_color_theme(void);
void try_migrate_from_json(void);
void init_all_persistent_data(void);
int  load_temp_playlist(void);
void save_temp_playlist(void);
void cleanup_temp_playlist(void);

/* ============================================================
 * Data management (defined in menus.c)
 * ============================================================ */

void load_history(void);
void load_favorites(void);
void load_dir_history(void);
void load_all_playlists(void);
void add_dir_history_entry(const char *path);
void clear_dir_history(void);
void add_history_entry(Track *track);
int  add_to_favorites(Track *track);
int  remove_from_favorites(int index);
int  create_user_playlist(const char *name);
int  delete_user_playlist(int index);
int  rename_user_playlist(int index, const char *new_name);
int  add_track_to_playlist(int playlist_idx, Track *track);
int  remove_track_from_playlist(int playlist_idx, int track_idx);

/* ============================================================
 * View input handler declarations (defined in respective view .c)
 * ============================================================ */

void handle_settings_input(int ch);
void handle_history_input(int ch);
void handle_playlist_input(int ch);
void handle_favorites_input(int ch);
void handle_info_input(int ch);
void handle_help_input(int ch);
void handle_language_input(int ch);

/* ============================================================
 * View reset functions (called from menus.c switch_to_view)
 * ============================================================ */

void reset_settings_view(void);
void reset_playlist_view(void);
void reset_help_view(void);
void reset_language_view(void);

/* ============================================================
 * Help view lifecycle (defined in help_view.c)
 * ============================================================ */

void help_free_lines(void);

/* ============================================================
 * Popup state (defined in controls.c)
 * ============================================================ */

extern PopupState g_popup;
int handle_popup_input(int ch);
int popup_handle_mouse_click(int screen_y, int screen_x);

/* ============================================================
 * Remote settings (defined in settings.c)
 * ============================================================ */

void remote_enter_list_mode(void);

#endif /* MENU_INTERNAL_H */
