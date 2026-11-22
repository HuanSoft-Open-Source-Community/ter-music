/**
 * @file appearance.c
 * @brief 前端运行期终端亮/暗探测：I/O、输入安全与重绘接线
 *
 * 设计要点（详见 t2 设计报告与 config/appearance.h）：
 *   - 只在 appearance_mode=auto 时探测；启动同步探测上限 200ms；
 *   - 回退链 OSC 11 → COLORFGBG → TERM 弱启发 → 保持暗色；
 *   - 主输入路径常驻 OSC 11 应答过滤器：应答被整段吞掉，绝不作为按键；
 *   - 焦点上报触发非阻塞重探，迟到应答同样被过滤器接管；
 *   - 探测结果只存在于本进程内存，绝不写配置。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/appearance.h"

#include "config/appearance.h"
#include "config/config.h"
#include "logger/logger.h"
#include "types.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "ui/appearance_portal.h"
#include "ui/appearance_terminal_theme.h"
#include "ui/appearance_terminal.h"

#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define APPEARANCE_QUERY       "\033]11;?\033\\"
#define APPEARANCE_FOCUS_ON    "\033[?1004h"
#define APPEARANCE_FOCUS_OFF   "\033[?1004l"
#define APPEARANCE_INPUT_TIMEOUT_MS 40   /* 与 ui.c 的 UI_INPUT_TIMEOUT_MS 一致 */
#define APPEARANCE_POLL_TIMEOUT_MS  10
#define APPEARANCE_STARTUP_BUDGET_MS 200
#define APPEARANCE_PORTAL_BUDGET_MS  120
#define APPEARANCE_MANUAL_BUDGET_MS  250
#define APPEARANCE_REPLY_WINDOW_MS   3000
#define APPEARANCE_PENDING_MAX       512
/* O1 加固：anchorless 收集的硬上限（时间窗 + 字节上限），
 * 到点/到量即原序回放，绝不定格成吞键状态。 */
#define APPEARANCE_ANCHORLESS_WINDOW_MS 80
#define APPEARANCE_ANCHORLESS_MAX       256

/* ── 进程内状态 ───────────────────────────────────────────────────── */

static int g_global_mode = APPEARANCE_MODE_AUTO; /* config.xml 默认 */
static int g_mode = APPEARANCE_MODE_AUTO;        /* 生效模式（终端覆盖优先） */
static AppearanceSignals g_signals;              /* 各探测通道信号 */
static int g_source = APPEARANCE_SOURCE_FALLBACK;
static int g_startup_probed = 0;
static int g_last_effective_mode = -1;   /* 上次 resolve 后的生效模式（F4） */
static int g_sync_calls = 0;
static int g_variant = APPEARANCE_VARIANT_DARK;

/* XDG 运行期状态（单线程节拍） */
static unsigned long long g_next_tick_ms = 0;
static unsigned long long g_last_osc11_query_ms = 0;
static unsigned long long g_xdg_apply_at_ms = 0;
static int g_xdg_pending = 0;
static int g_portal_fallback_pending = 0;   /* 适配器失效后的异步 portal 回退 */
static int g_osc11_reprobe_scheduled = 0;
static unsigned long long g_osc11_reprobe_due_ms = 0;

static int g_focus_reporting = 0;
static int g_focus_in = -1;
static int g_focus_out = -1;

/* “最近确实发出过查询”的窗口：只影响模态输入框前的悬挂应答清理。 */
static int g_reply_expected = 0;
static unsigned long long g_reply_deadline_ms = 0;

/* 供主循环回放的按键（ESC 消歧、探测期用户按键）。 */
static int g_pending[APPEARANCE_PENDING_MAX];
static int g_pending_head = 0;
static int g_pending_tail = 0;

/* OSC 11 应答收集状态机。 */
enum {
    OSC_IDLE = 0,
    OSC_SAW_ESC,       /* ESC */
    OSC_SAW_BRACKET,   /* ESC ] */
    OSC_SAW_ONE,       /* ESC ] 1 */
    OSC_SAW_11,        /* ESC ] 11 */
    OSC_PAYLOAD,       /* ESC ] 11 ; ... 收集到 BEL/ST 为止 */
    OSC_PAYLOAD_ST     /* 负载中已见 ESC，等待 '\\' */
};

static int g_osc_state = OSC_IDLE;
static char g_osc_buf[256];
static size_t g_osc_len = 0;

/* 无 ESC 锚点的有界收集状态（仅当处于 OSC11 查询应答窗口内才会启用）。 */
static int g_anchorless_active = 0;
static char g_anchorless_buf[APPEARANCE_ANCHORLESS_MAX];
static size_t g_anchorless_len = 0;
static unsigned long long g_anchorless_deadline_ms = 0;
static int g_anchorless_key = 0;   /* 收集中撞到的功能键码（>255），回放用 */

/* ── 时间与输出 ───────────────────────────────────────────────────── */

static unsigned long long appearance_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

static void appearance_write(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    fputs(text, stdout);
    fflush(stdout);
}

static void appearance_send_query(void)
{
    appearance_write(APPEARANCE_QUERY);
    g_reply_expected = 1;
    g_reply_deadline_ms = appearance_now_ms() + APPEARANCE_REPLY_WINDOW_MS;
    g_last_osc11_query_ms = appearance_now_ms();
}

