/**
 * @file appearance.h
 * @brief 终端亮/暗外观的**纯逻辑**定义与解析（无 ncurses、无 UI 依赖）
 *
 * 三层语义：
 *   1. 外观模式（`preferences.appearance_mode`）：0=自动跟随终端、1=固定亮色、
 *      2=固定暗色。这是唯一持久化的外观键；
 *   2. 终端背景探测结果：未知/暗色/亮色。只存在于前端进程内存，绝不落盘；
 *   3. 生效变体：由模式与探测结果解析（自动 + 未知 → 暗色，保证旧观感）。
 *
 * 主题的“出厂值”与“亮色变体”表也放在这里：配置层只读引用，
 * 界面层（menus.c / layout.c / settings.c）据此做渲染期解析，绝不回写配置。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CONFIG_APPEARANCE_H
#define CONFIG_APPEARANCE_H

#include <stddef.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 外观模式（持久化：preferences.appearance_mode） ───────────────── */
#define APPEARANCE_MODE_AUTO  0   /* 自动跟随终端亮/暗背景 */
#define APPEARANCE_MODE_LIGHT 1   /* 固定亮色 */
#define APPEARANCE_MODE_DARK  2   /* 固定暗色 */
#define APPEARANCE_MODE_COUNT 3

/* ── 终端背景探测结果（仅前端进程内存，不落盘） ───────────────────── */
#define APPEARANCE_BG_UNKNOWN 0
#define APPEARANCE_BG_DARK    1
#define APPEARANCE_BG_LIGHT   2

/* ── 生效变体（渲染期使用） ───────────────────────────────────────── */
#define APPEARANCE_VARIANT_DARK  0
#define APPEARANCE_VARIANT_LIGHT 1

/* 主题 12 个颜色槽，顺序与 config.xml 的 <theme> / config_fields.h 一致。
 * 仅供“出厂默认值 / 亮色变体”查表；用 appearance_theme_slot_ptr() 取字段地址。 */
enum {
    APPEARANCE_THEME_SLOT_PLAYLIST_FG = 0,
    APPEARANCE_THEME_SLOT_PLAYLIST_BG,
    APPEARANCE_THEME_SLOT_CONTROLS_FG,
    APPEARANCE_THEME_SLOT_CONTROLS_BG,
    APPEARANCE_THEME_SLOT_LYRICS_FG,
    APPEARANCE_THEME_SLOT_LYRICS_BG,
    APPEARANCE_THEME_SLOT_SIDEBAR_FG,
    APPEARANCE_THEME_SLOT_SIDEBAR_BG,
    APPEARANCE_THEME_SLOT_HIGHLIGHT_FG,
    APPEARANCE_THEME_SLOT_HIGHLIGHT_BG,
    APPEARANCE_THEME_SLOT_BORDER_FG,
    APPEARANCE_THEME_SLOT_BORDER_BG,
    APPEARANCE_THEME_SLOT_COUNT
};

/** @brief 出厂（暗色）默认值；槽越界返回 -1。 */
int appearance_theme_factory_colour(int slot);

/** @brief 亮色变体值；该槽不参与自适应时与出厂值相同；槽越界返回 -1。 */
int appearance_theme_light_colour(int slot);

/** @brief 该槽是否参与亮/暗自适应（出厂值与亮色值不同）。 */
int appearance_theme_slot_is_adaptive(int slot);

/**
 * @brief 解析一个主题槽的生效颜色。
 *
 * RULE（保证“显式自定义不被静默改写”）：只有 `raw == 出厂默认值` 的字段
 * 才视为“出厂值/自动”，在亮色变体下替换为亮色值；与出厂默认不同的字段
 * 是用户显式自定义，任何变体下都原样返回。
 *
 * @param slot    主题槽（APPEARANCE_THEME_SLOT_*）
 * @param raw     配置里的原值
 * @param variant APPEARANCE_VARIANT_DARK / LIGHT
 * @return 生效颜色；槽越界返回 raw。
 */
int appearance_theme_effective(int slot, int raw, int variant);

/** @brief 主题槽 → AppConfig.theme 字段指针；槽越界或 cfg 为 NULL 返回 NULL。 */
int *appearance_theme_slot_ptr(AppConfig *cfg, int slot);

