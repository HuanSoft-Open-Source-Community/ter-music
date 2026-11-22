/**
 * @file test_appearance.c
 * @brief 终端亮/暗自动适配的纯逻辑单测
 *
 * 覆盖：
 *   - OSC 11 应答解析（各 rgb/# 形态、半包、非应答、垃圾前缀；
 *   - COLORFGBG 回退解析；
 *   - WCAG 亮度分类与模式→变体解析；
 *   - 主题出厂/亮色表、自适应槽集合与“显式自定义不被改写”规则；
 *   - 7 个内置预设的亮/暗配色：互异性与 8/16/256 量化结果、对纯白对比度；
 *   - 仓库 data/layouts/layouts.json 的暗色值防漂移检查。
 *
 * 运行：scripts/test/run-unit-tests.sh test_appearance
 * 依赖（见同名 .srcs/.env）：
 *   src/.../config/appearance.c、src/.../ui/layout_spec.c（-lm 用于亮度）
 */

#include "config/appearance.h"
#include "ui/layout_spec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

/* ── OSC 11 解析 ──────────────────────────────────────────────────── */

static void check_scan(const char *buf, int expected, size_t expected_consumed,
                       int expected_r, int expected_g, int expected_b,
                       const char *what)
{
    size_t consumed = 999;
    int r = -1, g = -1, b = -1;
    int rc = appearance_osc11_scan(buf, strlen(buf), &consumed, &r, &g, &b);
    g_checks++;
    if (rc != expected || consumed != expected_consumed ||
        (expected == 1 && (r != expected_r || g != expected_g || b != expected_b))) {
        g_failures++;
        printf("  FAIL %s: rc=%d consumed=%zu rgb=%d,%d,%d\n",
               what, rc, consumed, r, g, b);
    }
}

static void test_osc11_scan(void)
{
    /* xterm 风格 4 位分量，ST / BEL 两种终止符 */
    check_scan("\x1b]11;rgb:ffff/ffff/ffff\x1b\\", 1, 25, 255, 255, 255,
               "4-digit rgb + ST");
    check_scan("\x1b]11;rgb:ffff/ffff/ffff\x07", 1, 24, 255, 255, 255,
               "4-digit rgb + BEL");
    /* 2 位 / 1 位分量 */
    check_scan("\x1b]11;rgb:e6/e6/e6\x07", 1, 18, 230, 230, 230,
               "2-digit rgb components");
    check_scan("\x1b]11;rgb:f/f/f\x07", 1, 15, 255, 255, 255,
               "1-digit rgb components");
    /* #RRGGBB / #RGB */
    check_scan("\x1b]11;#e6e6e6\x07", 1, 13, 230, 230, 230, "#RRGGBB");
    check_scan("\x1b]11;#fff\x07", 1, 10, 255, 255, 255, "#RGB");
    /* 半包：合法前缀应返回 0 */
    check_scan("\x1b]11;rgb:ff", 0, 0, 0, 0, 0, "partial payload");
    check_scan("\x1b]11;", 0, 0, 0, 0, 0, "prefix without payload");
    /* 非应答：只消费首字节 */
    check_scan("\x1b[A", -1, 1, 0, 0, 0, "arrow key is not an OSC reply");
    check_scan("\x1b]0;title\x07", -1, 1, 0, 0, 0, "window title is not a reply");
    check_scan("rgb:", -1, 1, 0, 0, 0, "bare rgq text is not a reply");
    check_scan("j", -1, 1, 0, 0, 0, "ordinary key is not a reply");
    /* 匹配了前缀但内容非法：整段吞掉（consumed 指向终止符之后） */
    check_scan("\x1b]11;rgba:1/2/3/4\x07", -1, 18, 0, 0, 0,
               "malformed rgba reply is consumed whole");
    check_scan("\x1b]11;rgb:zz/zz/zz\x07", -1, 18, 0, 0, 0,
               "non-hex reply is consumed whole");

    /* 垃圾 + 完整应答 + 尾随字节：逐字节跳过前缀后应精确定位终止符 */
    {
        const char *buf = "junk!\x1b]11;#fff\x07tail";
        size_t offset = 0;
        int rc = -1;
        size_t consumed = 0;
        int r = 0, g = 0, b = 0;
        while (offset < strlen(buf)) {
            rc = appearance_osc11_scan(buf + offset, strlen(buf) - offset,
                                       &consumed, &r, &g, &b);
            if (rc == 1) {
                break;
            }
            if (rc == 0) {
                offset++;   /* 合法前缀：等更多字节（测试里等价于跳过） */
            } else {
                offset += consumed > 0 ? consumed : 1;
            }
        }
        CHECK(rc == 1, "reply found after junk");
        CHECK(offset == 5, "junk prefix skipped one byte at a time");
        CHECK(consumed == 10, "consumed ends exactly after BEL");
        CHECK(r == 255 && g == 255 && b == 255, "junk-prefixed reply rgb");
    }
}

/* ── XDG color-scheme 映射与优先级裁决（t16 A） ───────────────────── */

