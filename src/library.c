/**
 * @file library.c
 * @brief SQLite-backed music library implementation
 *
 * Provides persistent metadata storage, recursive directory scanning,
 * incremental updates (mtime-based), FTS5 full-text search, and
 * artist/album/genre browsing dimensions.
 *
 * Integrates with existing infrastructure:
 *   - Reuses get_audio_metadata() from playlist.c for FFmpeg + APEv2 + GBK fixup
 *   - Reuses is_audio_file() and audio_extensions[] from playlist.c
 *   - Library query results feed into g_playlist via library_load_into_playlist()
 *   - Existing JSON persistence remains as fallback/backup
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-01
 */

#include "../include/library.h"
#include "../include/logger.h"
#include "../include/search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

/* ========== External declarations from playlist.c ========== */

/* Audio extensions list — defined in playlist.c */
extern const char *audio_extensions[];

/* File type check — defined in playlist.c */
extern int is_audio_file(const char *filename);

/* Metadata extraction — defined in playlist.c */
extern void get_audio_metadata(const char *path, char *title, char *artist, char *album);

/* ========== Internal state ========== */

static sqlite3 *g_db = NULL;
static int g_library_available = 0;
static pthread_mutex_t g_library_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Background scan state */
static pthread_t g_scan_thread;
static volatile int g_scan_in_progress = 0;
static volatile int g_scan_cancel = 0;
static volatile int g_scan_progress = 0;
static volatile int g_scan_total = 0;

/* Default config directory */
#define CONFIG_DIR "/.config/ter-music"
#define DB_FILENAME "library.db"

/* ========== Schema DDL ========== */

static const char *g_schema_sql =
    /* --- Tracks table --- */
    "CREATE TABLE IF NOT EXISTS tracks ("
    "  path            TEXT PRIMARY KEY,"
    "  title           TEXT NOT NULL DEFAULT '',"
    "  artist          TEXT NOT NULL DEFAULT '',"
    "  album           TEXT NOT NULL DEFAULT '',"
    "  genre           TEXT NOT NULL DEFAULT '',"
    "  year            INTEGER DEFAULT 0,"
    "  duration_seconds INTEGER DEFAULT 0,"
    "  mtime           INTEGER NOT NULL,"
    "  file_size       INTEGER DEFAULT 0,"
    "  added_at        INTEGER NOT NULL,"
    "  last_played_at  INTEGER DEFAULT 0,"
    "  play_count      INTEGER DEFAULT 0"
    ");"

    /* --- FTS5 full-text search virtual table --- */
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
    "  title, artist, album, genre,"
    "  content='tracks',"
    "  content_rowid='rowid',"
    "  tokenize='unicode61 remove_diacritics 2'"
    ");"

    /* --- FTS5 sync triggers --- */
    "CREATE TRIGGER IF NOT EXISTS tracks_ai AFTER INSERT ON tracks BEGIN"
    "  INSERT INTO tracks_fts(rowid, title, artist, album, genre)"
    "  VALUES (new.rowid, new.title, new.artist, new.album, new.genre);"
    "END;"

    "CREATE TRIGGER IF NOT EXISTS tracks_ad AFTER DELETE ON tracks BEGIN"
    "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, genre)"
    "  VALUES ('delete', old.rowid, old.title, old.artist, old.album, old.genre);"
    "END;"

    "CREATE TRIGGER IF NOT EXISTS tracks_au AFTER UPDATE ON tracks BEGIN"
    "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, genre)"
    "  VALUES ('delete', old.rowid, old.title, old.artist, old.album, old.genre);"
    "  INSERT INTO tracks_fts(rowid, title, artist, album, genre)"
    "  VALUES (new.rowid, new.title, new.artist, new.album, new.genre);"
    "END;"

    /* --- Favorites --- */
    "CREATE TABLE IF NOT EXISTS favorites ("
    "  track_path TEXT PRIMARY KEY REFERENCES tracks(path) ON DELETE CASCADE,"
    "  added_at INTEGER NOT NULL"
    ");"

    /* --- Play history --- */
    "CREATE TABLE IF NOT EXISTS play_history ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  track_path  TEXT NOT NULL REFERENCES tracks(path) ON DELETE CASCADE,"
    "  played_at   INTEGER NOT NULL,"
    "  position    INTEGER DEFAULT 0"
    ");"

    /* --- User playlists --- */
    "CREATE TABLE IF NOT EXISTS playlists ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name        TEXT NOT NULL,"
    "  created_at  INTEGER NOT NULL,"
    "  modified_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS playlist_tracks ("
    "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
    "  track_path  TEXT NOT NULL REFERENCES tracks(path) ON DELETE CASCADE,"
    "  position    INTEGER NOT NULL,"
    "  added_at    INTEGER NOT NULL,"
    "  PRIMARY KEY (playlist_id, track_path)"
    ");"

    /* --- Scan roots (persisted directories to scan) --- */
    "CREATE TABLE IF NOT EXISTS scan_roots ("
    "  path         TEXT PRIMARY KEY,"
    "  last_scanned INTEGER NOT NULL,"
    "  recursive    INTEGER NOT NULL DEFAULT 1"
    ");"

    /* --- Schema version for future migrations --- */
    "CREATE TABLE IF NOT EXISTS schema_version ("
    "  version INTEGER PRIMARY KEY"
    ");"
    "INSERT OR IGNORE INTO schema_version(version) VALUES (1);";

