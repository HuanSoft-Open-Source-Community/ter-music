/**
 * @file i18n.h
 * @brief Internationalization (i18n) engine for ter-music
 *
 * Loads XML translation files at startup and provides key-based
 * string lookups.  Uses libxml2 (already a project dependency).
 *
 * @author ter-music team
 * @date 2026-07
 */

#ifndef I18N_H
#define I18N_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ─────────────────────────────────────────────────── */

/**
 * Initialize the i18n system and load the requested language pack.
 * If @p lang_id is NULL or the file is missing, falls back to "zh_CN";
 * if that also fails, every i18n_get() call returns the key verbatim.
 *
 * @param lang_id  language identifier, e.g. "zh_CN", "en_US".
 * @return 0 on success, -1 if no translation file could be loaded.
 */
int i18n_init(const char *lang_id);

/**
 * Reload a different language at runtime.  Frees the old table
 * and loads the new XML file.
 *
 * @param lang_id  new language identifier.
 * @return 0 on success, -1 on failure (old translations remain).
 */
int i18n_reload(const char *lang_id);

/**
 * Free all translation resources.
 */
void i18n_cleanup(void);

/* ── Lookup ────────────────────────────────────────────────────── */

/**
 * Return the translated string for @p key.
 * Fallback chain: current language → "zh_CN" → raw key.
 * The returned pointer is valid until the next i18n_reload()
 * or i18n_cleanup() call.
 */
const char *i18n_get(const char *key);

/* ── Available languages ───────────────────────────────────────── */

#define I18N_MAX_LANGS 32

typedef struct {
    char id[32];        /* e.g. "zh_CN" */
    char name[64];      /* e.g. "中文（简体）" */
    int  builtin;       /* 1 = system-installed, 0 = user-added */
} I18nLanguage;

/**
 * Discover available language packs by scanning system and
 * user ( ~/.config/ter-music/lang/ ) directories.
 * Returns the count and fills @p langs (up to @p max entries).
 */
int i18n_available_languages(I18nLanguage *langs, int max);

/**
 * Return the language ID that is currently loaded (e.g. "zh_CN").
 */
const char *i18n_current_lang(void);

/* ── Coverage ────────────────────────────────────────────────── */

/**
 * Return the translation coverage of @p lang_id as a percentage
 * (0.0–100.0), measured against the zh_CN reference key set.
 * Returns -1.0 if the language file cannot be parsed.
 */
double i18n_coverage(const char *lang_id);

/* ── Language pack management (user dir) ─────────────────────── */

/**
 * Check whether @p lang_id is a built-in language (zh_CN, en_US).
 */
int i18n_is_builtin(const char *lang_id);

/**
 * Copy an external .xml translation file into the user
 * language directory (~/.config/ter-music/lang/<id>.xml).
 * Validates the XML structure before copying.
 * @return 0 on success, -1 on error.
 */
int i18n_add_language(const char *source_path);

/**
 * Delete a user-installed language pack.
 * Fails with -1 for built-in languages (zh_CN / en_US).
 * @return 0 on success, -1 on error.
 */
int i18n_delete_language(const char *lang_id);

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */
