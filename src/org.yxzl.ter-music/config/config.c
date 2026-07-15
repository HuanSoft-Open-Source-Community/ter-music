/**
 * @file config_xml.c
 * @brief XML serialization / deserialization for AppConfig using libxml2
 *
 * Provides save/load/validate functions for the v2 XML config format.
 * Password fields are encrypted on save and decrypted on load
 * (delegates to crypto.c).
 *
 * @author ter-music team
 * @date 2026-06-01
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "types.h"
#include "config/schema.h"
#include "config/crypto.h"
#include "playlist/encoding.h"
#include "logger/logger.h"
#include "audio/equalizer.h"
#include "i18n/i18n.h"

/* ── Forward declarations of internal helpers ─────────────────────── */

static void   clamp_config_values(AppConfig *cfg);
static int    xml_get_int(const xmlNode *parent, const char *name, int def);
static float  xml_get_float(const xmlNode *parent, const char *name, float def);
static void   xml_get_string(const xmlNode *parent, const char *name,
                             char *out, size_t out_size);
static xmlNodePtr xml_find_child(const xmlNode *parent, const char *name);

/* ncurses COLOR_* constants — not available here; use numeric values */
#define C_WHITE  7
#define C_BLACK  0
#define C_YELLOW 3
#define C_GREEN  2
#define C_CYAN   6

/* ── Public API ───────────────────────────────────────────────────── */

int config_validate_xml(const char *path)
{
    xmlDocPtr doc = xmlParseFile(path);
    if (!doc) {
        log_error("config_xml", "Failed to parse XML file '%s'", path);
        return -1;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) {
        log_error("config_xml", "Empty XML document '%s'", path);
        xmlFreeDoc(doc);
        return -1;
    }

    if (xmlStrcmp(root->name, (const xmlChar *)XML_ROOT) != 0) {
        log_error("config_xml", "Unexpected root element '%s', expected '%s'",
                  (const char *)root->name, XML_ROOT);
        xmlFreeDoc(doc);
        return -1;
    }

    xmlChar *ver = xmlGetProp(root, (const xmlChar *)XML_ATTR_VERSION);
    if (!ver) {
        log_error("config_xml", "Missing 'version' attribute on root element");
        xmlFreeDoc(doc);
        return -1;
    }

    int valid = (atoi((const char *)ver) >= CONFIG_MIN_SUPPORTED_VER) ? 0 : -1;
    if (valid != 0) {
        log_error("config_xml", "Unsupported config version '%s'", (const char *)ver);
    }
    xmlFree(ver);
    xmlFreeDoc(doc);
    return valid;
}