static void test_xdg_and_resolve(void)
{
    CHECK(appearance_xdg_color_scheme_to_bg(0) == APPEARANCE_BG_UNKNOWN,
          "xdg 0 = no preference");
    CHECK(appearance_xdg_color_scheme_to_bg(1) == APPEARANCE_BG_DARK,
          "xdg 1 = prefer dark");
    CHECK(appearance_xdg_color_scheme_to_bg(2) == APPEARANCE_BG_LIGHT,
          "xdg 2 = prefer light");
    CHECK(appearance_xdg_color_scheme_to_bg(3) == APPEARANCE_BG_UNKNOWN,
          "xdg unknown value");

    AppearanceSignals s;
    int source = -1;
    /* 显式模式永远赢。 */
    memset(&s, 0, sizeof(s));
    s.osc11 = APPEARANCE_BG_DARK;
    s.xdg = APPEARANCE_BG_DARK;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_LIGHT, &s, &source) ==
          APPEARANCE_VARIANT_LIGHT, "explicit light wins");
    CHECK(source == APPEARANCE_SOURCE_EXPLICIT, "explicit source");
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_DARK, &s, &source) ==
          APPEARANCE_VARIANT_DARK, "explicit dark wins");
    CHECK(source == APPEARANCE_SOURCE_EXPLICIT, "explicit dark source");

    /* auto 优先级：OSC11 > XDG > COLORFGBG > TERM。 */
    s.osc11 = APPEARANCE_BG_LIGHT;
    s.xdg = APPEARANCE_BG_DARK;
    s.colorfgbg = APPEARANCE_BG_DARK;
    s.term = APPEARANCE_BG_DARK;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_LIGHT, "osc11 beats xdg");
    CHECK(source == APPEARANCE_SOURCE_OSC11, "osc11 source");

    /* 终端主题适配器：OSC11 之后、XDG 之前。 */
    s.osc11 = APPEARANCE_BG_UNKNOWN;
    s.terminal_theme = APPEARANCE_BG_LIGHT;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_LIGHT, "terminal theme after osc11");
    CHECK(source == APPEARANCE_SOURCE_TERMINAL_THEME, "terminal theme source");
    s.osc11 = APPEARANCE_BG_DARK;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_DARK, "osc11 beats terminal theme");
    CHECK(source == APPEARANCE_SOURCE_OSC11, "osc11 sticky source");
    s.osc11 = APPEARANCE_BG_UNKNOWN;
    s.terminal_theme = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_DARK, "xdg next after osc11");
    CHECK(source == APPEARANCE_SOURCE_XDG, "xdg source");

    s.xdg = APPEARANCE_BG_UNKNOWN;
    s.colorfgbg = APPEARANCE_BG_LIGHT;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_LIGHT, "colorfgbg next");
    CHECK(source == APPEARANCE_SOURCE_COLORFGBG, "colorfgbg source");

    s.colorfgbg = APPEARANCE_BG_UNKNOWN;
    s.term = APPEARANCE_BG_LIGHT;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_LIGHT, "term last");
    CHECK(source == APPEARANCE_SOURCE_TERM, "term source");

    s.term = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, &source) ==
          APPEARANCE_VARIANT_DARK, "all unknown => dark");
    CHECK(source == APPEARANCE_SOURCE_FALLBACK, "fallback source");
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, &s, NULL) ==
          APPEARANCE_VARIANT_DARK, "NULL source_out allowed");
    CHECK(appearance_resolve_signals(APPEARANCE_MODE_AUTO, NULL, &source) ==
          APPEARANCE_VARIANT_DARK && source == APPEARANCE_SOURCE_FALLBACK,
          "NULL signals safe");

    s.osc11 = APPEARANCE_BG_LIGHT;
    CHECK(appearance_signal_osc11_known(&s) == 1, "osc11 known");
    s.osc11 = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_signal_osc11_known(&s) == 0, "osc11 unknown");
    CHECK(appearance_signal_osc11_known(NULL) == 0, "osc11 NULL safe");
}

/* ── 终端主题适配器（t22：deepin-terminal） ───────────────────────── */

static int theme_write_file(const char *path, const char *text)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        for (char *p = dir + 1; *p; p++) {
            if (*p != '/') {
                continue;
            }
            *p = '\0';
            (void)mkdir(dir, 0700);
            *p = '/';
        }
        (void)mkdir(dir, 0700);
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return -1;
    }
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, file) == len;
    fclose(file);
    return ok ? 0 : -1;
}

