/**
 * @file i18n.c
 * @brief Internationalization engine using libxml2-parsed XML files
 *
 * Translation files live in <data>/lang/<id>.xml.
 * Lookup order: user dir → system → source tree → raw key.
 *
 * @author ter-music team
 * @date 2026-07
 */

#include "types.h"
#include "i18n/i18n.h"
#include "logger/logger.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Hash table (simple linked-list buckets) ─────────────────── */

#define I18N_HASH_SIZE 256

typedef struct I18nEntry {
    char *key;
    char *value;
    struct I18nEntry *next;
} I18nEntry;

static I18nEntry *g_i18n_table[I18N_HASH_SIZE] = {0};
static char g_current_lang[32] = "";
static int  g_i18n_loaded = 0;

/* ── Fallback table (zh_CN) ──────────────────────────────────── */

static I18nEntry *g_fallback_table[I18N_HASH_SIZE] = {0};
static int  g_fallback_loaded = 0;

/* ── Hash function ───────────────────────────────────────────── */

static unsigned int i18n_hash(const char *key)
{
    unsigned int h = 5381;
    while (*key) {
        h = ((h << 5) + h) + (unsigned char)*key;
        key++;
    }
    return h % I18N_HASH_SIZE;
}

/* ── Table helpers ───────────────────────────────────────────── */

static void i18n_table_insert(I18nEntry **table, const char *key, const char *value)
{
    unsigned int idx = i18n_hash(key);
    I18nEntry *e = malloc(sizeof(I18nEntry));
    if (!e) return;
    e->key   = strdup(key);
    e->value = strdup(value);
    e->next  = table[idx];
    table[idx] = e;
}

static const char *i18n_table_lookup(I18nEntry **table, const char *key)
{
    unsigned int idx = i18n_hash(key);
    for (I18nEntry *e = table[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0)
            return e->value;
    }
    return NULL;
}

