/**
 * @file appearance.c
 * @brief 终端亮/暗外观的纯逻辑实现（无 ncurses、无 UI、不写任何文件）
 *
 * 这里做“查表 + 解析 + 分类”，以及终端主题适配器所需的**只读**配置文件/
 * 配色解析（路径与内容均由调用方给出，便于单测）。运行期探测、输入过滤与
 * 重绘在 ui/appearance.c / ui/appearance_terminal_theme.c；配置持久化在
 * config/config.c。三者边界清晰，本文件可以直接放进单元测试编译。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "config/appearance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 主题明暗表 ─────────────────────────────────────────────────────
 * 槽序 = APPEARANCE_THEME_SLOT_* = config.xml <theme> 字段序。
 * 出厂值即旧版硬编码默认值（暗色终端观感逐字节不变）。
 * 亮色值只使用 ANSI 0/1/2/4/5/6 桶（不用 7 白、3 黄），因此 16 色与
 * 8 色终端的量化结果相同；对纯白背景的对比度均 >= 4.70:1。
 * bg 槽两套都是 -1（终端默认背景）：亮色变体只改前景，绝不写文件。 */
static const int k_theme_factory[APPEARANCE_THEME_SLOT_COUNT] = {
    7, -1,   /* playlist fg/bg */
    3, -1,   /* controls fg/bg */
    2, -1,   /* lyrics fg/bg */
    6, -1,   /* sidebar fg/bg */
    0,  7,   /* highlight fg/bg（暗底：白底黑字） */
    6, -1    /* border fg/bg */
};

static const int k_theme_light[APPEARANCE_THEME_SLOT_COUNT] = {
    236, -1, /* playlist fg/bg：近黑中性色 */
    94,  -1, /* controls fg/bg：暗琥珀 */
    28,  -1, /* lyrics fg/bg：暗绿 */
    24,  -1, /* sidebar fg/bg：暗青 */
    15,  0,  /* highlight fg/bg（亮底：黑底白字，与暗底对称） */
    24,  -1  /* border fg/bg */
};

int appearance_theme_factory_colour(int slot)
{
    if (slot < 0 || slot >= APPEARANCE_THEME_SLOT_COUNT) {
        return -1;
    }
    return k_theme_factory[slot];
}

int appearance_theme_light_colour(int slot)
{
    if (slot < 0 || slot >= APPEARANCE_THEME_SLOT_COUNT) {
        return -1;
    }
    return k_theme_light[slot];
}

int appearance_theme_slot_is_adaptive(int slot)
{
    if (slot < 0 || slot >= APPEARANCE_THEME_SLOT_COUNT) {
        return 0;
    }
    return k_theme_factory[slot] != k_theme_light[slot];
}

int appearance_theme_effective(int slot, int raw, int variant)
{
    if (slot < 0 || slot >= APPEARANCE_THEME_SLOT_COUNT) {
        return raw;
    }
    if (variant != APPEARANCE_VARIANT_LIGHT) {
        return raw;
    }
    if (raw != k_theme_factory[slot]) {
        return raw;   /* 与出厂默认不同 = 用户显式自定义，原样使用 */
    }
    return k_theme_light[slot];
}

int *appearance_theme_slot_ptr(AppConfig *cfg, int slot)
{
    if (!cfg) {
        return NULL;
    }
    ColorTheme *t = &cfg->theme;
    switch (slot) {
        case APPEARANCE_THEME_SLOT_PLAYLIST_FG:  return &t->playlist_fg;
        case APPEARANCE_THEME_SLOT_PLAYLIST_BG:  return &t->playlist_bg;
        case APPEARANCE_THEME_SLOT_CONTROLS_FG:  return &t->controls_fg;
        case APPEARANCE_THEME_SLOT_CONTROLS_BG:  return &t->controls_bg;
        case APPEARANCE_THEME_SLOT_LYRICS_FG:    return &t->lyrics_fg;
        case APPEARANCE_THEME_SLOT_LYRICS_BG:    return &t->lyrics_bg;
        case APPEARANCE_THEME_SLOT_SIDEBAR_FG:   return &t->sidebar_fg;
        case APPEARANCE_THEME_SLOT_SIDEBAR_BG:   return &t->sidebar_bg;
        case APPEARANCE_THEME_SLOT_HIGHLIGHT_FG: return &t->highlight_fg;
        case APPEARANCE_THEME_SLOT_HIGHLIGHT_BG: return &t->highlight_bg;
        case APPEARANCE_THEME_SLOT_BORDER_FG:    return &t->border_fg;
        case APPEARANCE_THEME_SLOT_BORDER_BG:    return &t->border_bg;
        default:                                 return NULL;
    }
}

