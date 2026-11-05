/**
 * @file library.h
 * @brief SQLite-backed music library API
 *
 * Provides persistent metadata storage, recursive directory scanning,
 * incremental updates (mtime-based), FTS5 full-text search, and
 * artist/album/genre browsing dimensions.
 *
 * SQLite is the sole persistence layer — all favorites, history,
 * playlists, directory history, and temp playlist data are stored
 * in ~/.config/ter-music/library.db. Legacy v1 JSON persistence
 * has been fully removed.
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#ifndef LIBRARY_H
#define LIBRARY_H

#include "types.h"
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

/* ========== Lyrics Source Preference ========== */

/**
 * Get the lyrics source preference for a track by path.
 * Returns LYRICS_SOURCE_AUTO (0) if no preference is stored.
 */
int library_get_lyrics_source(const char *track_path);

/**
 * Set the lyrics source preference for a track by path.
 * Persisted immediately to the database.
 */
void library_set_lyrics_source(const char *track_path, int source);

/* ========== Paged Queries (D-Bus Library.GetPage / GetTree) ========== */
/*
 * 浏览视图的分页查询：与既有 *_get_* 接口并存，区别在于带 offset/limit，
 * 由 SQLite 的 LIMIT/OFFSET 完成，因此 1 万曲目下翻页也不会把整表读进内存。
 * 行数上限由调用方给出（cap），返回实际写入行数，-1 表示错误。
 */

typedef enum {
    LIBRARY_QUERY_ARTISTS = 0,   /* 聚合：name = 艺术家，track_count = 曲目数 */
    LIBRARY_QUERY_ALBUMS,        /* 聚合：artist + album + track_count */
    LIBRARY_QUERY_GENRES,        /* 聚合：name = 流派，track_count = 曲目数 */
    LIBRARY_QUERY_TRACKS,        /* 全部曲目 */
    LIBRARY_QUERY_SEARCH         /* FTS5/LIKE 搜索结果（query 必填） */
} LibraryQueryKind;

typedef struct {
    int kind;
    char artist[MAX_META_LEN];   /* 专辑/曲目过滤（可空） */
    char album[MAX_META_LEN];
    char genre[MAX_META_LEN];
    char query[MAX_META_LEN];    /* LIBRARY_QUERY_SEARCH 的关键词 */
} LibraryQuery;

typedef struct {
    int rowid;                   /* 曲目行；聚合行为 0 */
    int track_count;             /* 聚合行的计数；曲目行为 0 */
    int duration_seconds;
    char name[MAX_META_LEN];     /* 艺术家/流派名；曲目行为标题 */
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    char path[MAX_PATH_LEN];
} LibraryRow;

/* 分页查询实现（library_query.c）需要的底层句柄与锁：用 void* 声明，
 * 避免把 sqlite3.h 泄漏给 library.h 的全部使用者；调用方需自行加锁。 */
void *library_db_handle(void);
void library_lock(void);
void library_unlock(void);

/* 满足条件的总行数（用于分页总数，不读取行内容） */
int library_query_count(const LibraryQuery *query);
/* 取一页（offset/count）；out 至少可容纳 cap 行 */
int library_query_page(const LibraryQuery *query, int offset, int count,
                       LibraryRow *out, int cap);

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

/* ========== Directory History (SQLite-backed) ========== */

int library_dir_history_add(const char *path);
int library_dir_history_get_all(DirHistoryEntry *entries, int max_entries);
int library_dir_history_remove(const char *path);
void library_dir_history_clear(void);

/* ========== Play History (SQLite-backed) ========== */

void library_history_add(const char *track_path, int position);
int library_history_get_count(void);
int library_history_get_all(HistoryEntry *entries, int max_entries);
void library_history_clear(void);

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

/* ========== Temp Playlist (SQLite-backed) ========== */

int library_temp_playlist_save(const char *folder_path,
                                const char (*tracks)[MAX_PATH_LEN], int track_count);
int library_temp_playlist_load(char *folder_path, size_t folder_size,
                               char (*tracks)[MAX_PATH_LEN], int max_tracks);
void library_temp_playlist_cleanup(void);

/* ========== Statistics ========== */

int library_get_total_duration_seconds(void);

/* ========== Migration Helpers ========== */

/**
 * Returns 1 if the library database appears to have data (tracks table
 * is non-empty), 0 otherwise.
 */
int library_has_data(void);

#endif /* LIBRARY_H */