int config_save_to_xml(const char *path, const AppConfig *cfg)
{
    xmlDocPtr doc = xmlNewDoc((const xmlChar *)"1.0");
    xmlNodePtr root = xmlNewNode(NULL, (const xmlChar *)XML_ROOT);
    xmlSetProp(root, (const xmlChar *)XML_ATTR_VERSION,
               (const xmlChar *)CONFIG_XML_VERSION);
    xmlDocSetRootElement(doc, root);

    /* ── <paths> ────────────────────────────────────────────────── */
    xmlNodePtr paths = xmlNewChild(root, NULL, (const xmlChar *)XML_SECTION_PATHS, NULL);
    xmlNewChild(paths, NULL, (const xmlChar *)XML_PATH_DEFAULT_STARTUP,
                (const xmlChar *)cfg->default_startup_path);
    xmlNewChild(paths, NULL, (const xmlChar *)XML_PATH_LAST_OPENED,
                (const xmlChar *)cfg->last_opened_path);
    xmlNewChild(paths, NULL, (const xmlChar *)XML_PATH_LAST_PLAYED_FOLDER,
                (const xmlChar *)cfg->last_played_folder_path);
    xmlNewChild(paths, NULL, (const xmlChar *)XML_PATH_LAST_PLAYED_TRACK,
                (const xmlChar *)cfg->last_played_track_path);

    /* ── <theme> ────────────────────────────────────────────────── */
    xmlNodePtr theme = xmlNewChild(root, NULL, (const xmlChar *)XML_SECTION_THEME, NULL);
    char buf[64];
#define ADD_THEME_INT(child, elem_name, val) \
    snprintf(buf, sizeof(buf), "%d", val); \
    xmlNewChild(child, NULL, (const xmlChar *)(elem_name), (const xmlChar *)buf)

    ADD_THEME_INT(theme, XML_THEME_PLAYLIST_FG,  cfg->theme.playlist_fg);
    ADD_THEME_INT(theme, XML_THEME_PLAYLIST_BG,  cfg->theme.playlist_bg);
    ADD_THEME_INT(theme, XML_THEME_CONTROLS_FG,  cfg->theme.controls_fg);
    ADD_THEME_INT(theme, XML_THEME_CONTROLS_BG,  cfg->theme.controls_bg);
    ADD_THEME_INT(theme, XML_THEME_LYRICS_FG,    cfg->theme.lyrics_fg);
    ADD_THEME_INT(theme, XML_THEME_LYRICS_BG,    cfg->theme.lyrics_bg);
    ADD_THEME_INT(theme, XML_THEME_SIDEBAR_FG,   cfg->theme.sidebar_fg);
    ADD_THEME_INT(theme, XML_THEME_SIDEBAR_BG,   cfg->theme.sidebar_bg);
    ADD_THEME_INT(theme, XML_THEME_HIGHLIGHT_FG, cfg->theme.highlight_fg);
    ADD_THEME_INT(theme, XML_THEME_HIGHLIGHT_BG, cfg->theme.highlight_bg);
    ADD_THEME_INT(theme, XML_THEME_BORDER_FG,    cfg->theme.border_fg);
    ADD_THEME_INT(theme, XML_THEME_BORDER_BG,    cfg->theme.border_bg);
#undef ADD_THEME_INT

    /* ── <preferences> ──────────────────────────────────────────── */
    {
        xmlNodePtr prefs = xmlNewChild(root, NULL,
                                       (const xmlChar *)XML_SECTION_PREFERENCES, NULL);
#define SAVE_INT(elem, val) do { \
    snprintf(buf, sizeof(buf), "%d", (val)); \
    xmlNewChild(prefs, NULL, (const xmlChar *)(elem), (const xmlChar *)buf); \
} while(0)

        SAVE_INT(XML_PREF_AUTO_PLAY,       cfg->auto_play_on_start);
        SAVE_INT(XML_PREF_REMEMBER_PATH,   cfg->remember_last_path);
        SAVE_INT(XML_PREF_CLEAR_HISTORY,   cfg->clear_history_on_startup);
        SAVE_INT(XML_PREF_RESUME_PLAYBACK, cfg->resume_last_playback);
        SAVE_INT(XML_PREF_LAST_POSITION,   cfg->last_played_position);
        xmlNewChild(prefs, NULL, (const xmlChar *)XML_PREF_LANGUAGE,
                    (const xmlChar *)cfg->ui_language);
        SAVE_INT(XML_PREF_VOLUME,          cfg->volume_percent);
        SAVE_INT(XML_PREF_AUDIO_LATENCY,   cfg->audio_latency_ms);
        SAVE_INT(XML_PREF_SHOW_LYRICS,     cfg->show_lyrics_panel);
        SAVE_INT(XML_PREF_PLAY_MODE,       cfg->default_play_mode);
        SAVE_INT(XML_PREF_ADVANCED_PLAY_MODES, cfg->advanced_play_modes_enabled);

        snprintf(buf, sizeof(buf), "%.2f", cfg->default_playback_speed);
        xmlNewChild(prefs, NULL, (const xmlChar *)XML_PREF_PLAYBACK_SPEED,
                    (const xmlChar *)buf);

        SAVE_INT(XML_PREF_SHOW_COVER,      cfg->show_album_cover);
        SAVE_INT(XML_PREF_SEAMLESS_PRELOAD, cfg->seamless_preload);
        SAVE_INT(XML_PREF_LYRICS_ALIGN,    cfg->lyrics_alignment);
        SAVE_INT(XML_PREF_AUDIO_BACKEND,   cfg->audio_backend);
        SAVE_INT(XML_PREF_SORT_MODE,       cfg->sort_mode);
        SAVE_INT(XML_PREF_CUE_ENCODING,    cfg->cue_encoding);
#undef SAVE_INT
    }

    /* ── <equalizer> ─────────────────────────────────────────── */
    {
        xmlNodePtr eq_node = xmlNewChild(root, NULL,
                                          (const xmlChar *)XML_SECTION_EQUALIZER, NULL);
        snprintf(buf, sizeof(buf), "%d", cfg->eq_enabled);
        xmlNewChild(eq_node, NULL, (const xmlChar *)XML_EQ_ENABLED,
                    (const xmlChar *)buf);
        snprintf(buf, sizeof(buf), "%d", cfg->eq_preamp);
        xmlNewChild(eq_node, NULL, (const xmlChar *)XML_EQ_PREAMP,
                    (const xmlChar *)buf);

        for (int i = 0; i < EQ_BAND_COUNT; i++) {
            char freq_str[16];
            snprintf(freq_str, sizeof(freq_str), "%d", eq_band_frequencies[i]);
            char gain_str[16];
            snprintf(gain_str, sizeof(gain_str), "%d", cfg->eq_band_gains[i]);
            xmlNodePtr band = xmlNewChild(eq_node, NULL,
                                           (const xmlChar *)XML_EQ_BAND,
                                           (const xmlChar *)gain_str);
            xmlSetProp(band, (const xmlChar *)XML_ATTR_BAND_FREQUENCY,
                       (const xmlChar *)freq_str);
        }
    }

    /* ── <remote_connections> ───────────────────────────────────── */
    xmlNodePtr remotes = xmlNewChild(root, NULL,
                                     (const xmlChar *)XML_SECTION_REMOTE_CONNS, NULL);
    for (int i = 0; i < cfg->remote_connection_count && i < MAX_REMOTE_CONNECTIONS; i++) {
        const RemoteConnectionConfig *rc = &cfg->remote_connections[i];
        xmlNodePtr conn = xmlNewChild(remotes, NULL,
                                      (const xmlChar *)XML_REMOTE_CONN, NULL);

        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_NAME,
                    (const xmlChar *)rc->name);

        snprintf(buf, sizeof(buf), "%d", rc->protocol);
        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_PROTOCOL,
                    (const xmlChar *)buf);

        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_HOST,
                    (const xmlChar *)rc->host);

        snprintf(buf, sizeof(buf), "%d", rc->port);
        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_PORT,
                    (const xmlChar *)buf);

        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_USERNAME,
                    (const xmlChar *)rc->username);

        /* Encrypt password on save */
        xmlNodePtr pwdNode = xmlNewChild(conn, NULL,
                                         (const xmlChar *)XML_REMOTE_PASSWORD, NULL);
        if (rc->password[0]) {
            char encrypted[512];
            crypto_encrypt(rc->password, encrypted, sizeof(encrypted));
            xmlNodeSetContent(pwdNode, (const xmlChar *)encrypted);
            xmlSetProp(pwdNode, (const xmlChar *)XML_ATTR_PASSWORD_ENCRYPTED,
                       (const xmlChar *)XML_VAL_ENCRYPTED);
        }

        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_PRIVKEY,
                    (const xmlChar *)rc->private_key_path);

        xmlNewChild(conn, NULL, (const xmlChar *)XML_REMOTE_BASE_PATH,
                    (const xmlChar *)rc->base_path);
    }

    /* ── Write to file ──────────────────────────────────────────── */
    int ret = xmlSaveFormatFileEnc(path, doc, "UTF-8", 1);
    xmlFreeDoc(doc);

    if (ret < 0) {
        log_error("config_xml", "Failed to write XML config to '%s'", path);
        return -1;
    }

    log_info("config_xml", "Saved config to '%s' (%d bytes)", path, ret);
    return 0;
}