/* ── 背景亮度分类（WCAG 相对亮度） ───────────────────────────────── */

static double srgb_channel_to_linear(int channel)
{
    if (channel < 0)   channel = 0;
    if (channel > 255) channel = 255;
    double value = (double)channel / 255.0;
    if (value <= 0.04045) {
        return value / 12.92;
    }
    return pow((value + 0.055) / 1.055, 2.4);
}

int appearance_classify_rgb(int r, int g, int b)
{
    double luminance = 0.2126 * srgb_channel_to_linear(r) +
                       0.7152 * srgb_channel_to_linear(g) +
                       0.0722 * srgb_channel_to_linear(b);
    /* 0.179 = 黑字与白字对比度相等的交叉点，见头文件说明。 */
    return luminance >= 0.179 ? APPEARANCE_BG_LIGHT : APPEARANCE_BG_DARK;
}

/* ── OSC 11 应答解析 ─────────────────────────────────────────────── */

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1~4 位十六进制分量 → 0..255（按 16^n - 1 归一化，四舍五入）。 */
static int parse_hex_component(const char *text, size_t len, int *out)
{
    if (!text || !out || len < 1 || len > 4) {
        return -1;
    }
    unsigned long value = 0;
    for (size_t i = 0; i < len; i++) {
        int digit = hex_digit((unsigned char)text[i]);
        if (digit < 0) {
            return -1;
        }
        value = value * 16UL + (unsigned long)digit;
    }
    unsigned long max = (1UL << (4 * len)) - 1UL;
    unsigned long scaled = (value * 255UL + max / 2UL) / max;
    *out = (int)scaled;
    return 0;
}

static int parse_osc11_payload(const char *payload, size_t len,
                               int *r, int *g, int *b)
{
    if (!payload || len == 0 || !r || !g || !b) {
        return -1;
    }

    if (len >= 4 && memcmp(payload, "rgb:", 4) == 0) {
        const char *body = payload + 4;
        size_t body_len = len - 4;
        int components[3] = {0, 0, 0};
        size_t start = 0;
        int part = 0;
        for (size_t i = 0; i <= body_len; i++) {
            if (i == body_len || body[i] == '/') {
                if (part >= 3) {
                    return -1;
                }
                if (parse_hex_component(body + start, i - start,
                                        &components[part]) != 0) {
                    return -1;
                }
                part++;
                start = i + 1;
            }
        }
        if (part != 3) {
            return -1;
        }
        *r = components[0];
        *g = components[1];
        *b = components[2];
        return 0;
    }

    if (payload[0] == '#') {
        size_t digits = len - 1;
        if (digits == 3) {
            int values[3];
            for (int i = 0; i < 3; i++) {
                int digit = hex_digit((unsigned char)payload[1 + i]);
                if (digit < 0) {
                    return -1;
                }
                values[i] = digit * 17;   /* #RGB → #RRGGBB */
            }
            *r = values[0];
            *g = values[1];
            *b = values[2];
            return 0;
        }
        if (digits == 6) {
            int values[3];
            for (int i = 0; i < 3; i++) {
                int high = hex_digit((unsigned char)payload[1 + i * 2]);
                int low  = hex_digit((unsigned char)payload[2 + i * 2]);
                if (high < 0 || low < 0) {
                    return -1;
                }
                values[i] = high * 16 + low;
            }
            *r = values[0];
            *g = values[1];
            *b = values[2];
            return 0;
        }
        return -1;
    }

    return -1;
}

/* OSC 11 的固定前缀：ESC ] 1 1 ; */
static const char k_osc11_prefix[] = "\033]11;";
#define OSC11_PREFIX_LEN 5