/* 从任意窗口读取（win=NULL 时等价 getch）。所有输入过滤入口共用。 */
static int appearance_read(WINDOW *win)
{
    return win ? wgetch(win) : getch();
}

/* 设置任意窗口的读取超时（win=NULL 时等价 timeout）。 */
static void appearance_set_timeout(WINDOW *win, int ms)
{
    if (win) {
        wtimeout(win, ms);
    } else {
        timeout(ms);
    }
}

/* ── pending 按键队列（FIFO） ─────────────────────────────────────── */

static int pending_count(void)
{
    return (g_pending_tail - g_pending_head + APPEARANCE_PENDING_MAX) %
           APPEARANCE_PENDING_MAX;
}

static void pending_push(int ch)
{
    int next = (g_pending_tail + 1) % APPEARANCE_PENDING_MAX;
    if (next == g_pending_head) {
        log_warn("appearance", "pending key queue full, dropping %d", ch);
        return;
    }
    g_pending[g_pending_tail] = ch;
    g_pending_tail = next;
}

static int pending_pop(int *out)
{
    if (g_pending_head == g_pending_tail) {
        return 0;
    }
    if (out) {
        *out = g_pending[g_pending_head];
    }
    g_pending_head = (g_pending_head + 1) % APPEARANCE_PENDING_MAX;
    return 1;
}

/* ── OSC 11 应答状态机 ────────────────────────────────────────────── */

enum {
    FEED_KEY = 0,          /* 当前读到的字节是普通按键（由调用方决定返回/入队） */
    FEED_CONSUMED = 1,     /* 已吞掉，继续读 */
    FEED_REPLY_VALID = 2,  /* 完整合法应答已处理 */
    FEED_REPLY_INVALID = 3 /* 匹配前缀但内容非法，已整段吞掉 */
};

static void osc_reset(void)
{
    g_osc_state = OSC_IDLE;
    g_osc_len = 0;
}

static void osc_append(int ch)
{
    if (g_osc_len < sizeof(g_osc_buf) - 1) {
        g_osc_buf[g_osc_len++] = (char)ch;
    }
}

/* 把当前缓冲里的完整序列交给纯解析器；返回 FEED_REPLY_*。 */
static int osc_finish(void)
{
    size_t consumed = 0;
    size_t reply_len = g_osc_len;
    int r = 0, g = 0, b = 0;
    int rc = appearance_osc11_scan(g_osc_buf, g_osc_len, &consumed, &r, &g, &b);
    osc_reset();
    g_reply_expected = 0;

    if (rc == 1) {
        g_signals.osc11 = appearance_classify_rgb(r, g, b);
        log_info("appearance", "OSC 11 background rgb=%d,%d,%d -> %s",
                 r, g, b,
                 g_signals.osc11 == APPEARANCE_BG_LIGHT ? "light" : "dark");
        return FEED_REPLY_VALID;
    }
    log_warn("appearance", "Discarding malformed OSC 11 reply (%zu bytes)", reply_len);
    return FEED_REPLY_INVALID;
}

/* 探测到的“最高优先级背景”（不含显式模式），供 Settings/日志使用。 */
static int appearance_pick_detected(void)
{
    if (g_signals.osc11 != APPEARANCE_BG_UNKNOWN) return g_signals.osc11;
    if (g_signals.terminal_theme != APPEARANCE_BG_UNKNOWN) return g_signals.terminal_theme;
    if (g_signals.xdg != APPEARANCE_BG_UNKNOWN) return g_signals.xdg;
    if (g_signals.colorfgbg != APPEARANCE_BG_UNKNOWN) return g_signals.colorfgbg;
    if (g_signals.term != APPEARANCE_BG_UNKNOWN) return g_signals.term;
    return APPEARANCE_BG_UNKNOWN;
}

/* 统一裁决：唯一硬编码优先级的地方在 appearance_resolve_signals()。 */
static int appearance_resolve_now(void)
{
    int source = APPEARANCE_SOURCE_FALLBACK;
    int variant = appearance_resolve_signals(g_mode, &g_signals, &source);
    g_source = source;
    g_variant = variant;
    return variant;
}

/* 重新应用变体（若变化）：重刷主题/布局 pair + 标脏；不主动重绘当前视图。
 * 模态输入框前清理悬挂应答时用这个轻量版本，避免盖掉正在展开的弹层。 */
static void appearance_apply_pairs_if_changed(void)
{
    int previous = g_variant;
    int variant = appearance_resolve_now();
    if (variant == previous) {
        return;
    }
    log_info("appearance", "Appearance variant -> %s (source=%d)",
             variant == APPEARANCE_VARIANT_LIGHT ? "light" : "dark", g_source);
    apply_color_theme();
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                       UI_DIRTY_LYRICS | UI_DIRTY_INFO);
}