int config_load_from_xml(const char *path, AppConfig *cfg)
{
    xmlDocPtr doc = xmlParseFile(path);
    if (!doc) {
        log_error("config_xml", "Failed to parse '%s'", path);
        return -1;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || xmlStrcmp(root->name, (const xmlChar *)XML_ROOT) != 0) {
        log_error("config_xml", "Invalid root element in '%s'", path);
        xmlFreeDoc(doc);
        return -1;
    }

    /* ── Start from defaults, then overlay XML values ─────────────── */
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = CONFIG_CURRENT_VERSION;

    /* ── <paths> ────────────────────────────────────────────────── */
    xmlNodePtr paths = xml_find_child(root, XML_SECTION_PATHS);
    if (paths) {
        xml_get_string(paths, XML_PATH_DEFAULT_STARTUP,
                       cfg->default_startup_path, MAX_PATH_LEN);
        xml_get_string(paths, XML_PATH_LAST_OPENED,
                       cfg->last_opened_path, MAX_PATH_LEN);
        xml_get_string(paths, XML_PATH_LAST_PLAYED_FOLDER,
                       cfg->last_played_folder_path, MAX_PATH_LEN);
        xml_get_string(paths, XML_PATH_LAST_PLAYED_TRACK,
                       cfg->last_played_track_path, MAX_PATH_LEN);
    }

    /* ── <theme> ────────────────────────────────────────────────── */
    xmlNodePtr theme = xml_find_child(root, XML_SECTION_THEME);
    if (theme) {
        cfg->theme.playlist_fg  = xml_get_int(theme, XML_THEME_PLAYLIST_FG, C_WHITE);
        cfg->theme.playlist_bg  = xml_get_int(theme, XML_THEME_PLAYLIST_BG, C_BLACK);
        cfg->theme.controls_fg  = xml_get_int(theme, XML_THEME_CONTROLS_FG, C_YELLOW);
        cfg->theme.controls_bg  = xml_get_int(theme, XML_THEME_CONTROLS_BG, C_BLACK);
        cfg->theme.lyrics_fg    = xml_get_int(theme, XML_THEME_LYRICS_FG, C_GREEN);
        cfg->theme.lyrics_bg    = xml_get_int(theme, XML_THEME_LYRICS_BG, C_BLACK);
        cfg->theme.sidebar_fg   = xml_get_int(theme, XML_THEME_SIDEBAR_FG, C_CYAN);
        cfg->theme.sidebar_bg   = xml_get_int(theme, XML_THEME_SIDEBAR_BG, C_BLACK);
        cfg->theme.highlight_fg = xml_get_int(theme, XML_THEME_HIGHLIGHT_FG, C_BLACK);
        cfg->theme.highlight_bg = xml_get_int(theme, XML_THEME_HIGHLIGHT_BG, C_WHITE);
        cfg->theme.border_fg    = xml_get_int(theme, XML_THEME_BORDER_FG, C_CYAN);
        cfg->theme.border_bg    = xml_get_int(theme, XML_THEME_BORDER_BG, C_BLACK);
    }

    /* ── <preferences> ──────────────────────────────────────────── */
    xmlNodePtr prefs = xml_find_child(root, XML_SECTION_PREFERENCES);
    if (prefs) {
        cfg->auto_play_on_start      = xml_get_int(prefs, XML_PREF_AUTO_PLAY, 0);
        cfg->remember_last_path       = xml_get_int(prefs, XML_PREF_REMEMBER_PATH, 1);
        cfg->clear_history_on_startup = xml_get_int(prefs, XML_PREF_CLEAR_HISTORY, 0);
        cfg->resume_last_playback     = xml_get_int(prefs, XML_PREF_RESUME_PLAYBACK, 0);
        cfg->last_played_position     = xml_get_int(prefs, XML_PREF_LAST_POSITION, 0);
        xml_get_string(prefs, XML_PREF_LANGUAGE,
                       cfg->ui_language, sizeof(cfg->ui_language));
        if (cfg->ui_language[0] == '\0')
            strcpy(cfg->ui_language, "zh_CN");
        cfg->volume_percent           = xml_get_int(prefs, XML_PREF_VOLUME, 100);
        cfg->audio_latency_ms         = xml_get_int(prefs, XML_PREF_AUDIO_LATENCY, 80);
        cfg->show_lyrics_panel        = xml_get_int(prefs, XML_PREF_SHOW_LYRICS, 1);
        /* Migration: try new default_play_mode first, fallback to old default_loop_mode */
        {
            int new_mode = xml_get_int(prefs, XML_PREF_PLAY_MODE, -1);
            if (new_mode >= 0 && new_mode < PLAY_MODE_COUNT) {
                cfg->default_play_mode = new_mode;
            } else {
                int old_loop = xml_get_int(prefs, XML_PREF_LOOP_MODE, -1);
                static const int loop_to_play[] = {
                    PLAY_MODE_SEQUENTIAL,      /* LOOP_OFF(0)    → sequential */
                    PLAY_MODE_SINGLE_REPEAT,   /* LOOP_SINGLE(1) → single repeat */
                    PLAY_MODE_LIST_REPEAT,     /* LOOP_LIST(2)   → list repeat */
                    PLAY_MODE_SHUFFLE_REPEAT   /* LOOP_RANDOM(3) → shuffle repeat */
                };
                if (old_loop >= 0 && old_loop <= 3)
                    cfg->default_play_mode = loop_to_play[old_loop];
                else
                    cfg->default_play_mode = PLAY_MODE_SEQUENTIAL;
            }
        }
        cfg->advanced_play_modes_enabled = xml_get_int(prefs, XML_PREF_ADVANCED_PLAY_MODES, 0);
        cfg->default_playback_speed   = xml_get_float(prefs, XML_PREF_PLAYBACK_SPEED, 1.0f);
        cfg->show_album_cover         = xml_get_int(prefs, XML_PREF_SHOW_COVER, 1);
        cfg->seamless_preload         = xml_get_int(prefs, XML_PREF_SEAMLESS_PRELOAD, 0);
        cfg->lyrics_alignment         = xml_get_int(prefs, XML_PREF_LYRICS_ALIGN, 0);
        cfg->audio_backend            = xml_get_int(prefs, XML_PREF_AUDIO_BACKEND, AUDIO_BACKEND_AUTO);
        cfg->sort_mode                = xml_get_int(prefs, XML_PREF_SORT_MODE, SORT_DEFAULT);
        cfg->cue_encoding             = xml_get_int(prefs, XML_PREF_CUE_ENCODING, CUE_ENCODING_AUTO);
    }

    /* ── <remote_connections> ───────────────────────────────────── */
    xmlNodePtr remotes = xml_find_child(root, XML_SECTION_REMOTE_CONNS);
    if (remotes) {
        xmlNodePtr conn = remotes->children;
        int ri = 0;
        while (conn && ri < MAX_REMOTE_CONNECTIONS) {
            if (conn->type == XML_ELEMENT_NODE &&
                xmlStrcmp(conn->name, (const xmlChar *)XML_REMOTE_CONN) == 0) {

                RemoteConnectionConfig *rc = &cfg->remote_connections[ri];
                xml_get_string(conn, XML_REMOTE_NAME, rc->name, sizeof(rc->name));
                rc->protocol = xml_get_int(conn, XML_REMOTE_PROTOCOL, 0);

                xml_get_string(conn, XML_REMOTE_HOST, rc->host, sizeof(rc->host));
                rc->port = xml_get_int(conn, XML_REMOTE_PORT, 0);
                xml_get_string(conn, XML_REMOTE_USERNAME, rc->username, sizeof(rc->username));

                /* Decrypt password if encrypted attribute is set */
                xmlNodePtr pwdNode = xml_find_child(conn, XML_REMOTE_PASSWORD);
                if (pwdNode) {
                    xmlChar *content = xmlNodeGetContent(pwdNode);
                    if (content) {
                        xmlChar *encAttr = xmlGetProp(pwdNode,
                                      (const xmlChar *)XML_ATTR_PASSWORD_ENCRYPTED);
                        if (encAttr && xmlStrcmp(encAttr, (const xmlChar *)XML_VAL_ENCRYPTED) == 0) {
                            crypto_decrypt((const char *)content, rc->password,
                                           sizeof(rc->password));
                            xmlFree(encAttr);
                        } else {
                            strncpy(rc->password, (const char *)content, sizeof(rc->password) - 1);
                            rc->password[sizeof(rc->password) - 1] = '\0';
                        }
                        xmlFree(content);
                    }
                }

                xml_get_string(conn, XML_REMOTE_PRIVKEY,
                               rc->private_key_path, sizeof(rc->private_key_path));
                xml_get_string(conn, XML_REMOTE_BASE_PATH,
                               rc->base_path, sizeof(rc->base_path));

                ri++;
            }
            conn = conn->next;
        }
        cfg->remote_connection_count = ri;
    }

    /* ── <equalizer> ─────────────────────────────────────────────── */
    {
        xmlNodePtr eq = xml_find_child(root, XML_SECTION_EQUALIZER);
        if (eq) {
            cfg->eq_enabled = xml_get_int(eq, XML_EQ_ENABLED, 0);
            cfg->eq_preamp  = xml_get_int(eq, XML_EQ_PREAMP, 0);
            memset(cfg->eq_band_gains, 0, sizeof(cfg->eq_band_gains));

            for (xmlNodePtr child = eq->children; child; child = child->next) {
                if (child->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(child->name, (const xmlChar *)XML_EQ_BAND) == 0) {
                    xmlChar *freq_attr = xmlGetProp(child,
                                        (const xmlChar *)XML_ATTR_BAND_FREQUENCY);
                    xmlChar *content = xmlNodeGetContent(child);
                    if (freq_attr && content) {
                        int freq = atoi((const char *)freq_attr);
                        for (int i = 0; i < EQ_BAND_COUNT; i++) {
                            if (eq_band_frequencies[i] == freq) {
                                cfg->eq_band_gains[i] = atoi((const char *)content);
                                break;
                            }
                        }
                    }
                    xmlFree(freq_attr);
                    xmlFree(content);
                }
            }
        }
    }

    xmlFreeDoc(doc);

    /* Apply clamping / validation */
    clamp_config_values(cfg);

    log_info("config_xml", "Loaded config from '%s'", path);
    return 0;
}

