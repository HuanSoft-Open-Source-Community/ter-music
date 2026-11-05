/**
 * @file library_query.c
 * @brief 曲库浏览视图的分页查询（D-Bus Library.GetPage / GetTree 的后端）
 *
 * 既有 library_get_artists / library_get_albums / library_get_genres 等接口
 * 没有 offset 参数，调用方只能一次性取全量；曲库上万曲目时前端必须自己
 * 缓存整表。本文件用 SQLite 的 LIMIT/OFFSET 提供分页，行数由调用方给定，
 * 因此单次调用只读取一页。
 *
 * 与 library.c 的分工：本文件只做查询，不碰 schema/扫描/迁移；底层句柄与
 * 互斥锁通过 library_db_handle()/library_lock() 获取（后者保证同一把锁）。
 * 注意：持锁期间不得调用会自行加锁的公开接口（如 library_get_track_path），
 * 因此路径直接随查询一并 SELECT 出来。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "library/library.h"
#include "logger/logger.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* 组装曲目类查询的 WHERE 子句；返回绑定的参数个数 */
static int build_track_filter(const LibraryQuery *query, char *sql, size_t sql_size)
{
    int binds = 0;
    sql[0] = '\0';

    if (query->artist[0]) {
        snprintf(sql + strlen(sql), sql_size - strlen(sql), "%s artist = ?",
                 binds ? " AND" : " WHERE");
        binds++;
    }
    if (query->album[0]) {
        snprintf(sql + strlen(sql), sql_size - strlen(sql), "%s album = ?",
                 binds ? " AND" : " WHERE");
        binds++;
    }
    if (query->genre[0]) {
        snprintf(sql + strlen(sql), sql_size - strlen(sql), "%s genre = ?",
                 binds ? " AND" : " WHERE");
        binds++;
    }
    return binds;
}

static void bind_track_filter(sqlite3_stmt *stmt, const LibraryQuery *query)
{
    int index = 1;
    if (query->artist[0]) sqlite3_bind_text(stmt, index++, query->artist, -1, SQLITE_TRANSIENT);
    if (query->album[0])  sqlite3_bind_text(stmt, index++, query->album, -1, SQLITE_TRANSIENT);
    if (query->genre[0])  sqlite3_bind_text(stmt, index++, query->genre, -1, SQLITE_TRANSIENT);
}