/**
 * @brief WCAG 相对亮度分类：L >= 0.179 → 亮底，否则暗底。
 *
 * 0.179 是“黑字优于白字”的对比度交叉点（白字 1.05/(L+0.05) 与黑字
 * (L+0.05)/0.05 相等处），比 0.5 更贴近实际可读性。
 */
int appearance_classify_rgb(int r, int g, int b);

/**
 * @brief 扫描缓冲里的 OSC 11 背景色应答（纯函数，供启动探测与常驻过滤器共用）。
 *
 * 支持形态：
 *   - `ESC ] 11 ; rgb:RRRR/GGGG/BBBB ST|BEL`（1~4 位十六进制分量）
 *   - `ESC ] 11 ; #RRGGBB ST|BEL` / `ESC ] 11 ; #RGB ST|BEL`
 *
 * @param buf      输入缓冲；可含前缀垃圾（此时返回 -1、consumed=1）
 * @param len      缓冲长度
 * @param consumed 输出：本次消费的字节数。返回 1 时指向终止符之后；
 *                 返回 0 时为 0（仍需更多字节）；返回 -1 时为 1（只消费
 *                 首字节，其余交给调用方回退），但若已完整匹配
 *                 `ESC ] 11 ;` 前缀而应答内容非法，则 consumed 指向整段
 *                 非法应答之后（调用方应整段吞掉，避免污染按键）。
 * @param r,g,b    输出：解析出的 RGB（0..255）；失败时不修改
 * @return 1 完整合法应答；0 合法但不完整的前缀；-1 不是 OSC 11 应答
 */
int appearance_osc11_scan(const char *buf, size_t len, size_t *consumed,
                          int *r, int *g, int *b);

/**
 * @brief 扫描**无 ESC 锚点**的 OSC 11 余片（`]11;...`，开头 ESC 已被别处读取点消费）。
 *
 * 语义与 appearance_osc11_scan 相同，但前缀少一个 ESC：
 *   - 1 = 完整合法应答；0 = 仍是合法前缀；-1 = 确定不是应答。
 *
 * 注意：本函数只做语法判断；“是否值得吞掉”还要结合
 * appearance_anchorless_should_swallow() 的查询应答窗口约束。
 */
int appearance_osc11_scan_anchorless(const char *buf, size_t len, size_t *consumed,
                                     int *r, int *g, int *b);

/**
 * @brief anchorless 片段的分类（驱动有界收集与回放）。
 *
 * @return 1 = 完整合法的 anchorless OSC11 应答；
 *         0 = 仍是可能成为应答的前缀（可以再读少量字节）；
 *         -1 = 确定不是应答（字面量/非法负载，应立即按用户输入回放）。
 */
int appearance_anchorless_classify(const char *buf, size_t len,
                                   int *r, int *g, int *b);

/**
 * @brief XDG portal SettingChanged 信号匹配（NULL 安全，供 filter 与单测）。
 *
 * libdbus 对缺少 interface/member 的 signal 返回 NULL；filter 必须先用本函数
 * 判定，绝不能直接 strcmp(NULL, ...)。
 *
 * @param interface  消息 interface（可为 NULL）
 * @param member     消息 member（可为 NULL）
 * @param is_signal  消息是否为 DBUS_MESSAGE_TYPE_SIGNAL
 * @return 1 = 是本程序关心的 `org.freedesktop.portal.Settings.SettingChanged`
 */
int appearance_portal_signal_matches(const char *interface, const char *member,
                                     int is_signal);

/** @brief 查询应答窗口是否仍有效（reply_expected 且 now <= deadline）。 */
int appearance_reply_window_active(int reply_expected,
                                   unsigned long long now_ms,
                                   unsigned long long deadline_ms);

/**
 * @brief 判定 anchorless 余片是否应作为终端应答吞掉（O1 加固的核心判定）。
 *
 * 只有同时满足才吞：
 *   1) 仍处于“刚发过 OSC 11 查询”的应答窗口内
 *      （appearance_reply_window_active()，窗口 3s，覆盖约 1s 的迟到应答）；
 *   2) 片段是**完整合法**的 anchorless `]11;rgb:...` / `]11;#...` 应答。
 *
 * 字面量 `]11;`、`]11;hello`、无 BEL/ST 终止符的 `]11;rgb:...` 文本返回 0
 * （按普通用户输入回放）。完整应答必须带 BEL 或 ST 终止符，因此用户逐字输入
 * 的“应答文本”不会被误吞。
 */
