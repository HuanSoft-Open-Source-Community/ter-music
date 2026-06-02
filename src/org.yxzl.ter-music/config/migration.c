/**
 * @file config_migration.c
 * @brief v1 (JSON) → v2 (XML) automatic migration
 *
 * Detects old-format config.json, reads it using the deprecated
 * JSON extract functions, writes config.xml via config_xml.c,
 * then renames config.json → config.json.bak.
 *
 * @author ter-music team
 * @date 2026-06-01
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "types.h"
#include "config/schema.h"
#include "config/config.h"
#include "ui/menus.h"
#include "logger/logger.h"

/* ── Public API ───────────────────────────────────────────────────── */

int config_needs_migration(void)
{
    const char *home = getenv("HOME");
    if (!home) return 0;

    char old_path[MAX_PATH_LEN];
    char new_path[MAX_PATH_LEN];
    snprintf(old_path, sizeof(old_path), "%s/.config/ter-music/" CONFIG_FILE_OLD_NAME, home);
    snprintf(new_path, sizeof(new_path), "%s/.config/ter-music/" CONFIG_FILE_NAME, home);

    struct stat st_old, st_new;
    int has_old = (stat(old_path, &st_old) == 0);
    int has_new = (stat(new_path, &st_new) == 0);

    /* If XML already exists, no migration needed */
    if (has_new) return 0;

    /* If only JSON exists, migration is needed */
    return has_old ? 1 : 0;
}