int library_query_count(const LibraryQuery *query)
{
    sqlite3 *db = (sqlite3 *)library_db_handle();
    if (!query || !db || !library_is_available()) {
        return -1;
    }

    char sql[768];
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_ERROR;
    int total = -1;
    char pattern[MAX_META_LEN * 2 + 4];

    switch (query->kind) {
        case LIBRARY_QUERY_ARTISTS:
            rc = sqlite3_prepare_v2(db,
                "SELECT COUNT(DISTINCT artist) FROM tracks WHERE artist != ''",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_GENRES:
            rc = sqlite3_prepare_v2(db,
                "SELECT COUNT(DISTINCT genre) FROM tracks WHERE genre != ''",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_ALBUMS: {
            char filter[512];
            build_track_filter(query, filter, sizeof(filter));
            snprintf(sql, sizeof(sql),
                     "SELECT COUNT(*) FROM (SELECT DISTINCT artist, album FROM tracks%s)",
                     filter);
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            break;
        }
        case LIBRARY_QUERY_SEARCH:
            if (query->query[0] == '\0') {
                return 0;
            }
            rc = sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM tracks"
                " WHERE title LIKE ?1 OR artist LIKE ?1 OR album LIKE ?1",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_TRACKS:
        default: {
            char filter[512];
            build_track_filter(query, filter, sizeof(filter));
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM tracks%s", filter);
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            break;
        }
    }

    if (rc == SQLITE_OK) {
        library_lock();
        if (query->kind == LIBRARY_QUERY_SEARCH) {
            snprintf(pattern, sizeof(pattern), "%%%s%%", query->query);
            sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
        } else {
            /* 占位符按 artist/album/genre 顺序绑定（与 build_track_filter 一致） */
            bind_track_filter(stmt, query);
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int(stmt, 0);
        }
        library_unlock();
    }

    if (stmt) {
        sqlite3_finalize(stmt);
    }
    return total;
}

int library_query_page(const LibraryQuery *query, int offset, int count,
                       LibraryRow *out, int cap)
{
    sqlite3 *db = (sqlite3 *)library_db_handle();
    if (!query || !out || cap <= 0 || offset < 0 || count <= 0 || count > cap) {
        return -1;
    }
    if (!db || !library_is_available()) {
        return -1;
    }

    char sql[1024];
    char filter[512];
    char pattern[MAX_META_LEN * 2 + 4];
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_ERROR;
    int binds = 0;
    int written = 0;
    int is_aggregate = (query->kind == LIBRARY_QUERY_ARTISTS ||
                        query->kind == LIBRARY_QUERY_ALBUMS ||
                        query->kind == LIBRARY_QUERY_GENRES);

    if (query->kind == LIBRARY_QUERY_SEARCH && query->query[0] == '\0') {
        return 0;
    }

    switch (query->kind) {
        case LIBRARY_QUERY_ARTISTS:
            rc = sqlite3_prepare_v2(db,
                "SELECT 0, artist, artist, '', '', COUNT(*) FROM tracks WHERE artist != ''"
                " GROUP BY artist ORDER BY artist COLLATE NOCASE LIMIT ?1 OFFSET ?2",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_GENRES:
            rc = sqlite3_prepare_v2(db,
                "SELECT 0, genre, '', '', '', COUNT(*) FROM tracks WHERE genre != ''"
                " GROUP BY genre ORDER BY genre COLLATE NOCASE LIMIT ?1 OFFSET ?2",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_ALBUMS:
            binds = build_track_filter(query, filter, sizeof(filter));
            snprintf(sql, sizeof(sql),
                     "SELECT 0, '', artist, album, '', COUNT(*) FROM tracks%s"
                     " GROUP BY artist, album"
                     " ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE"
                     " LIMIT ?%d OFFSET ?%d", filter, binds + 1, binds + 2);
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_SEARCH:
            snprintf(pattern, sizeof(pattern), "%%%s%%", query->query);
            rc = sqlite3_prepare_v2(db,
                "SELECT rowid, title, artist, album, path FROM tracks"
                " WHERE title LIKE ?1 OR artist LIKE ?1 OR album LIKE ?1"
                " ORDER BY title COLLATE NOCASE LIMIT ?2 OFFSET ?3",
                -1, &stmt, NULL);
            break;
        case LIBRARY_QUERY_TRACKS:
        default:
            binds = build_track_filter(query, filter, sizeof(filter));
            snprintf(sql, sizeof(sql),
                     "SELECT rowid, title, artist, album, path FROM tracks%s"
                     " ORDER BY path COLLATE NOCASE LIMIT ?%d OFFSET ?%d",
                     filter, binds + 1, binds + 2);
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            break;
    }

    if (rc != SQLITE_OK) {
        log_warn("library", "library_query_page prepare failed: %s", sqlite3_errmsg(db));
        if (stmt) sqlite3_finalize(stmt);
        return -1;
    }

    if (is_aggregate) {
        bind_track_filter(stmt, query);
        sqlite3_bind_int(stmt, binds + 1, count);
        sqlite3_bind_int(stmt, binds + 2, offset);
    } else if (query->kind == LIBRARY_QUERY_SEARCH) {
        sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, count);
        sqlite3_bind_int(stmt, 3, offset);
    } else {
        bind_track_filter(stmt, query);
        sqlite3_bind_int(stmt, binds + 1, count);
        sqlite3_bind_int(stmt, binds + 2, offset);
    }

    library_lock();
    while (written < count && sqlite3_step(stmt) == SQLITE_ROW) {
        LibraryRow *row = &out[written];
        memset(row, 0, sizeof(*row));

        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        const char *artist = (const char *)sqlite3_column_text(stmt, 2);
        const char *album = (const char *)sqlite3_column_text(stmt, 3);
        const char *path = (const char *)sqlite3_column_text(stmt, 4);

        if (is_aggregate) {
            /* 聚合列序：1 = name，2 = artist，3 = album，5 = count */
            row->track_count = sqlite3_column_int(stmt, 5);
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
            snprintf(row->artist, sizeof(row->artist), "%s", artist ? artist : "");
            snprintf(row->album, sizeof(row->album), "%s", album ? album : "");
        } else {
            row->rowid = sqlite3_column_int(stmt, 0);
            snprintf(row->name, sizeof(row->name), "%s", title ? title : "");
            snprintf(row->artist, sizeof(row->artist), "%s", artist ? artist : "");
            snprintf(row->album, sizeof(row->album), "%s", album ? album : "");
            if (path) {
                snprintf(row->path, sizeof(row->path), "%s", path);
            }
        }
        written++;
    }
    library_unlock();

    sqlite3_finalize(stmt);
    return written;
}