int appearance_osc11_scan(const char *buf, size_t len, size_t *consumed,
                          int *r, int *g, int *b)
{
    if (consumed) {
        *consumed = 0;
    }
    if (!buf || len == 0) {
        return 0;   /* 还没有字节，等更多输入 */
    }

    size_t prefix_cmp = len < OSC11_PREFIX_LEN ? len : OSC11_PREFIX_LEN;
    if (memcmp(buf, k_osc11_prefix, prefix_cmp) != 0) {
        if (consumed) {
            *consumed = 1;   /* 只消费首字节，其余交给调用方回退 */
        }
        return -1;
    }
    if (len < OSC11_PREFIX_LEN) {
        return 0;            /* 合法前缀，等更多字节 */
    }

    /* 找到终止符：BEL 或 ST（ESC \）。 */
    size_t payload_start = OSC11_PREFIX_LEN;
    size_t payload_end = 0;
    size_t terminator_len = 0;
    int found = 0;
    for (size_t i = payload_start; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 0x07) {           /* BEL */
            payload_end = i;
            terminator_len = 1;
            found = 1;
            break;
        }
        if (ch == 0x1b) {           /* 可能是 ST 的 ESC */
            if (i + 1 >= len) {
                return 0;           /* 还差 ST 的第二个字节 */
            }
            if (buf[i + 1] == '\\') {
                payload_end = i;
                terminator_len = 2;
                found = 1;
                break;
            }
            /* OSC 负载里不允许裸 ESC：整段应答非法，消费到当前字节。 */
            if (consumed) {
                *consumed = i + 1;
            }
            return -1;
        }
    }

    if (!found) {
        if (len >= 256) {           /* 失控的应答：整段丢弃，防止缓冲膨胀 */
            if (consumed) {
                *consumed = len;
            }
            return -1;
        }
        return 0;                   /* 合法前缀，尚未收到终止符 */
    }

    size_t total = payload_end + terminator_len;
    int rr = 0, gg = 0, bb = 0;
    if (parse_osc11_payload(buf + payload_start,
                            payload_end - payload_start,
                            &rr, &gg, &bb) != 0) {
        /* 已完整匹配 ESC ] 11 ; 前缀：即使内容非法也整段吞掉，
         * 绝不让 rgb:/rgba: 之类的字节泄漏到按键分发。 */
        if (consumed) {
            *consumed = total;
        }
        return -1;
    }

    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
    if (consumed) {
        *consumed = total;
    }
    return 1;
}

/* ── 无 ESC 锚点的第二道防线（O1 加固） ────────────────────────────
 * 终端应答开头的 ESC 可能已被别的读取点消费，只剩 `]11;rgb:...`。
 * 该路径既要阻止应答字节泄漏，又绝不能把用户输入/粘贴的同一字面量
 * 误当应答吞掉。为此本模块提供纯逻辑判定，I/O 侧只做有界、非阻塞收集。
 * 见 appearance_anchorless_should_swallow() 的“完整合法 + 查询窗内”约束。 */

static const char k_anchorless_prefix[] = "]11;";
#define ANCHORLESS_PREFIX_LEN 4

int appearance_osc11_scan_anchorless(const char *buf, size_t len, size_t *consumed,
                                     int *r, int *g, int *b)
{
    if (consumed) {
        *consumed = 0;
    }
    if (!buf || len == 0) {
        return 0;
    }
    size_t prefix_cmp = len < ANCHORLESS_PREFIX_LEN ? len : ANCHORLESS_PREFIX_LEN;
    if (memcmp(buf, k_anchorless_prefix, prefix_cmp) != 0) {
        if (consumed) {
            *consumed = 1;
        }
        return -1;
    }
    if (len < ANCHORLESS_PREFIX_LEN) {
        return 0;
    }

    size_t payload_start = ANCHORLESS_PREFIX_LEN;
    size_t payload_end = 0;
    size_t terminator_len = 0;
    int found = 0;
    for (size_t i = payload_start; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 0x07) {           /* BEL */
            payload_end = i;
            terminator_len = 1;
            found = 1;
            break;
        }
        if (ch == 0x1b) {
            if (i + 1 >= len) {
                return 0;
            }
            if (buf[i + 1] == '\\') {
                payload_end = i;
                terminator_len = 2;
                found = 1;
                break;
            }
            if (consumed) {
                *consumed = i + 1;
            }
            return -1;
        }
    }
    if (!found) {
        if (len >= 256) {
            if (consumed) {
                *consumed = len;
            }
            return -1;
        }
        return 0;
    }

    size_t total = payload_end + terminator_len;
    int rr = 0, gg = 0, bb = 0;
    if (parse_osc11_payload(buf + payload_start,
                            payload_end - payload_start,
                            &rr, &gg, &bb) != 0) {
        if (consumed) {
            *consumed = total;
        }
        return -1;
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
    if (consumed) {
        *consumed = total;
    }
    return 1;
}

/* 负载（终止符之前）是否仍可能成为合法 OSC11 负载。
 * 只接受 rgb: + 1~4 位十六进制三段，或 # + 1~6 位十六进制。 */
