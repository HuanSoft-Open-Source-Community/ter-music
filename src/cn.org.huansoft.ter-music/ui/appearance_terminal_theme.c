/**
 * @file appearance_terminal_theme.c
 * @brief 终端主题适配器运行期实现（deepin-terminal）
 *
 * 设计（t22）：
 *   - 只读：config.conf、terminalwidget6/color-schemes/*.colorscheme；
 *   - 运行期低频：每 2s 对 config + 解析到的 colorscheme 做 stat(mtime,size)，
 *     变化才重新读取；绝不写文件、绝不 sleep/阻塞；
 *   - 失败（缺失/损坏/无权限）一律 unknown，由 appearance.c 回退原链。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/appearance_terminal_theme.h"

#include "logger/logger.h"

#include "config/appearance.h"
#include "ui/appearance_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define THEME_POLL_INTERVAL_MS 2000ULL
#define THEME_PATH_MAX APPEARANCE_TERMINAL_THEME_PATH_MAX
#define THEME_DATA_DIRS_MAX 8

typedef struct {
    int exists;
    long long size;
    long long mtime_ns;
    long long ino;
} ThemeFileSig;

static int g_known = 0;
static int g_bg = APPEARANCE_BG_UNKNOWN;
static int g_changed = 0;
static unsigned long long g_next_check_ms = 0;
static int g_have_signature = 0;
static ThemeFileSig g_config_sig;
static ThemeFileSig g_scheme_sig;
static char g_config_path[THEME_PATH_MAX];
static char g_scheme_path[THEME_PATH_MAX];

static unsigned long long theme_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

static ThemeFileSig theme_stat(const char *path)
{
    ThemeFileSig sig;
    memset(&sig, 0, sizeof(sig));
    if (!path || !path[0]) {
        return sig;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return sig;
    }
    sig.exists = 1;
    sig.size = (long long)st.st_size;
    sig.mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL +
                   (long long)st.st_mtim.tv_nsec;
    sig.ino = (long long)st.st_ino;
    return sig;
}

static int theme_sig_equal(ThemeFileSig a, ThemeFileSig b)
{
    return a.exists == b.exists && a.size == b.size &&
           a.mtime_ns == b.mtime_ns && a.ino == b.ino;
}

/* 解析 XDG 目录：config_dir/data_home/data_dirs（data_dirs 指向 dirs_copy）。 */
static int theme_resolve_env(char *config_dir, size_t config_size,
                             char *data_home, size_t data_home_size,
                             char *dirs_copy, size_t dirs_size,
                             const char **data_dirs, size_t *data_dir_count)
{
    const char *home = getenv("HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    const char *xdg_dirs = getenv("XDG_DATA_DIRS");

    if (xdg_config && xdg_config[0]) {
        snprintf(config_dir, config_size, "%s", xdg_config);
    } else if (home && home[0]) {
        snprintf(config_dir, config_size, "%s/.config", home);
    } else {
        return 0;
    }

    if (xdg_data && xdg_data[0]) {
        snprintf(data_home, data_home_size, "%s", xdg_data);
    } else if (home && home[0]) {
        snprintf(data_home, data_home_size, "%s/.local/share", home);
    } else {
        data_home[0] = '\0';
    }

    if (!xdg_dirs || !xdg_dirs[0]) {
        xdg_dirs = "/usr/local/share:/usr/share";
    }
    snprintf(dirs_copy, dirs_size, "%s", xdg_dirs);

    size_t count = 0;
    char *save = NULL;
    char *token = strtok_r(dirs_copy, ":", &save);
    while (token && count < THEME_DATA_DIRS_MAX) {
        if (token[0]) {
            data_dirs[count++] = token;
        }
        token = strtok_r(NULL, ":", &save);
    }
    *data_dir_count = count;
    return 1;
}

/* 完整采样：读文件、更新缓存路径与 signature；不修改 changed。 */
static int theme_sample(int *known_out)
{
    if (known_out) {
        *known_out = 0;
    }
    if (!appearance_terminal_theme_supported()) {
        g_known = 0;
        g_bg = APPEARANCE_BG_UNKNOWN;
        return APPEARANCE_BG_UNKNOWN;
    }

    char config_dir[THEME_PATH_MAX];
    char data_home[THEME_PATH_MAX];
    char dirs_copy[THEME_PATH_MAX];
    const char *data_dirs[THEME_DATA_DIRS_MAX];
    size_t data_dir_count = 0;
    if (!theme_resolve_env(config_dir, sizeof(config_dir),
                           data_home, sizeof(data_home),
                           dirs_copy, sizeof(dirs_copy),
                           data_dirs, &data_dir_count)) {
        g_known = 0;
        g_bg = APPEARANCE_BG_UNKNOWN;
        return APPEARANCE_BG_UNKNOWN;
    }

    int bg = APPEARANCE_BG_UNKNOWN;
    int ok = appearance_deepin_theme_bg(config_dir, data_home, data_dirs,
                                        data_dir_count,
                                        g_config_path, sizeof(g_config_path),
                                        g_scheme_path, sizeof(g_scheme_path),
                                        &bg);
    g_config_sig = theme_stat(g_config_path);
    g_scheme_sig = theme_stat(g_scheme_path);
    g_have_signature = 1;
    g_next_check_ms = theme_now_ms() + THEME_POLL_INTERVAL_MS;

    log_debug("appearance", "theme sample: config=%s scheme=%s ok=%d bg=%d",
              g_config_path, g_scheme_path, ok, bg);
    g_known = ok ? 1 : 0;
    g_bg = ok ? bg : APPEARANCE_BG_UNKNOWN;
    if (known_out) {
        *known_out = g_known;
    }
    return g_bg;
}

int appearance_terminal_theme_supported(void)
{
    return appearance_terminal_is_deepin();
}

int appearance_terminal_theme_probe(int *bg_out)
{
    int known = 0;
    int bg = theme_sample(&known);
    g_changed = 0;
    if (bg_out) {
        *bg_out = known ? bg : APPEARANCE_BG_UNKNOWN;
    }
    return known;
}

void appearance_terminal_theme_tick(void)
{
    if (!appearance_terminal_theme_supported()) {
        return;
    }
    unsigned long long now = theme_now_ms();
    if (now < g_next_check_ms) {
        return;
    }
    g_next_check_ms = now + THEME_POLL_INTERVAL_MS;

    if (!g_have_signature) {
        int old_known = g_known;
        int old_bg = g_bg;
        (void)theme_sample(NULL);
        if (g_known != old_known || g_bg != old_bg) {
            g_changed = 1;
        }
        return;
    }

    ThemeFileSig config_sig = theme_stat(g_config_path);
    ThemeFileSig scheme_sig = theme_stat(g_scheme_path);
    if (theme_sig_equal(config_sig, g_config_sig) &&
        theme_sig_equal(scheme_sig, g_scheme_sig)) {
        return;   /* 仅 stat，无文件读取 */
    }

    int old_known = g_known;
    int old_bg = g_bg;
    (void)theme_sample(NULL);
    if (g_known != old_known || g_bg != old_bg) {
        g_changed = 1;
        log_debug("appearance", "Terminal theme changed: known=%d bg=%d",
                  g_known, g_bg);
    }
}

int appearance_terminal_theme_take_changed(int *bg_out)
{
    if (!g_changed) {
        return 0;
    }
    g_changed = 0;
    if (bg_out) {
        *bg_out = g_known ? g_bg : APPEARANCE_BG_UNKNOWN;
    }
    return 1;
}