static void i18n_table_free(I18nEntry **table)
{
    for (int i = 0; i < I18N_HASH_SIZE; i++) {
        I18nEntry *e = table[i];
        while (e) {
            I18nEntry *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
        table[i] = NULL;
    }
}

/* ── Path resolution ─────────────────────────────────────────── */

static const char *i18n_user_lang_dir(void)
{
    static char path[MAX_PATH_LEN] = "";
    if (!path[0]) {
        const char *home = getenv("HOME");
        if (home)
            snprintf(path, sizeof(path), "%s/.config/ter-music/lang", home);
    }
    return path[0] ? path : NULL;
}

static int i18n_find_lang_file(const char *lang_id, char *path, size_t path_size)
{
    /* 0. User lang dir (~/.config/ter-music/lang/) — highest priority */
    const char *user_dir = i18n_user_lang_dir();
    if (user_dir) {
        snprintf(path, path_size, "%s/%s.xml", user_dir, lang_id);
        if (access(path, R_OK) == 0) return 0;
    }

    /* 1. TER_MUSIC_DATA_DIR (compile-time install prefix) */
    snprintf(path, path_size, TER_MUSIC_DATA_DIR "/lang/%s.xml", lang_id);
    if (access(path, R_OK) == 0) return 0;

    /* 2. /usr/share/ter-music/lang/ */
    snprintf(path, path_size, "/usr/share/ter-music/lang/%s.xml", lang_id);
    if (access(path, R_OK) == 0) return 0;

    /* 3. Relative to executable (../share/ter-music/lang/) */
    char exe_path[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *dir = strrchr(exe_path, '/');
        if (dir) {
            *dir = '\0';
            snprintf(path, path_size, "%s/../share/ter-music/lang/%s.xml",
                     exe_path, lang_id);
            if (access(path, R_OK) == 0) return 0;
        }
    }

    /* 4. Local data/lang/ (development — relative to CWD) */
    snprintf(path, path_size, "data/lang/%s.xml", lang_id);
    if (access(path, R_OK) == 0) return 0;

    /* 5. Source tree data/lang/ (development — build-dir safe) */
    snprintf(path, path_size, TER_MUSIC_SOURCE_DIR "/data/lang/%s.xml", lang_id);
    if (access(path, R_OK) == 0) return 0;

    return -1;
}

/* ── XML parsing ─────────────────────────────────────────────── */

static int i18n_parse_xml(I18nEntry **table, const char *path, char *lang_name_out, size_t name_size)
{
    xmlDocPtr doc = xmlReadFile(path, NULL, XML_PARSE_NOBLANKS);
    if (!doc) {
        log_error("i18n", "Failed to parse %s", path);
        return -1;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "lang") != 0) {
        log_error("i18n", "Missing <lang> root in %s", path);
        xmlFreeDoc(doc);
        return -1;
    }

    /* Extract lang name from attribute */
    xmlChar *name_attr = xmlGetProp(root, (const xmlChar *)"name");
    if (name_attr && lang_name_out) {
        strncpy(lang_name_out, (const char *)name_attr, name_size - 1);
        lang_name_out[name_size - 1] = '\0';
        xmlFree(name_attr);
    }

    /* Iterate <string key="..." >value</string> */
    for (xmlNodePtr node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (strcmp((const char *)node->name, "string") != 0) continue;

        xmlChar *key = xmlGetProp(node, (const xmlChar *)"key");
        if (!key) continue;

        xmlChar *content = xmlNodeGetContent(node);
        if (content) {
            i18n_table_insert(table, (const char *)key, (const char *)content);
            xmlFree(content);
        }
        xmlFree(key);
    }

    xmlFreeDoc(doc);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int i18n_init(const char *lang_id)
{
    i18n_cleanup();
    log_debug("i18n", "i18n_init: lang_id=%s", lang_id ? lang_id : "(null)");

    /* Always load zh_CN as fallback */
    char path[MAX_PATH_LEN];
    char dummy[64];
    if (i18n_find_lang_file("zh_CN", path, sizeof(path)) == 0) {
        if (i18n_parse_xml(g_fallback_table, path, dummy, sizeof(dummy)) == 0) {
            g_fallback_loaded = 1;
            log_info("i18n", "Fallback language zh_CN loaded from %s", path);
        }
    }

    /* Load requested language */
    const char *target = lang_id ? lang_id : "zh_CN";
    if (i18n_find_lang_file(target, path, sizeof(path)) == 0) {
        if (i18n_parse_xml(g_i18n_table, path, NULL, 0) == 0) {
            strncpy(g_current_lang, target, sizeof(g_current_lang) - 1);
            g_current_lang[sizeof(g_current_lang) - 1] = '\0';
            g_i18n_loaded = 1;
            log_info("i18n", "Language '%s' loaded from %s", target, path);
            return 0;
        }
    }

    /* If requested language fails but fallback loaded, use that as primary */
    if (g_fallback_loaded) {
        for (int i = 0; i < I18N_HASH_SIZE; i++) {
            g_i18n_table[i] = g_fallback_table[i];
            g_fallback_table[i] = NULL;
        }
        strncpy(g_current_lang, "zh_CN", sizeof(g_current_lang) - 1);
        g_current_lang[sizeof(g_current_lang) - 1] = '\0';
        g_i18n_loaded = 1;
        g_fallback_loaded = 0;
        log_warn("i18n", "Language '%s' not found, using zh_CN fallback", target);
        return 0;
    }

    log_error("i18n", "No translation files found; i18n_get() will return raw keys");
    return -1;
}

int i18n_reload(const char *lang_id)
{
    if (!lang_id || !lang_id[0]) return -1;

    log_debug("i18n", "i18n_reload: switching to %s", lang_id);

    char path[MAX_PATH_LEN];
    if (i18n_find_lang_file(lang_id, path, sizeof(path)) != 0) {
        log_error("i18n", "Language file not found: %s", lang_id);
        return -1;
    }

    i18n_table_free(g_i18n_table);
    g_i18n_loaded = 0;

    if (i18n_parse_xml(g_i18n_table, path, NULL, 0) == 0) {
        strncpy(g_current_lang, lang_id, sizeof(g_current_lang) - 1);
        g_current_lang[sizeof(g_current_lang) - 1] = '\0';
        g_i18n_loaded = 1;
        log_info("i18n", "Reloaded language '%s'", lang_id);
        return 0;
    }

    return -1;
}

void i18n_cleanup(void)
{
    i18n_table_free(g_i18n_table);
    i18n_table_free(g_fallback_table);
    g_current_lang[0] = '\0';
    g_i18n_loaded = 0;
    g_fallback_loaded = 0;
}

const char *i18n_get(const char *key)
{
    if (!key) return "";

    if (g_i18n_loaded) {
        const char *val = i18n_table_lookup(g_i18n_table, key);
        if (val) return val;
    }

    if (g_fallback_loaded) {
        const char *val = i18n_table_lookup(g_fallback_table, key);
        if (val) return val;
    }

    return key;
}

const char *i18n_current_lang(void)
{
    return g_current_lang[0] ? g_current_lang : "zh_CN";
}

/* ── Language discovery ──────────────────────────────────────── */

int i18n_available_languages(I18nLanguage *langs, int max)
{
    int count = 0;

    #define SCAN_DIR(dirpath, is_builtin) do { \
        DIR *dp = opendir(dirpath); \
        if (dp) { \
            struct dirent *entry; \
            while ((entry = readdir(dp)) && count < max) { \
                const char *name = entry->d_name; \
                size_t len = strlen(name); \
                if (len <= 4 || strcmp(name + len - 4, ".xml") != 0) continue; \
                char lid[32]; \
                size_t il = len - 4; \
                if (il >= sizeof(lid)) il = sizeof(lid) - 1; \
                memcpy(lid, name, il); lid[il] = '\0'; \
                int dup = 0; \
                for (int i = 0; i < count; i++) { \
                    if (strcmp(langs[i].id, lid) == 0) { dup = 1; break; } \
                } \
                if (dup) continue; \
                char fpath[MAX_PATH_LEN]; \
                snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, name); \
                char lname[64] = ""; \
                xmlDocPtr doc = xmlReadFile(fpath, NULL, 0); \
                if (doc) { \
                    xmlNodePtr root = xmlDocGetRootElement(doc); \
                    if (root) { \
                        xmlChar *na = xmlGetProp(root, (const xmlChar *)"name"); \
                        if (na) { strncpy(lname, (const char *)na, sizeof(lname)-1); xmlFree(na); } \
                    } \
                    xmlFreeDoc(doc); \
                } \
                if (!lname[0]) strncpy(lname, lid, sizeof(lname)-1); \
                strncpy(langs[count].id, lid, sizeof(langs[count].id)-1); \
                langs[count].id[sizeof(langs[count].id)-1] = '\0'; \
                strncpy(langs[count].name, lname, sizeof(langs[count].name)-1); \
                langs[count].name[sizeof(langs[count].name)-1] = '\0'; \
                langs[count].builtin = (is_builtin); \
                count++; \
            } \
            closedir(dp); \
        } \
    } while(0)

    const char *user_dir = i18n_user_lang_dir();
    if (user_dir) SCAN_DIR(user_dir, 0);

    SCAN_DIR(TER_MUSIC_DATA_DIR "/lang", 1);
    SCAN_DIR("/usr/share/ter-music/lang", 1);
    SCAN_DIR("data/lang", 1);
    SCAN_DIR(TER_MUSIC_SOURCE_DIR "/data/lang", 1);

    #undef SCAN_DIR
    return count;
}