/* 重新应用变体（若变化）：重刷 pair 并标脏。
 *
 * 刻意**不**在这里调用 rerender_active_view()：本函数会在模态输入循环读输入
 * 时被触达（应答/焦点重探可能发生在搜索框、帮助搜索、语言包路径框打开期间），
 * 立即整屏重绘会把模态提示行盖掉，看起来像“应答取消了模态框”。主循环会在
 * 同一轮/下一轮消费 request_ui_refresh() 完成重绘；模态循环关掉后自然恢复。 */
static void appearance_apply_variant_if_changed(void)
{
    appearance_apply_pairs_if_changed();
}

/* 处理一个输入字节。返回 FEED_KEY 时，*key_out 给出应当回放的按键
 * （普通字节原样，ESC 前缀不匹配时是 27）；其余已入队的字节会经
 * pending 队列按序回放。 */
static int osc_feed_byte(int ch, int *key_out)
{
    if (key_out) {
        *key_out = ch;
    }
    switch (g_osc_state) {
        case OSC_IDLE:
            if (ch == 27) {
                osc_append(ch);
                g_osc_state = OSC_SAW_ESC;
                return FEED_CONSUMED;
            }
            return FEED_KEY;

        case OSC_SAW_ESC:
            if (ch == ']') {
                osc_append(ch);
                g_osc_state = OSC_SAW_BRACKET;
                return FEED_CONSUMED;
            }
            pending_push(ch);   /* ESC 后面的字节原序回放 */
            osc_reset();
            if (key_out) *key_out = 27;
            return FEED_KEY;    /* ESC 本身是按键 */

        case OSC_SAW_BRACKET:
            if (ch == '1') {
                osc_append(ch);
                g_osc_state = OSC_SAW_ONE;
                return FEED_CONSUMED;
            }
            pending_push(']');
            pending_push(ch);
            osc_reset();
            if (key_out) *key_out = 27;
            return FEED_KEY;

        case OSC_SAW_ONE:
            if (ch == '1') {
                osc_append(ch);
                g_osc_state = OSC_SAW_11;
                return FEED_CONSUMED;
            }
            pending_push(']');
            pending_push('1');
            pending_push(ch);
            osc_reset();
            if (key_out) *key_out = 27;
            return FEED_KEY;

        case OSC_SAW_11:
            if (ch == ';') {
                osc_append(ch);
                g_osc_state = OSC_PAYLOAD;
                return FEED_CONSUMED;
            }
            pending_push(']');
            pending_push('1');
            pending_push('1');
            pending_push(ch);
            osc_reset();
            if (key_out) *key_out = 27;
            return FEED_KEY;

        case OSC_PAYLOAD:
            if (ch == 0x07) {            /* BEL */
                osc_append(ch);
                return osc_finish();
            }
            if (ch == 27) {
                osc_append(ch);
                g_osc_state = OSC_PAYLOAD_ST;
                return FEED_CONSUMED;
            }
            osc_append(ch);
            if (g_osc_len >= sizeof(g_osc_buf) - 1) {
                log_warn("appearance", "OSC 11 reply too long, discarding");
                osc_reset();
                g_reply_expected = 0;
                return FEED_REPLY_INVALID;
            }
            return FEED_CONSUMED;

        case OSC_PAYLOAD_ST:
            if (ch == '\\') {            /* ST */
                osc_append(ch);
                return osc_finish();
            }
            /* 负载里出现裸 ESC：继续按负载字符收集，等待真正的终止符。 */
            osc_append(ch);
            g_osc_state = OSC_PAYLOAD;
            return FEED_CONSUMED;

        default:
            osc_reset();
            return FEED_CONSUMED;
    }
}

/* 已经确认 ESC ] 11 ; 前缀，继续非阻塞收集负载（窗口版本，win=NULL=stdscr）。
 * @return 1 = 已吞掉一段应答（继续取下一键）；0 = 仍不完整（等下一次输入）。 */
static int osc_collect_nonblocking(WINDOW *win)
{
    appearance_set_timeout(win, 0);
    for (;;) {
        int ch = appearance_read(win);
        if (ch == ERR) {
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 0;
        }
        int key = 0;
        int rc = osc_feed_byte(ch, &key);
        if (rc == FEED_REPLY_VALID || rc == FEED_REPLY_INVALID) {
            if (rc == FEED_REPLY_VALID) {
                /* 运行期迟到应答：按新背景重算变体并重绘。 */
                appearance_apply_variant_if_changed();
            }
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 1;
        }
        if (rc == FEED_KEY) {
            /* 只可能来自 OSC_SAW_* 状态：ESC 作为按键入队，其余字节已入队。 */
            pending_push(key ? key : 27);
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 1;
        }
    }
}

/* 当前没有输入时，把“还不完整的 ESC / ESC ] 1 ...”复位成普通按键。 */
static void osc_flush_idle_prefix(void)
{
    if (g_osc_state == OSC_SAW_ESC) {
        pending_push(27);
        osc_reset();
        return;
    }
    if (g_osc_state == OSC_SAW_BRACKET || g_osc_state == OSC_SAW_ONE ||
        g_osc_state == OSC_SAW_11) {
        for (size_t i = 1; i < g_osc_len; i++) {
            pending_push((unsigned char)g_osc_buf[i]);
        }
        osc_reset();
    }
    /* OSC_PAYLOAD / OSC_PAYLOAD_ST：应答正在到达，保持状态等下一次输入。 */
}

