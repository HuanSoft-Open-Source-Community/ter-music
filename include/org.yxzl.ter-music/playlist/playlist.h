#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "types.h"
#include "playlist/cue_parser.h"

/* ── Extern globals ── */
extern Playlist g_playlist;
extern int g_selected_index;
extern SortState g_sort_state;
extern PlaylistManager g_playlist_manager;
extern CueSheet g_cue_sheet;  /* parsed CUE data for the current directory */

/* ── Function prototypes ── */
int load_playlist(const char *folder_path);
int append_playlist(const char *folder_path);
int load_single_file(const char *file_path);
int load_remote_playlist(const RemoteConnectionConfig *conn, const char *subpath);
void reset_playlist_state(void);
void playlist_lock(void);
void playlist_unlock(void);
int playlist_count(void);
int playlist_is_loaded(void);
int playlist_has_multiple_sources(void);
void playlist_copy_folder_path(char *dest, size_t dest_size);
int playlist_get_track_path(int index, char *dest, size_t dest_size);
int playlist_find_track_index_by_path(const char *track_path);
int track_matches_query(int index, const char *query);
int get_track_metadata(int index, Track *out);
void preload_visible_tracks(int start, int end);
void clear_metadata_cache(void);
void recompute_sort_order(void);
void decode_html_entities(char *str);

/* CUE helpers */
int  cue_get_offset(int track_index);
int  cue_get_track_number(int track_index);
void cue_clear_sheet(void);
int  cue_find_next_offset(int current_index);

/* Tree browsing */
int  playlist_tree_is_active(void);
int  playlist_visible_count(void);
int  get_visible_node_tree_index(int visible_idx);
int  get_visible_node_type(int visible_idx);
int  get_visible_node_track_index(int visible_idx);
int  get_tree_node_depth(int tree_idx);
const char *get_tree_node_name(int tree_idx);
void playlist_toggle_directory_expand(int tree_idx);

#endif