/* ========== Forward declarations of internal functions ========== */
static void build_db_path(char *buf, size_t buf_size);
static int exec_sql(const char *sql);
static int exec_sql_printf(const char *format, ...);
static time_t get_file_mtime(const char *path);
static void extract_extended_metadata(const char *path, char *genre, size_t genre_size, int *year);
static int track_exists_and_unchanged(const char *path, time_t mtime);
static int insert_or_update_track(const char *path, time_t mtime);
static void *scan_thread_func(void *arg);

/* ========== Helper: construct DB path ========== */
static void build_db_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    snprintf(buf, buf_size, "%s%s", home, CONFIG_DIR);
    /* Ensure directory exists */
    struct stat st = {0};
    if (stat(buf, &st) == -1) {
        mkdir(buf, 0755);
    }
    size_t len = strlen(buf);
    snprintf(buf + len, buf_size - len, "/%s", DB_FILENAME);
}

/* ========== Helper: execute SQL ========== */
static int exec_sql(const char *sql) {
    if (!g_db) return -1;
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        log_error("library", "SQL error: %s (SQL: %.100s)", errmsg ? errmsg : "?", sql);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int exec_sql_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return exec_sql(buf);
}

/* ========== Helper: get file mtime ========== */
static time_t get_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_mtime;
}

/* ========== Helper: get file size ========== */
static long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

/* ========== Helper: extract genre and year from audio file ========== */
static void extract_extended_metadata(const char *path, char *genre, size_t genre_size, int *year) {
    if (genre) genre[0] = '\0';
    if (year) *year = 0;

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) != 0) {
        return;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    /* Check format-level metadata first, then stream-level */
    AVDictionary *dict = fmt_ctx->metadata;
    const char *genre_str = NULL;
    const char *year_str = NULL;
    const char *date_str = NULL;

    if (dict) {
        genre_str = av_dict_get(dict, "genre", NULL, AV_DICT_IGNORE_SUFFIX) ?
                    av_dict_get(dict, "genre", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
        if (!genre_str)
            genre_str = av_dict_get(dict, "GENRE", NULL, 0) ?
                        av_dict_get(dict, "GENRE", NULL, 0)->value : NULL;
        year_str = av_dict_get(dict, "year", NULL, AV_DICT_IGNORE_SUFFIX) ?
                   av_dict_get(dict, "year", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
        date_str = av_dict_get(dict, "date", NULL, AV_DICT_IGNORE_SUFFIX) ?
                   av_dict_get(dict, "date", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
    }

    /* Check stream metadata if not found in format metadata */
    if ((!genre_str || !year_str) && fmt_ctx->nb_streams > 0) {
        AVDictionary *stream_dict = fmt_ctx->streams[0]->metadata;
        if (stream_dict) {
            if (!genre_str) {
                genre_str = av_dict_get(stream_dict, "genre", NULL, AV_DICT_IGNORE_SUFFIX) ?
                            av_dict_get(stream_dict, "genre", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
                if (!genre_str)
                    genre_str = av_dict_get(stream_dict, "GENRE", NULL, 0) ?
                                av_dict_get(stream_dict, "GENRE", NULL, 0)->value : NULL;
            }
            if (!year_str) {
                year_str = av_dict_get(stream_dict, "year", NULL, AV_DICT_IGNORE_SUFFIX) ?
                           av_dict_get(stream_dict, "year", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
            }
            if (!date_str) {
                date_str = av_dict_get(stream_dict, "date", NULL, AV_DICT_IGNORE_SUFFIX) ?
                           av_dict_get(stream_dict, "date", NULL, AV_DICT_IGNORE_SUFFIX)->value : NULL;
            }
        }
    }

    if (genre && genre_str) {
        strncpy(genre, genre_str, genre_size - 1);
        genre[genre_size - 1] = '\0';
    }

    if (year) {
        const char *y = year_str ? year_str : date_str;
        if (y) {
            /* Parse first 4 digits as year */
            while (*y && !(*y >= '0' && *y <= '9')) y++;
            if (*y) {
                *year = (int)strtol(y, NULL, 10);
                if (*year < 1900 || *year > 2100) *year = 0;
            }
        }
    }

    /* Get duration */
    if (fmt_ctx->duration > 0) {
        /* duration is in AV_TIME_BASE microseconds */
        /* We store duration_seconds in the INSERT below, but this is called
         * from insert_or_update_track which receives duration as a return
         * value. For now we store duration from the scan path directly. */
    }

    avformat_close_input(&fmt_ctx);
}

/* ========== Helper: check if track exists and unchanged ========== */
static int track_exists_and_unchanged(const char *path, time_t mtime) {
    if (!g_db) return 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT mtime FROM tracks WHERE path = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    int exists = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t stored_mtime = (time_t)sqlite3_column_int64(stmt, 0);
        if (stored_mtime == mtime) {
            exists = 1;  /* Same mtime → unchanged */
        }
    }
    sqlite3_finalize(stmt);
    return exists;
}

/* ========== Helper: insert or update a track in the database ========== */
static int insert_or_update_track(const char *path, time_t mtime) {
    if (!g_db) return -1;

    char title[MAX_META_LEN] = "";
    char artist[MAX_META_LEN] = "";
    char album[MAX_META_LEN] = "";
    char genre[MAX_META_LEN] = "";
    int year = 0;
    long file_size = get_file_size(path);

    /* Reuse existing metadata extraction pipeline from playlist.c */
    get_audio_metadata(path, title, artist, album);

    /* Extract additional metadata (genre, year, duration) */
    int duration_seconds = 0;
    {
        AVFormatContext *fmt_ctx = NULL;
        if (avformat_open_input(&fmt_ctx, path, NULL, NULL) == 0) {
            if (avformat_find_stream_info(fmt_ctx, NULL) >= 0) {
                if (fmt_ctx->duration > 0) {
                    duration_seconds = (int)(fmt_ctx->duration / AV_TIME_BASE);
                }
                /* Detect first audio stream duration as fallback */
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                    if (fmt_ctx->streams[i]->codecpar &&
                        fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                        fmt_ctx->streams[i]->duration > 0) {
                        int64_t stream_dur = fmt_ctx->streams[i]->duration;
                        AVRational tb = fmt_ctx->streams[i]->time_base;
                        if (tb.den > 0) {
                            int sdur = (int)(stream_dur * tb.num / tb.den);
                            if (sdur > duration_seconds) duration_seconds = sdur;
                        }
                        break;
                    }
                }
            }
            avformat_close_input(&fmt_ctx);
        }

        /* Get genre and year separately */
        extract_extended_metadata(path, genre, sizeof(genre), &year);
    }

    time_t now = time(NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "INSERT OR REPLACE INTO tracks("
        "  path, title, artist, album, genre, year, duration_seconds,"
        "  mtime, file_size, added_at, last_played_at, play_count"
        ") VALUES("
        "  ?, ?, ?, ?, ?, ?, ?,"
        "  ?, ?, COALESCE((SELECT added_at FROM tracks WHERE path = ?), ?),"
        "  COALESCE((SELECT last_played_at FROM tracks WHERE path = ?), 0),"
        "  COALESCE((SELECT play_count FROM tracks WHERE path = ?), 0)"
        ")",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_error("library", "Failed to prepare insert statement: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, artist, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, album, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, genre, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, year);
    sqlite3_bind_int(stmt, 7, duration_seconds);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)mtime);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)file_size);
    sqlite3_bind_text(stmt, 10, path, -1, SQLITE_STATIC);  /* subquery for added_at */
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)now);       /* default added_at */
    sqlite3_bind_text(stmt, 12, path, -1, SQLITE_STATIC);   /* subquery for last_played_at */
    sqlite3_bind_text(stmt, 13, path, -1, SQLITE_STATIC);   /* subquery for play_count */

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        log_error("library", "Failed to insert track '%s': %s", path, sqlite3_errmsg(g_db));
        return -1;
    }

    return 1;
}