int config_migrate_v1_to_v2(void)
{
    const char *home = getenv("HOME");
    if (!home) {
        log_error("migration", "HOME not set, cannot migrate");
        return -1;
    }

    char old_path[MAX_PATH_LEN];
    char new_path[MAX_PATH_LEN];
    char bak_path[MAX_PATH_LEN];
    snprintf(old_path, sizeof(old_path), "%s/.config/ter-music/" CONFIG_FILE_OLD_NAME, home);
    snprintf(new_path, sizeof(new_path), "%s/.config/ter-music/" CONFIG_FILE_NAME, home);
    snprintf(bak_path, sizeof(bak_path), "%s" CONFIG_FILE_BACKUP_SUFFIX, old_path);

    log_info("migration", "Migrating config from %s to %s", old_path, new_path);

    /* ── Read the old JSON file ──────────────────────────────────── */
    FILE *f = fopen(old_path, "r");
    if (!f) {
        log_error("migration", "Cannot open old config '%s'", old_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        log_error("migration", "Old config '%s' is empty", old_path);
        fclose(f);
        return -1;
    }

    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        log_error("migration", "Out of memory reading '%s'", old_path);
        return -1;
    }

    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);

    /* ── Parse JSON into AppConfig ───────────────────────────────── */
    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.config_version = CONFIG_CURRENT_VERSION;

    /* Strings */
    extract_json_string(json, "default_startup_path", cfg.default_startup_path, MAX_PATH_LEN);
    extract_json_string(json, "last_opened_path", cfg.last_opened_path, MAX_PATH_LEN);
    extract_json_string(json, "last_played_folder_path", cfg.last_played_folder_path, MAX_PATH_LEN);
    extract_json_string(json, "last_played_track_path", cfg.last_played_track_path, MAX_PATH_LEN);

    /* Theme */
    const char *theme_pos = strstr(json, "\"theme\"");
    if (theme_pos) {
        char theme_section[1024];
        const char *theme_start = strchr(theme_pos, '{');
        const char *theme_end = strchr(theme_start ? theme_start : theme_pos, '}');
        if (theme_start && theme_end && theme_end > theme_start) {
            size_t len = theme_end - theme_start + 1;
            if (len < sizeof(theme_section)) {
                strncpy(theme_section, theme_start, len);
                theme_section[len] = '\0';

                cfg.theme.playlist_fg  = (int)extract_json_int(theme_section, "playlist_fg");
                cfg.theme.playlist_bg  = (int)extract_json_int(theme_section, "playlist_bg");
                cfg.theme.controls_fg  = (int)extract_json_int(theme_section, "controls_fg");
                cfg.theme.controls_bg  = (int)extract_json_int(theme_section, "controls_bg");
                cfg.theme.lyrics_fg    = (int)extract_json_int(theme_section, "lyrics_fg");
                cfg.theme.lyrics_bg    = (int)extract_json_int(theme_section, "lyrics_bg");
                cfg.theme.sidebar_fg   = (int)extract_json_int(theme_section, "sidebar_fg");
                cfg.theme.sidebar_bg   = (int)extract_json_int(theme_section, "sidebar_bg");
                cfg.theme.highlight_fg = (int)extract_json_int(theme_section, "highlight_fg");
                cfg.theme.highlight_bg = (int)extract_json_int(theme_section, "highlight_bg");
                cfg.theme.border_fg    = (int)extract_json_int(theme_section, "border_fg");
                cfg.theme.border_bg    = (int)extract_json_int(theme_section, "border_bg");
            }
        }
    }

    /* Preferences */
    if (strstr(json, "\"auto_play_on_start\""))
        cfg.auto_play_on_start = (int)extract_json_int(json, "auto_play_on_start");
    if (strstr(json, "\"remember_last_path\""))
        cfg.remember_last_path = (int)extract_json_int(json, "remember_last_path");
    if (strstr(json, "\"clear_history_on_startup\""))
        cfg.clear_history_on_startup = (int)extract_json_int(json, "clear_history_on_startup");
    if (strstr(json, "\"resume_last_playback\""))
        cfg.resume_last_playback = (int)extract_json_int(json, "resume_last_playback");
    if (strstr(json, "\"last_played_position\""))
        cfg.last_played_position = (int)extract_json_int(json, "last_played_position");
    if (strstr(json, "\"ui_language\""))
        cfg.ui_language = (int)extract_json_int(json, "ui_language");
    if (strstr(json, "\"volume_percent\""))
        cfg.volume_percent = (int)extract_json_int(json, "volume_percent");
    if (strstr(json, "\"audio_latency_ms\""))
        cfg.audio_latency_ms = (int)extract_json_int(json, "audio_latency_ms");
    if (strstr(json, "\"show_lyrics_panel\""))
        cfg.show_lyrics_panel = (int)extract_json_int(json, "show_lyrics_panel");
    if (strstr(json, "\"default_loop_mode\""))
        cfg.default_loop_mode = (int)extract_json_int(json, "default_loop_mode");
    if (strstr(json, "\"default_playback_speed\""))
        cfg.default_playback_speed = (float)extract_json_float(json, "default_playback_speed");
    if (strstr(json, "\"show_album_cover\""))
        cfg.show_album_cover = (int)extract_json_int(json, "show_album_cover");
    if (strstr(json, "\"lyrics_alignment\""))
        cfg.lyrics_alignment = (int)extract_json_int(json, "lyrics_alignment");
    if (strstr(json, "\"audio_backend\""))
        cfg.audio_backend = (int)extract_json_int(json, "audio_backend");
    if (strstr(json, "\"sort_mode\""))
        cfg.sort_mode = (int)extract_json_int(json, "sort_mode");

    /* Remote connections */
    int old_version = 0;
    if (strstr(json, "\"config_version\""))
        old_version = (int)extract_json_int(json, "config_version");
    if (strstr(json, "\"remote_connection_count\"")) {
        int count = (int)extract_json_int(json, "remote_connection_count");
        if (count > MAX_REMOTE_CONNECTIONS) count = MAX_REMOTE_CONNECTIONS;
        if (count < 0) count = 0;
        cfg.remote_connection_count = count;

        for (int ri = 0; ri < count; ri++) {
            RemoteConnectionConfig *rc = &cfg.remote_connections[ri];
            char key[64];
            snprintf(key, sizeof(key), "remote_%d_name", ri);
            extract_json_string(json, key, rc->name, sizeof(rc->name));
            snprintf(key, sizeof(key), "remote_%d_protocol", ri);
            rc->protocol = (int)extract_json_int(json, key);
            snprintf(key, sizeof(key), "remote_%d_host", ri);
            extract_json_string(json, key, rc->host, sizeof(rc->host));
            snprintf(key, sizeof(key), "remote_%d_port", ri);
            rc->port = (int)extract_json_int(json, key);
            snprintf(key, sizeof(key), "remote_%d_username", ri);
            extract_json_string(json, key, rc->username, sizeof(rc->username));
            snprintf(key, sizeof(key), "remote_%d_password", ri);
            extract_json_string(json, key, rc->password, sizeof(rc->password));
            snprintf(key, sizeof(key), "remote_%d_private_key_path", ri);
            extract_json_string(json, key, rc->private_key_path, sizeof(rc->private_key_path));
            snprintf(key, sizeof(key), "remote_%d_base_path", ri);
            extract_json_string(json, key, rc->base_path, sizeof(rc->base_path));
        }
    }

    /* v0→v1 migration: clear unencrypted passwords from old v0 files */
    if (old_version < 1) {
        for (int ri = 0; ri < MAX_REMOTE_CONNECTIONS; ri++) {
            cfg.remote_connections[ri].password[0] = '\0';
        }
    }

    free(json);

    /* ── Write XML config ────────────────────────────────────────── */
    if (config_save_to_xml(new_path, &cfg) != 0) {
        log_error("migration", "Failed to write XML config to '%s'", new_path);
        return -1;
    }

    /* ── Verify the written XML by reading it back ───────────────── */
    AppConfig verify_cfg;
    if (config_load_from_xml(new_path, &verify_cfg) != 0) {
        log_error("migration", "Verification of migrated config failed");
        /* Remove the incomplete XML file */
        unlink(new_path);
        return -1;
    }

    /* ── Rename old config → config.json.bak (destructive marker) ── */
    if (rename(old_path, bak_path) != 0) {
        log_warn("migration", "Could not rename '%s' to '%s': %s",
                 old_path, bak_path, strerror(errno));
        /* Non-fatal — XML is already written */
    } else {
        log_info("migration", "Old config backed up to '%s'", bak_path);
    }

    log_info("migration", "Migration v1→v2 completed successfully");
    return 0;
}
