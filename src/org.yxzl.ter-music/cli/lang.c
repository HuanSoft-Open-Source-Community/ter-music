/**
 * @file lang.c
 * @brief CLI 语言选项解析、只读配置读取与 i18n 初始化
 *
 * 语言优先级（与已确认规格一致）：
 *   --lang > TER_MUSIC_CLI_LANG > 配置 ui_language > LC_ALL > LC_MESSAGES > LANG > zh_CN
 *
 * 配置读取严格只读：仅 ensure_config_dir_exists() + stat() +
 * config_load_from_xml()，不调用 save_config() / config_run_migrations()。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/lang.h"

#include "config/config.h"
#include "i18n/i18n.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --lang / TER_MUSIC_CLI_LANG / locale 语言 ID 的长度上限 */
#define CLI_LANG_MAX_LEN 64

/* --lang 提供的非空值；g_lang_opt_set=0 表示未设置（空值/超长被忽略） */
static char g_lang_opt[CLI_LANG_MAX_LEN + 1];
static int  g_lang_opt_set = 0;

/* 最近一次解析结果；cli_lang_resolve()/cli_lang_init() 的返回值指向此处 */
static char g_lang_resolved[CLI_LANG_MAX_LEN + 1];

/* ── 归一化 ────────────────────────────────────────────────────── */

static int cli_lang_char_valid(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

const char *cli_lang_normalize(const char *raw, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return NULL;

    out[0] = '\0';
    if (!raw || raw[0] == '\0')
        return out;

    size_t raw_len = strlen(raw);
    if (raw_len > CLI_LANG_MAX_LEN) {
        snprintf(out, out_size, "zh_CN");
        return out;
    }

    char buf[CLI_LANG_MAX_LEN + 1];
    memcpy(buf, raw, raw_len + 1);

    /* 去掉 @修饰 与 .编码：en_US.UTF-8@mod → en_US */
    char *cut = strchr(buf, '@');
    if (cut)
        *cut = '\0';
    cut = strchr(buf, '.');
    if (cut)
        *cut = '\0';

    if (buf[0] == '\0' ||
        strcmp(buf, "C") == 0 || strcmp(buf, "POSIX") == 0)
        return out;   /* 无偏好 */

    for (char *p = buf; *p; p++) {
        if (*p == '-') {
            *p = '_';
        } else if (!cli_lang_char_valid((unsigned char)*p)) {
            snprintf(out, out_size, "zh_CN");
            return out;
        }
    }

    if (strncasecmp(buf, "zh", 2) == 0)
        snprintf(out, out_size, "zh_CN");
    else if (strncasecmp(buf, "en", 2) == 0)
        snprintf(out, out_size, "en_US");
    else
        snprintf(out, out_size, "%s", buf);

    return out;
}

/* ── --lang 预扫描 ─────────────────────────────────────────────── */

static void cli_lang_store_option(const char *value)
{
    if (!value)
        return;

    size_t len = strlen(value);
    if (len == 0 || len > CLI_LANG_MAX_LEN)
        return;   /* 空值/超长：忽略，保持未设置 */

    memcpy(g_lang_opt, value, len + 1);
    g_lang_opt_set = 1;
}

void cli_lang_extract_option(int *argc, char **argv)
{
    if (!argc || !argv || *argc <= 0)
        return;

    /* 与 cli_extract_global_options() 的 --bus 处理保持一致：
     * 原地压缩 argv（保持其余参数顺序），末尾补 NULL。 */
    int write_index = 1;
    for (int read_index = 1; read_index < *argc; read_index++) {
        const char *arg = argv[read_index];
        if (!arg)
            continue;

        if (strcmp(arg, "--lang") == 0) {
            if (read_index + 1 < *argc)
                cli_lang_store_option(argv[++read_index]);
            continue;
        }
        if (strncmp(arg, "--lang=", 7) == 0) {
            cli_lang_store_option(arg + 7);
            continue;
        }

        argv[write_index++] = argv[read_index];
    }

    *argc = write_index;
    argv[write_index] = NULL;
}

/* ── 显式语言 ──────────────────────────────────────────────────── */

int cli_lang_is_explicit(void)
{
    if (g_lang_opt_set)
        return 1;

    const char *env = getenv("TER_MUSIC_CLI_LANG");
    return (env && env[0]) ? 1 : 0;
}

/* 取出优先级最高的显式语言（已归一化）。无显式偏好/显式值无偏好时返回 0。 */
static int cli_lang_explicit_value(char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;

    out[0] = '\0';

    if (g_lang_opt_set &&
        cli_lang_normalize(g_lang_opt, out, out_size)[0])
        return 1;

    const char *env = getenv("TER_MUSIC_CLI_LANG");
    if (env && env[0] &&
        cli_lang_normalize(env, out, out_size)[0])
        return 1;

    out[0] = '\0';
    return 0;
}

/* ── 只读配置 ──────────────────────────────────────────────────── */

/* 读取配置中的 ui_language。文件不存在/解析失败 → out 为空串，
 * 以便继续按 locale 环境变量回退。绝不触发迁移或写盘。 */
static void cli_lang_config_value(char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;

    out[0] = '\0';

    ensure_config_dir_exists();
    const char *path = get_config_file_path();
    if (!path || !path[0])
        return;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;

    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config_load_from_xml(path, &cfg) != 0)
        return;

    if (!cfg.ui_language[0])
        return;

    snprintf(out, out_size, "%s", cfg.ui_language);
}

