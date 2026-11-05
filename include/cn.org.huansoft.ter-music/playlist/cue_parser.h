#ifndef CUE_PARSER_H
#define CUE_PARSER_H

#include "types.h"

#define CUE_MAX_TRACKS 200

/**
 * @brief A single track entry parsed from a CUE sheet.
 */
typedef struct {
    char file_path[MAX_PATH_LEN];   /**< Audio file this track belongs to */
    char title[MAX_META_LEN];       /**< TRACK TITLE */
    char artist[MAX_META_LEN];      /**< TRACK PERFORMER (falls back to global PERFORMER) */
    char album[MAX_META_LEN];       /**< Global TITLE from CUE (album name) */
    int track_number;               /**< CUE track number (1-99) */
    int index_00_offset;            /**< INDEX 00 (pregap) in seconds, -1 if absent */
    int index_01_offset;            /**< INDEX 01 in seconds (main start offset) */
} CueTrack;

/**
 * @brief Parsed CUE sheet result.
 */
typedef struct {
    CueTrack tracks[CUE_MAX_TRACKS];
    int count;
    char loaded_cue_path[MAX_PATH_LEN]; /**< Path of the .cue file that was loaded */
    char album_artist[MAX_META_LEN];    /**< Global PERFORMER from CUE */
    char album[MAX_META_LEN];           /**< Global TITLE from CUE */
} CueSheet;

/**
 * @brief Parse a .cue file into a CueSheet structure.
 *
 * Supports:
 *   - REM / PERFORMER / TITLE / FILE / TRACK / INDEX directives
 *   - MM:SS:FF time format
 *   - UTF-8 BOM detection and skipping
 *   - Multi-FILE blocks
 *   - Quoted and unquoted string values
 *
 * @param cue_path  Path to the .cue file.
 * @param sheet     Output structure (will be zeroed before parsing).
 * @return Number of tracks parsed on success, -1 on error.
 */
int cue_parse_file(const char *cue_path, CueSheet *sheet);

/**
 * @brief Check if a .cue file matching the given audio file exists in a directory.
 *
 * First tries exact basename match (e.g. "song.flac" -> "song.cue"),
 * then falls back to case-insensitive match.
 *
 * @param audio_path Full path to the audio file.
 * @param cue_dir    Directory to search for .cue files (may be NULL to use audio_path's dir).
 * @param out_path   Buffer to receive the full .cue path if found (MAX_PATH_LEN).
 * @return 1 if a matching .cue was found, 0 otherwise.
 */
int cue_match_file(const char *audio_path, const char *cue_dir, char *out_path);

/**
 * @brief Look up a CueTrack by track_number within a CueSheet.
 *
 * @param sheet         Parsed CueSheet.
 * @param track_number  Track number to find.
 * @param out           Output CueTrack if found.
 * @return 1 if found, 0 otherwise.
 */
int cue_lookup_by_track_number(const CueSheet *sheet, int track_number, CueTrack *out);

#endif /* CUE_PARSER_H */