/* ========== Public API: Init / Shutdown ========== */

int library_init(void) {
    pthread_mutex_lock(&g_library_mutex);

    if (g_db) {
        log_warn("library", "library_init called but database is already open");
        pthread_mutex_unlock(&g_library_mutex);
        return 0;
    }

    /* Configure SQLite threading mode */
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    /* Build database path */
    char db_path[MAX_PATH_LEN];
    build_db_path(db_path, sizeof(db_path));
    log_info("library", "Opening library database: %s", db_path);

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        log_error("library", "Failed to open database: %s", sqlite3_errmsg(g_db));
        g_db = NULL;
        g_library_available = 0;
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    /* Enable WAL mode for better concurrent read performance */
    exec_sql("PRAGMA journal_mode=WAL");
    /* Enable foreign keys */
    exec_sql("PRAGMA foreign_keys=ON");

    /* Create schema */
    if (exec_sql(g_schema_sql) != 0) {
        log_error("library", "Failed to create database schema");
        sqlite3_close(g_db);
        g_db = NULL;
        g_library_available = 0;
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    g_library_available = 1;
    log_info("library", "Library database initialized successfully");
    pthread_mutex_unlock(&g_library_mutex);
    return 0;
}

void library_shutdown(void) {
    pthread_mutex_lock(&g_library_mutex);

    if (g_scan_in_progress) {
        g_scan_cancel = 1;
        pthread_mutex_unlock(&g_library_mutex);
        pthread_join(g_scan_thread, NULL);
        pthread_mutex_lock(&g_library_mutex);
    }

    if (g_db) {
        /* Close all prepared statements would go here in later phases */
        sqlite3_close(g_db);
        g_db = NULL;
    }
    g_library_available = 0;
    pthread_mutex_unlock(&g_library_mutex);
}

int library_is_available(void) {
    return g_library_available && g_db != NULL;
}

/* ========== Public API: Scan Engine ========== */

int library_scan_directory(const char *dir_path) {
    if (!library_is_available()) return -1;
    if (!dir_path || !dir_path[0]) return -1;

    pthread_mutex_lock(&g_library_mutex);

    int count = 0;
    sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    /* Use a stack-based iterative approach to avoid deep recursion */
    /* For simplicity in Phase 1, use a single-level scan */
    /* Full recursive scan will be implemented in Phase 2 */
    DIR *dir = opendir(dir_path);
    if (!dir) {
        log_error("library", "Cannot open directory: %s", dir_path);
        sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (g_scan_cancel) break;

        if (entry->d_name[0] == '.') continue;  /* skip hidden files and . and .. */

        /* Build full path */
        char full_path[MAX_PATH_LEN];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s",
                                dir_path, entry->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(full_path)) continue;

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recursion will be handled in Phase 2 */
            /* For now, skip subdirectories in the scan */
            continue;
        }

        if (!is_audio_file(entry->d_name)) continue;

        time_t mtime = st.st_mtime;
        if (track_exists_and_unchanged(full_path, mtime)) {
            continue;  /* Incremental: skip unchanged files */
        }

        if (insert_or_update_track(full_path, mtime) > 0) {
            count++;
        }
    }
    closedir(dir);

    sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);
    log_info("library", "Scanned '%s': %d tracks added/updated", dir_path, count);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_scan_file(const char *file_path) {
    if (!library_is_available()) return -1;
    if (!file_path || !file_path[0]) return -1;

    /* Check if it's an audio file */
    const char *ext = strrchr(file_path, '.');
    if (!ext) return 0;
    int is_audio = 0;
    for (int i = 0; audio_extensions[i]; i++) {
        if (strcmp(ext, audio_extensions[i]) == 0) {
            is_audio = 1;
            break;
        }
    }
    if (!is_audio) return 0;

    time_t mtime = get_file_mtime(file_path);
    if (mtime == 0) return -1;

    pthread_mutex_lock(&g_library_mutex);
    int result;
    if (track_exists_and_unchanged(file_path, mtime)) {
        result = 0;
    } else {
        result = insert_or_update_track(file_path, mtime);
    }
    pthread_mutex_unlock(&g_library_mutex);
    return result;
}