int appearance_anchorless_should_swallow(const char *buf, size_t len,
                                         int reply_expected,
                                         unsigned long long now_ms,
                                         unsigned long long deadline_ms,
                                         int *r, int *g, int *b);

/**
 * @brief 解析 COLORFGBG（形如 "fg;bg"），只看最后一个字段。
 *
 * 0-6 与 8 → 暗底；7 与 9-15 → 亮底；"default"/空/越界/非数字 → 未知。
 */
int appearance_parse_colorfgbg(const char *value);

/** @brief 模式 + 探测结果 → 生效变体（auto + unknown → 暗色）。 */
int appearance_resolve_variant(int mode, int detected);

/* ── XDG desktop portal color-scheme（org.freedesktop.appearance） ───── */
#define APPEARANCE_XDG_NO_PREFERENCE 0
#define APPEARANCE_XDG_PREFER_DARK   1
#define APPEARANCE_XDG_PREFER_LIGHT  2

/** @brief portal 的 color-scheme 值 → APPEARANCE_BG_*（0/其它 → UNKNOWN）。 */
int appearance_xdg_color_scheme_to_bg(unsigned int value);

/** @brief 探测来源（Settings 显示 + 优先级裁决）。 */
typedef enum {
    APPEARANCE_SOURCE_FALLBACK = 0,   /* 全部信号未知：保持暗色 */
    APPEARANCE_SOURCE_OSC11,          /* 终端实际底色（权威） */
    APPEARANCE_SOURCE_TERMINAL_THEME, /* 终端自身主题（当前：deepin-terminal） */
    APPEARANCE_SOURCE_XDG,            /* 桌面 portal 偏好 */
    APPEARANCE_SOURCE_COLORFGBG,      /* 环境变量回退 */
    APPEARANCE_SOURCE_TERM,           /* TERM 名称提示 */
    APPEARANCE_SOURCE_EXPLICIT        /* 用户显式 Light/Dark */
} AppearanceSource;

/** @brief 各探测通道的原始信号（前端进程内存；绝不落盘）。 */
typedef struct {
    int osc11;          /* APPEARANCE_BG_*；最后一次有效 OSC11 应答（粘性） */
    int terminal_theme; /* APPEARANCE_BG_*；终端主题适配器（deepin-terminal） */
    int xdg;            /* APPEARANCE_BG_*；portal 值 */
    int colorfgbg;      /* APPEARANCE_BG_* */
    int term;           /* APPEARANCE_BG_* */
} AppearanceSignals;

/** @brief osc11 是否已有过有效应答（决定它是否权威）。 */
int appearance_signal_osc11_known(const AppearanceSignals *signals);

/**
 * @brief 唯一的优先级裁决入口（禁止在别处硬编码优先级）。
 *
 * 规则：显式 Light/Dark 直接赢（source=EXPLICIT）；auto 下按
 * OSC11（实际底色，权威）→ 终端主题适配器 → XDG（桌面偏好）→ COLORFGBG
 * → TERM 取第一个非 UNKNOWN 的信号；全未知 → DARK / FALLBACK。
 *
 * @param mode        APPEARANCE_MODE_*
 * @param signals     各通道信号
 * @param source_out  可选；返回 APPEARANCE_SOURCE_*
 * @return APPEARANCE_VARIANT_DARK / LIGHT
 */
int appearance_resolve_signals(int mode, const AppearanceSignals *signals,
                               int *source_out);

/* ── 按终端身份键（纯逻辑，可单测） ───────────────────────────────── */

/** @brief 身份探测所需的终端环境变量（全部可为 NULL）。 */
typedef struct {
    const char *term_program;   /* $TERM_PROGRAM */
    const char *term;           /* $TERM */
    const char *colorterm;      /* $COLORTERM */
    const char *tmux;           /* $TMUX */
    const char *sty;            /* $STY（screen） */
    const char *ssh_tty;        /* $SSH_TTY */
    const char *ssh_connection; /* $SSH_CONNECTION */
} AppearanceTerminalEnv;