/* ── Internal helpers ─────────────────────────────────────────────── */

static xmlNodePtr xml_find_child(const xmlNode *parent, const char *name)
{
    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, (const xmlChar *)name) == 0)
            return child;
        child = child->next;
    }
    return NULL;
}

static int xml_get_int(const xmlNode *parent, const char *name, int def)
{
    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, (const xmlChar *)name) == 0) {
            xmlChar *content = xmlNodeGetContent(child);
            if (!content) return def;
            int val = atoi((const char *)content);
            xmlFree(content);
            return val;
        }
        child = child->next;
    }
    return def;
}

static float xml_get_float(const xmlNode *parent, const char *name, float def)
{
    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, (const xmlChar *)name) == 0) {
            xmlChar *content = xmlNodeGetContent(child);
            if (!content) return def;
            float val = (float)atof((const char *)content);
            xmlFree(content);
            return val;
        }
        child = child->next;
    }
    return def;
}

static void xml_get_string(const xmlNode *parent, const char *name,
                           char *out, size_t out_size)
{
    if (!out || out_size == 0) return;

    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, (const xmlChar *)name) == 0) {
            xmlChar *content = xmlNodeGetContent(child);
            if (content) {
                strncpy(out, (const char *)content, out_size - 1);
                out[out_size - 1] = '\0';
                xmlFree(content);
            }
            return;
        }
        child = child->next;
    }

    /* Not found: ensure null-terminated empty */
    out[0] = '\0';
}