/* ── Language pack management ────────────────────────────────── */

int i18n_is_builtin(const char *lang_id)
{
    if (!lang_id) return 0;
    return (strcmp(lang_id, "zh_CN") == 0 || strcmp(lang_id, "en_US") == 0);
}

int i18n_add_language(const char *source_path)
{
    if (!source_path) return -1;

    /* Reject directories and non-regular files early */
    {
        struct stat st;
        if (stat(source_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            log_error("i18n", "Not a regular file: %s", source_path);
            return -1;
        }
    }

    /* Validate XML structure */
    xmlDocPtr doc = xmlReadFile(source_path, NULL, 0);
    if (!doc) { log_error("i18n", "Cannot parse %s", source_path); return -1; }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "lang") != 0) {
        log_error("i18n", "Missing <lang> root in %s", source_path);
        xmlFreeDoc(doc);
        return -1;
    }

    xmlChar *id_attr = xmlGetProp(root, (const xmlChar *)"id");
    if (!id_attr || !((const char *)id_attr)[0]) {
        log_error("i18n", "Missing 'id' attribute on <lang> in %s", source_path);
        if (id_attr) xmlFree(id_attr);
        xmlFreeDoc(doc);
        return -1;
    }

    char lang_id[32];
    strncpy(lang_id, (const char *)id_attr, sizeof(lang_id) - 1);
    lang_id[sizeof(lang_id) - 1] = '\0';
    xmlFree(id_attr);
    xmlFreeDoc(doc);

    const char *user_dir = i18n_user_lang_dir();
    if (!user_dir) { log_error("i18n", "Cannot determine user lang dir"); return -1; }
    mkdir(user_dir, 0755);

    char dest[MAX_PATH_LEN];
    snprintf(dest, sizeof(dest), "%s/%s.xml", user_dir, lang_id);

    FILE *src = fopen(source_path, "rb");
    if (!src) { log_error("i18n", "Cannot open source %s", source_path); return -1; }

    FILE *dst = fopen(dest, "wb");
    if (!dst) { fclose(src); log_error("i18n", "Cannot write %s", dest); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);

    fclose(src);
    fclose(dst);

    log_debug("i18n", "i18n_add_language: source=%s dest=%s", source_path, dest);
    log_info("i18n", "Language '%s' installed to %s", lang_id, dest);
    return 0;
}