/* ── 终端能力与探测 ──────────────────────────────────────────────── */

static int appearance_probe_supported(void)
{
    const char *term = getenv("TERM");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return 0;
    }
    if (!term || !term[0] || strcmp(term, "dumb") == 0) {
        return 0;
    }
    return 1;
}

/* 同步探测一次 OSC 11；返回 APPEARANCE_BG_*（UNKNOWN = 没应答/非法）。 */
static int appearance_probe_osc11(int budget_ms)
{
    if (!appearance_probe_supported()) {
        return APPEARANCE_BG_UNKNOWN;
    }
    appearance_send_query();
    int previous = g_signals.osc11;
    int reply_valid = 0;
    int reply_seen = 0;
    g_signals.osc11 = APPEARANCE_BG_UNKNOWN;

    unsigned long long deadline = appearance_now_ms() + (unsigned long long)budget_ms;
    timeout(APPEARANCE_POLL_TIMEOUT_MS);
    while (appearance_now_ms() < deadline) {
        int ch = getch();
        if (ch == ERR) {
            osc_flush_idle_prefix();
            continue;
        }
        int key = 0;
        int rc = osc_feed_byte(ch, &key);
        if (rc == FEED_REPLY_VALID) {
            reply_valid = 1;
            reply_seen = 1;
            break;
        }
        if (rc == FEED_REPLY_INVALID) {
            reply_seen = 1;
            break;
        }
        if (rc == FEED_KEY) {
            pending_push(key ? key : 27);   /* 探测期间的用户按键原序保留 */
        }
    }
    timeout(APPEARANCE_INPUT_TIMEOUT_MS);
    osc_flush_idle_prefix();

    if (reply_valid) {
        return g_signals.osc11;
    }
    g_signals.osc11 = previous;
    if (!reply_seen) {
        log_info("appearance", "No OSC 11 reply within %dms", budget_ms);
    }
    return APPEARANCE_BG_UNKNOWN;
}

static int appearance_term_hint(void)
{
    const char *term = getenv("TERM");
    if (!term) {
        return APPEARANCE_BG_UNKNOWN;
    }
    if (strstr(term, "light")) {
        return APPEARANCE_BG_LIGHT;
    }
    if (strstr(term, "dark")) {
        return APPEARANCE_BG_DARK;
    }
    return APPEARANCE_BG_UNKNOWN;
}

/* 启动/重探：填充各通道信号（不做裁决、不重绘；调用方统一 resolve/apply）。
 * 优先级顺序由 appearance_resolve_signals() 决定，探测顺序只影响成本。 */
static void appearance_collect_signals(int osc_budget_ms, int portal_budget_ms)
{
    g_signals.osc11 = appearance_probe_osc11(osc_budget_ms);
    if (g_signals.osc11 != APPEARANCE_BG_UNKNOWN) {
        log_info("appearance", "Background detected via OSC 11");
    } else {
        int theme_bg = APPEARANCE_BG_UNKNOWN;
        int theme_known = appearance_terminal_theme_supported() &&
                          appearance_terminal_theme_probe(&theme_bg);
        if (theme_known) {
            g_signals.terminal_theme = theme_bg;
            log_info("appearance", "Background from terminal theme adapter=%d",
                     theme_bg);
        } else {
            int bg = APPEARANCE_BG_UNKNOWN;
            if (appearance_portal_init() &&
                appearance_portal_read(portal_budget_ms, &bg) == 1) {
                g_signals.xdg = bg;
                log_info("appearance", "Background from XDG portal color-scheme=%d", bg);
            }
        }
        g_signals.colorfgbg = appearance_parse_colorfgbg(getenv("COLORFGBG"));
        if (g_signals.colorfgbg != APPEARANCE_BG_UNKNOWN) {
            log_info("appearance", "Background detected via COLORFGBG=%s",
                     getenv("COLORFGBG"));
        }
        g_signals.term = appearance_term_hint();
        if (g_signals.term != APPEARANCE_BG_UNKNOWN) {
            log_info("appearance", "Background inferred from TERM=%s", getenv("TERM"));
        }
    }
    if (appearance_pick_detected() == APPEARANCE_BG_UNKNOWN) {
        log_info("appearance", "Background unknown; keeping the legacy dark look");
    }
}
/* ── 焦点上报（运行期重探触发器） ────────────────────────────────── */

static void appearance_enable_focus_reporting(void)
{
    if (g_focus_reporting) {
        return;
    }
    if (!appearance_probe_supported()) {
        return;
    }
    g_focus_in = key_defined("\033[I");
    g_focus_out = key_defined("\033[O");
    if (g_focus_in <= 0 || g_focus_out <= 0) {
        log_info("appearance", "termInfo has no focus keys; runtime re-detect disabled");
        g_focus_in = -1;
        g_focus_out = -1;
        return;
    }
    appearance_write(APPEARANCE_FOCUS_ON);
    g_focus_reporting = 1;
    log_info("appearance", "Focus reporting enabled (in=%d out=%d)",
             g_focus_in, g_focus_out);
}