static void clamp_config_values(AppConfig *cfg)
{
    /* Boolean-normalize */
    cfg->auto_play_on_start      = cfg->auto_play_on_start ? 1 : 0;
    cfg->remember_last_path       = cfg->remember_last_path ? 1 : 0;
    cfg->clear_history_on_startup = cfg->clear_history_on_startup ? 1 : 0;
    cfg->resume_last_playback     = cfg->resume_last_playback ? 1 : 0;
    cfg->show_lyrics_panel        = cfg->show_lyrics_panel ? 1 : 0;
    cfg->show_album_cover         = cfg->show_album_cover ? 1 : 0;
    cfg->seamless_preload         = cfg->seamless_preload ? 1 : 0;

    /* Range clamping */
    if (cfg->last_played_position < 0)
        cfg->last_played_position = 0;

    if (cfg->ui_language[0] == '\0' ||
        (strcmp(cfg->ui_language, "zh_CN") != 0 &&
         strcmp(cfg->ui_language, "en_US") != 0))
        strcpy(cfg->ui_language, "zh_CN");

    if (cfg->volume_percent < 0)   cfg->volume_percent = 0;
    if (cfg->volume_percent > 100) cfg->volume_percent = 100;

    if (cfg->audio_latency_ms < 20)  cfg->audio_latency_ms = 20;
    if (cfg->audio_latency_ms > 250) cfg->audio_latency_ms = 250;

    if (cfg->default_play_mode < 0 || cfg->default_play_mode >= PLAY_MODE_COUNT)
        cfg->default_play_mode = PLAY_MODE_SEQUENTIAL;
    cfg->advanced_play_modes_enabled = cfg->advanced_play_modes_enabled ? 1 : 0;

    if (cfg->default_playback_speed < 0.5f)
        cfg->default_playback_speed = 0.5f;
    if (cfg->default_playback_speed > 3.0f)
        cfg->default_playback_speed = 3.0f;

    if (cfg->lyrics_alignment < 0 || cfg->lyrics_alignment > 2)
        cfg->lyrics_alignment = 0;

    if (cfg->audio_backend < AUDIO_BACKEND_AUTO || cfg->audio_backend > AUDIO_BACKEND_PIPEWIRE)
        cfg->audio_backend = AUDIO_BACKEND_AUTO;

    if (cfg->sort_mode < SORT_DEFAULT || cfg->sort_mode > SORT_FILENAME)
        cfg->sort_mode = SORT_DEFAULT;

    if (cfg->cue_encoding < CUE_ENCODING_AUTO || cfg->cue_encoding >= CUE_ENCODING_COUNT)
        cfg->cue_encoding = CUE_ENCODING_AUTO;

    /* ── Equalizer ── */
    cfg->eq_enabled = cfg->eq_enabled ? 1 : 0;
    if (cfg->eq_preamp < EQ_PREAMP_MIN) cfg->eq_preamp = EQ_PREAMP_MIN;
    if (cfg->eq_preamp > EQ_PREAMP_MAX) cfg->eq_preamp = EQ_PREAMP_MAX;
    for (int i = 0; i < EQ_BAND_COUNT; i++) {
        if (cfg->eq_band_gains[i] < EQ_GAIN_MIN) cfg->eq_band_gains[i] = EQ_GAIN_MIN;
        if (cfg->eq_band_gains[i] > EQ_GAIN_MAX) cfg->eq_band_gains[i] = EQ_GAIN_MAX;
    }
}
