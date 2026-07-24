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

#include <zlib.h>

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

static char g_current_lang_name[64] = "";

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
        char lang_name[64] = "";
        if (i18n_parse_xml(g_i18n_table, path, lang_name, sizeof(lang_name)) == 0) {
            strncpy(g_current_lang, target, sizeof(g_current_lang) - 1);
            g_current_lang[sizeof(g_current_lang) - 1] = '\0';
            strncpy(g_current_lang_name, lang_name, sizeof(g_current_lang_name) - 1);
            g_current_lang_name[sizeof(g_current_lang_name) - 1] = '\0';
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

    char lang_name[64] = "";
    if (i18n_parse_xml(g_i18n_table, path, lang_name, sizeof(lang_name)) == 0) {
        strncpy(g_current_lang, lang_id, sizeof(g_current_lang) - 1);
        g_current_lang[sizeof(g_current_lang) - 1] = '\0';
        strncpy(g_current_lang_name, lang_name, sizeof(g_current_lang_name) - 1);
        g_current_lang_name[sizeof(g_current_lang_name) - 1] = '\0';
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

const char *i18n_current_lang_name(void)
{
    return g_current_lang_name[0] ? g_current_lang_name : "zh_CN";
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

/* ── tar.gz extraction helpers ─────────────────────────────────── */

/* Read a full block from the gz stream; returns 0 on success, -1 on error/EOF */
static inline int tar_read_block(gzFile gz, unsigned char *buf, int size)
{
    int total = 0;
    while (total < size) {
        int n = gzread(gz, buf + total, size - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

/* Parse octal size field from tar header (12 chars at offset 124) */
static size_t tar_parse_size(const unsigned char *header)
{
    size_t size = 0;
    for (int i = 0; i < 12; i++) {
        unsigned char c = header[124 + i];
        if (c == 0 || c == ' ') break;
        if (c < '0' || c > '7') break;
        size = (size << 3) | (size_t)(c - '0');
    }
    return size;
}

/* Extract lang_id from an in-memory XML buffer by parsing <lang id="...">
 * Returns 0 on success, -1 on failure. */
static int tar_extract_lang_id(const unsigned char *data, size_t size,
                               char *lang_id_out, size_t lang_id_size)
{
    xmlDocPtr doc = xmlReadMemory((const char *)data, (int)size, NULL, NULL, 0);
    if (!doc) return -1;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "lang") != 0) {
        xmlFreeDoc(doc);
        return -1;
    }
    xmlChar *id_attr = xmlGetProp(root, (const xmlChar *)"id");
    if (!id_attr || !((const char *)id_attr)[0]) {
        if (id_attr) xmlFree(id_attr);
        xmlFreeDoc(doc);
        return -1;
    }
    strncpy(lang_id_out, (const char *)id_attr, lang_id_size - 1);
    lang_id_out[lang_id_size - 1] = '\0';
    xmlFree(id_attr);
    xmlFreeDoc(doc);
    return 0;
}

/* Extract a .tar.gz language pack.  On entry the caller has validated that
 * source_path is a regular file.  The tarball must contain lang.xml with a
 * <lang id="..."> attribute.  If help.txt is present it is also extracted.
 *
 * Extracted files go to tmp_dir; lang_id_out receives the language id.
 * Returns: 0 on success, -1 on format error.
 */
static int i18n_tar_gz_extract(const char *source_path, const char *tmp_dir,
                               char *lang_id_out, size_t lang_id_size)
{
    gzFile gz = gzopen(source_path, "rb");
    if (!gz) return -1;

    int found_xml = 0;
    char xml_path[MAX_PATH_LEN];
    char help_path[MAX_PATH_LEN];
    snprintf(xml_path, sizeof(xml_path), "%s/lang.xml", tmp_dir);
    snprintf(help_path, sizeof(help_path), "%s/help.txt", tmp_dir);

    unsigned char header[512];
    int ret = -1;  /* default: error */

    for (;;) {
        if (tar_read_block(gz, header, 512) != 0) break;

        /* Check for end-of-archive (two zero blocks) */
        int all_zero = 1;
        for (int i = 0; i < 512; i++) {
            if (header[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            unsigned char second[512];
            tar_read_block(gz, second, 512);
            break;
        }

        /* Get filename (max 100 chars) */
        char name_buf[256];
        memcpy(name_buf, header, 100);
        name_buf[100] = '\0';

        /* USTAR prefix at offset 345 (155 bytes) */
        const char *prefix = "";
        if (header[257] == 'u' && header[258] == 's' && header[259] == 't'
            && header[260] == 'a' && header[261] == 'r') {
            static char pfx_buf[156];
            memcpy(pfx_buf, header + 345, 155);
            pfx_buf[155] = '\0';
            if (pfx_buf[0]) prefix = pfx_buf;
        }

        char fullname[512];
        if (prefix[0])
            snprintf(fullname, sizeof(fullname), "%s/%s", prefix, name_buf);
        else {
            strncpy(fullname, name_buf, sizeof(fullname) - 1);
            fullname[sizeof(fullname) - 1] = '\0';
        }

        size_t file_size = tar_parse_size(header);
        unsigned char typeflag = header[156];

        /* Skip directories and non-regular files */
        if (typeflag == '5' || (fullname[0] && fullname[strlen(fullname) - 1] == '/')) {
            if (file_size > 0) {
                size_t skip_blocks = (file_size + 511) / 512;
                unsigned char skip_buf[512];
                for (size_t i = 0; i < skip_blocks; i++)
                    tar_read_block(gz, skip_buf, 512);
            }
            continue;
        }

        /* Only match files named "lang.xml" or "help.txt" */
        const char *basename = strrchr(fullname, '/');
        basename = basename ? basename + 1 : fullname;
        int is_xml  = (strcmp(basename, "lang.xml") == 0);
        int is_help = (strcmp(basename, "help.txt") == 0);

        if (!is_xml && !is_help) {
            if (file_size > 0) {
                size_t skip_blocks = (file_size + 511) / 512;
                unsigned char skip_buf[512];
                for (size_t i = 0; i < skip_blocks; i++)
                    tar_read_block(gz, skip_buf, 512);
            }
            continue;
        }

        /* Read file content (50 MB sanity cap) */
        unsigned char *data = NULL;
        if (file_size > 0 && file_size < 50 * 1024 * 1024) {
            data = malloc(file_size);
            if (data && tar_read_block(gz, data, (int)file_size) != 0) {
                free(data);
                break;
            }
        }

        /* Skip padding to 512-byte boundary */
        size_t padding = (512 - (file_size % 512)) % 512;
        if (padding) {
            unsigned char pad[512];
            tar_read_block(gz, pad, (int)padding);
        }

        if (!data) continue;

        if (is_xml) {
            if (tar_extract_lang_id(data, file_size, lang_id_out, lang_id_size) != 0) {
                free(data);
                gzclose(gz);
                return -1;
            }
            FILE *f = fopen(xml_path, "wb");
            if (f) { fwrite(data, 1, file_size, f); fclose(f); found_xml = 1; }
        } else if (is_help) {
            FILE *f = fopen(help_path, "wb");
            if (f) { fwrite(data, 1, file_size, f); fclose(f); }
        }
        free(data);
    }

    gzclose(gz);
    if (found_xml) ret = 0;
    return ret;
}

/* ── User help dir ─────────────────────────────────────────────── */

static const char *i18n_user_help_dir(void)
{
    static char path[MAX_PATH_LEN] = "";
    if (!path[0]) {
        const char *home = getenv("HOME");
        if (home)
            snprintf(path, sizeof(path), "%s/.config/ter-music/help", home);
    }
    return path[0] ? path : NULL;
}

int i18n_is_builtin(const char *lang_id)
{
    if (!lang_id) return 0;
    return (strcmp(lang_id, "zh_CN") == 0 || strcmp(lang_id, "en_US") == 0);
}

/* Forward declaration for temp-dir cleanup helper */
static void rmdir_safe(const char *dir);

int i18n_add_language(const char *source_path)
{
    if (!source_path) return -1;

    /* Reject non-regular files */
    {
        struct stat st;
        if (stat(source_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            log_error("i18n", "Not a regular file: %s", source_path);
            return -1;
        }
    }

    /* Require .tar.gz or .tgz extension */
    const char *ext = strrchr(source_path, '.');
    int is_tar_gz = 0;
    if (ext) {
        size_t len = strlen(source_path);
        if ((len >= 7 && strcmp(source_path + len - 7, ".tar.gz") == 0) ||
            (len >= 4 && strcmp(source_path + len - 4, ".tgz") == 0))
            is_tar_gz = 1;
    }

    if (!is_tar_gz) {
        log_error("i18n", "Not a .tar.gz file: %s", source_path);
        return -1;
    }

    /* Create temp dir for extraction */
    char tmp_dir[MAX_PATH_LEN];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/ter-music-lang-XXXXXX");
    if (!mkdtemp(tmp_dir)) {
        log_error("i18n", "Cannot create temp dir");
        return -1;
    }

    /* Extract tar.gz */
    char lang_id[32] = "";
    if (i18n_tar_gz_extract(source_path, tmp_dir, lang_id, sizeof(lang_id)) != 0) {
        log_error("i18n", "tar.gz extraction failed: %s", source_path);
        rmdir(tmp_dir);
        return -1;
    }

    /* Validate extracted XML (defensive — tar_gz_extract already did basic validation) */
    char xml_src[MAX_PATH_LEN];
    snprintf(xml_src, sizeof(xml_src), "%s/lang.xml", tmp_dir);

    xmlDocPtr doc = xmlReadFile(xml_src, NULL, 0);
    if (!doc) {
        log_error("i18n", "Cannot parse lang.xml from %s", source_path);
        rmdir_safe(tmp_dir);
        return -1;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "lang") != 0) {
        log_error("i18n", "Missing <lang> root in tar.gz lang.xml");
        xmlFreeDoc(doc);
        rmdir_safe(tmp_dir);
        return -1;
    }
    xmlFreeDoc(doc);

    /* Install lang.xml to ~/.config/ter-music/lang/ */
    const char *user_dir = i18n_user_lang_dir();
    if (!user_dir) {
        log_error("i18n", "Cannot determine user lang dir");
        rmdir_safe(tmp_dir);
        return -1;
    }
    mkdir(user_dir, 0755);

    char dest[MAX_PATH_LEN];
    snprintf(dest, sizeof(dest), "%s/%s.xml", user_dir, lang_id);

    FILE *src = fopen(xml_src, "rb");
    if (!src) {
        log_error("i18n", "Cannot open extracted lang.xml");
        rmdir_safe(tmp_dir);
        return -1;
    }
    FILE *dst = fopen(dest, "wb");
    if (!dst) {
        fclose(src);
        rmdir_safe(tmp_dir);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);

    log_info("i18n", "Language '%s' installed from %s", lang_id, source_path);

    /* Install help.txt if present in tarball */
    char help_src[MAX_PATH_LEN];
    snprintf(help_src, sizeof(help_src), "%s/help.txt", tmp_dir);
    {
        struct stat hst;
        if (stat(help_src, &hst) == 0 && S_ISREG(hst.st_mode)) {
            const char *help_dir = i18n_user_help_dir();
            if (help_dir) {
                mkdir(help_dir, 0755);
                char help_dest[MAX_PATH_LEN];
                snprintf(help_dest, sizeof(help_dest), "%s/help-%s.txt", help_dir, lang_id);

                FILE *hs = fopen(help_src, "rb");
                FILE *hd = fopen(help_dest, "wb");
                if (hs && hd) {
                    size_t m;
                    while ((m = fread(buf, 1, sizeof(buf), hs)) > 0)
                        fwrite(buf, 1, m, hd);
                    log_info("i18n", "Help text installed to %s", help_dest);
                }
                if (hs) fclose(hs);
                if (hd) fclose(hd);
            }
        }
    }

    /* Clean up temp dir */
    rmdir_safe(tmp_dir);

    return 0;
}

/* Helper: remove extracted files from tmp_dir then remove dir */
static void rmdir_safe(const char *dir)
{
    if (!dir || !dir[0]) return;
    char p1[MAX_PATH_LEN], p2[MAX_PATH_LEN];
    snprintf(p1, sizeof(p1), "%s/lang.xml", dir);
    snprintf(p2, sizeof(p2), "%s/help.txt", dir);
    unlink(p1);
    unlink(p2);
    rmdir(dir);
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

    /* Delete XML */
    snprintf(path, sizeof(path), "%s/%s.xml", user_dir, lang_id);
    if (unlink(path) != 0) {
        log_error("i18n", "Failed to delete %s", path);
        return -1;
    }

    /* Delete associated help.txt (failure is non-fatal) */
    const char *help_dir = i18n_user_help_dir();
    if (help_dir) {
        char help_path[MAX_PATH_LEN];
        snprintf(help_path, sizeof(help_path), "%s/help-%s.txt", help_dir, lang_id);
        unlink(help_path);
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