/**
 * @brief 生成稳定的终端身份键（决定“按终端记住外观”的键）。
 *
 * 优先级（高→低）：
 *   1) `$TERM_PROGRAM` → `tp:<...>`；
 *   2) 祖先进程链中的终端模拟器 allowlist（取**最外层**匹配）→ `anc:<comm>`；
 *   3) `$TMUX` → `tmux:<term>`；4) `$STY` → `screen:<term>`；
 *   5) `$SSH_TTY`/`$SSH_CONNECTION` → `ssh:<term>`；6) `$TERM` → `term:<term>`；
 *   7) `$COLORTERM` → `ct:<colorterm>`；8) 都没有 → `default`（不写覆盖键）。
 *
 * 输出统一小写、只保留 `[a-z0-9:._-]`（其它字符→`_`）、最长 96 字节。
 *
 * @param ancestors       祖先进程名数组，顺序为“自身 → 最外层”（可为 NULL）
 * @param ancestor_count  数组长度（最多 8）
 * @return 1 = 具体身份键；0 = 退化到 "default"
 */
int appearance_terminal_key_build(const AppearanceTerminalEnv *env,
                                  const char *const *ancestors,
                                  size_t ancestor_count,
                                  char *out, size_t out_size);

/** @brief 把任意终端标识统一成键格式（小写、[a-z0-9:._-]、NUL 结尾）。 */
void appearance_terminal_key_sanitize(const char *src,
                                      char *out, size_t out_size);

/* ── 终端主题适配器（首个：deepin-terminal；纯文件/路径逻辑） ───────── */
#define APPEARANCE_TERMINAL_THEME_PATH_MAX 512

/**
 * @brief 解析 INI 文本中 `[section] key=value` 的值。
 *
 * 仅做只读文本解析：支持 CRLF、前后空白、`;`/`#` 整行注释、值两端可选引号；
 * 不处理多行值。section/key 精确匹配。
 *
 * @return 1 = 找到（值可为空串）；0 = 未找到或参数非法
 */
int appearance_ini_get_value(const char *text, size_t len,
                             const char *section, const char *key,
                             char *out, size_t out_size);

/**
 * @brief 解析 .colorscheme 的 `[Background] Color=R, G, B`。
 *
 * 仅接受 3 个 0..255 的整数；缺段/缺键/alpha/transparent/损坏 → 0。
 * @return 1 = 解析成功；0 = 失败
 */
int appearance_colorscheme_background(const char *text, size_t len,
                                      int *r, int *g, int *b);

/**
 * @brief 读取 deepin-terminal 配置并解析实际主题背景色（只读）。
 *
 * 解析规则（t22）：
 *   - `[basic.interface.expand_theme] value` 非空：按其值解析
 *     `terminalwidget6/color-schemes/<name>.colorscheme`（搜索 data_home、
 *     data_dirs、/usr/share）；值为 `customTheme` 时读
 *     `<config_dir>/deepin/deepin-terminal/customTheme.colorscheme`；
 *   - 否则用 `[basic.interface.theme] value`（Light/Dark）对应
 *     `terminalwidget6/color-schemes/<theme>.colorscheme`；
 *   - `[Background] Color=R,G,B` 交给 appearance_classify_rgb() 判亮暗。
 *
 * 任何缺失/损坏/非法都返回 0（*bg_out=UNKNOWN），绝不写文件、不崩溃。
 * `config_path_out`/`scheme_path_out` 会尽量填出候选路径（scheme 可为缺失的
 * 首选候选），以便运行期用 stat(mtime+size) 检测后续创建/修改。
 *
 * @return 1 = 成功判明亮暗；0 = unknown
 */
int appearance_deepin_theme_bg(const char *config_dir,
                               const char *data_home,
                               const char *const *data_dirs,
                               size_t data_dir_count,
                               char *config_path_out, size_t config_path_size,
                               char *scheme_path_out, size_t scheme_path_size,
                               int *bg_out);

/**
 * @brief 纯逻辑身份判定：当前环境是否为 deepin-terminal。
 *
 * `$TERM_PROGRAM` 为 deepin-terminal，或祖先进程名匹配 deepin-terminal
 * （comm 可能被截到 15 字符）时为 1。其它终端一律 0。
 */
int appearance_terminal_is_deepin_env(const AppearanceTerminalEnv *env,
                                      const char *const *ancestors,
                                      size_t ancestor_count);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_APPEARANCE_H */