static int anchorless_payload_possible(const char *payload, size_t len)
{
    /* ST 是 ESC '\\' 两字节；若缓冲恰好停在 ST 的 ESC 上（scan 返回“需更多
     * 字节”），这里必须把末尾 ESC 视为“待续 ST 前缀”而不是非法负载字符，
     * 否则 anchorless_poll 会走 cls<0 提前回放，ST 终止的合法应答永远不会被
     * 吞掉（t11 F1）。内部 ESC 若后面不是 '\\' 已在 scan 阶段返回 -1。 */
    if (len > 0 && (unsigned char)payload[len - 1] == 0x1b) {
        len--;
    }
    if (len == 0) {
        return 1;
    }
    if (len <= 4 && memcmp(payload, "rgb:", len) == 0) {
        return 1;   /* r / rg / rgb / rgb: 的渐进前缀 */
    }
    if (len >= 4 && memcmp(payload, "rgb:", 4) == 0) {
        int part = 0;
        size_t digits = 0;
        for (size_t i = 4; i < len; i++) {
            int ch = (unsigned char)payload[i];
            if (hex_digit(ch) >= 0) {
                if (++digits > 4) {
                    return 0;
                }
            } else if (ch == '/') {
                if (digits == 0 || part >= 2) {
                    return 0;
                }
                part++;
                digits = 0;
            } else {
                return 0;
            }
        }
        return 1;
    }
    if (payload[0] == '#') {
        if (len > 7) {
            return 0;   /* '#' + 最多 6 位 */
        }
        for (size_t i = 1; i < len; i++) {
            if (hex_digit((unsigned char)payload[i]) < 0) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

int appearance_anchorless_classify(const char *buf, size_t len,
                                   int *r, int *g, int *b)
{
    int rr = 0, gg = 0, bb = 0;
    /* 避免调用方传窄缓冲时越界：本函数只看前 256 字节。 */
    if (len > 256) {
        len = 256;
    }
    int rc = appearance_osc11_scan_anchorless(buf, len, NULL, &rr, &gg, &bb);
    if (rc == 1) {
        if (r) *r = rr;
        if (g) *g = gg;
        if (b) *b = bb;
        return 1;
    }
    if (rc == -1) {
        return -1;
    }
    if (len < ANCHORLESS_PREFIX_LEN) {
        return 0;   /* 仍在 `]11;` 前缀内 */
    }
    return anchorless_payload_possible(buf + ANCHORLESS_PREFIX_LEN,
                                       len - ANCHORLESS_PREFIX_LEN) ? 0 : -1;
}

int appearance_portal_signal_matches(const char *interface, const char *member,
                                     int is_signal)
{
    if (!is_signal || !interface || !member) {
        return 0;
    }
    return strcmp(interface, "org.freedesktop.portal.Settings") == 0 &&
           strcmp(member, "SettingChanged") == 0;
}

int appearance_reply_window_active(int reply_expected,
                                   unsigned long long now_ms,
                                   unsigned long long deadline_ms)
{
    return reply_expected && now_ms <= deadline_ms;
}

int appearance_anchorless_should_swallow(const char *buf, size_t len,
                                         int reply_expected,
                                         unsigned long long now_ms,
                                         unsigned long long deadline_ms,
                                         int *r, int *g, int *b)
{
    if (!appearance_reply_window_active(reply_expected, now_ms, deadline_ms)) {
        return 0;   /* 没发过查询/窗口已过：一定是用户输入 */
    }
    return appearance_anchorless_classify(buf, len, r, g, b) == 1;
}

int appearance_parse_colorfgbg(const char *value)
{
    if (!value || value[0] == '\0') {
        return APPEARANCE_BG_UNKNOWN;
    }
    const char *last = strrchr(value, ';');
    const char *token = last ? last + 1 : value;
    while (*token == ' ' || *token == '\t') {
        token++;
    }
    if (*token == '\0' || strncmp(token, "default", 7) == 0) {
        return APPEARANCE_BG_UNKNOWN;
    }
    char *end = NULL;
    long parsed = strtol(token, &end, 10);
    if (end == token || (end && *end != '\0')) {
        return APPEARANCE_BG_UNKNOWN;
    }
    /* 0-6 与 8（亮黑/深灰）按暗底；7（白）与 9-15（亮色）按亮底。 */
    if ((parsed >= 0 && parsed <= 6) || parsed == 8) {
        return APPEARANCE_BG_DARK;
    }
    if (parsed == 7 || (parsed >= 9 && parsed <= 15)) {
        return APPEARANCE_BG_LIGHT;
    }
    return APPEARANCE_BG_UNKNOWN;
}

int appearance_resolve_variant(int mode, int detected)
{
    if (mode == APPEARANCE_MODE_LIGHT) {
        return APPEARANCE_VARIANT_LIGHT;
    }
    if (mode == APPEARANCE_MODE_DARK) {
        return APPEARANCE_VARIANT_DARK;
    }
    /* auto：探测到亮底才用亮色；未知/暗底一律保持旧观感。 */
    return detected == APPEARANCE_BG_LIGHT ? APPEARANCE_VARIANT_LIGHT
                                           : APPEARANCE_VARIANT_DARK;
}

/* ── XDG portal / 信号优先级 / 终端身份键 ───────────────────────────── */

int appearance_xdg_color_scheme_to_bg(unsigned int value)
{
    switch (value) {
        case APPEARANCE_XDG_PREFER_DARK:  return APPEARANCE_BG_DARK;
        case APPEARANCE_XDG_PREFER_LIGHT: return APPEARANCE_BG_LIGHT;
        default:                          return APPEARANCE_BG_UNKNOWN;
    }
}

int appearance_signal_osc11_known(const AppearanceSignals *signals)
{
    return signals && signals->osc11 != APPEARANCE_BG_UNKNOWN;
}

int appearance_resolve_signals(int mode, const AppearanceSignals *signals,
                               int *source_out)
{
    if (source_out) {
        *source_out = APPEARANCE_SOURCE_FALLBACK;
    }
    if (!signals) {
        return APPEARANCE_VARIANT_DARK;
    }

    if (mode == APPEARANCE_MODE_LIGHT) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_EXPLICIT;
        }
        return APPEARANCE_VARIANT_LIGHT;
    }
    if (mode == APPEARANCE_MODE_DARK) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_EXPLICIT;
        }
        return APPEARANCE_VARIANT_DARK;
    }

    /* auto：按固定优先级取第一个已知信号（见头文件规则）。 */
    if (signals->osc11 != APPEARANCE_BG_UNKNOWN) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_OSC11;
        }
        return signals->osc11 == APPEARANCE_BG_LIGHT ? APPEARANCE_VARIANT_LIGHT
                                                     : APPEARANCE_VARIANT_DARK;
    }
    if (signals->terminal_theme != APPEARANCE_BG_UNKNOWN) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_TERMINAL_THEME;
        }
        return signals->terminal_theme == APPEARANCE_BG_LIGHT
                   ? APPEARANCE_VARIANT_LIGHT : APPEARANCE_VARIANT_DARK;
    }
    if (signals->xdg != APPEARANCE_BG_UNKNOWN) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_XDG;
        }
        return signals->xdg == APPEARANCE_BG_LIGHT ? APPEARANCE_VARIANT_LIGHT
                                                   : APPEARANCE_VARIANT_DARK;
    }
    if (signals->colorfgbg != APPEARANCE_BG_UNKNOWN) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_COLORFGBG;
        }
        return signals->colorfgbg == APPEARANCE_BG_LIGHT ? APPEARANCE_VARIANT_LIGHT
                                                         : APPEARANCE_VARIANT_DARK;
    }
    if (signals->term != APPEARANCE_BG_UNKNOWN) {
        if (source_out) {
            *source_out = APPEARANCE_SOURCE_TERM;
        }
        return signals->term == APPEARANCE_BG_LIGHT ? APPEARANCE_VARIANT_LIGHT
                                                    : APPEARANCE_VARIANT_DARK;
    }
    return APPEARANCE_VARIANT_DARK;
}

