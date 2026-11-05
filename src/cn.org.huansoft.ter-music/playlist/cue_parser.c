/**
 * @file cue_parser.c
 * @brief CUE sheet parser for split-track playback support.
 *
 * Parses standard CUE sheet files (.cue) used for lossless image (FLAC/APE/WV/WAV)
 * track splitting. Supports:
 *   - REM / PERFORMER / TITLE / FILE / TRACK / INDEX directives
 *   - MM:SS:FF time format (75 frames/second)
 *   - UTF-8 BOM detection
 *   - Quoted and unquoted string values
 *   - Multi-FILE blocks in a single CUE sheet
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-04
 */

#include "playlist/cue_parser.h"
#include "playlist/encoding.h"
#include "config/config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Helpers ── */

/**
 * @brief Strip trailing whitespace and CR/LF in-place.
 */
static void strip_line(char *line)
{
    if (!line || !line[0]) return;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
}

/**
 * @brief Skip leading whitespace and return pointer to first non-space.
 */
static const char *skip_spaces(const char *p)
{
    if (!p) return NULL;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/**
 * @brief Extract a quoted or unquoted string value from a line.
 *
 * CUE format:  TITLE "Some Title"  or  TITLE SomeTitle
 * Handles escaped quotes inside quoted strings.
 *
 * @param p      Pointer into the line, positioned after the directive keyword.
 * @param out    Output buffer.
 * @param out_sz Output buffer size.
 * @return Pointer after the extracted value, or NULL on error.
 */
static const char *extract_string_value(const char *p, char *out, size_t out_sz)
{
    if (!p || !out || out_sz == 0) return NULL;
    out[0] = '\0';

    p = skip_spaces(p);
    if (!p || !*p) return NULL;

    if (*p == '"') {
        /* Quoted string */
        p++; /* skip opening quote */
        size_t pos = 0;
        while (*p && *p != '"' && pos < out_sz - 1) {
            if (*p == '\\' && *(p + 1) == '"') {
                /* Escaped quote */
                if (pos < out_sz - 1) out[pos++] = '"';
                p += 2;
            } else {
                out[pos++] = *p++;
            }
        }
        out[pos] = '\0';
        if (*p == '"') p++; /* skip closing quote */
    } else {
        /* Unquoted string — read until space or end */
        size_t pos = 0;
        while (*p && *p != ' ' && *p != '\t' && pos < out_sz - 1)
            out[pos++] = *p++;
        out[pos] = '\0';
    }

    return p;
}

/**
 * @brief Convert CUE time string (MM:SS:FF) to seconds.
 *
 * CD frame rate is 75 frames/second.
 * MM:SS:FF -> (MM * 60) + SS + (FF / 75)
 *
 * @param time_str CUE time string (e.g. "04:35:20")
 * @return Seconds (integer, rounds down), or -1 on parse error.
 */
static int cue_time_to_seconds(const char *time_str)
{
    if (!time_str || !*time_str) return -1;

    int m = 0, s = 0, f = 0;
    if (sscanf(time_str, "%d:%d:%d", &m, &s, &f) >= 2) {
        if (m < 0) m = 0;
        if (s < 0) s = 0;
        if (f < 0) f = 0;
        return m * 60 + s + f / 75;
    }
    return -1;
}

/**
 * @brief Extract the basename (filename without extension) from a path.
 *
 * @param path Input file path.
 * @param out  Output buffer.
 * @param sz   Output buffer size.
 */
static void get_basename_no_ext(const char *path, char *out, size_t sz)
{
    if (!path || !out || sz == 0) return;
    out[0] = '\0';

    /* Find last '/' */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    /* Copy basename */
    size_t pos = 0;
    while (*base && *base != '.' && pos < sz - 1)
        out[pos++] = *base++;
    out[pos] = '\0';
}

/**
 * @brief Resolve a FILE reference from a CUE sheet.
 *
 * CUE FILE paths may be:
 *   - "filename.wav" (just filename, relative to .cue location)
 *   - "/absolute/path/filename.wav" (absolute)
 *   - "relative/path/filename.wav"
 *
 * This function attempts to resolve relative paths against the .cue file's directory.
 *
 * @param cue_path   Path to the .cue file.
 * @param file_ref   The FILE value from the CUE sheet.
 * @param resolved   Output buffer for resolved path.
 * @param resolved_sz Buffer size.
 */
static void resolve_cue_file_path(const char *cue_path, const char *file_ref,
                                  char *resolved, size_t resolved_sz)
{
    if (!cue_path || !file_ref || !resolved || resolved_sz == 0) return;

    /* If file_ref is already absolute, use it directly */
    if (file_ref[0] == '/') {
        strncpy(resolved, file_ref, resolved_sz - 1);
        resolved[resolved_sz - 1] = '\0';
        return;
    }

    /* Derive directory from cue_path */
    char cue_dir[MAX_PATH_LEN];
    strncpy(cue_dir, cue_path, sizeof(cue_dir) - 1);
    cue_dir[sizeof(cue_dir) - 1] = '\0';
    char *slash = strrchr(cue_dir, '/');
    if (slash) {
        *(slash + 1) = '\0'; /* keep trailing slash */
    } else {
        cue_dir[0] = '.';
        cue_dir[1] = '/';
        cue_dir[2] = '\0';
    }

    snprintf(resolved, resolved_sz, "%s%s", cue_dir, file_ref);
}

/* ================================================================
 * Public API
 * ================================================================ */

int cue_parse_file(const char *cue_path, CueSheet *sheet)
{
    if (!cue_path || !sheet) return -1;

    memset(sheet, 0, sizeof(*sheet));
    strncpy(sheet->loaded_cue_path, cue_path, sizeof(sheet->loaded_cue_path) - 1);

    FILE *fp = fopen(cue_path, "rb");
    if (!fp) {
        log_warn("cue", "Cannot open CUE file: '%s'", cue_path);
        return -1;
    }

    /* ── Read the whole file into memory for line-by-line parsing ── */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return -1; }
    rewind(fp);

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t bytes_read = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    buf[bytes_read] = '\0';

    /* ── Auto-detect encoding and convert to UTF-8 ── */
    char *converted = NULL;
    if (encoding_detect_and_convert((const unsigned char *)buf, bytes_read,
                                     g_app_config.cue_encoding,
                                     &converted) == 0 && converted) {
        free(buf);
        buf = converted;
        bytes_read = strlen(buf);
    }

    /* ── State for parsing ── */
    char *p = buf;
    char current_file[MAX_PATH_LEN] = "";
    char global_performer[MAX_META_LEN] = "";
    char global_title[MAX_META_LEN] = "";
    int track_count = 0;

    /* ── Line-by-line parse ── */
    char line[4096];
    while (*p && track_count < CUE_MAX_TRACKS) {
        /* Extract one line */
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1) {
            if (*p != '\r') line[li++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        line[li] = '\0';

        /* Skip empty lines */
        const char *cmd = skip_spaces(line);
        if (!cmd || !*cmd) continue;

        /* Determine directive */
        if (strncmp(cmd, "REM", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\t' || cmd[3] == '\0')) {
            /* Comment — skip */
            continue;
        }

        if (strncmp(cmd, "PERFORMER", 9) == 0 && (cmd[9] == ' ' || cmd[9] == '\t')) {
            char val[MAX_META_LEN];
            if (extract_string_value(cmd + 9, val, sizeof(val))) {
                /* If before any TRACK, it's the global album artist */
                if (track_count == 0 || sheet->count == 0) {
                    strncpy(global_performer, val, sizeof(global_performer) - 1);
                    strncpy(sheet->album_artist, val, sizeof(sheet->album_artist) - 1);
                } else {
                    /* It's a track-level PERFORMER — apply to current track */
                    sheet->tracks[sheet->count - 1].artist[0] = '\0';
                    strncpy(sheet->tracks[sheet->count - 1].artist, val,
                            sizeof(sheet->tracks[sheet->count - 1].artist) - 1);
                }
            }
            continue;
        }

        if (strncmp(cmd, "TITLE", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\t')) {
            char val[MAX_META_LEN];
            if (extract_string_value(cmd + 5, val, sizeof(val))) {
                if (track_count == 0 && sheet->count == 0) {
                    /* Global TITLE = album name */
                    strncpy(global_title, val, sizeof(global_title) - 1);
                    strncpy(sheet->album, val, sizeof(sheet->album) - 1);
                } else {
                    /* Track-level TITLE */
                    strncpy(sheet->tracks[sheet->count - 1].title, val,
                            sizeof(sheet->tracks[sheet->count - 1].title) - 1);
                }
            }
            continue;
        }

        if (strncmp(cmd, "FILE", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\t')) {
            char file_val[MAX_PATH_LEN];
            const char *after = extract_string_value(cmd + 4, file_val, sizeof(file_val));
            /* file_val is the filename; after points at the file type (e.g. WAVE) */
            (void)after;

            /* Resolve relative to .cue location */
            resolve_cue_file_path(cue_path, file_val, current_file, sizeof(current_file));
            continue;
        }

        if (strncmp(cmd, "TRACK", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\t')) {
            if (current_file[0] == '\0') {
                log_warn("cue", "TRACK without FILE in '%s', skipping", cue_path);
                continue;
            }

            CueTrack *ct = &sheet->tracks[track_count];
            memset(ct, 0, sizeof(*ct));

            strncpy(ct->file_path, current_file, sizeof(ct->file_path) - 1);
            strncpy(ct->album, global_title, sizeof(ct->album) - 1);
            strncpy(ct->artist, global_performer, sizeof(ct->artist) - 1);
            ct->index_00_offset = -1;
            ct->index_01_offset = -1;

            /* Parse track number */
            const char *tp = skip_spaces(cmd + 5);
            if (tp && *tp) {
                int tn = 0;
                if (sscanf(tp, "%d", &tn) == 1 && tn > 0)
                    ct->track_number = tn;
            }

            track_count++;
            sheet->count = track_count;
            continue;
        }

        if (strncmp(cmd, "INDEX", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\t')) {
            if (track_count == 0) continue;

            CueTrack *ct = &sheet->tracks[track_count - 1];
            const char *ip = skip_spaces(cmd + 5);

            /* Parse index number (00 or 01 usually) */
            int idx_num = 0;
            if (ip && *ip) {
                if (sscanf(ip, "%d", &idx_num) != 1) continue;
                /* Move past the index number */
                while (*ip && !isspace((unsigned char)*ip)) ip++;
                ip = skip_spaces(ip);
            }

            if (!ip || !*ip) continue;

            int sec = cue_time_to_seconds(ip);
            if (sec < 0) continue;

            if (idx_num == 0) {
                ct->index_00_offset = sec;
            } else if (idx_num == 1) {
                ct->index_01_offset = sec;
            }
            continue;
        }

        /* Unknown directive — silently skip (CUE allows custom REM-like entries) */
    }

    free(buf);

    if (track_count == 0) {
        log_warn("cue", "No tracks found in CUE file: '%s'", cue_path);
        return -1;
    }

    /* Post-process: ensure all tracks have INDEX 01, fill gaps */
    for (int i = 0; i < track_count; i++) {
        CueTrack *ct = &sheet->tracks[i];

        /* Default INDEX 01 if missing (fall back to previous track's end) */
        if (ct->index_01_offset < 0) {
            if (i == 0) {
                ct->index_01_offset = 0;
            } else {
                ct->index_01_offset = sheet->tracks[i - 1].index_01_offset;
            }
        }

        /* Use global artist if no track-level performer */
        if (ct->artist[0] == '\0' && sheet->album_artist[0] != '\0') {
            strncpy(ct->artist, sheet->album_artist, sizeof(ct->artist) - 1);
        }
        if (ct->artist[0] == '\0') {
            strncpy(ct->artist, "Unknown Artist", sizeof(ct->artist) - 1);
        }

        /* Use global album if no track-level album set */
        if (ct->album[0] == '\0' && sheet->album[0] != '\0') {
            strncpy(ct->album, sheet->album, sizeof(ct->album) - 1);
        }

        /* Default title from track number */
        if (ct->title[0] == '\0') {
            char default_title[32];
            snprintf(default_title, sizeof(default_title), "Track %02d", ct->track_number);
            strncpy(ct->title, default_title, sizeof(ct->title) - 1);
        }
    }

    log_info("cue", "Parsed %d tracks from '%s'", track_count, cue_path);
    return track_count;
}

/* ================================================================
 * CUE file matching
 * ================================================================ */

int cue_match_file(const char *audio_path, const char *cue_dir, char *out_path)
{
    if (!audio_path || !out_path) return 0;
    out_path[0] = '\0';

    /* Determine directory to search */
    char dir_buf[MAX_PATH_LEN];
    if (!cue_dir || cue_dir[0] == '\0') {
        /* Derive from audio_path */
        strncpy(dir_buf, audio_path, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *slash = strrchr(dir_buf, '/');
        if (slash) {
            *slash = '\0';
            cue_dir = dir_buf;
        } else {
            cue_dir = ".";
        }
    }

    /* Get basename without extension */
    char base[256];
    get_basename_no_ext(audio_path, base, sizeof(base));
    if (base[0] == '\0') return 0;

    DIR *dir = opendir(cue_dir);
    if (!dir) return 0;

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL && !found) {
        const char *name = entry->d_name;
        const char *ext = strrchr(name, '.');

        if (!ext) continue;
        if (strcasecmp(ext, ".cue") != 0) continue;

        /* Compare basename: first exact, then case-insensitive */
        char entry_base[256];
        get_basename_no_ext(name, entry_base, sizeof(entry_base));

        if (strcmp(entry_base, base) == 0) {
            /* Exact match */
            found = 1;
        } else if (strcasecmp(entry_base, base) == 0) {
            /* Case-insensitive match — prefer exact match later if found */
            found = 1;
        }

        if (found) {
            snprintf(out_path, MAX_PATH_LEN, "%s/%s", cue_dir, name);
        }
    }

    closedir(dir);

    /* If we didn't find a match via basename, also check for .cue files
     * that refer to this audio file in their FILE directive. */
    /* (This is a secondary fallback — most CUE files are named after the image) */

    return found;
}

/* ================================================================
 * CUE lookup
 * ================================================================ */

int cue_lookup_by_track_number(const CueSheet *sheet, int track_number, CueTrack *out)
{
    if (!sheet || !out) return 0;

    for (int i = 0; i < sheet->count; i++) {
        if (sheet->tracks[i].track_number == track_number) {
            *out = sheet->tracks[i];
            return 1;
        }
    }
    return 0;
}