static void test_deepin_theme_adapter(void)
{
    /* INI 解析 */
    const char *ini =
        "[basic.interface.theme]\nvalue=Light\n"
        "[basic.interface.expand_theme]\nvalue=\"customTheme\"\n"
        "[other]\nvalue=Dark\n";
    char value[64];
    CHECK(appearance_ini_get_value(ini, strlen(ini), "basic.interface.theme",
                                   "value", value, sizeof(value)) == 1 &&
              strcmp(value, "Light") == 0, "ini parses basic theme");
    CHECK(appearance_ini_get_value(ini, strlen(ini),
                                   "basic.interface.expand_theme", "value",
                                   value, sizeof(value)) == 1 &&
              strcmp(value, "customTheme") == 0, "ini strips quotes");
    CHECK(appearance_ini_get_value(ini, strlen(ini), "other", "value",
                                   value, sizeof(value)) == 1 &&
              strcmp(value, "Dark") == 0, "ini follows section");
    CHECK(appearance_ini_get_value(ini, strlen(ini), "basic.interface.theme",
                                   "missing", value, sizeof(value)) == 0,
          "ini missing key");
    CHECK(appearance_ini_get_value("no section\nvalue=1\n", 20,
                                   "basic.interface.theme", "value",
                                   value, sizeof(value)) == 0,
          "ini outside section ignored");
    /* 空 value 也要能返回（expand_theme 为空是合法情形） */
    const char *empty_value = "[basic.interface.expand_theme]\nvalue=\n";
    CHECK(appearance_ini_get_value(empty_value, strlen(empty_value),
                                   "basic.interface.expand_theme", "value",
                                   value, sizeof(value)) == 1 &&
              value[0] == '\0', "ini empty value is found");

    /* colorscheme 解析 */
    int r = 0, g = 0, b = 0;
    const char *scheme_light = "[Background]\nColor=248, 248, 248\n";
    CHECK(appearance_colorscheme_background(scheme_light, strlen(scheme_light),
                                            &r, &g, &b) == 1 &&
              r == 248 && g == 248 && b == 248, "colorscheme light rgb");
    const char *scheme_dark = "[Background]\nColor=37,37,37\n";
    CHECK(appearance_colorscheme_background(scheme_dark, strlen(scheme_dark),
                                            &r, &g, &b) == 1 &&
              r == 37 && g == 37 && b == 37, "colorscheme dark rgb");
    const char *scheme_alpha = "[Background]\nColor=1,2,3,4\n";
    CHECK(appearance_colorscheme_background(scheme_alpha, strlen(scheme_alpha),
                                            &r, &g, &b) == 0,
          "colorscheme rejects alpha/4th component");
    const char *scheme_bad = "[Background]\nColor=248, 248\n";
    CHECK(appearance_colorscheme_background(scheme_bad, strlen(scheme_bad),
                                            &r, &g, &b) == 0,
          "colorscheme rejects missing component");
    CHECK(appearance_colorscheme_background("[Background]\nColor=300,0,0\n",
                                            34, &r, &g, &b) == 0,
          "colorscheme rejects out-of-range");

    /* 文件/目录解析（临时根） */
    char root[] = "/tmp/tm-deepin-theme-XXXXXX";
    char *tmp = mkdtemp(root);
    CHECK(tmp != NULL, "mkdtemp for deepin adapter");
    if (!tmp) {
        return;
    }
    char config_dir[512], data_home[512], data_dir[512];
    char config_path[1024], scheme_light_path[1024], scheme_dir[1024];
    snprintf(config_dir, sizeof(config_dir), "%s/config", root);
    snprintf(data_home, sizeof(data_home), "%s/data", root);
    snprintf(data_dir, sizeof(data_dir), "%s/extra", root);
    snprintf(scheme_dir, sizeof(scheme_dir),
             "%s/terminalwidget6/color-schemes", data_home);
    (void)mkdir(scheme_dir, 0700);
    snprintf(config_path, sizeof(config_path),
             "%s/deepin/deepin-terminal/config.conf", config_dir);
    snprintf(scheme_light_path, sizeof(scheme_light_path),
             "%s/Light.colorscheme", scheme_dir);
    CHECK(theme_write_file(scheme_light_path, scheme_light) == 0,
          "write Light colorscheme");
    char scheme_dark_path[1024];
    snprintf(scheme_dark_path, sizeof(scheme_dark_path),
             "%s/Dark.colorscheme", scheme_dir);
    CHECK(theme_write_file(scheme_dark_path, scheme_dark) == 0,
          "write Dark colorscheme");
    const char *light_config =
        "[basic.interface.theme]\nvalue=Light\n"
        "[basic.interface.expand_theme]\nvalue=\n";
    CHECK(theme_write_file(config_path, light_config) == 0,
          "write deepin config Light");
    char out_config[1024], out_scheme[1024];
    int bg = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     out_config, sizeof(out_config),
                                     out_scheme, sizeof(out_scheme), &bg) == 1 &&
              bg == APPEARANCE_BG_LIGHT, "deepin basic Light => light");
    CHECK(strstr(out_scheme, "Light.colorscheme") != NULL,
          "resolved Light colorscheme path");

    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Dark\n"
        "[basic.interface.expand_theme]\nvalue=\n") == 0,
          "write deepin config Dark");
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, NULL, 0, &bg) == 1 &&
              bg == APPEARANCE_BG_DARK, "deepin basic Dark => dark");

    /* expand_theme 按名解析 data_dirs */
    char named_dir[1024], named_path[1024];
    snprintf(named_dir, sizeof(named_dir),
             "%s/terminalwidget6/color-schemes", data_dir);
    (void)mkdir(named_dir, 0700);
    snprintf(named_path, sizeof(named_path), "%s/Solarized.colorscheme", named_dir);
    CHECK(theme_write_file(named_path, "[Background]\nColor=250,250,250\n") == 0,
          "write named colorscheme in data dir");
    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Dark\n"
        "[basic.interface.expand_theme]\nvalue=Solarized\n") == 0,
          "write expand_theme config");
    const char *dirs[1] = {data_dir};
    bg = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, dirs, 1,
                                     NULL, 0, out_scheme, sizeof(out_scheme),
                                     &bg) == 1 && bg == APPEARANCE_BG_LIGHT,
          "expand_theme named resolves data dir => light");
    CHECK(strstr(out_scheme, "Solarized.colorscheme") != NULL,
          "named scheme path resolved");

    /* 主题名带空格也应能解析（用户自定义命名常见） */
    char space_dir[1024], space_path[1024];
    snprintf(space_dir, sizeof(space_dir),
             "%s/terminalwidget6/color-schemes", data_home);
    snprintf(space_path, sizeof(space_path), "%s/My Theme.colorscheme", space_dir);
    CHECK(theme_write_file(space_path, "[Background]\nColor=252,252,252\n") == 0,
          "write scheme with space in name");
    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Dark\n"
        "[basic.interface.expand_theme]\nvalue=My Theme\n") == 0,
          "write expand_theme with space");
    bg = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, out_scheme, sizeof(out_scheme),
                                     &bg) == 1 && bg == APPEARANCE_BG_LIGHT,
          "expand_theme with space resolves");
    CHECK(strstr(out_scheme, "My Theme.colorscheme") != NULL,
          "space name scheme path resolved");
    (void)remove(space_path);

    /* customTheme 走配置目录 */
    char custom_path[1024];
    snprintf(custom_path, sizeof(custom_path),
             "%s/deepin/deepin-terminal/customTheme.colorscheme", config_dir);
    CHECK(theme_write_file(custom_path, "[Background]\nColor=37,37,37\n") == 0,
          "write customTheme colorscheme");
    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Light\n"
        "[basic.interface.expand_theme]\nvalue=customTheme\n") == 0,
          "write customTheme config");
    bg = APPEARANCE_BG_UNKNOWN;
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, out_scheme, sizeof(out_scheme),
                                     &bg) == 1 && bg == APPEARANCE_BG_DARK,
          "customTheme resolves config-dir scheme => dark");

    /* 缺失/损坏/路径穿越 */
    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Dark\n"
        "[basic.interface.expand_theme]\nvalue=missing-theme\n") == 0,
          "write missing expand theme");
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, out_scheme, sizeof(out_scheme),
                                     &bg) == 0 && bg == APPEARANCE_BG_UNKNOWN,
          "missing scheme => unknown (candidate path kept)");
    CHECK(out_scheme[0] != '\0', "missing scheme candidate path is available for stat");

    CHECK(theme_write_file(config_path,
        "[basic.interface.theme]\nvalue=Light\n"
        "[basic.interface.expand_theme]\nvalue=../escape\n") == 0,
          "write traversal theme");
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, NULL, 0, &bg) == 0,
          "path traversal rejected => unknown");

    CHECK(theme_write_file(scheme_light_path, "garbage without Background\n") == 0,
          "corrupt Light colorscheme");
    CHECK(theme_write_file(config_path, light_config) == 0,
          "restore Light config");
    CHECK(appearance_deepin_theme_bg(config_dir, data_home, NULL, 0,
                                     NULL, 0, NULL, 0, &bg) == 0 &&
              bg == APPEARANCE_BG_UNKNOWN, "corrupt colorscheme => unknown");

    char missing_dir[512];
    snprintf(missing_dir, sizeof(missing_dir), "%s/no-such-config", root);
    CHECK(appearance_deepin_theme_bg(missing_dir, data_home, NULL, 0,
                                     out_config, sizeof(out_config),
                                     NULL, 0, &bg) == 0 &&
              bg == APPEARANCE_BG_UNKNOWN, "missing config => unknown");
    CHECK(out_config[0] != '\0', "missing config path reported");

    /* 身份判定 */
    AppearanceTerminalEnv env;
    memset(&env, 0, sizeof(env));
    env.term_program = "deepin-terminal";
    CHECK(appearance_terminal_is_deepin_env(&env, NULL, 0) == 1,
          "TERM_PROGRAM deepin-terminal => adapter");
    /* TERM_PROGRAM 非空时是权威：即使祖先链含 deepin-terminal 也不启用。 */
    memset(&env, 0, sizeof(env));
    env.term_program = "vscode";
    const char *anc_deepin[] = {"bash", "deepin-terminal"};
    CHECK(appearance_terminal_is_deepin_env(&env, anc_deepin, 2) == 0,
          "TERM_PROGRAM=vscode beats deepin ancestor");
    env.term_program = "kitty";
    CHECK(appearance_terminal_is_deepin_env(&env, anc_deepin, 2) == 0,
          "TERM_PROGRAM=kitty beats deepin ancestor");
    env.term_program = "xterm";
    CHECK(appearance_terminal_is_deepin_env(&env, anc_deepin, 2) == 0,
          "TERM_PROGRAM=xterm beats deepin ancestor");
    /* 仅当 TERM_PROGRAM 为空/未设置时才使用祖先链（deepin 自身/tmux 等）。 */
    memset(&env, 0, sizeof(env));
    env.term_program = "";
    CHECK(appearance_terminal_is_deepin_env(&env, anc_deepin, 2) == 1,
          "empty TERM_PROGRAM falls back to deepin ancestor");
    memset(&env, 0, sizeof(env));
    CHECK(appearance_terminal_is_deepin_env(&env, anc_deepin, 2) == 1,
          "unset TERM_PROGRAM falls back to deepin ancestor");
    const char *anc_other[] = {"bash", "xterm"};
    CHECK(appearance_terminal_is_deepin_env(&env, anc_other, 2) == 0,
          "other ancestors => no adapter");
    memset(&env, 0, sizeof(env));
    CHECK(appearance_terminal_is_deepin_env(&env, NULL, 0) == 0,
          "no identity => no adapter");
    memset(&env, 0, sizeof(env));
    env.term_program = "Deepin-Terminal";
    CHECK(appearance_terminal_is_deepin_env(&env, NULL, 0) == 1,
          "TERM_PROGRAM case-insensitive");
}