static void appearance_disable_focus_reporting(void)
{
    if (!g_focus_reporting) {
        return;
    }
    appearance_write(APPEARANCE_FOCUS_OFF);
    g_focus_reporting = 0;
}

/* ── 公开 API ─────────────────────────────────────────────────────── */

int appearance_variant(void)
{
    return appearance_resolve_signals(g_mode, &g_signals, NULL);
}

int appearance_detected(void)
{
    return appearance_pick_detected();
}

int appearance_source(void)
{
    int source = APPEARANCE_SOURCE_FALLBACK;
    (void)appearance_resolve_signals(g_mode, &g_signals, &source);
    return source;
}

int appearance_detection_source(void)
{
    int source = APPEARANCE_SOURCE_FALLBACK;
    (void)appearance_resolve_signals(APPEARANCE_MODE_AUTO, &g_signals, &source);
    return source;
}

int appearance_global_mode(void)
{
    return g_global_mode;
}

/* 每约 100ms 一次的非阻塞节拍：portal 消息泵 + SettingChanged 去抖 + OSC11 重探。 */
void appearance_tick(void)
{
    unsigned long long now = appearance_now_ms();
    if (now < g_next_tick_ms) {
        return;
    }
    g_next_tick_ms = now + 100;

    /* 应答窗口过期：必须清掉 g_reply_expected（并复位半截 OSC 状态），
     * 否则一次无人应答的重探会让后续 XDG 变化永远无法再触发重探（F5）。 */
    if (g_reply_expected && now > g_reply_deadline_ms) {
        g_reply_expected = 0;
        if (g_osc_state != OSC_IDLE) {
            osc_reset();
        }
    }

    appearance_portal_poll();

    /* 适配器失效后的 portal 回退：异步结果在 poll 中推进，绝不阻塞输入。 */
    if (g_portal_fallback_pending) {
        int fallback_bg = APPEARANCE_BG_UNKNOWN;
        if (appearance_portal_take_async_result(&fallback_bg)) {
            g_portal_fallback_pending = 0;
            if (fallback_bg != APPEARANCE_BG_UNKNOWN) {
                g_signals.xdg = fallback_bg;
            }
            log_info("appearance", "Async XDG fallback completed: bg=%d",
                     fallback_bg);
            appearance_apply_pairs_if_changed();
        }
    }

    int bg = APPEARANCE_BG_UNKNOWN;
    if (appearance_portal_take_changed(&bg)) {
        g_signals.xdg = bg;
        g_xdg_pending = 1;
        g_xdg_apply_at_ms = now + 300;   /* 合并 300ms 内的信号风暴 */
    }
    if (g_xdg_pending && now >= g_xdg_apply_at_ms) {
        g_xdg_pending = 0;
        if (g_mode != APPEARANCE_MODE_AUTO) {
            /* 显式模式不跟随系统外观。 */
        } else if (appearance_signal_osc11_known(&g_signals)) {
            /* OSC11 是实际底色（权威）：XDG 变化不直接改观感，只安排一次
             * 去抖的非阻塞 OSC11 重探（≤1 次/500ms）；查询在途时不重发。 */
            if (!g_osc11_reprobe_scheduled) {
                g_osc11_reprobe_scheduled = 1;
                g_osc11_reprobe_due_ms = now + 500;
                if (g_osc11_reprobe_due_ms < g_last_osc11_query_ms + 500) {
                    g_osc11_reprobe_due_ms = g_last_osc11_query_ms + 500;
                }
            }
        } else {
            appearance_apply_pairs_if_changed();
        }
    }

    if (g_osc11_reprobe_scheduled && now >= g_osc11_reprobe_due_ms &&
        !g_reply_expected) {
        appearance_send_query();
        g_osc11_reprobe_scheduled = 0;
    }

    /* 终端主题适配器：仅在 auto 且身份匹配时低频轮询（模块内部 2s 节流）。 */
    if (g_mode == APPEARANCE_MODE_AUTO && appearance_terminal_theme_supported()) {
        appearance_terminal_theme_tick();
        int theme_bg = APPEARANCE_BG_UNKNOWN;
        if (appearance_terminal_theme_take_changed(&theme_bg)) {
            log_info("appearance", "Terminal theme adapter change: bg=%d",
                     theme_bg);
            if (theme_bg != APPEARANCE_BG_UNKNOWN) {
                g_signals.terminal_theme = theme_bg;
            } else {
                g_signals.terminal_theme = APPEARANCE_BG_UNKNOWN;
                g_signals.xdg = APPEARANCE_BG_UNKNOWN;
                /* 适配器失效：回退原链。portal 读取必须异步，不能在输入路径
                 * 调 send_with_reply_and_block（t24 F2）。本次 tick 先按现有
                 * 信号回退，结果由后续 poll 取回后再重新裁决。 */
                g_portal_fallback_pending =
                    appearance_portal_request_async_read();
                log_info("appearance",
                         "Terminal theme lost; async XDG fallback requested=%d",
                         g_portal_fallback_pending);
            }
            appearance_apply_pairs_if_changed();
        }
    }
}