/* 本地、无 locale 依赖的 ASCII 小写。 */
static int terminal_ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

/* 统一键格式：小写、只保留 [a-z0-9:._-]，其它字符→'_'，末尾 NUL。 */
void appearance_terminal_key_sanitize(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t write = 0;
    for (size_t i = 0; src && src[i] && write + 1 < out_size; i++) {
        int ch = terminal_ascii_lower((unsigned char)src[i]);
        int allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                      ch == ':' || ch == '.' || ch == '_' || ch == '-';
        out[write++] = (char)(allowed ? ch : '_');
    }
    out[write] = '\0';
}

/* 终端模拟器 allowlist（不含多路复用器；comm 最长 15 字符，允许前缀截断）。 */
static const char *const k_terminal_allowlist[] = {
    "deepin-terminal", "gnome-terminal", "gnome-terminal-", "konsole",
    "xterm", "alacritty", "kitty", "wezterm", "foot", "terminator",
    "tilix", "xfce4-terminal", "lxterminal", "qterminal", "mate-terminal",
    "st", "urxvt"
};

static int terminal_name_matches(const char *comm, const char *allow)
{
    if (!comm || !allow || comm[0] == '\0') {
        return 0;
    }
    size_t comm_len = strlen(comm);
    size_t allow_len = strlen(allow);
    if (comm_len < 3) {
        return 0;
    }
    /* comm 可能被 Linux 截到 15 字符（如 gnome-terminal-），两个方向都接受。 */
    if (comm_len <= allow_len &&
        strncmp(comm, allow, comm_len) == 0) {
        return 1;
    }
    if (allow_len < comm_len &&
        strncmp(allow, comm, allow_len) == 0) {
        return 1;
    }
    return 0;
}