/* ── XDG portal 信号匹配（t18 F3：NULL 安全） ─────────────────────── */

static void test_portal_signal_match(void)
{
    const char *iface = "org.freedesktop.portal.Settings";
    CHECK(appearance_portal_signal_matches(iface, "SettingChanged", 1) == 1,
          "portal signal matches");
    CHECK(appearance_portal_signal_matches(NULL, "SettingChanged", 1) == 0,
          "NULL interface is not a crash/match");
    CHECK(appearance_portal_signal_matches(iface, NULL, 1) == 0,
          "NULL member is not a crash/match");
    CHECK(appearance_portal_signal_matches(iface, "SettingChanged", 0) == 0,
          "non-signal message rejected");
    CHECK(appearance_portal_signal_matches("org.example.Other", "SettingChanged", 1) == 0,
          "wrong interface rejected");
    CHECK(appearance_portal_signal_matches(iface, "OtherMember", 1) == 0,
          "wrong member rejected");
    CHECK(appearance_portal_signal_matches(NULL, NULL, 0) == 0,
          "all-NULL rejected");
}

/* ── 终端身份键（t16 D1） ─────────────────────────────────────────── */

static void test_terminal_identity_key(void)
{
    char key[160];
    AppearanceTerminalEnv env;
    memset(&env, 0, sizeof(env));

    env.term_program = "iTerm.app";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1,
          "term_program key exists");
    CHECK(strcmp(key, "tp:iterm.app") == 0, "term_program lowercased");

    memset(&env, 0, sizeof(env));
    env.term = "xterm-256color";
    const char *anc[] = {"bash", "deepin-terminal"};
    CHECK(appearance_terminal_key_build(&env, anc, 2, key, sizeof(key)) == 1,
          "ancestor terminal key");
    CHECK(strcmp(key, "anc:deepin-terminal") == 0, "ancestor allowlist match");

    /* 最外层优先：即使内层也像终端名（构造），取靠外的。 */
    const char *anc2[] = {"xterm", "bash", "deepin-terminal"};
    CHECK(appearance_terminal_key_build(&env, anc2, 3, key, sizeof(key)) == 1 &&
          strcmp(key, "anc:deepin-terminal") == 0,
          "outermost terminal ancestor wins");

    /* gnome-terminal comm 截断仍命中。 */
    const char *anc3[] = {"gnome-terminal-"};
    env.term = "xterm";
    CHECK(appearance_terminal_key_build(&env, anc3, 1, key, sizeof(key)) == 1 &&
          strcmp(key, "anc:gnome-terminal-") == 0,
          "truncated comm allowlist match");

    env.tmux = "1";
    env.term = "screen-256color";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1 &&
          strcmp(key, "tmux:screen-256color") == 0, "tmux fallback key");

    memset(&env, 0, sizeof(env));
    env.sty = "1";
    env.term = "screen";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1 &&
          strcmp(key, "screen:screen") == 0, "screen fallback key");

    memset(&env, 0, sizeof(env));
    env.ssh_tty = "/dev/pts/1";
    env.term = "xterm-256color";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1 &&
          strcmp(key, "ssh:xterm-256color") == 0, "ssh fallback key");

    memset(&env, 0, sizeof(env));
    env.term = "XTerm 256 Color";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1 &&
          strcmp(key, "term:xterm_256_color") == 0, "sanitized TERM key");

    memset(&env, 0, sizeof(env));
    env.colorterm = "truecolor";
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1 &&
          strcmp(key, "ct:truecolor") == 0, "colorterm fallback key");

    memset(&env, 0, sizeof(env));
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 0,
          "no identity returns 0");
    CHECK(strcmp(key, "default") == 0, "no identity => default");

    /* 超长截断到 96 字节；非法字符统一替换为 '_'。 */
    char long_term[200];
    memset(long_term, 'A', sizeof(long_term) - 1);
    long_term[sizeof(long_term) - 1] = '\\0';
    env.term = long_term;
    CHECK(appearance_terminal_key_build(&env, NULL, 0, key, sizeof(key)) == 1,
          "long TERM key");
    CHECK(strlen(key) == 96, "key truncated to 96 bytes");

    char small[8];
    CHECK(appearance_terminal_key_build(&env, NULL, 0, small, sizeof(small)) == 1,
          "small buffer safe");
    CHECK(strlen(small) < sizeof(small), "small buffer NUL terminated");
}

