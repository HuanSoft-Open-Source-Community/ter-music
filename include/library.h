/**
 * @file library.h
 * @brief SQLite-backed music library API
 *
 * Provides persistent metadata storage, recursive directory scanning,
 * incremental updates (mtime-based), FTS5 full-text search, and
 * artist/album/genre browsing dimensions.
 *
 * Design principle: The existing folder-browsing path (load_playlist,
 * scan_playlist_directory_into, g_playlist, LRU cache, JSON files)
 * is completely unchanged. SQLite is an additional data source that
 * feeds into the same g_playlist playing queue.
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#ifndef LIBRARY_H
#define LIBRARY_H

#include "defs.h"
#include <time.h>
#include <stdint.h>

/* ========== Initialization / Lifecycle ========== */

/**
 * Open or create the database at ~/.config/ter-music/library.db.
 * Creates all tables, indexes, and FTS triggers if they don't exist.
 * Must be called once at startup (from init_all_persistent_data).
 * Returns 0 on success, -1 on error.
 */
int library_init(void);

/**
 * Close the database and finalize all prepared statements.
 * Safe to call even if library_init() failed or was never called.
 */
void library_shutdown(void);

/**
 * Returns 1 if the library database is open and usable, 0 otherwise.
 */
int library_is_available(void);

/* ========== Scan Engine ========== */

/**
 * Recursively scan a directory for audio files.
 * For each file, reads metadata via get_audio_metadata() (reusing the
 * existing FFmpeg + APEv2 + GBK fixup pipeline from playlist.c), then
 * INSERT OR REPLACEs into the tracks table with the current mtime.
 * Files whose mtime has not changed since last scan are skipped
 * (incremental update).
 *
 * Returns number of tracks added/updated, or -1 on error.
 */
int library_scan_directory(const char *dir_path);

/**
 * Scan a single audio file, adding or updating it in the tracks table.
 * Returns 1 if added/updated, 0 if unchanged, -1 on error.
 */
int library_scan_file(const char *file_path);

/**
 * Check if a file at 'path' has been modified since the last scan.
 * Returns 1 if changed (or new), 0 if unchanged, -1 on error.
 */
int library_needs_rescan(const char *path, time_t current_mtime);

/**
 * Register a directory as a scan root (persisted in scan_roots table).
 * The directory is scanned when library_scan_all_roots() is called.
 * Returns 0 on success, -1 on error.
 */
int library_add_scan_root(const char *path);

/**
 * Remove a scan root.
 * Returns 0 on success, -1 on error.
 */
int library_remove_scan_root(const char *path);

/**
 * Get the list of registered scan roots. Caller must free each string
 * and the array itself using free().
 * Returns number of roots, or 0 if none.
 */
char **library_get_scan_roots(int *count);

/**
 * Scan all registered scan roots. Returns total tracks added/updated,
 * or -1 on error.
 */
int library_scan_all_roots(void);

/**
 * Remove tracks from the database whose files no longer exist on disk.
 * Returns number of tracks removed.
 */
int library_prune_missing(void);

/* ========== Background Scan Thread ========== */

/**
 * Start a background thread to recursively scan a directory.
 * The thread acquires the library mutex and runs library_scan_directory().
 * Returns 0 on success, -1 if a scan is already in progress.
 */
int library_scan_directory_async(const char *dir_path);

/**
 * Cancel a background scan in progress (sets cancel flag).
 */
void library_scan_cancel(void);

/**
 * Returns 1 if a background scan is currently running, 0 otherwise.
 */
int library_scan_in_progress(void);

/**
 * Returns the number of files processed so far in the current scan.
 */
int library_scan_progress(void);

/**
 * Returns the total number of files discovered so far in the current scan.
 */
int library_scan_total(void);

/* ========== Track Queries ========== */

/**
 * Get all track rowids from the library, up to max_results.
 * Returns the number of tracks, or -1 on error.
 */
int library_get_all_track_rowids(int *out_rowids, int max_results);

/**
 * Search tracks using FTS5 full-text search.
 * Falls back to LIKE '%query%' if FTS5 is not available.
 * Results are ordered by relevance (FTS5 rank) or alphabetically (LIKE).
 * Returns number of results, or -1 on error.
 */
int library_search_tracks(const char *query, int *out_rowids, int max_results);