static const char *terminal_find_outermost(const char *const *ancestors,
                                           size_t ancestor_count)
{
    if (!ancestors) {
        return NULL;
    }
    /* 数组为“自身 → 最外层”，从最外层往回找第一个匹配。 */
    for (size_t i = ancestor_count; i > 0; i--) {
        const char *comm = ancestors[i - 1];
        if (!comm || !comm[0]) {
            continue;
        }
        for (size_t a = 0;
             a < sizeof(k_terminal_allowlist) / sizeof(k_terminal_allowlist[0]);
             a++) {
            if (terminal_name_matches(comm, k_terminal_allowlist[a])) {
                return comm;
            }
        }
    }
    return NULL;
}

int appearance_terminal_key_build(const AppearanceTerminalEnv *env,
                                  const char *const *ancestors,
                                  size_t ancestor_count,
                                  char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (ancestor_count > 8) {
        ancestor_count = 8;
    }
    if (!env) {
        env = &(AppearanceTerminalEnv){0};
    }

    char raw[160];
    raw[0] = '\0';
    if (env->term_program && env->term_program[0]) {
        snprintf(raw, sizeof(raw), "tp:%s", env->term_program);
    } else {
        const char *terminal = terminal_find_outermost(ancestors, ancestor_count);
        if (terminal) {
            snprintf(raw, sizeof(raw), "anc:%s", terminal);
        } else if (env->tmux && env->tmux[0]) {
            snprintf(raw, sizeof(raw), "tmux:%s",
                     env->term && env->term[0] ? env->term : "unknown");
        } else if (env->sty && env->sty[0]) {
            snprintf(raw, sizeof(raw), "screen:%s",
                     env->term && env->term[0] ? env->term : "unknown");
        } else if ((env->ssh_tty && env->ssh_tty[0]) ||
                   (env->ssh_connection && env->ssh_connection[0])) {
            snprintf(raw, sizeof(raw), "ssh:%s",
                     env->term && env->term[0] ? env->term : "unknown");
        } else if (env->term && env->term[0]) {
            snprintf(raw, sizeof(raw), "term:%s", env->term);
        } else if (env->colorterm && env->colorterm[0]) {
            snprintf(raw, sizeof(raw), "ct:%s", env->colorterm);
        } else {
            snprintf(raw, sizeof(raw), "default");
        }
    }

    char sanitized[160];
    appearance_terminal_key_sanitize(raw, sanitized, sizeof(sanitized));
    size_t limit = out_size - 1;
    if (limit > 96) {
        limit = 96;   /* 设计规定最长 96 字节 */
    }
    size_t copy = strlen(sanitized);
    if (copy > limit) {
        copy = limit;
    }
    memcpy(out, sanitized, copy);
    out[copy] = '\0';

    return strcmp(out, "default") == 0 ? 0 : 1;
}

/* ── 终端主题适配器（deepin-terminal；t22） ────────────────────────── */

#define DEEPIN_THEME_FILE_MAX (64 * 1024)