/* ── 无 ESC 锚点余片的 O1 判定（纯逻辑） ─────────────────────────── */

static void check_anchorless_scan(const char *buf, int expected, int expected_rgb,
                                  int r, int g, int b, const char *what)
{
    size_t consumed = 999;
    int rr = -1, gg = -1, bb = -1;
    int rc = appearance_osc11_scan_anchorless(buf, strlen(buf), &consumed,
                                              &rr, &gg, &bb);
    g_checks++;
    if (rc != expected ||
        (expected == 1 && (rr != r || gg != g || bb != b))) {
        g_failures++;
        printf("  FAIL %s: rc=%d rgb=%d,%d,%d\n", what, rc, rr, gg, bb);
    }
}

static void test_anchorless_filter(void)
{
    /* 1) 无 ESC 锚点的完整应答（BEL / ST） */
    check_anchorless_scan("]11;rgb:ffff/ffff/ffff\x1b\\", 1, 1, 255, 255, 255,
                          "anchorless rgb+ST");
    check_anchorless_scan("]11;rgb:e6/e6/e6\x07", 1, 1, 230, 230, 230,
                          "anchorless rgb+BEL");
    check_anchorless_scan("]11;#fff\x07", 1, 1, 255, 255, 255,
                          "anchorless #RGB+BEL");
    /* 2) 不完整 / 非应答 */
    check_anchorless_scan("]11;rgb:ff", 0, 0, 0, 0, 0, "anchorless incomplete");
    check_anchorless_scan("]11;", 0, 0, 0, 0, 0, "anchorless prefix only");
    check_anchorless_scan("]11;hello\x07", -1, 0, 0, 0, 0,
                          "anchorless invalid payload");
    check_anchorless_scan("]11;rgb:1234/5678/9abc", 0, 0, 0, 0, 0,
                          "anchorless lookalike without terminator");
    check_anchorless_scan("junk", -1, 0, 0, 0, 0, "anchorless not a reply");

    /* 3) classify：1=完整；0=可能前缀；-1=确定不是应答 */
    struct {
        const char *buf;
        int expected;
        const char *what;
    } cases[] = {
        {"]11;rgb:ffff/ffff/ffff\x07", 1, "classify complete rgb"},
        {"]11;#fff\x1b\\", 1, "classify complete #RGB"},
        {"]11;", 0, "classify prefix only"},
        {"]11;r", 0, "classify rgb prefix"},
        {"]11;rgb:", 0, "classify rgb: prefix"},
        {"]11;rgb:ff", 0, "classify partial component"},
        {"]11;rgb:1234/5678/9abc", 0, "classify complete-looking no terminator"},
        {"]11;#123456", 0, "classify # six digits no terminator"},
        {"]11;hello", -1, "classify literal text"},
        {"]11;rgba:", -1, "classify rgba literal"},
        {"]11;#zz", -1, "classify non-hex #"},
        {"", 0, "classify empty is possible prefix"},
        {"x", -1, "classify not bracket"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_checks++;
        int rc = appearance_anchorless_classify(cases[i].buf,
                                                strlen(cases[i].buf), NULL, NULL, NULL);
        if (rc != cases[i].expected) {
            g_failures++;
            printf("  FAIL %s: got %d expected %d\n",
                   cases[i].what, rc, cases[i].expected);
        }
    }

    /* 3b) ST 半包：`]11;rgb:...` + ESC 的每个前缀（含末尾 ESC）都必须
     * 返回 0（可继续收集），补上 '\\' 后返回 1。t11 F1 回归。 */
    {
        static const char *const st_cases[] = {
            "]11;rgb:1234/5678/9abc\x1b\\",
            "]11;#fff\x1b\\",
        };
        for (size_t c = 0; c < sizeof(st_cases) / sizeof(st_cases[0]); c++) {
            size_t full = strlen(st_cases[c]);
            for (size_t i = 0; i < full; i++) {
                g_checks++;
                int rc = appearance_anchorless_classify(st_cases[c], i,
                                                        NULL, NULL, NULL);
                if (rc != 0) {
                    g_failures++;
                    printf("  FAIL ST half-packet prefix %zu/%zu -> %d (expected 0)\n",
                           i, full, rc);
                }
            }
            g_checks++;
            if (appearance_anchorless_classify(st_cases[c], full,
                                               NULL, NULL, NULL) != 1) {
                g_failures++;
                printf("  FAIL ST full packet clasify != 1 (%s)\n", st_cases[c]);
            }
        }
        const char *st = "]11;rgb:ffff/ffff/ffff\x1b\\";
        CHECK(appearance_anchorless_should_swallow(st, strlen(st), 1, 100, 200,
                                                   NULL, NULL, NULL) == 1,
              "window+ST-terminated reply => swallow");
        CHECK(appearance_anchorless_should_swallow(st, strlen(st) - 1, 1, 100,
                                                   200, NULL, NULL, NULL) == 0,
              "window+ST half packet => still pending, not swallowed yet");
    }

    /* 4) 查询应答窗口判定 */
    CHECK(appearance_reply_window_active(1, 100, 200) == 1, "window active");
    CHECK(appearance_reply_window_active(1, 200, 200) == 1, "window boundary inclusive");
    CHECK(appearance_reply_window_active(1, 201, 200) == 0, "window expired");
    CHECK(appearance_reply_window_active(0, 100, 200) == 0, "no query => inactive");

    /* 5) 核心判定：窗口内完整合法才吞；字面量/不完整/窗口外一律不吞 */
    int r = 0, g = 0, b = 0;
    const char *full = "]11;rgb:1234/5678/9abc\x1b\\";
    const char *full_bel = "]11;rgb:ffff/ffff/ffff\x07";
    const char *literal = "]11;hello";
    const char *no_term = "]11;rgb:1234/5678/9abc";
    CHECK(appearance_anchorless_should_swallow(full, strlen(full), 1, 100, 200,
                                               &r, &g, &b) == 1,
          "window+complete => swallow");
    CHECK(r == 0x12 && g == 0x56 && b == 0x9a, "swallow reports rgb");
    CHECK(appearance_anchorless_should_swallow(full_bel, strlen(full_bel), 1,
                                               100, 200, NULL, NULL, NULL) == 1,
          "window+BEL reply => swallow");
    CHECK(appearance_anchorless_should_swallow(full, strlen(full), 0, 100, 200,
                                               NULL, NULL, NULL) == 0,
          "no query => literal");
    CHECK(appearance_anchorless_should_swallow(full, strlen(full), 1, 300, 200,
                                               NULL, NULL, NULL) == 0,
          "expired window => literal");
    CHECK(appearance_anchorless_should_swallow(literal, strlen(literal), 1, 100,
                                               200, NULL, NULL, NULL) == 0,
          "literal text => not swallowed");
    CHECK(appearance_anchorless_should_swallow(no_term, strlen(no_term), 1, 100,
                                               200, NULL, NULL, NULL) == 0,
          "lookalike without terminator => not swallowed");
    CHECK(appearance_anchorless_should_swallow("]11;", 4, 1, 100, 200,
                                               NULL, NULL, NULL) == 0,
          "prefix only => not swallowed");
}

/* ── COLORFGBG / 分类 / 变体 ──────────────────────────────────────── */

static void test_colorfgbg(void)
{
    CHECK(appearance_parse_colorfgbg("0;15") == APPEARANCE_BG_LIGHT, "0;15 is light");
    CHECK(appearance_parse_colorfgbg("15;0") == APPEARANCE_BG_DARK, "15;0 is dark");
    CHECK(appearance_parse_colorfgbg("7;7") == APPEARANCE_BG_LIGHT, "7;7 is light");
    CHECK(appearance_parse_colorfgbg("default;15") == APPEARANCE_BG_LIGHT, "default;15 is light");
    CHECK(appearance_parse_colorfgbg("0;8") == APPEARANCE_BG_DARK, "0;8 is dark");
    CHECK(appearance_parse_colorfgbg("0") == APPEARANCE_BG_DARK, "single 0 is dark");
    CHECK(appearance_parse_colorfgbg("x;y") == APPEARANCE_BG_UNKNOWN, "x;y unknown");
    CHECK(appearance_parse_colorfgbg("") == APPEARANCE_BG_UNKNOWN, "empty unknown");
    CHECK(appearance_parse_colorfgbg(NULL) == APPEARANCE_BG_UNKNOWN, "NULL unknown");
    CHECK(appearance_parse_colorfgbg("0;16") == APPEARANCE_BG_UNKNOWN, "out-of-range unknown");
    CHECK(appearance_parse_colorfgbg("0;default") == APPEARANCE_BG_UNKNOWN, "default unknown");
}

static void test_classify(void)
{
    CHECK(appearance_classify_rgb(255, 255, 255) == APPEARANCE_BG_LIGHT, "#ffffff light");
    CHECK(appearance_classify_rgb(253, 246, 227) == APPEARANCE_BG_LIGHT, "solarized light bg");
    CHECK(appearance_classify_rgb(230, 230, 230) == APPEARANCE_BG_LIGHT, "#e6e6e6 light");
    CHECK(appearance_classify_rgb(176, 176, 176) == APPEARANCE_BG_LIGHT, "#b0b0b0 light");
    CHECK(appearance_classify_rgb(128, 128, 128) == APPEARANCE_BG_LIGHT, "#808080 light");
    CHECK(appearance_classify_rgb(110, 110, 110) == APPEARANCE_BG_DARK, "#6e6e6e dark");
    CHECK(appearance_classify_rgb(30, 30, 30) == APPEARANCE_BG_DARK, "#1e1e1e dark");
    CHECK(appearance_classify_rgb(0, 0, 0) == APPEARANCE_BG_DARK, "#000000 dark");
}

static void test_resolve_variant(void)
{
    for (int mode = 0; mode < APPEARANCE_MODE_COUNT; mode++) {
        for (int detected = 0; detected <= APPEARANCE_BG_LIGHT; detected++) {
            int variant = appearance_resolve_variant(mode, detected);
            if (mode == APPEARANCE_MODE_LIGHT) {
                CHECK(variant == APPEARANCE_VARIANT_LIGHT, "explicit light ignores detection");
            } else if (mode == APPEARANCE_MODE_DARK) {
                CHECK(variant == APPEARANCE_VARIANT_DARK, "explicit dark ignores detection");
            } else if (detected == APPEARANCE_BG_LIGHT) {
                CHECK(variant == APPEARANCE_VARIANT_LIGHT, "auto + light → light");
            } else {
                CHECK(variant == APPEARANCE_VARIANT_DARK, "auto + dark/unknown → dark");
            }
        }
    }
}

/* ── 主题表 ───────────────────────────────────────────────────────── */

static void test_theme_tables(void)
{
    static const int expected_factory[APPEARANCE_THEME_SLOT_COUNT] = {
        7, -1, 3, -1, 2, -1, 6, -1, 0, 7, 6, -1
    };
    static const int expected_light[APPEARANCE_THEME_SLOT_COUNT] = {
        236, -1, 94, -1, 28, -1, 24, -1, 15, 0, 24, -1
    };
    for (int slot = 0; slot < APPEARANCE_THEME_SLOT_COUNT; slot++) {
        CHECK(appearance_theme_factory_colour(slot) == expected_factory[slot],
              "factory theme table matches design");
        CHECK(appearance_theme_light_colour(slot) == expected_light[slot],
              "light theme table matches design");
        int adaptive = (expected_factory[slot] != expected_light[slot]) ? 1 : 0;
        CHECK(appearance_theme_slot_is_adaptive(slot) == adaptive,
              "adaptive slot set matches design");
    }
    /* 恰好 7 个自适应槽：{0,2,4,6,8,9,10} */
    int adaptive_count = 0;
    for (int slot = 0; slot < APPEARANCE_THEME_SLOT_COUNT; slot++) {
        if (appearance_theme_slot_is_adaptive(slot)) {
            adaptive_count++;
        }
    }
    CHECK(adaptive_count == 7, "exactly seven adaptive theme slots");

    /* 出厂值在亮色下被替换；显式自定义在任何变体都原样 */
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_PLAYLIST_FG, 7,
                                     APPEARANCE_VARIANT_LIGHT) == 236,
          "factory playlist fg becomes light value");
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_PLAYLIST_FG, 7,
                                     APPEARANCE_VARIANT_DARK) == 7,
          "dark variant keeps factory value");
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_PLAYLIST_FG, 15,
                                     APPEARANCE_VARIANT_LIGHT) == 15,
          "explicit custom colour is never rewritten");
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_HIGHLIGHT_BG, 7,
                                     APPEARANCE_VARIANT_LIGHT) == 0,
          "factory highlight bg flips to black on light");
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_HIGHLIGHT_BG, 4,
                                     APPEARANCE_VARIANT_LIGHT) == 4,
          "custom highlight bg is preserved");
    CHECK(appearance_theme_effective(APPEARANCE_THEME_SLOT_PLAYLIST_BG, -1,
                                     APPEARANCE_VARIANT_LIGHT) == -1,
          "transparent background stays transparent");

    /* 槽越界安全 */
    CHECK(appearance_theme_factory_colour(-1) == -1, "factory slot -1 safe");
    CHECK(appearance_theme_factory_colour(12) == -1, "factory slot 12 safe");
    CHECK(appearance_theme_effective(12, 4, APPEARANCE_VARIANT_LIGHT) == 4,
          "effective out-of-range returns raw");

    /* 字段指针映射与槽序一致 */
    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int *ptr = appearance_theme_slot_ptr(&cfg, APPEARANCE_THEME_SLOT_LYRICS_FG);
    CHECK(ptr == &cfg.theme.lyrics_fg, "slot pointer maps lyrics fg");
    CHECK(appearance_theme_slot_ptr(&cfg, 99) == NULL, "slot pointer out of range");
    CHECK(appearance_theme_slot_ptr(NULL, 0) == NULL, "slot pointer NULL cfg");
}