int i18n_delete_language(const char *lang_id)
{
    if (!lang_id) return -1;

    if (i18n_is_builtin(lang_id)) {
        log_error("i18n", "Cannot delete built-in language '%s'", lang_id);
        return -1;
    }

    const char *user_dir = i18n_user_lang_dir();
    if (!user_dir) return -1;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.xml", user_dir, lang_id);

    if (unlink(path) != 0) {
        log_error("i18n", "Failed to delete %s", path);
        return -1;
    }

    log_debug("i18n", "i18n_delete_language: lang_id=%s", lang_id);
    log_info("i18n", "Language '%s' deleted", lang_id);
    return 0;
}

/* ── Coverage calculation ────────────────────────────────────── */

double i18n_coverage(const char *lang_id)
{
    if (!lang_id || !lang_id[0]) return -1.0;

    /* zh_CN is the reference — always 100% */
    if (strcmp(lang_id, "zh_CN") == 0) return 100.0;

    /* Count total reference keys from the fallback table */
    int ref_total = 0;
    for (int i = 0; i < I18N_HASH_SIZE; i++) {
        for (I18nEntry *e = g_fallback_table[i]; e; e = e->next)
            ref_total++;
    }
    if (ref_total == 0) return -1.0;

    /* Find and parse the target language XML */
    char path[MAX_PATH_LEN];
    if (i18n_find_lang_file(lang_id, path, sizeof(path)) != 0)
        return -1.0;

    xmlDocPtr doc = xmlReadFile(path, NULL, XML_PARSE_NOBLANKS);
    if (!doc) return -1.0;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "lang") != 0) {
        xmlFreeDoc(doc);
        return -1.0;
    }

    int matched = 0;
    for (xmlNodePtr node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (strcmp((const char *)node->name, "string") != 0) continue;

        xmlChar *key = xmlGetProp(node, (const xmlChar *)"key");
        if (!key) continue;

        if (i18n_table_lookup(g_fallback_table, (const char *)key))
            matched++;

        xmlFree(key);
    }

    xmlFreeDoc(doc);

    return (double)(matched * 100.0) / (double)ref_total;
}
