/**
 * @file config_schema.h
 * @brief XML config schema constants for ter-music v2 config format
 *
 * Defines XML element names, attribute names, version identifiers
 * used by config_xml.c for serialization/deserialization.
 *
 * @author ter-music team
 * @date 2026-06-01
 */

#ifndef CONFIG_SCHEMA_H
#define CONFIG_SCHEMA_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version identifiers ──────────────────────────────────────────── */
#define CONFIG_XML_VERSION          "2.2"
#define CONFIG_CURRENT_VERSION      4
#define CONFIG_MIN_SUPPORTED_VER    2

/* ── Root element ─────────────────────────────────────────────────── */
#define XML_ROOT                    "ter-music-config"
#define XML_ATTR_VERSION            "version"

/* ── Top-level sections ───────────────────────────────────────────── */
#define XML_SECTION_PATHS           "paths"
#define XML_SECTION_THEME           "theme"
#define XML_SECTION_PREFERENCES     "preferences"
#define XML_SECTION_REMOTE_CONNS    "remote_connections"

/* ── Paths ────────────────────────────────────────────────────────── */
#define XML_PATH_DEFAULT_STARTUP    "default_startup_path"
#define XML_PATH_LAST_OPENED        "last_opened_path"
#define XML_PATH_LAST_PLAYED_FOLDER "last_played_folder_path"
#define XML_PATH_LAST_PLAYED_TRACK  "last_played_track_path"

/* ── Theme ────────────────────────────────────────────────────────── */
#define XML_THEME_PLAYLIST_FG       "playlist_fg"
#define XML_THEME_PLAYLIST_BG       "playlist_bg"
#define XML_THEME_CONTROLS_FG       "controls_fg"
#define XML_THEME_CONTROLS_BG       "controls_bg"
#define XML_THEME_LYRICS_FG         "lyrics_fg"
#define XML_THEME_LYRICS_BG         "lyrics_bg"
#define XML_THEME_SIDEBAR_FG        "sidebar_fg"
#define XML_THEME_SIDEBAR_BG        "sidebar_bg"
#define XML_THEME_HIGHLIGHT_FG      "highlight_fg"
#define XML_THEME_HIGHLIGHT_BG      "highlight_bg"
#define XML_THEME_BORDER_FG         "border_fg"
#define XML_THEME_BORDER_BG         "border_bg"

/* ── Preferences ──────────────────────────────────────────────────── */
#define XML_PREF_AUTO_PLAY          "auto_play_on_start"
#define XML_PREF_REMEMBER_PATH      "remember_last_path"
#define XML_PREF_CLEAR_HISTORY      "clear_history_on_startup"
#define XML_PREF_RESUME_PLAYBACK    "resume_last_playback"
#define XML_PREF_LAST_POSITION      "last_played_position"
#define XML_PREF_LANGUAGE           "ui_language"
#define XML_PREF_VOLUME             "volume_percent"
#define XML_PREF_AUDIO_LATENCY      "audio_latency_ms"
#define XML_PREF_SHOW_LYRICS        "show_lyrics_panel"
#define XML_PREF_LOOP_MODE          "default_loop_mode"
#define XML_PREF_PLAY_MODE          "default_play_mode"
#define XML_PREF_ADVANCED_PLAY_MODES "advanced_play_modes_enabled"
#define XML_PREF_PLAYBACK_SPEED     "default_playback_speed"
#define XML_PREF_SHOW_COVER         "show_album_cover"
#define XML_PREF_SEAMLESS_PRELOAD   "seamless_preload"
#define XML_PREF_LYRICS_ALIGN       "lyrics_alignment"
#define XML_PREF_AUDIO_BACKEND      "audio_backend"
#define XML_PREF_SORT_MODE          "sort_mode"
#define XML_PREF_CUE_ENCODING       "cue_encoding"

/* ── Equalizer ────────────────────────────────────────────────────── */
#define XML_SECTION_EQUALIZER       "equalizer"
#define XML_EQ_ENABLED              "enabled"
#define XML_EQ_PREAMP               "preamp"
#define XML_EQ_BAND                 "band"
#define XML_ATTR_BAND_FREQUENCY     "frequency"

/* ── Remote connections ───────────────────────────────────────────── */
#define XML_REMOTE_CONN             "connection"
#define XML_REMOTE_NAME             "name"
#define XML_REMOTE_PROTOCOL         "protocol"
#define XML_REMOTE_HOST             "host"
#define XML_REMOTE_PORT             "port"
#define XML_REMOTE_USERNAME         "username"
#define XML_REMOTE_PASSWORD         "password"
#define XML_REMOTE_PRIVKEY          "private_key_path"
#define XML_REMOTE_BASE_PATH        "base_path"

/* Attribute used on <password> to indicate encryption */
#define XML_ATTR_PASSWORD_ENCRYPTED "encrypted"
#define XML_VAL_ENCRYPTED           "1"

/* ── File names ───────────────────────────────────────────────────── */
#define CONFIG_FILE_NAME            "config.xml"
#define CONFIG_FILE_OLD_NAME        "config.json"
#define CONFIG_FILE_BACKUP_SUFFIX   ".bak"

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SCHEMA_H */