static char *theme_trim(char *start)
{
    while (*start == ' ' || *start == '\t' || *start == '\r') {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0) {
        char ch = start[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        start[--len] = '\0';
    }
    return start;
}

int appearance_ini_get_value(const char *text, size_t len,
                             const char *section, const char *key,
                             char *out, size_t out_size)
{
    if (!text || !section || !key) {
        return 0;
    }
    if (out && out_size) {
        out[0] = '\0';
    }

    const char *cursor = text;
    const char *end = text + len;
    int in_section = 0;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!line_end) {
            line_end = end;
        }
        size_t line_len = (size_t)(line_end - cursor);
        char line[512];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';
        cursor = line_end < end ? line_end + 1 : end;

        char *work = theme_trim(line);
        if (!work[0] || work[0] == ';' || work[0] == '#') {
            continue;
        }
        if (work[0] == '[') {
            char *close = strchr(work, ']');
            if (!close) {
                in_section = 0;
                continue;
            }
            *close = '\0';
            in_section = strcmp(theme_trim(work + 1), section) == 0;
            continue;
        }
        if (!in_section) {
            continue;
        }
        char *equals = strchr(work, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        char *name = theme_trim(work);
        if (strcmp(name, key) != 0) {
            continue;
        }
        char *value = theme_trim(equals + 1);
        /* 行尾整行注释：仅当 ';'/'#' 前是空白时截断，避免误伤合法字符。 */
        for (char *p = value; *p; p++) {
            if ((*p == ';' || *p == '#') &&
                (p == value || p[-1] == ' ' || p[-1] == '\t')) {
                *p = '\0';
                theme_trim(value);
                break;
            }
        }
        size_t value_len = strlen(value);
        if (value_len >= 2 &&
            ((value[0] == '"' && value[value_len - 1] == '"') ||
             (value[0] == '\'' && value[value_len - 1] == '\''))) {
            value[value_len - 1] = '\0';
            value++;
        }
        if (out && out_size) {
            snprintf(out, out_size, "%s", value);
        }
        return 1;
    }
    return 0;
}

int appearance_colorscheme_background(const char *text, size_t len,
                                      int *r, int *g, int *b)
{
    char color[128];
    if (!appearance_ini_get_value(text, len, "Background", "Color",
                                  color, sizeof(color))) {
        return 0;
    }
    char *cursor = color;
    int values[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        char *next = NULL;
        long value = strtol(cursor, &next, 10);
        if (next == cursor || value < 0 || value > 255) {
            return 0;
        }
        values[i] = (int)value;
        cursor = next;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (i < 2) {
            if (*cursor != ',') {
                return 0;   /* 缺分隔符（含 alpha/transparent 等非 RGB） */
            }
            cursor++;
        }
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != '\0') {
        return 0;   /* 第 4 个分量（alpha）或垃圾尾部 */
    }
    if (r) *r = values[0];
    if (g) *g = values[1];
    if (b) *b = values[2];
    return 1;
}

static int theme_read_file(const char *path, char **text_out)
{
    if (!path || !text_out) {
        return 0;
    }
    *text_out = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0 || size > DEEPIN_THEME_FILE_MAX) {
        fclose(file);
        return 0;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return 0;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    *text_out = text;
    return 1;
}

static int theme_name_safe(const char *name)
{
    if (!name || !name[0] || strlen(name) > 96) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\' || ch == '\'') {
            return 0;
        }
    }
    if (strstr(name, "..")) {
        return 0;
    }
    return 1;
}

/* 在候选目录里找 <base>/terminalwidget6/color-schemes/<name>.colorscheme；
 * 即使文件不存在也把首个候选写入 scheme_path（供 stat 检测后续创建）。 */
static int theme_find_scheme(const char *name, const char *data_home,
                             const char *const *data_dirs, size_t data_dir_count,
                             char *scheme_path, size_t scheme_path_size)
{
    char candidate[APPEARANCE_TERMINAL_THEME_PATH_MAX];
    int first = 1;
    if (data_home && data_home[0]) {
        snprintf(candidate, sizeof(candidate),
                 "%s/terminalwidget6/color-schemes/%s.colorscheme",
                 data_home, name);
        if (first && scheme_path && scheme_path_size) {
            snprintf(scheme_path, scheme_path_size, "%s", candidate);
            first = 0;
        }
        if (access(candidate, R_OK) == 0) {
            if (scheme_path && scheme_path_size) {
                snprintf(scheme_path, scheme_path_size, "%s", candidate);
            }
            return 1;
        }
    }
    for (size_t i = 0; i < data_dir_count; i++) {
        if (!data_dirs || !data_dirs[i] || !data_dirs[i][0]) {
            continue;
        }
        snprintf(candidate, sizeof(candidate),
                 "%s/terminalwidget6/color-schemes/%s.colorscheme",
                 data_dirs[i], name);
        if (first && scheme_path && scheme_path_size) {
            snprintf(scheme_path, scheme_path_size, "%s", candidate);
            first = 0;
        }
        if (access(candidate, R_OK) == 0) {
            if (scheme_path && scheme_path_size) {
                snprintf(scheme_path, scheme_path_size, "%s", candidate);
            }
            return 1;
        }
    }
    snprintf(candidate, sizeof(candidate),
             "/usr/share/terminalwidget6/color-schemes/%s.colorscheme", name);
    if (first && scheme_path && scheme_path_size) {
        snprintf(scheme_path, scheme_path_size, "%s", candidate);
    }
    if (access(candidate, R_OK) == 0) {
        if (scheme_path && scheme_path_size) {
            snprintf(scheme_path, scheme_path_size, "%s", candidate);
        }
        return 1;
    }
    return 0;
}