/* ── 解析 / 初始化 ─────────────────────────────────────────────── */

const char *cli_lang_resolve(void)
{
    char norm[CLI_LANG_MAX_LEN + 1];

    /* 1. --lang / TER_MUSIC_CLI_LANG（显式优先） */
    if (cli_lang_explicit_value(norm, sizeof(norm))) {
        memcpy(g_lang_resolved, norm, strlen(norm) + 1);
        return g_lang_resolved;
    }

    /* 2. 配置 ui_language（只读） */
    char config_lang[CLI_LANG_MAX_LEN + 1];
    cli_lang_config_value(config_lang, sizeof(config_lang));
    if (config_lang[0] &&
        cli_lang_normalize(config_lang, norm, sizeof(norm))[0]) {
        memcpy(g_lang_resolved, norm, strlen(norm) + 1);
        return g_lang_resolved;
    }

    /* 3. locale 环境变量：LC_ALL → LC_MESSAGES → LANG */
    static const char *const locale_vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (size_t i = 0; i < sizeof(locale_vars) / sizeof(locale_vars[0]); i++) {
        const char *env = getenv(locale_vars[i]);
        if (!env || !env[0])
            continue;
        if (cli_lang_normalize(env, norm, sizeof(norm))[0]) {
            memcpy(g_lang_resolved, norm, strlen(norm) + 1);
            return g_lang_resolved;
        }
    }

    /* 4. 最终默认值 */
    strcpy(g_lang_resolved, "zh_CN");
    return g_lang_resolved;
}

/* 复用 i18n 语言发现逻辑（不拼接 i18n 私有路径）。
 * i18n_available_languages() 只填充调用方数组，不分配内存。 */
static int cli_lang_installed(const char *lang_id)
{
    I18nLanguage langs[I18N_MAX_LANGS];
    int count = i18n_available_languages(langs, I18N_MAX_LANGS);

    for (int i = 0; i < count; i++) {
        if (strcmp(langs[i].id, lang_id) == 0)
            return 1;
    }
    return 0;
}

const char *cli_lang_init(void)
{
    const char *lang = cli_lang_resolve();
    char explicit_norm[CLI_LANG_MAX_LEN + 1];

    if (cli_lang_explicit_value(explicit_norm, sizeof(explicit_norm)) &&
        !cli_lang_installed(explicit_norm)) {
        fprintf(stderr,
                "警告：语言包 '%s' 未安装，已回退到 zh_CN"
                " (language pack not installed, falling back to zh_CN)\n",
                explicit_norm);
        lang = "zh_CN";
    }

    i18n_init(lang);

    /* lang 可能指向字符串字面量；仅在内容不同时写回静态缓冲。 */
    if (strcmp(g_lang_resolved, lang) != 0)
        snprintf(g_lang_resolved, sizeof(g_lang_resolved), "%s", lang);

    return g_lang_resolved;
}