/* ── 内置预设亮色表 ──────────────────────────────────────────────── */

static double theme_contrast_with_white(int colour)
{
    int r = 0, g = 0, b = 0;
    layout_color_rgb(colour, &r, &g, &b);
    double lin[3];
    int rgb[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        double v = (double)rgb[i] / 255.0;
        lin[i] = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    double luminance = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
    return 1.05 / (luminance + 0.05);
}

static void test_preset_light_tables(void)
{
    static const char *const ids[] = {
        "classic", "vinyl", "karaoke", "visualizer", "focus", "stage", "neon"
    };
    /* 每个预设的亮色定义（-1 表示该预设没有这个窗格）；列序：
     * playlist/info/controls/spectrum/lyrics/vinyl */
    static const int expected_light[7][LAYOUT_PANE_COUNT] = {
        { 236, 24, 94, 28, 90, -1 },
        { 236, 24, 94, 28, -1, 90 },
        {  94, 236, 90, -1, 24, -1 },
        { 236, 25, 94, 28, -1, 126 },
        { 236, 24, 94, -1, -1, -1 },
        {  94, 236, 24, 28, 90, -1 },
        { 236, 24, 94, 28, 90, -1 },
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int seen_256[LAYOUT_PANE_COUNT];
        int seen_16[LAYOUT_PANE_COUNT];
        int seen_8[LAYOUT_PANE_COUNT];
        int n = 0;
        for (int kind = 0; kind < LAYOUT_PANE_COUNT; kind++) {
            int light = 0;
            int rc = layout_preset_light_colour(ids[i], (LayoutPaneKind)kind,
                                                LAYOUT_COLOUR_FG, &light);
            if (expected_light[i][kind] < 0) {
                CHECK(rc == 0, "absent pane has no light variant");
                continue;
            }
            CHECK(rc == 1 && light == expected_light[i][kind],
                  "preset light fg matches design");
            CHECK(theme_contrast_with_white(light) >= 4.5,
                  "preset light fg contrast against white >= 4.5");
            seen_256[n] = light;
            seen_16[n] = layout_color_quantize(light, 16);
            seen_8[n] = layout_color_quantize(light, 8);
            CHECK(seen_16[n] >= 0 && seen_16[n] <= 7,
                  "light value quantizes into ANSI 0..6 bucket (16 colours)");
            CHECK(seen_8[n] >= 0 && seen_8[n] <= 7,
                  "light value quantizes into ANSI 0..6 bucket (8 colours)");
            /* 只使用 ANSI 0/1/2/4/5/6：不含 3 黄、7 白 */
            CHECK(seen_16[n] != 3 && seen_16[n] != 7,
                  "light value avoids yellow/white buckets");
            n++;
        }
        /* 同一预设内各窗格互不相同（256 / 16 / 8 三级） */
        for (int a = 0; a < n; a++) {
            for (int b = a + 1; b < n; b++) {
                CHECK(seen_256[a] != seen_256[b], "preset colours distinct (256)");
                CHECK(seen_16[a] != seen_16[b], "preset colours distinct (16)");
                CHECK(seen_8[a] != seen_8[b], "preset colours distinct (8)");
            }
        }
    }

    /* 背景没有亮色变体；未定义字段不误报 */
    int dummy = 0;
    CHECK(layout_preset_light_colour("classic", LAYOUT_PANE_PLAYLIST,
                                     LAYOUT_COLOUR_BG, &dummy) == 0,
          "background has no light variant");
    CHECK(layout_preset_light_colour("classic", LAYOUT_PANE_VINYL,
                                     LAYOUT_COLOUR_FG, &dummy) == 0,
          "classic has no vinyl pane");
    CHECK(layout_preset_light_colour("user-layout", LAYOUT_PANE_PLAYLIST,
                                     LAYOUT_COLOUR_FG, &dummy) == 0,
          "user layout is not in the built-in light table");
    CHECK(layout_preset_light_colour(NULL, LAYOUT_PANE_PLAYLIST,
                                     LAYOUT_COLOUR_FG, &dummy) == 0,
          "NULL layout id safe");
}

/* ── data/layouts/layouts.json 暗色值防漂移 ─────────────────────── */

static const char *find_preset_file(char *path, size_t path_size)
{
    static const char *const candidates[] = {
        "data/layouts/layouts.json",
        "../data/layouts/layouts.json",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *file = fopen(candidates[i], "rb");
        if (file) {
            fclose(file);
            snprintf(path, path_size, "%s", candidates[i]);
            return path;
        }
    }
    const char *source = getenv("TER_MUSIC_SOURCE_DIR");
    if (source && source[0]) {
        snprintf(path, path_size, "%s/data/layouts/layouts.json", source);
        FILE *file = fopen(path, "rb");
        if (file) {
            fclose(file);
            return path;
        }
    }
    return NULL;
}

static void test_repository_presets(void)
{
    static const char *const ids[] = {
        "classic", "vinyl", "karaoke", "visualizer", "focus", "stage", "neon"
    };
    /* 暗色（现有）前景色；-1 = 该预设没有这个窗格 */
    static const int expected_dark[7][LAYOUT_PANE_COUNT] = {
        { 15, 37, 208, 10, 198, -1 },
        { 15, 37, 208, 10, -1, 203 },
        { 208, 15, 198, -1, 203, -1 },
        { 15, 75, 118, 10, -1, 93 },
        { 15, 37, 208, -1, -1, -1 },
        { 208, 15, 37, 10, 198, -1 },
        { 15, 14, 11, 10, 13, -1 },
    };
    /* neon 在仓库里实际是 lyrics 13（不是 vinyl），修正上表最后一列。 */

    char path[512];
    if (!find_preset_file(path, sizeof(path))) {
        printf("  SKIP data/layouts/layouts.json not found\n");
        return;
    }
    LayoutCatalog catalog;
    char err[256] = "";
    CHECK(layout_spec_parse_file(path, &catalog, err, sizeof(err)) == 0,
          "repository layouts parse");
    CHECK(catalog.count == 7, "repository has seven presets");

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const LayoutSpec *spec = layout_catalog_find(&catalog, ids[i]);
        CHECK(spec != NULL, "repository preset exists");
        if (!spec) {
            continue;
        }
        int seen_16[LAYOUT_PANE_COUNT];
        int seen_8[LAYOUT_PANE_COUNT];
        int n = 0;
        for (int kind = 0; kind < LAYOUT_PANE_COUNT; kind++) {
            const LayoutPaneSpec *pane =
                layout_spec_pane_colour(spec, (LayoutPaneKind)kind);
            if (expected_dark[i][kind] < 0) {
                continue;
            }
            CHECK(pane && pane->enabled, "expected pane is enabled");
            CHECK(pane && pane->fg == expected_dark[i][kind],
                  "dark preset fg matches design table (no data drift)");
            int dark_api = -1;
            CHECK(layout_preset_dark_colour(ids[i], (LayoutPaneKind)kind,
                                            LAYOUT_COLOUR_FG, &dark_api) == 1 &&
                      dark_api == expected_dark[i][kind],
                  "layout_preset_dark_colour matches repository data");
            CHECK(layout_preset_dark_colour(ids[i], (LayoutPaneKind)kind,
                                            LAYOUT_COLOUR_BG, NULL) == 0,
                  "dark preset has no BG override");
            int fg = pane ? pane->fg : 0;
            seen_16[n] = layout_color_quantize(fg, 16);
            seen_8[n] = layout_color_quantize(fg, 8);
            n++;
        }
        for (int a = 0; a < n; a++) {
            for (int b = a + 1; b < n; b++) {
                CHECK(seen_16[a] != seen_16[b],
                      "dark preset colours distinct (16)");
                CHECK(seen_8[a] != seen_8[b],
                      "dark preset colours distinct (8)");
            }
        }
    }
}

int main(void)
{
    printf("test_appearance: OSC 11 scan\n");
    test_osc11_scan();
    printf("test_appearance: XDG color-scheme + resolve\n");
    test_xdg_and_resolve();
    printf("test_appearance: deepin terminal theme adapter\n");
    test_deepin_theme_adapter();
    printf("test_appearance: XDG portal signal match\n");
    test_portal_signal_match();
    printf("test_appearance: terminal identity key\n");
    test_terminal_identity_key();
    printf("test_appearance: anchorless O1 filter\n");
    test_anchorless_filter();
    printf("test_appearance: COLORFGBG\n");
    test_colorfgbg();
    printf("test_appearance: luminance classification\n");
    test_classify();
    printf("test_appearance: mode/variant resolution\n");
    test_resolve_variant();
    printf("test_appearance: theme tables\n");
    test_theme_tables();
    printf("test_appearance: built-in preset light variants\n");
    test_preset_light_tables();
    printf("test_appearance: repository preset dark colours\n");
    test_repository_presets();

    if (g_failures == 0) {
        printf("PASS test_appearance (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL test_appearance: %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