int library_needs_rescan(const char *path, time_t current_mtime) {
    if (!library_is_available() || !path) return 1;
    return track_exists_and_unchanged(path, current_mtime) ? 0 : 1;
}

int library_add_scan_root(const char *path) {
    if (!library_is_available()) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO scan_roots(path, last_scanned, recursive) VALUES(?, 0, 1)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_remove_scan_root(const char *path) {
    if (!library_is_available()) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "DELETE FROM scan_roots WHERE path = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return 0;
}

char **library_get_scan_roots(int *count) {
    *count = 0;
    if (!library_is_available()) return NULL;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT path FROM scan_roots ORDER BY path", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    /* First pass: count */
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cnt++;
    sqlite3_reset(stmt);

    char **roots = calloc((size_t)(cnt + 1), sizeof(char *));
    if (!roots) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    /* Second pass: collect */
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        roots[idx] = strdup(val ? val : "");
        if (!roots[idx]) break;
        idx++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);

    *count = idx;
    return roots;
}

int library_scan_all_roots(void) {
    if (!library_is_available()) return -1;

    int count = 0;
    char **roots = library_get_scan_roots(&count);
    if (!roots) return 0;

    int total = 0;
    for (int i = 0; i < count; i++) {
        if (roots[i] && roots[i][0]) {
            int n = library_scan_directory(roots[i]);
            if (n > 0) total += n;
            free(roots[i]);
        }
    }
    free(roots);
    return total;
}

int library_prune_missing(void) {
    if (!library_is_available()) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT path FROM tracks", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    /* Collect paths to check */
    char **paths = NULL;
    int count = 0, cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        if (!p) continue;
        if (count >= cap) {
            int new_cap = cap ? cap * 2 : 1024;
            char **tmp = realloc(paths, (size_t)new_cap * sizeof(char *));
            if (!tmp) break;
            paths = tmp;
            cap = new_cap;
        }
        paths[count] = strdup(p);
        count++;
    }
    sqlite3_finalize(stmt);

    /* Check each path; delete if missing */
    int removed = 0;
    sqlite3_stmt *del_stmt = NULL;
    sqlite3_prepare_v2(g_db, "DELETE FROM tracks WHERE path = ?", -1, &del_stmt, NULL);

    for (int i = 0; i < count; i++) {
        if (access(paths[i], F_OK) != 0) {
            sqlite3_bind_text(del_stmt, 1, paths[i], -1, SQLITE_STATIC);
            if (sqlite3_step(del_stmt) == SQLITE_DONE) removed++;
            sqlite3_reset(del_stmt);
        }
        free(paths[i]);
    }
    sqlite3_finalize(del_stmt);
    free(paths);

    pthread_mutex_unlock(&g_library_mutex);
    return removed;
}

/* ========== Background Scan Thread ========== */

int library_scan_directory_async(const char *dir_path) {
    if (!library_is_available()) return -1;
    if (g_scan_in_progress) return -1;

    g_scan_cancel = 0;
    g_scan_progress = 0;
    g_scan_total = 0;
    g_scan_in_progress = 1;

    char *path_copy = strdup(dir_path);
    if (!path_copy) {
        g_scan_in_progress = 0;
        return -1;
    }

    if (pthread_create(&g_scan_thread, NULL, scan_thread_func, path_copy) != 0) {
        free(path_copy);
        g_scan_in_progress = 0;
        return -1;
    }
    pthread_detach(g_scan_thread);
    return 0;
}

void library_scan_cancel(void) {
    g_scan_cancel = 1;
}

int library_scan_in_progress(void) {
    return g_scan_in_progress;
}

int library_scan_progress(void) {
    return g_scan_progress;
}

int library_scan_total(void) {
    return g_scan_total;
}

static void *scan_thread_func(void *arg) {
    char *dir_path = (char *)arg;

    /* Count total files first for progress reporting */
    int total = 0;
    {
        DIR *d = opendir(dir_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] != '.') total++;
            }
            closedir(d);
        }
    }
    g_scan_total = total;
    g_scan_progress = 0;

    /* Do the actual scan */
    int result = library_scan_directory(dir_path);
    (void)result;

    free(dir_path);
    g_scan_in_progress = 0;
    return NULL;
}

/* ========== Public API: Track Queries ========== */