int appearance_deepin_theme_bg(const char *config_dir,
                               const char *data_home,
                               const char *const *data_dirs,
                               size_t data_dir_count,
                               char *config_path_out, size_t config_path_size,
                               char *scheme_path_out, size_t scheme_path_size,
                               int *bg_out)
{
    if (bg_out) {
        *bg_out = APPEARANCE_BG_UNKNOWN;
    }
    if (config_path_out && config_path_size) {
        config_path_out[0] = '\0';
    }
    if (scheme_path_out && scheme_path_size) {
        scheme_path_out[0] = '\0';
    }
    if (!config_dir || !config_dir[0]) {
        return 0;
    }

    char config_path[APPEARANCE_TERMINAL_THEME_PATH_MAX];
    snprintf(config_path, sizeof(config_path),
             "%s/deepin/deepin-terminal/config.conf", config_dir);
    if (config_path_out && config_path_size) {
        snprintf(config_path_out, config_path_size, "%s", config_path);
    }

    char *config_text = NULL;
    if (!theme_read_file(config_path, &config_text)) {
        return 0;
    }
    char theme[128] = "";
    char expand[128] = "";
    (void)appearance_ini_get_value(config_text, strlen(config_text),
                                   "basic.interface.theme", "value",
                                   theme, sizeof(theme));
    (void)appearance_ini_get_value(config_text, strlen(config_text),
                                   "basic.interface.expand_theme", "value",
                                   expand, sizeof(expand));
    free(config_text);

    char scheme_path[APPEARANCE_TERMINAL_THEME_PATH_MAX];
    scheme_path[0] = '\0';
    int have_scheme = 0;
    if (expand[0]) {
        if (strcmp(expand, "customTheme") == 0) {
            snprintf(scheme_path, sizeof(scheme_path),
                     "%s/deepin/deepin-terminal/customTheme.colorscheme",
                     config_dir);
            have_scheme = access(scheme_path, R_OK) == 0;
        } else if (theme_name_safe(expand)) {
            have_scheme = theme_find_scheme(expand, data_home, data_dirs,
                                            data_dir_count, scheme_path,
                                            sizeof(scheme_path));
        }
    } else if (theme[0] && theme_name_safe(theme)) {
        have_scheme = theme_find_scheme(theme, data_home, data_dirs,
                                        data_dir_count, scheme_path,
                                        sizeof(scheme_path));
    }
    if (scheme_path_out && scheme_path_size) {
        snprintf(scheme_path_out, scheme_path_size, "%s", scheme_path);
    }
    if (!have_scheme) {
        return 0;
    }

    char *scheme_text = NULL;
    if (!theme_read_file(scheme_path, &scheme_text)) {
        return 0;
    }
    int r = 0, g = 0, b = 0;
    int ok = appearance_colorscheme_background(scheme_text, strlen(scheme_text),
                                               &r, &g, &b);
    free(scheme_text);
    if (!ok) {
        return 0;
    }
    int bg = appearance_classify_rgb(r, g, b);
    if (bg_out) {
        *bg_out = bg;
    }
    return 1;
}

int appearance_terminal_is_deepin_env(const AppearanceTerminalEnv *env,
                                      const char *const *ancestors,
                                      size_t ancestor_count)
{
    if (env && env->term_program && env->term_program[0]) {
        /* TERM_PROGRAM 是权威：只要它非空，就只按它判定，绝不回退祖先链。
         * 典型场景：从 deepin-terminal 启动 vscode/kitty，再在其中运行 TUI，
         * 祖先链含 deepin-terminal，但 TERM_PROGRAM=vscode/kitty 必须不启用
         * 本适配器（t24 F1）。仅当 TERM_PROGRAM 为空/未设置时才看祖先。 */
        char lowered[64];
        size_t i = 0;
        for (; env->term_program[i] && i + 1 < sizeof(lowered); i++) {
            lowered[i] = (char)terminal_ascii_lower((unsigned char)env->term_program[i]);
        }
        lowered[i] = '\0';
        return strcmp(lowered, "deepin-terminal") == 0;
    }
    if (!ancestors) {
        return 0;
    }
    if (ancestor_count > 8) {
        ancestor_count = 8;
    }
    for (size_t i = 0; i < ancestor_count; i++) {
        if (terminal_name_matches(ancestors[i], "deepin-terminal")) {
            return 1;
        }
    }
    return 0;
}