void appearance_sync_from_config(void)
{
    int global = g_app_config.appearance_mode;
    if (global < APPEARANCE_MODE_AUTO || global >= APPEARANCE_MODE_COUNT) {
        global = APPEARANCE_MODE_AUTO;
    }
    g_global_mode = global;

    appearance_terminal_init();
    int override = appearance_terminal_override_get();
    int previous_mode = g_last_effective_mode;
    g_mode = (override >= 0 && override < APPEARANCE_MODE_COUNT) ? override : global;

    if (g_mode == APPEARANCE_MODE_AUTO) {
        /* F4：从非 Auto 进入 Auto（Settings/覆盖切换）时必须重新完整采集，
         * 不能沿用切到显式模式之前的旧信号。预算复用启动链（≤320ms）。 */
        int reenter_auto = (previous_mode >= 0 &&
                            previous_mode != APPEARANCE_MODE_AUTO);
        if (!g_startup_probed || reenter_auto) {
            g_startup_probed = 1;
            if (reenter_auto) {
                memset(&g_signals, 0, sizeof(g_signals));
                g_source = APPEARANCE_SOURCE_FALLBACK;
                g_portal_fallback_pending = 0;
            }
            appearance_collect_signals(APPEARANCE_STARTUP_BUDGET_MS,
                                       APPEARANCE_PORTAL_BUDGET_MS);
        }
        appearance_enable_focus_reporting();
        (void)appearance_portal_init();   /* 已有连接/无总线时立即返回 */
    } else {
        appearance_disable_focus_reporting();
        appearance_portal_shutdown();
    }

    appearance_resolve_now();
    g_last_effective_mode = g_mode;
    g_sync_calls++;
    log_info("appearance",
             "Mode=%d (global=%d override=%d) detected=%d source=%d variant=%d (sync #%d)",
             g_mode, g_global_mode, override, appearance_pick_detected(),
             g_source, g_variant, g_sync_calls);
}

int appearance_force_probe(void)
{
    /* 完整链路：清缓存 → portal 200ms → OSC11 250ms → COLORFGBG → TERM → 裁决。 */
    memset(&g_signals, 0, sizeof(g_signals));
    g_source = APPEARANCE_SOURCE_FALLBACK;
    g_portal_fallback_pending = 0;

    int theme_bg = APPEARANCE_BG_UNKNOWN;
    int theme_known = appearance_terminal_theme_supported() &&
                      appearance_terminal_theme_probe(&theme_bg);
    if (theme_known) {
        g_signals.terminal_theme = theme_bg;
    } else {
        int bg = APPEARANCE_BG_UNKNOWN;
        if (appearance_portal_init() &&
            appearance_portal_read(APPEARANCE_MANUAL_BUDGET_MS, &bg) == 1) {
            g_signals.xdg = bg;
        }
    }
    g_signals.osc11 = appearance_probe_osc11(APPEARANCE_MANUAL_BUDGET_MS);
    g_signals.colorfgbg = appearance_parse_colorfgbg(getenv("COLORFGBG"));
    g_signals.term = appearance_term_hint();

    appearance_apply_variant_if_changed();
    appearance_resolve_now();   /* 即使变体没变也刷新 source */
    return appearance_pick_detected();
}
/* ── 无 ESC 锚点的第二道防线（O1 加固） ─────────────────────────────
 * 终端应答开头的 ESC 可能已被别的读取点消费，只剩 `]11;rgb:...`。该路径既要
 * 阻止应答字节泄漏进主分发，又绝不能把用户输入/粘贴的同一字面量误当应答吞掉。
 *
 * 设计依据与取舍（时间窗/缓冲区均写死，不靠猜测）：
 *   1) 只在 `g_reply_expected` 的查询窗口内考虑（发送 OSC11 查询后 3s），给约
 *      1s 的迟到应答留足余量；窗口外一律视为用户输入，零读取、零延迟放行。
 *   2) 只吞“完整合法”的 `]11;rgb:...` / `]11;#...`，且必须带 BEL/ST 终止符。
 *      字面量 `]11;`、`]11;hello`、以及没有终止符的 `]11;rgb:...` 文本永远
 *      是“不完整片段”，会在有界窗口结束时按原序回放。
 *   3) 收集有界且非阻塞：最多 APPEARANCE_ANCHORLESS_WINDOW_MS（80ms，约两个
 *      输入节拍）或 APPEARANCE_ANCHORLESS_MAX 字节后强制回放；没有 sleep 或
 *      阻塞读取，因此不会出现“无限收集、吞掉后续按键”的死状态。
 *   4) 不采用“窗口内已有真实按键到过就否决”的判据：t5 F1 里 F8、`/` 等按键
 *      先于迟到应答到达，按“有按键就否决”会让合法应答泄漏。可信度由
 *      “完整合法应答 + 仍在查询窗口”共同决定。
 *   5) ESC 锚定的应答仍走原有 OSC_PAYLOAD 状态机（支持拆包），不受影响。
 */
enum {
    ANCHORLESS_NEED_MORE = 0,
    ANCHORLESS_SWALLOWED = 1,
    ANCHORLESS_REPLAY = 2
};

static void anchorless_reset(void)
{
    g_anchorless_active = 0;
    g_anchorless_len = 0;
    g_anchorless_deadline_ms = 0;
    g_anchorless_key = 0;
}