int library_get_all_track_rowids(int *out_rowids, int max_results) {
    if (!library_is_available() || !out_rowids || max_results <= 0) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT rowid FROM tracks ORDER BY title COLLATE NOCASE", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    int count = 0;
    while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
        out_rowids[count++] = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_search_tracks(const char *query, int *out_rowids, int max_results) {
    if (!library_is_available() || !query || !out_rowids || max_results <= 0) return -1;
    if (!query[0]) return library_get_all_track_rowids(out_rowids, max_results);

    /* Check if FTS5 is available */
    int have_fts5 = sqlite3_compileoption_used("SQLITE_ENABLE_FTS5");

    pthread_mutex_lock(&g_library_mutex);

    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (have_fts5) {
        /* Use FTS5: escape FTS5 special characters and build MATCH query */
        /* Simple approach: wrap each term with double quotes and AND them */
        char fts_query[1024];
        char sanitized[1024];
        int si = 0;
        for (int i = 0; query[i] && si < (int)sizeof(sanitized) - 4; i++) {
            char c = query[i];
            if (c == '"' || c == '(' || c == ')' || c == '*' ||
                c == '+' || c == '-' || c == '^' || c == '~') {
                /* Escape FTS5 special characters */
                /* For simplicity, skip special chars in the FTS query */
                continue;
            }
            sanitized[si++] = c;
        }
        sanitized[si] = '\0';

        /* Build quoted term query: "term1" AND "term2" ... */
        /* For CJK, FTS5 unicode61 tokenizer handles each character as a token */
        snprintf(fts_query, sizeof(fts_query), "\"%s\"", sanitized);

        int rc = sqlite3_prepare_v2(g_db,
            "SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?"
            " ORDER BY rank LIMIT ?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, max_results);

            while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
                out_rowids[count++] = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
            stmt = NULL;
        } else {
            log_warn("library", "FTS5 query failed, falling back to LIKE: %s", sqlite3_errmsg(g_db));
            have_fts5 = 0;  /* Fall through to LIKE */
        }
    }

    if (!have_fts5 && !stmt) {
        /* Fallback: LIKE query across title/artist/album */
        char like_pattern[1024];
        snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", query);

        int rc = sqlite3_prepare_v2(g_db,
            "SELECT rowid FROM tracks WHERE"
            "  title LIKE ? OR artist LIKE ? OR album LIKE ?"
            " ORDER BY title COLLATE NOCASE LIMIT ?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, like_pattern, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, like_pattern, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, max_results);

            while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
                out_rowids[count++] = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_get_artist_track_rowids(const char *artist, int *out_rowids, int max_results) {
    if (!library_is_available() || !artist || !out_rowids || max_results <= 0) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT rowid FROM tracks WHERE artist = ?"
        " ORDER BY album COLLATE NOCASE, rowid",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, artist, -1, SQLITE_STATIC);

    int count = 0;
    while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
        out_rowids[count++] = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_get_album_track_rowids(const char *artist, const char *album,
                                   int *out_rowids, int max_results) {
    if (!library_is_available() || !album || !out_rowids || max_results <= 0) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (artist && artist[0]) {
        rc = sqlite3_prepare_v2(g_db,
            "SELECT rowid FROM tracks WHERE artist = ? AND album = ?"
            " ORDER BY title COLLATE NOCASE",
            -1, &stmt, NULL);
    } else {
        rc = sqlite3_prepare_v2(g_db,
            "SELECT rowid FROM tracks WHERE album = ?"
            " ORDER BY title COLLATE NOCASE",
            -1, &stmt, NULL);
    }
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    int idx = 1;
    if (artist && artist[0]) {
        sqlite3_bind_text(stmt, idx++, artist, -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt, idx, album, -1, SQLITE_STATIC);

    int count = 0;
    while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
        out_rowids[count++] = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_get_genre_track_rowids(const char *genre, int *out_rowids, int max_results) {
    if (!library_is_available() || !genre || !out_rowids || max_results <= 0) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT rowid FROM tracks WHERE genre = ?"
        " ORDER BY title COLLATE NOCASE",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, genre, -1, SQLITE_STATIC);

    int count = 0;
    while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
        out_rowids[count++] = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_get_track_metadata(int rowid, Track *out) {
    if (!library_is_available() || !out) return -1;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT path, title, artist, album FROM tracks WHERE rowid = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, rowid);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    const char *title = (const char *)sqlite3_column_text(stmt, 1);
    const char *artist = (const char *)sqlite3_column_text(stmt, 2);
    const char *album = (const char *)sqlite3_column_text(stmt, 3);

    strncpy(out->path, path ? path : "", sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    strncpy(out->title, title ? title : "", sizeof(out->title) - 1);
    out->title[sizeof(out->title) - 1] = '\0';
    strncpy(out->artist, artist ? artist : "", sizeof(out->artist) - 1);
    out->artist[sizeof(out->artist) - 1] = '\0';
    strncpy(out->album, album ? album : "", sizeof(out->album) - 1);
    out->album[sizeof(out->album) - 1] = '\0';

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return 0;
}

const char *library_get_track_path(int rowid) {
    if (!library_is_available()) return NULL;

    static char path_buf[MAX_PATH_LEN];
    path_buf[0] = '\0';

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT path FROM tracks WHERE rowid = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }
    sqlite3_bind_int(stmt, 1, rowid);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        if (p) {
            strncpy(path_buf, p, sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return path_buf[0] ? path_buf : NULL;
}

int library_get_track_count(void) {
    if (!library_is_available()) return 0;
    pthread_mutex_lock(&g_library_mutex);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM tracks", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

/* ========== Public API: Browsing Dimensions ========== */

char **library_get_artists(int *count) {
    *count = 0;
    if (!library_is_available()) return NULL;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT DISTINCT artist FROM tracks"
        " WHERE artist != '' ORDER BY artist COLLATE NOCASE",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    /* Count first */
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cnt++;
    sqlite3_reset(stmt);

    char **artists = calloc((size_t)(cnt + 1), sizeof(char *));
    if (!artists) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val && val[0]) {
            artists[idx] = strdup(val);
            if (!artists[idx]) break;
            idx++;
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    *count = idx;
    return artists;
}

void library_free_artists(char **artists, int count) {
    if (!artists) return;
    for (int i = 0; i < count; i++) {
        free(artists[i]);
    }
    free(artists);
}

LibraryAlbum *library_get_albums(const char *filter_artist, int *count) {
    *count = 0;
    if (!library_is_available()) return NULL;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (filter_artist && filter_artist[0]) {
        rc = sqlite3_prepare_v2(g_db,
            "SELECT artist, album, COUNT(*) as cnt FROM tracks"
            " WHERE album != '' AND artist = ?"
            " GROUP BY artist, album ORDER BY album COLLATE NOCASE",
            -1, &stmt, NULL);
    } else {
        rc = sqlite3_prepare_v2(g_db,
            "SELECT artist, album, COUNT(*) as cnt FROM tracks"
            " WHERE album != ''"
            " GROUP BY artist, album ORDER BY album COLLATE NOCASE",
            -1, &stmt, NULL);
    }
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    if (filter_artist && filter_artist[0]) {
        sqlite3_bind_text(stmt, 1, filter_artist, -1, SQLITE_STATIC);
    }

    /* Count first */
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cnt++;
    sqlite3_reset(stmt);
    if (filter_artist && filter_artist[0]) {
        sqlite3_bind_text(stmt, 1, filter_artist, -1, SQLITE_STATIC);
    }

    LibraryAlbum *albums = calloc((size_t)cnt, sizeof(LibraryAlbum));
    if (!albums) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    int idx = 0;
    while (idx < cnt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *art = (const char *)sqlite3_column_text(stmt, 0);
        const char *alb = (const char *)sqlite3_column_text(stmt, 1);
        int tc = sqlite3_column_int(stmt, 2);
        if (art) {
            strncpy(albums[idx].artist, art, sizeof(albums[idx].artist) - 1);
            albums[idx].artist[sizeof(albums[idx].artist) - 1] = '\0';
        }
        if (alb) {
            strncpy(albums[idx].album, alb, sizeof(albums[idx].album) - 1);
            albums[idx].album[sizeof(albums[idx].album) - 1] = '\0';
        }
        albums[idx].track_count = tc;
        idx++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    *count = idx;
    return albums;
}

void library_free_albums(LibraryAlbum *albums, int count) {
    (void)count;
    free(albums);
}

char **library_get_genres(int *count) {
    *count = 0;
    if (!library_is_available()) return NULL;

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT DISTINCT genre FROM tracks"
        " WHERE genre != '' ORDER BY genre COLLATE NOCASE",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cnt++;
    sqlite3_reset(stmt);

    char **genres = calloc((size_t)(cnt + 1), sizeof(char *));
    if (!genres) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val && val[0]) {
            genres[idx] = strdup(val);
            if (!genres[idx]) break;
            idx++;
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    *count = idx;
    return genres;
}

void library_free_genres(char **genres, int count) {
    if (!genres) return;
    for (int i = 0; i < count; i++) {
        free(genres[i]);
    }
    free(genres);
}

/* ========== Public API: Bridge to g_playlist ========== */

int library_load_into_playlist(const int *rowids, int count, int append) {
    if (!library_is_available() || !rowids || count <= 0) return -1;

    if (count > MAX_TRACKS) count = MAX_TRACKS;

    /* Build a new Playlist from rowids */
    Playlist new_pl = {0};

    if (append) {
        playlist_lock();
        /* Copy existing playlist */
        memcpy(&new_pl, &g_playlist, sizeof(Playlist));
        /* Keep has_multiple_sources if we already had it or are appending */
        if (new_pl.count > 0) new_pl.has_multiple_sources = 1;
        playlist_unlock();
    }

    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT path FROM tracks WHERE rowid = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return -1;
    }

    int loaded = 0;
    for (int i = 0; i < count && new_pl.count < MAX_TRACKS; i++) {
        sqlite3_bind_int(stmt, 1, rowids[i]);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            if (path && path[0]) {
                strncpy(new_pl.tracks[new_pl.count], path, MAX_PATH_LEN - 1);
                new_pl.tracks[new_pl.count][MAX_PATH_LEN - 1] = '\0';
                new_pl.count++;
                loaded++;
            }
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);

    /* Set folder_path: use directory of first track */
    if (loaded > 0) {
        const char *first_path = new_pl.tracks[0];
        const char *slash = strrchr(first_path, '/');
        if (slash) {
            size_t len = (size_t)(slash - first_path);
            memcpy(new_pl.folder_path, first_path, len);
            new_pl.folder_path[len] = '\0';
        }
        new_pl.is_loaded = 1;
    }

    /* Replace g_playlist */
    if (loaded > 0) {
        playlist_lock();
        g_playlist = new_pl;
        if (g_selected_index >= g_playlist.count) g_selected_index = 0;
        playlist_unlock();
        search_clear();
        recompute_sort_order();
    }

    return loaded;
}

/* ========== Public API: Favorites (SQLite-backed) ========== */

int library_favorites_get_count(void) {
    if (!library_is_available()) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM favorites", -1, &stmt, NULL);
    int count = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_favorites_get_all(Track *tracks, int max_tracks) {
    if (!library_is_available() || !tracks || max_tracks <= 0) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT t.path, t.title, t.artist, t.album FROM favorites f"
        " JOIN tracks t ON t.path = f.track_path"
        " ORDER BY f.added_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return 0;
    }
    sqlite3_bind_int(stmt, 1, max_tracks);
    int count = 0;
    while (count < max_tracks && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        const char *a = (const char *)sqlite3_column_text(stmt, 2);
        const char *al = (const char *)sqlite3_column_text(stmt, 3);
        if (p) strncpy(tracks[count].path, p, sizeof(tracks[count].path) - 1);
        if (t) strncpy(tracks[count].title, t, sizeof(tracks[count].title) - 1);
        if (a) strncpy(tracks[count].artist, a, sizeof(tracks[count].artist) - 1);
        if (al) strncpy(tracks[count].album, al, sizeof(tracks[count].album) - 1);
        count++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_favorites_add(const char *track_path) {
    if (!library_is_available() || !track_path) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO favorites(track_path, added_at) VALUES(?, ?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, track_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_favorites_remove(const char *track_path) {
    if (!library_is_available() || !track_path) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "DELETE FROM favorites WHERE track_path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, track_path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_favorites_has(const char *track_path) {
    if (!library_is_available() || !track_path) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "SELECT 1 FROM favorites WHERE track_path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, track_path, -1, SQLITE_STATIC);
    int has = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return has;
}

/* ========== Public API: Play History (SQLite-backed) ========== */

void library_history_add(const char *track_path, int position) {
    if (!library_is_available() || !track_path) return;
    /* Keep history bounded: delete oldest if over limit */
    pthread_mutex_lock(&g_library_mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "INSERT INTO play_history(track_path, played_at, position) VALUES(?, ?, ?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, track_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 3, position);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Prune if over MAX_HISTORY_COUNT */
    sqlite3_prepare_v2(g_db,
        "DELETE FROM play_history WHERE id NOT IN ("
        "  SELECT id FROM play_history ORDER BY played_at DESC LIMIT ?)",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, MAX_HISTORY_COUNT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
}

int library_history_get_count(void) {
    if (!library_is_available()) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM play_history", -1, &stmt, NULL);
    int count = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_history_get_all(HistoryEntry *entries, int max_entries) {
    if (!library_is_available() || !entries || max_entries <= 0) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT t.path, t.title, t.artist, h.played_at FROM play_history h"
        " JOIN tracks t ON t.path = h.track_path"
        " ORDER BY h.played_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return 0;
    }
    sqlite3_bind_int(stmt, 1, max_entries);
    int count = 0;
    while (count < max_entries && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        const char *a = (const char *)sqlite3_column_text(stmt, 2);
        time_t pt = (time_t)sqlite3_column_int64(stmt, 3);
        if (p) strncpy(entries[count].path, p, sizeof(entries[count].path) - 1);
        if (t) strncpy(entries[count].title, t, sizeof(entries[count].title) - 1);
        if (a) strncpy(entries[count].artist, a, sizeof(entries[count].artist) - 1);
        entries[count].play_time = pt;
        count++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

/* ========== Public API: User Playlists (SQLite-backed) ========== */

int library_playlist_create(const char *name) {
    if (!library_is_available() || !name || !name[0]) return -1;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "INSERT INTO playlists(name, created_at, modified_at) VALUES(?, ?, ?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
    int rc = sqlite3_step(stmt);
    sqlite3_int64 rowid = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(g_db) : 0;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? (int)rowid : -1;
}

int library_playlist_delete(int playlist_id) {
    if (!library_is_available()) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "DELETE FROM playlists WHERE id = ?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, playlist_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_playlist_rename(int playlist_id, const char *new_name) {
    if (!library_is_available() || !new_name) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "UPDATE playlists SET name = ?, modified_at = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 3, playlist_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_playlist_get_count(void) {
    if (!library_is_available()) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM playlists", -1, &stmt, NULL);
    int count = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_playlist_get_all(int *out_ids, char (*names)[MAX_PLAYLIST_NAME_LEN], int max) {
    if (!library_is_available() || !out_ids || !names || max <= 0) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "SELECT id, name FROM playlists ORDER BY name COLLATE NOCASE LIMIT ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, max);
    int count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        out_ids[count] = sqlite3_column_int(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        if (n) {
            strncpy(names[count], n, MAX_PLAYLIST_NAME_LEN - 1);
            names[count][MAX_PLAYLIST_NAME_LEN - 1] = '\0';
        } else {
            names[count][0] = '\0';
        }
        count++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

int library_playlist_add_track(int playlist_id, const char *track_path) {
    if (!library_is_available() || !track_path) return -1;
    pthread_mutex_lock(&g_library_mutex);

    /* Get next position */
    int position = 0;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_tracks WHERE playlist_id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, playlist_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) position = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO playlist_tracks(playlist_id, track_path, position, added_at)"
        " VALUES(?, ?, ?, ?)",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, playlist_id);
    sqlite3_bind_text(stmt, 2, track_path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, position);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Update playlists.modified_at */
    sqlite3_prepare_v2(g_db,
        "UPDATE playlists SET modified_at = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 2, playlist_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_playlist_remove_track(int playlist_id, int position) {
    if (!library_is_available()) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "DELETE FROM playlist_tracks WHERE playlist_id = ? AND position = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, playlist_id);
    sqlite3_bind_int(stmt, 2, position);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Update playlists.modified_at */
    sqlite3_prepare_v2(g_db,
        "UPDATE playlists SET modified_at = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 2, playlist_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_playlist_remove_track_by_path(int playlist_id, const char *track_path) {
    if (!library_is_available() || !track_path) return -1;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, playlist_id);
    sqlite3_bind_text(stmt, 2, track_path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Update playlists.modified_at */
    sqlite3_prepare_v2(g_db,
        "UPDATE playlists SET modified_at = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 2, playlist_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_library_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int library_playlist_get_tracks(int playlist_id, Track *tracks, int max_tracks) {
    if (!library_is_available() || !tracks || max_tracks <= 0) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT t.path, t.title, t.artist, t.album FROM playlist_tracks pt"
        " JOIN tracks t ON t.path = pt.track_path"
        " WHERE pt.playlist_id = ?"
        " ORDER BY pt.position ASC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_library_mutex);
        return 0;
    }
    sqlite3_bind_int(stmt, 1, playlist_id);
    sqlite3_bind_int(stmt, 2, max_tracks);
    int count = 0;
    while (count < max_tracks && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        const char *a = (const char *)sqlite3_column_text(stmt, 2);
        const char *al = (const char *)sqlite3_column_text(stmt, 3);
        if (p) strncpy(tracks[count].path, p, sizeof(tracks[count].path) - 1);
        if (t) strncpy(tracks[count].title, t, sizeof(tracks[count].title) - 1);
        if (a) strncpy(tracks[count].artist, a, sizeof(tracks[count].artist) - 1);
        if (al) strncpy(tracks[count].album, al, sizeof(tracks[count].album) - 1);
        count++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return count;
}

/* ========== Public API: Statistics ========== */

int library_get_total_duration_seconds(void) {
    if (!library_is_available()) return 0;
    pthread_mutex_lock(&g_library_mutex);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db, "SELECT COALESCE(SUM(duration_seconds), 0) FROM tracks", -1, &stmt, NULL);
    int total = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_library_mutex);
    return total;
}

/* ========== Public API: Migration ========== */

int library_has_data(void) {
    return library_get_track_count() > 0;
}

int library_migrate_from_json(void) {
    if (!library_is_available()) return -1;

    log_info("library", "Starting JSON-to-SQLite migration");

    /* Migration is done by reading existing JSON data via the existing
     * load functions and writing to SQLite. This is called from
     * init_all_persistent_data() in menu_views.c after JSON files are loaded.
     *
     * The actual migration logic is in menu_views.c since it has access to
     * the loaded g_favorites, g_play_history, and g_playlist_manager structs.
     * Here we provide the SQLite-side functions that menu_views.c calls. */
    return 0;
}