/**
 * Get track rowids filtered by artist name (exact match).
 * Returns number of results, or -1 on error.
 */
int library_get_artist_track_rowids(const char *artist, int *out_rowids, int max_results);

/**
 * Get track rowids filtered by artist + album (exact match on both).
 * Returns number of results, or -1 on error.
 */
int library_get_album_track_rowids(const char *artist, const char *album,
                                   int *out_rowids, int max_results);

/**
 * Get track rowids filtered by genre (exact match).
 * Returns number of results, or -1 on error.
 */
int library_get_genre_track_rowids(const char *genre, int *out_rowids, int max_results);

/**
 * Fill a Track struct with metadata for the given rowid.
 * Returns 0 on success, -1 if not found.
 */
int library_get_track_metadata(int rowid, Track *out);

/**
 * Get the file path for a given track rowid.
 * Returns a pointer to a static buffer (NOT thread-safe; copy immediately)
 * or NULL if not found.
 */
const char *library_get_track_path(int rowid);

/**
 * Get total number of tracks in the library.
 */
int library_get_track_count(void);

/* ========== Browsing Dimensions ========== */

/**
 * Get a sorted list of distinct artist names.
 * Caller must free each string and the array using free_artists().
 * Returns number of artists, or 0 if none.
 */
char **library_get_artists(int *count);
void library_free_artists(char **artists, int count);

/**
 * Album browsing result.
 */
typedef struct {
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    int track_count;
} LibraryAlbum;

/**
 * Get a sorted list of distinct (artist, album) pairs, optionally
 * filtered by artist. If filter_artist is NULL or empty, returns all.
 * Caller must free the array using free_albums().
 * Returns number of albums, or 0 if none.
 */
LibraryAlbum *library_get_albums(const char *filter_artist, int *count);
void library_free_albums(LibraryAlbum *albums, int count);

/**
 * Get a sorted list of distinct genre names.
 * Caller must free each string and the array using free_genres().
 * Returns number of genres, or 0 if none.
 */
char **library_get_genres(int *count);
void library_free_genres(char **genres, int count);

/* ========== Bridge to g_playlist ========== */

/**
 * Populate the global g_playlist from an array of database rowids.
 * If append is non-zero, appends to the existing playlist instead of
 * replacing. The bridge resolves each rowid to its file path and
 * copies it into the g_playlist.tracks[] array.
 *
 * Returns the number of tracks loaded, or -1 on error.
 */
int library_load_into_playlist(const int *rowids, int count, int append);

/* ========== Favorites (SQLite-backed) ========== */

int library_favorites_get_count(void);
int library_favorites_get_all(Track *tracks, int max_tracks);
int library_favorites_add(const char *track_path);
int library_favorites_remove(const char *track_path);
int library_favorites_has(const char *track_path);

/* ========== Play History (SQLite-backed) ========== */

void library_history_add(const char *track_path, int position);
int library_history_get_count(void);
int library_history_get_all(HistoryEntry *entries, int max_entries);

/* ========== User Playlists (SQLite-backed) ========== */

int library_playlist_create(const char *name);
int library_playlist_delete(int playlist_id);
int library_playlist_rename(int playlist_id, const char *new_name);
int library_playlist_get_count(void);
int library_playlist_get_all(int *out_ids, char (*names)[MAX_PLAYLIST_NAME_LEN], int max);
int library_playlist_add_track(int playlist_id, const char *track_path);
int library_playlist_remove_track(int playlist_id, int position);
int library_playlist_remove_track_by_path(int playlist_id, const char *track_path);
int library_playlist_get_tracks(int playlist_id, Track *tracks, int max_tracks);

/* ========== Statistics ========== */

int library_get_total_duration_seconds(void);

/* ========== Migration Helpers ========== */

/**
 * Migrate data from existing JSON files (favorites.json, history.json,
 * playlists/*.json) into SQLite tables.
 *
 * The migration runs inside a single SQLite transaction. If it fails,
 * the transaction is rolled back and JSON files are left untouched.
 *
 * Existing JSON files are NOT deleted after migration (user can
 * manually clean up later).
 *
 * Returns 0 on success, -1 on failure.
 */
int library_migrate_from_json(void);

/**
 * Returns 1 if the library database appears to have data (tracks table
 * is non-empty), 0 otherwise.
 */
int library_has_data(void);

#endif /* LIBRARY_H */