/* 把已读到的候选字节（含开头的 ']'）按原序放回 pending 队列。 */
static void anchorless_replay(void)
{
    for (size_t i = 0; i < g_anchorless_len; i++) {
        pending_push((unsigned char)g_anchorless_buf[i]);
    }
    if (g_anchorless_key) {
        pending_push(g_anchorless_key);
    }
    anchorless_reset();
}

/* 非阻塞推进 anchorless 收集。 */
static int anchorless_poll(WINDOW *win)
{
    int r = 0, g = 0, b = 0;
    if (!g_anchorless_active) {
        return ANCHORLESS_REPLAY;
    }
    appearance_set_timeout(win, 0);
    for (;;) {
        unsigned long long now = appearance_now_ms();
        if (!appearance_reply_window_active(g_reply_expected, now,
                                            g_reply_deadline_ms)) {
            break;   /* 查询窗口失效：立刻按用户输入回放 */
        }
        int cls = appearance_anchorless_classify(g_anchorless_buf,
                                                 g_anchorless_len, &r, &g, &b);
        if (cls == 1) {
            if (!appearance_anchorless_should_swallow(g_anchorless_buf,
                                                      g_anchorless_len,
                                                      g_reply_expected, now,
                                                      g_reply_deadline_ms,
                                                      &r, &g, &b)) {
                break;
            }
            g_signals.osc11 = appearance_classify_rgb(r, g, b);
            g_reply_expected = 0;
            log_info("appearance",
                     "Anchorless OSC 11 reply rgb=%d,%d,%d -> %s",
                     r, g, b,
                     g_signals.osc11 == APPEARANCE_BG_LIGHT ? "light" : "dark");
            anchorless_reset();
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            appearance_apply_variant_if_changed();
            return ANCHORLESS_SWALLOWED;
        }
        if (cls < 0 || g_anchorless_len >= APPEARANCE_ANCHORLESS_MAX ||
            now >= g_anchorless_deadline_ms) {
            break;
        }
        int c = appearance_read(win);
        if (c == ERR) {
            if (appearance_now_ms() >= g_anchorless_deadline_ms) {
                break;
            }
            /* 暂无更多字节：不阻塞，交给调用方的输入节拍继续推进。 */
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return ANCHORLESS_NEED_MORE;
        }
        if (c < 0 || c > 255) {
            g_anchorless_key = c;
            break;
        }
        g_anchorless_buf[g_anchorless_len++] = (char)c;
    }
    appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
    anchorless_replay();
    return ANCHORLESS_REPLAY;
}

static int appearance_wgetch_impl(WINDOW *win)
{
    int key = 0;
    appearance_tick();
    if (pending_pop(&key)) {
        return key;
    }

    if (g_anchorless_active) {
        int rc = anchorless_poll(win);
        if (rc == ANCHORLESS_NEED_MORE) {
            return ERR;
        }
        if (rc == ANCHORLESS_REPLAY) {
            if (pending_pop(&key)) {
                return key;
            }
            return ERR;
        }
        /* 应答已吞掉：继续按调用方超时取下一个真实按键。 */
    }

    if (g_osc_state != OSC_IDLE) {
        int swallowed = osc_collect_nonblocking(win);
        if (pending_pop(&key)) {
            return key;
        }
        if (!swallowed) {
            return ERR;
        }
        /* 应答已吞掉：继续按调用方的超时取下一个真实按键。 */
    }

    for (;;) {
        int ch = appearance_read(win);
        if (ch == ERR) {
            osc_flush_idle_prefix();
            if (pending_pop(&key)) {
                return key;
            }
            return ERR;
        }
        if (ch != 27 && ch != ']') {
            return ch;
        }

        /* 第二道防线：ESC 可能已被别的读取点消费，只剩 `]11;` 余字节。
         * 仅在查询应答窗口内、且有界收集确认是完整合法应答时才吞；
         * 其它情况（含字面量 `]11;`）立即或最多 80ms 后原序回放。 */
        if (ch == ']') {
            unsigned long long now = appearance_now_ms();
            if (!appearance_reply_window_active(g_reply_expected, now,
                                                g_reply_deadline_ms)) {
                return ']';   /* 窗口外：普通输入，零开销直接放行 */
            }
            g_anchorless_active = 1;
            g_anchorless_buf[0] = ']';
            g_anchorless_len = 1;
            g_anchorless_key = 0;
            g_anchorless_deadline_ms = now + APPEARANCE_ANCHORLESS_WINDOW_MS;
            int rc = anchorless_poll(win);
            if (rc == ANCHORLESS_SWALLOWED) {
                if (pending_pop(&key)) {
                    return key;
                }
                continue;
            }
            if (rc == ANCHORLESS_REPLAY) {
                if (pending_pop(&key)) {
                    return key;
                }
                return ERR;
            }
            return ERR;   /* NEED_MORE：最多等待 80ms，下一次调用继续推进 */
        }

        /* ESC 消歧：立即看下一字节，不阻塞、不改变 ESC 的即时语义。 */
        appearance_set_timeout(win, 0);
        int n1 = appearance_read(win);
        if (n1 == ERR) {
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        if (n1 != ']') {
            pending_push(n1);
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        int n2 = appearance_read(win);
        if (n2 == ERR) {
            pending_push(']');
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        if (n2 != '1') {
            pending_push(']');
            pending_push(n2);
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        int n3 = appearance_read(win);
        if (n3 == ERR) {
            pending_push(']');
            pending_push('1');
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        if (n3 != '1') {
            pending_push(']');
            pending_push('1');
            pending_push(n3);
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        int n4 = appearance_read(win);
        if (n4 == ERR) {
            pending_push(']');
            pending_push('1');
            pending_push('1');
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }
        if (n4 != ';') {
            pending_push(']');
            pending_push('1');
            pending_push('1');
            pending_push(n4);
            appearance_set_timeout(win, APPEARANCE_INPUT_TIMEOUT_MS);
            return 27;
        }

        /* 已确认 ESC ] 11 ; —— 开始收集负载；绝不把这段字节当按键。 */
        osc_reset();
        osc_append(27);
        osc_append(']');
        osc_append('1');
        osc_append('1');
        osc_append(';');
        g_osc_state = OSC_PAYLOAD;
        int swallowed = osc_collect_nonblocking(win);
        if (pending_pop(&key)) {
            return key;
        }
        if (!swallowed) {
            return ERR;   /* 应答分片到达：余下字节下次输入时继续吞 */
        }
    }
}

int appearance_getch(void)
{
    return appearance_wgetch_impl(NULL);
}

int appearance_wgetch(WINDOW *win)
{
    return appearance_wgetch_impl(win);
}

int appearance_handle_focus(int ch)
{
    if (!g_focus_reporting) {
        return 0;
    }
    if (g_focus_in > 0 && ch == g_focus_in) {
        /* 焦点进入：非阻塞查询，不等待应答、不阻塞输入。 */
        appearance_send_query();
        (void)appearance_portal_init();
        log_info("appearance", "Focus in; querying terminal background");
        return 1;
    }
    if (g_focus_out > 0 && ch == g_focus_out) {
        return 1;   /* 焦点离开：只消费，不发查询 */
    }
    return 0;
}

void appearance_drop_dangling_reply(void)
{
    if (g_anchorless_active) {
        /* 模态框开始前把有界收集结算掉（吞掉完整应答或原序回放）。 */
        (void)anchorless_poll(NULL);
    }
    if (!g_reply_expected || g_osc_state != OSC_IDLE) {
        return;
    }
    if (appearance_now_ms() > g_reply_deadline_ms) {
        g_reply_expected = 0;
        return;
    }

    timeout(0);
    int first = getch();
    if (first == ERR) {
        timeout(APPEARANCE_INPUT_TIMEOUT_MS);
        return;
    }
    if (first != 27) {
        /* 不是应答起始：原样退回，不影响模态输入框。 */
        ungetch(first);
        timeout(APPEARANCE_INPUT_TIMEOUT_MS);
        return;
    }

    /* 从 ESC 起非阻塞收集一段；只有完整应答才吞掉，否则原序退回。 */
    char seq[48];
    size_t len = 0;
    seq[len++] = 27;
    size_t consumed = 0;
    int r = 0, g = 0, b = 0;
    int rc = 0;
    unsigned long long deadline = appearance_now_ms() + 20;
    while (len < sizeof(seq)) {
        int rc_scan = appearance_osc11_scan(seq, len, &consumed, &r, &g, &b);
        if (rc_scan != 0) {
            rc = rc_scan;
            break;
        }
        if (appearance_now_ms() > deadline) {
            break;
        }
        int ch = getch();
        if (ch == ERR) {
            if (appearance_now_ms() > deadline) {
                break;
            }
            continue;
        }
        if (ch < 0 || ch > 255) {
            /* 功能键码混入：放弃清理，原序退回已有字节。 */
            break;
        }
        seq[len++] = (char)ch;
    }
    timeout(APPEARANCE_INPUT_TIMEOUT_MS);
    if (rc == 0) {
        rc = appearance_osc11_scan(seq, len, &consumed, &r, &g, &b);
    }

    if (rc == 1) {
        g_signals.osc11 = appearance_classify_rgb(r, g, b);
        g_reply_expected = 0;
        log_info("appearance", "Swallowed dangling OSC 11 reply rgb=%d,%d,%d", r, g, b);
        appearance_apply_pairs_if_changed();
        return;
    }
    if (rc == -1 && consumed > 1) {
        /* 已完整匹配前缀但内容非法：同样是终端应答，整段吞掉。 */
        g_reply_expected = 0;
        log_warn("appearance", "Swallowed malformed dangling OSC 11 reply");
        return;
    }

    /* 不是应答（或仍是前缀）：原序退回，交给调用方的输入循环。 */
    for (size_t i = len; i > 0; i--) {
        if (ungetch((unsigned char)seq[i - 1]) == ERR) {
            log_warn("appearance", "ungetch failed while restoring %zu bytes", i);
            break;
        }
    }
}

void appearance_shutdown(void)
{
    appearance_disable_focus_reporting();
    appearance_portal_shutdown();
    g_reply_expected = 0;
    g_portal_fallback_pending = 0;
    osc_reset();
    anchorless_reset();
}
