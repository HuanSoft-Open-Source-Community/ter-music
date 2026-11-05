/**
 * @file ui.c
 * @brief UI 框架层 — 初始化、事件循环、资源清理
 *
 * 各渲染模块已拆分到独立文件：
 *   - layout.c        : 窗口布局创建
 *   - visualizer.c    : 频谱可视化/专辑封面
 *   - controls.c      : 控制栏按钮/激活
 *   - progress_ui.c   : 进度条更新
 *   - playlist_render.c : 播放列表内容渲染
 *   - mouse.c         : 鼠标事件处理
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "ui/ui.h"
#include "core/core.h"
#include "info/info.h"
#include "player/player.h"
#include "i18n/i18n.h"
#include "ui/dialog.h"
#include "audio/audio.h"
#include "playlist/playlist.h"
#include "config/config.h"
#include "media/session.h"
#include "ui/lyrics.h"
#include "ui/keymap.h"
#include "ui/vinyl.h"
#include "ui/menus.h"
#include "library/library.h"
#include "library/browser/browser.h"
#include "ui/cover/album_cover.h"
#include "search/search.h"
#include "ui/scrollbar.h"
#include "remote/remote.h"
#include "logger/logger.h"
#include "audio/play_queue.h"
#include "ui/menu_internal.h"
#include "ui/context_menu.h"
#include "ui/layout.h"
#include "ui/layout_editor.h"
#include "ui/rainbow.h"
#include "ui/remote_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ncursesw/ncurses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <termios.h>

/* ── Forward declarations from split modules ── */
extern void render_playlist_content(void);
extern void render_controls(void);
extern void create_layout(void);
extern void render_visualizer_with_album_cover(void);
extern void seek_relative_seconds(int delta_seconds);
extern void format_display_text(char *dest, size_t dest_size, const char *src, int width, int pad);
extern void sanitize_ascii_text(char *dest, size_t dest_size, const char *src);
extern int g_playlist_tab_mode;
extern int g_queue_selected_index;
extern int g_saved_browser_index;


uint64_t get_ui_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ============================================================
 * Global window variables
 * ============================================================ */

WINDOW *win_playlist;
WINDOW *win_info;
WINDOW *win_controls;
WINDOW *win_spectrum;
WINDOW *win_lyrics;
WINDOW *win_vinyl;

/* ── Lyric cursor mode ── */
int g_lyric_cursor_mode = 0;
int g_lyric_cursor_index = -1;

/* ── UI refresh state ── */
static pthread_mutex_t g_ui_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_pending_ui_mask = 0;

/* ── ASCII fallback flag ── */
int g_ascii_fallback_ui = 0;

/* use_default_colors() 是否成功；流光彩蛋据此决定透明背景或黑回退。 */
static int g_default_colors_ok = 0;

/* ── Const values ──
 * 快进/音量步进量已上移到 ui.h（UI_SEEK_STEP_SECONDS / UI_VOLUME_STEP_PERCENT）：
 * 控制区窗格的滚轮要用同一份，不能在两个文件里各写一个魔数。 */
#define UI_INPUT_TIMEOUT_MS 40
#define ESC_TIMEOUT_MS 3000

/* ============================================================
 * Locale / UTF-8 helpers
 * ============================================================ */

/* ensure_utf8_locale() 见 ui/utf8.c（公开给非 TUI 模块复用） */

static int locale_uses_utf8(void)
{
    const char *codeset = nl_langinfo(CODESET);
    if (!codeset) return 0;
    return strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0;
}

static int locale_supports_cjk_width(void)
{
    static const char *sample = "中";
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t wc = 0;
    size_t len = mbrtowc(&wc, sample, strlen(sample), &state);
    if (len == (size_t)-1 || len == (size_t)-2 || len == 0) return 0;
    return wcwidth(wc) == 2;
}

static int terminal_needs_ascii_fallback(void)
{
    const char *term = getenv("TERM");
    const char *force_ascii = getenv("TER_MUSIC_FORCE_ASCII");
    const char *force_utf8  = getenv("TER_MUSIC_FORCE_UTF8");

    if (force_utf8  && strcmp(force_utf8,  "1") == 0) return 0;
    if (force_ascii && strcmp(force_ascii, "1") == 0) return 1;
    if (!locale_uses_utf8()) return 1;
    if (MB_CUR_MAX <= 1 || !locale_supports_cjk_width()) return 1;
    if (!term || term[0] == '\0') return 0;
    if (strcmp(term, "dumb") == 0) return 1;
    return 0;
}

/* ============================================================
 * UTF-8 next character helper
 * ============================================================ */

size_t utf8_next_char(const char *src, wchar_t *wc_out, int *width_out)
{
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t wc = 0;
    size_t char_len = mbrtowc(&wc, src, MB_CUR_MAX, &state);
    if (char_len == (size_t)-1 || char_len == (size_t)-2) {
        wc = (unsigned char)src[0];
        char_len = 1;
    } else if (char_len == 0) {
        if (wc_out)   *wc_out = L'\0';
        if (width_out) *width_out = 0;
        return 0;
    }

    int width = wcwidth(wc);
    if (width < 0) width = 1;
    if (wc_out)   *wc_out = wc;
    if (width_out) *width_out = width;
    return char_len;
}

/* ============================================================
 * Refresh system
 * ============================================================ */

void request_ui_refresh(int dirty_mask)
{
    if (dirty_mask == 0) return;
    pthread_mutex_lock(&g_ui_request_mutex);
    g_pending_ui_mask |= dirty_mask;
    pthread_mutex_unlock(&g_ui_request_mutex);
}

void process_pending_ui_refresh(void)
{
    int dirty_mask = 0;
    pthread_mutex_lock(&g_ui_request_mutex);
    dirty_mask = g_pending_ui_mask;
    g_pending_ui_mask = 0;
    pthread_mutex_unlock(&g_ui_request_mutex);

    if (dirty_mask == 0 || g_current_view != VIEW_MAIN) return;

    if (dirty_mask & UI_DIRTY_PLAYLIST) render_playlist_content();
    if (dirty_mask & UI_DIRTY_INFO)     render_info_bar();
    if (dirty_mask & UI_DIRTY_CONTROLS) render_controls();
    if (dirty_mask & UI_DIRTY_LYRICS)   { render_lyrics(); render_vinyl_pane(); }
}

/* ============================================================
 * ncurses initialization
 * ============================================================ */

/* stderr 守卫状态：>=0 表示 fd2 已被改道，值为保存的真实 stderr fd */
static int g_saved_stderr_fd = -1;

/* TUI 期间的 stderr 守卫（只改道 fd2，**绝不改道 fd1**）。
 *
 * fd1 不可改道：ncurses 自身经 stdout 输出，实测 initscr() 之后把 fd1
 * dup2 到 /dev/null 会让界面立即冻结，因此 stdout 侧的污染（audio.c 的
 * 后端名 printf）走定向修复覆盖，不碰 fd1。
 * fd2 改道用于拦截第三方库与遗留 stderr 写入：FFmpeg 扫描日志、libxml2
 * 的 config.xml 缺失警告等在 initscr() 之后落到提示条行的输出。
 * --debug 时改道到调试日志 fd（logger_output_fd()），否则丢进 /dev/null。
 * 幂等：已守卫时直接返回；任何一步失败都收干净 dup 的 fd，保持未守卫状态。 */
static void ui_guard_stdio(void)
{
    if (g_saved_stderr_fd >= 0) return;

    int fd = dup(STDERR_FILENO);
    if (fd < 0) return; /* dup 失败即放弃，不改道 */

    int target = logger_output_fd();
    int opened_fd = -1;
    if (target < 0) {
        opened_fd = open("/dev/null", O_WRONLY);
        target = opened_fd;
    }
    if (target < 0 || dup2(target, STDERR_FILENO) < 0) {
        /* 失败路径：把已持有的 fd 全部收干净，保持未守卫状态 */
        if (opened_fd >= 0) close(opened_fd);
        close(fd);
        return;
    }
    if (opened_fd >= 0) close(opened_fd); /* dup2 已复制引用，临时 fd 可关 */
    g_saved_stderr_fd = fd;
}

/* 恢复真实 stderr（幂等）：退出路径与崩溃处理器在打印错误前调用，
 * 保证 fprintf(stderr, …) / backtrace_symbols_fd(…, STDERR_FILENO)
 * 仍能输出到真实终端。 */
void ui_restore_stdio(void)
{
    if (g_saved_stderr_fd < 0) return;
    fflush(stderr);
    dup2(g_saved_stderr_fd, STDERR_FILENO);
    close(g_saved_stderr_fd);
    g_saved_stderr_fd = -1;
}

void init_ncurses(void)
{
    log_info("ui", "Initializing ncurses");
    ensure_utf8_locale();
    g_ascii_fallback_ui = terminal_needs_ascii_fallback();

    if (isatty(STDOUT_FILENO)) {
        printf("\033%%G");
        fflush(stdout);
    }


    initscr();
    cbreak();
    /* cbreak() 不清 IXON：终端的软件流控会吞掉 Ctrl+Q/Ctrl+S，使这两个
     * 合法键名（keyname 允许 ctrl+q / ctrl+s）永远收不到。全屏 TUI 不需要
     * XON/XOFF，关掉它们；ISIG 保持开启，Ctrl+C 仍按信号退出。 */
    {
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_iflag &= ~(IXON | IXOFF);
            tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    }
    noecho();
    keypad(stdscr, TRUE);
    /* 鼠标上报：主键点击（单击/双击/三击/release，均归一成一次点击）+
     * 右键按下 + 滚轮。
     * 滚轮在 xterm 协议里只有 BUTTON4/BUTTON5_PRESSED（无 release 语义），
     * 横向滚轮 BUTTON6/BUTTON7 有意不申请——本程序没有横向滚动区域。
     * 右键只申请 BUTTON3_PRESSED：按下即一条事件，不依赖终端合成 CLICKED，
     * 不会出现"press+click 两条"让刚开的右键菜单立刻被第二条关掉。
     * REPORT_MOUSE_POSITION 同样不申请：鼠标移动会淹没事件循环，且界面
     * 不需要 hover 状态。 */
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED |
              BUTTON1_RELEASED | BUTTON3_PRESSED |
              BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
    curs_set(0);
    clear();

    if (has_colors()) {
        start_color();
        g_default_colors_ok =
            (use_default_colors() == OK);  /* enable -1 for transparent bg */
        assume_default_colors(-1, -1);  /* pair 0 also uses terminal defaults */
        init_pair(COLOR_PAIR_BORDER,    COLOR_CYAN,   -1);  /* transparent bg */
        init_pair(COLOR_PAIR_PLAYLIST,  COLOR_WHITE,  -1);
        init_pair(COLOR_PAIR_CONTROLS,  COLOR_YELLOW, -1);
        init_pair(COLOR_PAIR_LYRICS,    COLOR_GREEN,  -1);
        init_pair(COLOR_PAIR_SIDEBAR,   COLOR_CYAN,   -1);
    init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK,  COLOR_WHITE);
    }

        bkgdset(' ' | COLOR_PAIR(0));  /* transparent bg via border pair */
        ui_guard_stdio();  /* initscr 之后改道 fd2：拦截第三方库/遗留 stderr 输出污染提示条 */
        clear();
}

/* 彩蛋只改前景；背景统一透明，终端不支持默认色时回退黑。 */
int ui_transparent_background(void)
{
    return g_default_colors_ok ? -1 : COLOR_BLACK;
}

/* ============================================================
 * Event loop
 * ============================================================ */

/* 把门面的修订号变化翻译成界面脏标记：事件循环里没有任何回调风暴，
 * 远程后端的异步到达也走同一条路径。 */
/* 按**内容下标**播放：前端把下标翻译成队列位置后经门面下发。
 * 队列过期（列表编辑过、尚未重推）时先整表重推再定位，保证远程模式播对曲目。
 * 导出的播放入口统一走这里，避免各界面入口把内容下标当队列位置盲下发。 */
void ui_play_track_index(int track_index)
{
    if (track_index < 0) {
        return;
    }
    char path[MAX_PATH_LEN];
    if (playlist_get_track_path(track_index, path, sizeof(path)) != 0) {
        return;
    }
    int position = player_queue_find(path);
    if (position < 0) {
        /* 不在队列里（例如刚被列表编辑过）：整表重推后再找一次 */
        player_queue_push();
        position = player_queue_find(path);
    }
    if (position >= 0) {
        player_queue_play(position);
    }
}

/* 主视图"当前列表"的选中项移动一格：键盘上下键与播放列表窗格滚轮共用。
 *
 * 形态判定按**状态**而非按键：搜索态用命中下标，播放队列页用队列下标，
 * 其余用播放列表/文件树下标。这与改动前四个 case 的行为逐条一致
 * （键盘在搜索态只会解析到 search.up/down，见主循环的上下文选择），
 * 所以提取不改变任何按键语义。
 *
 * 边界钳制在这里做，越界不重绘；滚动偏移本身由各窗格的渲染函数按选中项
 * 推导（clamp_playlist_start / get_playlist_scroll_offset），因此
 * "滚轮移动选中项"就等价于"滚动"，不需要引入第二份滚动状态。 */
void ui_move_main_list_selection(int delta, int guard_lyric_cursor)
{
    if (guard_lyric_cursor && g_lyric_cursor_mode && player_lyrics_total() > 0) {
        return;
    }

    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
        int next = g_queue_selected_index + delta;
        if (next < 0) next = 0;
        if (next > player_queue_count() - 1) next = player_queue_count() - 1;
        if (next == g_queue_selected_index) return;
        g_queue_selected_index = next;
        render_playlist_content();
        return;
    }

    if (g_search_state.active || g_search_state.in_progress) {
        int next = g_search_state.selected_index + delta;
        if (next < 0) next = 0;
        if (next > g_search_state.result_count - 1) next = g_search_state.result_count - 1;
        if (next == g_search_state.selected_index) return;
        g_search_state.selected_index = next;
        render_playlist_content();
        return;
    }

    int max_count = playlist_tree_is_active() ? playlist_visible_count() : playlist_count();
    int next = g_selected_index + delta;
    if (next < 0) next = 0;
    if (next > max_count - 1) next = max_count - 1;
    if (next == g_selected_index) return;
    g_selected_index = next;
    render_playlist_content();
}

/* 歌词跳转光标（**界面私有**）：移动一格并跳到该行时间戳。
 * 光标位置、是否处于跳转模式都由界面持有；行数据与来源归后端。
 *
 * 键盘（Ctrl+L 后上下键）与鼠标滚轮（歌词窗格）共用这一条路径：
 * 滚轮即"快进到下一句/上一句"，不改变跳转模式开关。
 * 声明见 ui.h（鼠标模块要用）。 */
void ui_lyric_cursor_step(int delta)
{
    int highlight = -1;
    if (!player_lyrics_highlight(&highlight, NULL, NULL)) {
        return;
    }
    if (g_lyric_cursor_index < 0) {
        g_lyric_cursor_index = highlight;
    }

    int target = g_lyric_cursor_index + delta;
    int total = player_lyrics_total();
    if (target < 0 || target >= total) {
        return;
    }

    LyricLine line;
    if (player_lyrics_line_at(target, &line) != 0) {
        return;
    }

    g_lyric_cursor_index = target;
    render_lyrics();
    /* 只依赖门面的播放状态，不再查本地进度追踪器：remote 下 progress_tracker
     * 恒不就绪会让 Ctrl+L 回车跳转永久失效。核心 seek 自带钳制（越界夹到时长）。 */
    if (player_play_state() != PLAY_STATE_STOPPED) {
        player_seek_seconds((int)line.timestamp);
    }
}

/* 断线浮层：只画一行提示（复用状态行区域），不吞按键。
 * 连续在线时不做事；恢复连接时提示一次。 */
static void render_offline_overlay(int connected)
{
    static int was_offline = 0;
    int offline = !connected && player_is_offline();

    if (offline) {
        char line[256];
        snprintf(line, sizeof(line), "%s — %s（%s）",
                 i18n_get("core.offline.title"),
                 i18n_get("core.offline.hint"),
                 i18n_get("core.offline.restart"));
        update_controls_status(line);
        was_offline = 1;
    } else if (was_offline) {
        update_controls_status(i18n_get("core.restart.done"));
        was_offline = 0;
    }
}

static void sync_player_revisions(void)
{
    static uint64_t state = 0;
    static uint64_t queue = 0;
    static uint64_t playlist = 0;
    static uint64_t lyrics = 0;
    static uint64_t config = 0;

    uint64_t now_state = player_state_revision();
    uint64_t now_queue = player_queue_revision();
    /* 播放列表内容归前端：版本号来自内容模块本身 */
    uint64_t now_playlist = playlist_content_revision();
    uint64_t now_lyrics = player_lyrics_revision();
    uint64_t now_config = player_config_revision();

    int dirty = 0;
    if (now_playlist != playlist) dirty |= UI_DIRTY_PLAYLIST | UI_DIRTY_INFO;
    if (now_queue != queue) {
        log_debug("ui", "queue revision %llu -> %llu",
                  (unsigned long long)queue, (unsigned long long)now_queue);
        dirty |= UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_INFO;
    }
    if (now_state != state)      dirty |= UI_DIRTY_CONTROLS | UI_DIRTY_INFO;
    if (now_lyrics != lyrics)    dirty |= UI_DIRTY_LYRICS;

    /* 歌词跳转模式只在当前曲目确实有歌词时成立；切到无歌词曲目后
     * 必须在下一帧退出，否则 n/p 与历史会被静默禁用。 */
    if (g_lyric_cursor_mode && player_lyrics_total() == 0) {
        g_lyric_cursor_mode = 0;
        g_lyric_cursor_index = -1;
        dirty |= UI_DIRTY_LYRICS | UI_DIRTY_CONTROLS;
    }

    if (dirty) {
        request_ui_refresh(dirty);
    }
    if (now_config != config) {
        /* 远端前端的配置镜像来自核心：核心/CLI 改了 keybindings 时，
         * 这里必须同步重建键位表，否则设置页显示已改而按键仍是旧语义。 */
        keymap_reload();
        apply_color_theme();
        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                           UI_DIRTY_LYRICS | UI_DIRTY_INFO);
    }

    /* 按曲播放历史（Task 5.1）：本地与远程都在这一处统一检测核心快照的"当前曲目"
     * 变化——换曲才写一条，连续重复播同一首由 add_history_entry 内部去重。
     * 历史是前端自有内容，写入发生在前端进程（local 时前端即核心，remote 时是客户端），
     * 因此不依赖任何后端直调。元数据取自门面快照（InfoTrack 已带 path/title/artist）。 */
    static char last_hist_track_id[96] = "";
    static int last_hist_valid = 0;
    const InfoTrack *hist_track = player_track();
    if (hist_track && hist_track->valid &&
        (!last_hist_valid || strcmp(hist_track->track_id, last_hist_track_id) != 0)) {
        Track t;
        memset(&t, 0, sizeof(t));
        snprintf(t.path, sizeof(t.path), "%s", hist_track->path);
        snprintf(t.title, sizeof(t.title), "%s", hist_track->title);
        snprintf(t.artist, sizeof(t.artist), "%s", hist_track->artist);
        snprintf(t.album, sizeof(t.album), "%s", hist_track->album);
        if (t.path[0]) {
            add_history_entry(&t);
        }
        snprintf(last_hist_track_id, sizeof(last_hist_track_id), "%s", hist_track->track_id);
        last_hist_valid = 1;
    } else if (!hist_track || !hist_track->valid) {
        last_hist_valid = 0;
        last_hist_track_id[0] = '\0';
    }

    state = now_state;
    queue = now_queue;
    playlist = now_playlist;
    lyrics = now_lyrics;
    config = now_config;

    /* 双格布局里歌词格随曲目有无歌词让位：把"期望"与 create_layout() 实际
     * 生效的值对账，不一致才重建一次。不能用边沿检测——启动自动播放时歌词
     * 先到、首次 sync 后到，唯一的翻转会被"首帧只记种子"吃掉，期望值与
     * 生效几何永久不一致（歌词格再不出现）。设置页/编辑器里只记录不重建，
     * 回到主界面时 exit_current_view() 的 create_layout() 会按最新值生效。 */
    {
        int want = ui_layout_lyrics_pane_suppressed();
        int applied = ui_layout_lyrics_pane_applied();
        if (applied >= 0 && want != applied &&
            g_current_view == VIEW_MAIN && !layout_editor_active()) {
            create_layout();
            request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                               UI_DIRTY_LYRICS | UI_DIRTY_INFO);
        }
    }
}

/* 配置重载后的界面侧处理：核心已应用配置，这里只重刷配色与脏标记。
 * 与“状态消息”监听同构，均由核心在重载路径回调。 */
static void ui_on_config_reloaded(void)
{
    log_info("ui", "Applying UI-side config reload (theme + keymap + panel refresh)");
    /* 配置/语言/键位可能已变，旧菜单的标签与动作不再可信：先关闭，
     * 随后的重绘会把界面恢复干净。 */
    ui_context_menu_close(0);
    keymap_reload();
    apply_color_theme();
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                       UI_DIRTY_LYRICS | UI_DIRTY_INFO);
    rerender_active_view();
}

/* 列表窗格的文件/队列模式切换：Ctrl+Tab 动作与右键菜单共用。
 * 只切换列表模式；歌词来源切换由 ACTION_LYRICS_SOURCE_SWITCH 负责。
 * 搜索只属于文件浏览模式：切到队列页前先取消搜索，避免标题/行/命中
 * 各按不同状态解析。 */
int ui_toggle_playlist_tab_mode(void)
{
    if (!playlist_is_loaded()) {
        return 0;
    }
    /* Save browser cursor position before switching to queue */
    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (g_search_state.active || g_search_state.in_progress) {
            search_clear();
        }
        g_saved_browser_index = g_selected_index;
    }

    g_playlist_tab_mode = !g_playlist_tab_mode;

    /* Restore browser cursor when switching back from queue */
    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (g_saved_browser_index >= 0)
            g_selected_index = g_saved_browser_index;
        g_saved_browser_index = -1;
    }

    /* 队列内容由内容列表下发产生（playlist_queue_sync）：切到
     * 队列视图时只把光标对到当前曲目，不再自行“填队列”。 */
    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE &&
        player_queue_count() > 0) {
        int anchor = player_track_index() >= 0 ? player_track_index() : 0;
        g_queue_selected_index = 0;
        for (int i = 0; i < player_queue_count(); i++) {
            if (player_queue_index_at(i) == anchor) {
                g_queue_selected_index = i;
                break;
            }
        }
    }
    render_playlist_content();
    render_controls();
    update_controls_status(
        g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE
            ? i18n_get("status.switched_queue")
            : i18n_get("status.switched_browser"));
    log_debug("ui", "playlist tab mode -> %d", g_playlist_tab_mode);
    return 1;
}

/* 主视图的动作分发：键盘与右键菜单共用同一条路径。
 * 返回 1 = 已处理（事件循环继续），2 = 退出程序。
 * 从 run_event_loop() 原样抽出：continue -> return 1，
 * ACTION_APP_QUIT -> return 2。 */
int handle_main_action(ActionId action)
{
    /* 全局/主视图动作：不受列表是否加载影响（与原代码顺序一致）。 */
    switch (action) {
        case ACTION_APP_QUIT:
            log_info("ui", "Event loop exiting on quit action");
            return 2;
        case ACTION_VIEW_MAIN:
        case ACTION_VIEW_SETTINGS:
        case ACTION_VIEW_HISTORY:
        case ACTION_VIEW_PLAYLIST:
        case ACTION_VIEW_FAVORITES:
        case ACTION_VIEW_INFO:
        case ACTION_VIEW_LANGUAGE:
        case ACTION_VIEW_HELP:
        case ACTION_LIBRARY_TOGGLE:
            execute_global_action(action);
            return 1;
        case ACTION_VOLUME_UP:
            player_adjust_volume(UI_VOLUME_STEP_PERCENT);
            render_controls();
            return 1;
        case ACTION_VOLUME_DOWN:
            player_adjust_volume(-UI_VOLUME_STEP_PERCENT);
            render_controls();
            return 1;
        case ACTION_CORE_RESTART:
            /* 断线时 R = 一键重启核心（状态机在门面里，界面不阻塞） */
            if (player_is_offline()) {
                if (player_restart_core() == 0) {
                    update_controls_status(i18n_get("core.restart.done"));
                } else {
                    update_controls_status(i18n_get("core.restart.failed"));
                }
                render_controls();
            }
            return 1;
        case ACTION_FOCUS_CONTROLS:
            if (!win_controls) {
                update_controls_status(i18n_get("controls.pane_hidden"));
                return 1;
            }
            g_control_focus = 1;
            g_current_control_idx = 1;
            render_playlist_content();
            render_controls();
            return 1;
        case ACTION_FOCUS_LIST:
            if (g_popup.active) {
                controls_reset_popup();
            }
            g_control_focus = 0;
            render_controls();
            render_playlist_content();
            return 1;
        case ACTION_LYRICS_SEEK_MODE: {
            if (!win_lyrics) {
                update_controls_status(i18n_get("lyrics.pane_hidden"));
                return 1;
            }
            /* 已开启时无条件允许退出：切到无歌词曲目后不能让模式卡死。 */
            if (g_lyric_cursor_mode) {
                g_lyric_cursor_mode = 0;
                g_lyric_cursor_index = -1;
                update_controls_status(i18n_get("lyrics.seek_disabled"));
                render_controls();
                render_lyrics();
                return 1;
            }
            int highlight = -1;
            int has_lyrics = player_lyrics_highlight(&highlight, NULL, NULL);
            if (!has_lyrics || highlight < 0) {
                update_controls_status(i18n_get("lyrics.no_position"));
                return 1;
            }
            g_lyric_cursor_mode = 1;
            g_lyric_cursor_index = highlight;   /* 光标是界面私有状态 */
            update_controls_status(i18n_get("lyrics.seek_enabled"));
            render_controls();
            render_lyrics();
            return 1;
        }
        default:
            break;
    }

    // Control focus mode
    if (g_control_focus == 1) {
        switch (action) {
            case ACTION_CONTROL_UP:
                if (g_current_control_idx == CONTROL_IDX_VOLUME) {
                    player_adjust_volume(UI_VOLUME_STEP_PERCENT);
                    render_controls();
                }
                break;
            case ACTION_CONTROL_DOWN:
                if (g_current_control_idx == CONTROL_IDX_VOLUME) {
                    player_adjust_volume(-UI_VOLUME_STEP_PERCENT);
                    render_controls();
                }
                break;
            case ACTION_CONTROL_LEFT:
                g_current_control_idx--;
                if (g_current_control_idx < 0) g_current_control_idx = CONTROL_COUNT - 2;
                render_controls();
                break;
            case ACTION_CONTROL_RIGHT:
                g_current_control_idx++;
                if (g_current_control_idx >= CONTROL_COUNT - 1) g_current_control_idx = 0;
                render_controls();
                break;
            case ACTION_SEEK_BACK: seek_relative_seconds(-UI_SEEK_STEP_SECONDS); break;
            case ACTION_SEEK_FORWARD: seek_relative_seconds(UI_SEEK_STEP_SECONDS); break;
            case ACTION_CONTROL_ACTIVATE:
                activate_current_control();
                render_playlist_content();
                render_controls();
                break;
            default:
                break;
        }
        return 1;
    }

    // List focus mode
    if (!playlist_is_loaded()) {
        /* 未加载播放列表时仍允许“打开/追加目录”，其余列表动作 no-op。 */
        switch (action) {
            case ACTION_FOLDER_OPEN:
                prompt_open_folder();
                render_playlist_content();
                break;
            case ACTION_QUEUE_INSERT_NEXT:
            case ACTION_FOLDER_APPEND:
                prompt_append_folder();
                render_playlist_content();
                break;
            default:
                break;
        }
        return 1;
    }

    switch (action) {
        case ACTION_LIST_UP:
            /* 歌词定位模式（有歌词）下 j/k（回退解析为 list.up/down）不移动列表：
             * 该护栏由 ui_move_main_list_selection 的 guard 参数保留。 */
            ui_move_main_list_selection(-1, 1);
            break;
        case ACTION_LIST_DOWN:
            ui_move_main_list_selection(1, 1);
            break;
        case ACTION_SEARCH_UP:
            ui_move_main_list_selection(-1, 1);
            break;
        case ACTION_SEARCH_DOWN:
            ui_move_main_list_selection(1, 1);
            break;
        case ACTION_LIST_ACTIVATE:
            if (g_search_state.in_progress) {
                update_controls_status(i18n_get("status.search_wait"));
                break;
            }
            if (g_search_state.active && g_search_state.result_count > 0) {
                int original_index = g_search_state.result_indices[g_search_state.selected_index];
                ui_play_track_index(original_index);
                g_search_state.active = 0;
                g_selected_index = g_sort_state.active ? 0 : original_index;
                render_playlist_content();
            } else if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE && player_queue_count() > 0) {
                int queue_pos = g_queue_selected_index >= 0 && g_queue_selected_index < player_queue_count()
                    ? g_queue_selected_index : 0;
                player_queue_play(queue_pos);
                int play_idx = player_queue_index_at(queue_pos);
                if (play_idx >= 0) {
                    g_selected_index = play_idx;
                }
            } else {
                /* Tree mode: toggle directory expand, or play file */
                if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
                    && !g_search_state.active) {
                    int node_type = get_visible_node_type(g_selected_index);
                    if (node_type == TREE_NODE_DIRECTORY) {
                        int tree_idx = get_visible_node_tree_index(g_selected_index);
                        playlist_toggle_directory_expand(tree_idx);
                        if (g_selected_index >= playlist_visible_count())
                            g_selected_index = playlist_visible_count() - 1;
                        if (g_selected_index < 0) g_selected_index = 0;
                        render_playlist_content();
                        break;
                    }
                    /* File node: get real track index */
                    int real_idx = get_visible_node_track_index(g_selected_index);
                    if (real_idx >= 0)
                        ui_play_track_index(real_idx);
                    break;
                }
                int play_idx = g_selected_index;
                if (g_sort_state.active) play_idx = g_sort_state.sorted_indices[g_selected_index];
                ui_play_track_index(play_idx);
            }
            break;
        case ACTION_FOLDER_OPEN:
            prompt_open_folder();
            render_playlist_content();
            break;
        case ACTION_QUEUE_INSERT_NEXT:
            if (player_queue_count() > 0 && player_track_index() >= 0) {
                int track_idx = g_selected_index;
                if (g_search_state.active)
                    track_idx = g_search_state.result_indices[g_search_state.selected_index];
                else if (g_sort_state.active)
                    track_idx = g_sort_state.sorted_indices[g_selected_index];
                else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                    track_idx = get_visible_node_track_index(g_selected_index);
                    if (track_idx < 0) break;
                }
                player_queue_insert_after(track_idx);
                update_controls_status(i18n_get("status.inserted_next"));
            }
            break;
        case ACTION_QUEUE_APPEND:
            if (playlist_count() > 0) {
                int track_idx = g_selected_index;
                if (g_search_state.active) {
                    pthread_mutex_lock(&g_search_mutex);
                    track_idx = g_search_state.result_indices[g_search_state.selected_index];
                    pthread_mutex_unlock(&g_search_mutex);
                } else if (g_sort_state.active) {
                    track_idx = g_sort_state.sorted_indices[g_selected_index];
                } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                    track_idx = get_visible_node_track_index(g_selected_index);
                    if (track_idx < 0) break;
                }
                player_queue_append(track_idx);
                update_controls_status(i18n_get("status.added_to_queue"));
            }
            break;
        case ACTION_FOLDER_APPEND:
            prompt_append_folder();
            render_playlist_content();
            break;
        case ACTION_FAVORITE_TOGGLE:
            if (playlist_count() > 0) {
                Track t;
                int track_idx = g_selected_index;
                if (g_search_state.active) {
                    pthread_mutex_lock(&g_search_mutex);
                    track_idx = g_search_state.result_indices[g_search_state.selected_index];
                    pthread_mutex_unlock(&g_search_mutex);
                } else if (g_sort_state.active) {
                    track_idx = g_sort_state.sorted_indices[g_selected_index];
                } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                    track_idx = get_visible_node_track_index(g_selected_index);
                    if (track_idx < 0) break;
                }
                get_track_metadata(track_idx, &t);
                int result = add_to_favorites(&t);
                update_controls_status(result == 0
                    ? i18n_get("favorites.added")
                    : i18n_get("favorites.exists_or_full"));
            }
            break;
        case ACTION_SEARCH_OPEN:
            if (g_current_view == VIEW_MAIN) {
                /* 搜索只作用于文件浏览模式；在队列页按 / 先切回浏览器。 */
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE &&
                    !ui_toggle_playlist_tab_mode()) {
                    break;
                }
                search_prompt();
                return 1;
            }
            break;
        case ACTION_VIEW_TAB_SWITCH:
            /* 歌词来源切换走 CTX_MAIN_LYRIC 的 ACTION_LYRICS_SOURCE_SWITCH；
             * 这里只负责文件/队列模式切换。 */
            (void)ui_toggle_playlist_tab_mode();
            break;
        case ACTION_PLAYLIST_ADD:
            if (playlist_count() > 0 && g_playlist_manager.count > 0) {
                Track t;
                int track_idx = g_selected_index;
                if (g_search_state.active) {
                    pthread_mutex_lock(&g_search_mutex);
                    track_idx = g_search_state.result_indices[g_search_state.selected_index];
                    pthread_mutex_unlock(&g_search_mutex);
                } else if (g_sort_state.active) {
                    track_idx = g_sort_state.sorted_indices[g_selected_index];
                } else if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
                    track_idx = get_visible_node_track_index(g_selected_index);
                    if (track_idx < 0) break;
                }
                get_track_metadata(track_idx, &t);

                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                WINDOW *win_win = newwin(max_y - 4, max_x - 4, 2, 2);
                rounded_box(win_win);
                mvwprintw(win_win, 0, 2, "%s", i18n_get("playlist_mgr.select_playlist"));
                wbkgd(win_win, COLOR_PAIR(COLOR_PAIR_PLAYLIST));

                int start_y = 2;
                int visible_lines = max_y - 8;
                int selected = 0, offset = 0;

                while (1) {
                    for (int i = 0; i < visible_lines && (offset + i) < g_playlist_manager.count; i++) {
                        int idx = offset + i;
                        UserPlaylist *pl = &g_playlist_manager.playlists[idx];
                        char playlist_name[MAX_PLAYLIST_NAME_LEN + 8];
                        format_display_text(playlist_name, sizeof(playlist_name), pl->name, 30, 1);
                        if (idx == selected) {
                            wattron(win_win, A_REVERSE);
                            mvwprintw(win_win, start_y + i, 2,
                                      i18n_get("playlist_mgr.track_count_fmt"),
                                      playlist_name, pl->track_count);
                            wattroff(win_win, A_REVERSE);
                        } else {
                            mvwprintw(win_win, start_y + i, 2,
                                      i18n_get("playlist_mgr.track_count_fmt"),
                                      playlist_name, pl->track_count);
                        }
                    }

                    mvwprintw(win_win, max_y - 6, 2, "%s",
                              i18n_get("playlist_mgr.select_hint"));
                    wrefresh(win_win);
                    wtimeout(win_win, UI_INPUT_TIMEOUT_MS);
                    int c = wgetch(win_win);
                    if (c == ERR) { media_session_tick(); continue; }
                    /* Esc 为系统保留键；其余按弹窗上下文解析。 */
                    ActionId popup_action = (c == 27)
                        ? ACTION_POPUP_CANCEL
                        : keymap_resolve(CTX_POPUP, c);
                    if (popup_action == ACTION_POPUP_CANCEL) break;
                    if (popup_action == ACTION_POPUP_UP && selected > 0) {
                        selected--;
                        if (selected < offset) offset = selected;
                    }
                    if (popup_action == ACTION_POPUP_DOWN && selected < g_playlist_manager.count - 1) {
                        selected++;
                        if (selected >= offset + visible_lines) offset = selected - visible_lines + 1;
                    }
                    if (popup_action == ACTION_POPUP_ACCEPT) {
                        int r = add_track_to_playlist(selected, &t);
                        if (r == 0)       update_controls_status(i18n_get("playlist_mgr.added_to_playlist"));
                        else if (r == -3) update_controls_status(i18n_get("playlist_mgr.playlist_full"));
                        else              update_controls_status(i18n_get("playlist_mgr.already_in_playlist"));
                        break;
                    }
                }
                delwin(win_win);
                request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                                   UI_DIRTY_LYRICS | UI_DIRTY_INFO);
            } else if (g_playlist_manager.count == 0) {
                update_controls_status(
                    i18n_get("playlist_mgr.no_playlists_yet"));
            }
            break;
        case ACTION_QUEUE_REMOVE:
            if (player_queue_count() > 0) {
                int target_pos;
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                    target_pos = g_queue_selected_index;
                } else {
                    int track_idx = g_selected_index;
                    if (g_search_state.active) {
                        pthread_mutex_lock(&g_search_mutex);
                        track_idx = g_search_state.result_indices[g_search_state.selected_index];
                        pthread_mutex_unlock(&g_search_mutex);
                    } else if (g_sort_state.active) {
                        track_idx = g_sort_state.sorted_indices[g_selected_index];
                    }
                    target_pos = -1;
                    for (int i = 0; i < player_queue_count(); i++) {
                        if (player_queue_index_at(i) == track_idx) { target_pos = i; break; }
                    }
                }
                if (target_pos >= 0) {
                    player_queue_remove_at(target_pos);
                    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                        if (g_queue_selected_index >= player_queue_count())
                            g_queue_selected_index = player_queue_count() > 0 ? player_queue_count() - 1 : 0;
                    }
                    render_playlist_content();
                } else {
                    update_controls_status(i18n_get("status.queue_item_missing"));
                }
            }
            break;
        case ACTION_QUEUE_CLEAR:
            if (player_queue_count() > 0) {
                player_queue_clear();
                g_queue_selected_index = 0;
                render_playlist_content();
                update_controls_status(i18n_get("status.queue_cleared"));
            }
            break;
        case ACTION_QUEUE_MOVE_DOWN:
            if (player_queue_count() > 1) {
                int target_pos;
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                    target_pos = g_queue_selected_index;
                } else {
                    int track_idx = g_selected_index;
                    if (g_search_state.active) {
                        pthread_mutex_lock(&g_search_mutex);
                        track_idx = g_search_state.result_indices[g_search_state.selected_index];
                        pthread_mutex_unlock(&g_search_mutex);
                    } else if (g_sort_state.active) {
                        track_idx = g_sort_state.sorted_indices[g_selected_index];
                    }
                    target_pos = -1;
                    for (int i = 0; i < player_queue_count(); i++) {
                        if (player_queue_index_at(i) == track_idx) { target_pos = i; break; }
                    }
                }
                if (target_pos >= 0 && target_pos < player_queue_count() - 1) {
                    player_queue_move_down(target_pos);
                    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                        g_queue_selected_index++;
                    render_playlist_content();
                } else if (target_pos < 0) {
                    update_controls_status(i18n_get("status.queue_item_missing"));
                } else {
                    update_controls_status(i18n_get("status.queue_at_edge"));
                }
            }
            break;
        case ACTION_QUEUE_MOVE_UP:
            if (player_queue_count() > 1) {
                int target_pos;
                if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE) {
                    target_pos = g_queue_selected_index;
                } else {
                    int track_idx = g_selected_index;
                    if (g_search_state.active) {
                        pthread_mutex_lock(&g_search_mutex);
                        track_idx = g_search_state.result_indices[g_search_state.selected_index];
                        pthread_mutex_unlock(&g_search_mutex);
                    } else if (g_sort_state.active) {
                        track_idx = g_sort_state.sorted_indices[g_selected_index];
                    }
                    target_pos = -1;
                    for (int i = 0; i < player_queue_count(); i++) {
                        if (player_queue_index_at(i) == track_idx) { target_pos = i; break; }
                    }
                }
                if (target_pos > 0) {
                    player_queue_move_up(target_pos);
                    if (g_playlist_tab_mode == PLAYLIST_MODE_PLAY_QUEUE)
                        g_queue_selected_index--;
                    render_playlist_content();
                } else if (target_pos < 0) {
                    update_controls_status(i18n_get("status.queue_item_missing"));
                } else {
                    update_controls_status(i18n_get("status.queue_at_edge"));
                }
            }
            break;
        case ACTION_PLAYMODE_SEQUENTIAL: player_set_play_mode(PLAY_MODE_SEQUENTIAL); render_controls(); break;
        case ACTION_PLAYMODE_SINGLE: player_set_play_mode(PLAY_MODE_SINGLE_REPEAT); render_controls(); break;
        case ACTION_PLAYMODE_LIST: player_set_play_mode(PLAY_MODE_LIST_REPEAT); render_controls(); break;
        case ACTION_PLAYMODE_SHUFFLE: player_set_play_mode(PLAY_MODE_SHUFFLE_REPEAT); render_controls(); break;
        case ACTION_PLAYMODE_FOLDER: player_set_play_mode(PLAY_MODE_FOLDER_SEQUENTIAL); render_controls(); break;
        case ACTION_LYRICS_PREV:
            if (player_lyrics_total() > 0) {
                ui_lyric_cursor_step(-1);
            }
            break;
        case ACTION_LYRICS_NEXT:
            if (player_lyrics_total() > 0) {
                ui_lyric_cursor_step(1);
            }
            break;
        case ACTION_LYRICS_SOURCE_SWITCH:
            if (g_lyric_cursor_mode && player_lyrics_total() > 0) {
                int new_source = (player_lyrics_source() == LYRICS_SOURCE_EMBEDDED)
                    ? LYRICS_SOURCE_EXTERNAL : LYRICS_SOURCE_EMBEDDED;
                if (player_lyrics_reload_source(new_source) == 0 &&
                    player_backend() == PLAYER_BACKEND_REMOTE) {
                    const InfoTrack *track = player_track();
                    if (track && track->path[0] != '\0') {
                        library_set_lyrics_source(track->path, new_source);
                    }
                }
                update_controls_status(
                    new_source == LYRICS_SOURCE_EMBEDDED
                        ? i18n_get("lyrics.source_embedded")
                        : i18n_get("lyrics.source_external"));
            }
            break;
        case ACTION_TRACK_NEXT:
            if (!g_lyric_cursor_mode) {
                player_next();
                return 1;
            }
            break;
        case ACTION_TRACK_PREV:
            if (!g_lyric_cursor_mode) {
                player_prev();
                return 1;
            }
            break;
        case ACTION_HISTORY_SHOW:
            if (g_lyric_cursor_mode || g_play_history.count == 0) break;
            {
            /* Simple inline history: up to 10 rows, scrolls over all entries */
            int my, mx;
            getmaxyx(stdscr, my, mx);
            int visible = g_play_history.count > 10 ? 10 : g_play_history.count;
            int pw = 60, ph = visible + 4;
            int px = (mx - pw) / 2, py = (my - ph) / 2;
            WINDOW *w = newwin(ph, pw, py, px);
            rounded_box(w);
            mvwprintw(w, 0, 2, "%s", i18n_get("play_history.title"));
            int sel = 0, off = 0;
            while (1) {
                if (sel < off) off = sel;
                else if (sel >= off + visible) off = sel - visible + 1;
                if (off < 0) off = 0;
                for (int i = 0; i < visible; i++) {
                    int idx = off + i;
                    if (idx >= g_play_history.count) break;
                    const HistoryEntry *e = &g_play_history.entries[idx];
                    char label[80];
                    if (e->title[0])
                        snprintf(label, sizeof(label), "%s - %s", e->title, e->artist);
                    else
                        snprintf(label, sizeof(label), "%s", e->path);
                    if (idx == sel) {
                        wattron(w, A_REVERSE);
                        mvwprintw(w, i + 1, 2, " %s ", label);
                        wattroff(w, A_REVERSE);
                    } else {
                        mvwprintw(w, i + 1, 2, " %s", label);
                    }
                }
                mvwprintw(w, ph - 2, 2, "%s", i18n_get("play_history.hint"));
                wrefresh(w);
                wtimeout(w, UI_INPUT_TIMEOUT_MS);
                int c = wgetch(w);
                if (c == ERR) { media_session_tick(); continue; }
                ActionId popup_action = (c == 27)
                    ? ACTION_POPUP_CANCEL
                    : keymap_resolve(CTX_POPUP, c);
                if (popup_action == ACTION_POPUP_CANCEL) break;
                if (popup_action == ACTION_POPUP_UP && sel > 0) sel--;
                if (popup_action == ACTION_POPUP_DOWN &&
                    sel < g_play_history.count - 1) sel++;
                if (popup_action == ACTION_POPUP_ACCEPT) {
                    const char *hpath = g_play_history.entries[sel].path;
                    int found = playlist_find_track_index_by_path(hpath);
                    /* 历史里的曲目可能不在当前列表：追加到内容列表尾部后播放
                     * （Task 5.2，与收藏/歌单同一复用路径）。 */
                    if (found < 0 && append_playlist(hpath) > 0) {
                        found = playlist_find_track_index_by_path(hpath);
                    }
                    if (found >= 0) { ui_play_track_index(found); g_selected_index = found; }
                    break;
                }
            }
            delwin(w);
            render_playlist_content();
            render_controls();
            return 1;
        }
        default:
            break;
    }
    return 1;
}

void run_event_loop(void)
{
    extern volatile sig_atomic_t g_should_exit;
    int ch;
    log_info("ui", "Event loop started");

    /* 键位表按配置重建一次；此后所有输入都走上下文 → 动作分发。 */
    keymap_ensure_loaded();

    render_playlist_content();
    render_info_bar();
    render_controls();
    render_lyrics();
    render_vinyl_pane();
    render_spectrum_pane();

    timeout(UI_INPUT_TIMEOUT_MS);

    int esc_pending = 0;
    uint64_t esc_pending_time = 0;

    /* 歌词状态由本模块持有，核心经钩子调用其推进函数 */
    /* 状态消息：核心推送时同步到界面状态栏 */
    core_set_status_listener(show_status_message);
    /* 配置重载：核心应用后由界面重刷配色与内容 */
    core_set_config_listener(ui_on_config_reloaded);

    while (1) {
        /* 核心工作（回收线程/挂起动作/歌词推进/D-Bus tick/配置重载） */
        core_tick();
        /* 门面刷新：本地后端重算快照并与上次比较，修订号变化即请求重绘；
         * 远端后端在这里做心跳与重连（非阻塞），返回 -1 表示与核心断开。 */
        int connected = (player_pump() == 0);
        /* 前端远程：取回后台线程的列目录结果与已下载曲目（交给核心） */
        remote_view_tick();
        sync_player_revisions();
        process_pending_ui_refresh();
        controls_status_tick();

        /* 频谱独立窗格按自己的 40ms 节拍刷新；无歌词黑胶动画 100ms。
         * 二者都不再触发整块歌词/列表重绘。 */
        spectrum_tick();
        vinyl_tick();
        lyrics_animation_tick();

        /* 断线浮层：不清空快照（保留最后一帧），提示自动重连与一键重启 */
        render_offline_overlay(connected);

        /* 动画窗格在上一拍重绘过；右键菜单必须重新合成到最上层，
         * 否则会被频谱/黑胶/歌词动画擦掉。 */
        ui_context_menu_refresh();

        /* 响应终端关闭、SIGTERM/SIGINT 等退出信号 */
        if (g_should_exit) {
            log_info("ui", "Event loop exiting on g_should_exit signal");
            break;
        }

        ch = getch();

        if (player_play_state() == PLAY_STATE_PLAYING ||
            player_play_state() == PLAY_STATE_PAUSED) {
            update_progress_bar();
        }

        process_pending_ui_refresh();

        /* 进度条/脏标记重绘之后再把菜单盖回去，然后才做流光合成。 */
        ui_context_menu_refresh();

        /* 彩蛋前景合成：放在所有本轮内容刷新之后，相位由时间决定；
         * 未激活时立即返回，不产生额外终端输出。 */
        ui_rainbow_paint();

        if (ch == ERR) {
            /* 终端断开检测：stdin 不再是 tty 时触发安全退出 */
            if (!isatty(STDIN_FILENO)) {
                log_info("ui", "Event loop exiting: stdin is no longer a TTY");
                g_should_exit = 1;
                break;
            }
            if (esc_pending) {
                uint64_t now = get_ui_time_ms();
                if (now - esc_pending_time > ESC_TIMEOUT_MS) esc_pending = 0;
            }
            continue;
        }

        /* 右键菜单展开时：键盘输入先归菜单（未识别键=关闭并吞掉）。
         * KEY_MOUSE 仍走归属分发（菜单是 overlay）；KEY_RESIZE 先关菜单，
         * 再交给下面的既有 resize 分支重建布局。 */
        if (ui_context_menu_active()) {
            if (ch == KEY_RESIZE) {
                ui_context_menu_close(0);
            } else if (ch != KEY_MOUSE) {
                ui_context_menu_handle_key(ch);
                ui_context_menu_refresh();
                ui_rainbow_paint();
                continue;
            }
        }

        /* 曲库浏览器激活时优先按浏览器上下文解析：浏览器动作 →
         * handle_library_action；全局动作 → execute_global_action；其余不处理。
         * KEY_RESIZE/KEY_MOUSE 仍走既有终端与鼠标路径。ESC 是系统保留键，
         * 必须直达浏览器（返回上层/退出），不能进 esc_pending 功能键链。 */
        if (g_library_state.active && g_current_view == VIEW_MAIN &&
            ch != KEY_RESIZE && ch != KEY_MOUSE) {
            if (ch == 27) {
                handle_library_input(ch);
                continue;
            }
            ActionId library_action = keymap_resolve(CTX_BROWSER, ch);
            if (actions_context(library_action) == CTX_BROWSER) {
                handle_library_action(library_action);
                request_ui_refresh(UI_DIRTY_PLAYLIST);
                continue;
            }
            if (actions_context(library_action) == CTX_GLOBAL) {
                if (library_action == ACTION_APP_QUIT) {
                    log_info("ui", "Event loop exiting on quit action");
                    break;
                }
                execute_global_action(library_action);
            }
            continue;
        }

        // ESC prefix handling
        if (esc_pending) {
            uint64_t now = get_ui_time_ms();
            if (now - esc_pending_time > ESC_TIMEOUT_MS) {
                esc_pending = 0;
            } else if (ch >= '1' && ch <= '9') {
                /* 旧 ESC+1..9 链合成逻辑 F1..F9，按当前键位触发全局动作。 */
                ActionId fkey_action =
                    keymap_resolve(CTX_GLOBAL, KEY_F(1 + (ch - '1')));
                if (fkey_action != ACTION_NONE) {
                    execute_global_action(fkey_action);
                }
                esc_pending = 0;
                continue;
            }
            esc_pending = 0;
            if (ch == 27) {
                if (g_current_view != VIEW_MAIN) {
                    esc_pending = 0;
                } else {
                    esc_pending = 1;
                    esc_pending_time = get_ui_time_ms();
                    continue;
                }
            }
        }

        if (ch == 27) {
            if (g_current_view == VIEW_MAIN) {
                /* When a popup is active, ESC must dismiss it immediately.
                 * Do NOT enter the esc_pending chain so the byte falls through
                 * to the popup cancel action below. */
                if (g_popup.active && g_control_focus == 1) {
                    /* fall through — popup handler will catch the ESC */
                } else {
                    esc_pending = 1;
                    esc_pending_time = get_ui_time_ms();
                    continue;
                }
            }
        }

        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                /* bstate 原值只在 --debug 下记录：右键/滚轮在终端间的上报位
                 * 有差异（BUTTON3_PRESSED vs CLICKED），排查时不必再改代码。 */
                log_debug("ui", "Mouse event at (%d,%d) bstate=0x%lx",
                          event.x, event.y, (unsigned long)event.bstate);
                /* 唯一的鼠标入口：提示条、主视图窗格、子视图、曲库浏览器、
                 * 布局编辑器、以及展开中的弹层，都由 handle_global_mouse_event
                 * 统一判归属（规则见 ui/mouse.h），这里不再各自命中。 */
                handle_global_mouse_event(&event);
            }
            continue;
        }

        if (ch == KEY_RESIZE) {
            clear();
            create_layout();
            if (layout_editor_active()) {
                layout_editor_render();
            } else if (g_current_view != VIEW_MAIN) {
                rerender_active_view();
            } else {
                request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS |
                                   UI_DIRTY_LYRICS | UI_DIRTY_INFO);
            }
            continue;
        }

        if (g_current_view != VIEW_MAIN) {
            /* 布局编辑器是模态的：除 Ctrl+C 外，所有键先交给它。
             * 否则 q 会在编辑器看到之前退出整个程序。 */
            if (layout_editor_active() && ch != 3) {
                handle_menu_input(ch);
                continue;
            }
            /* 退出是全局动作，必须在视图分发之前生效（原 q/Q 分支位置）。 */
            if (keymap_resolve(CTX_GLOBAL, ch) == ACTION_APP_QUIT) {
                log_info("ui", "Event loop exiting on quit action");
                break;
            }
            handle_menu_input(ch);
            continue;
        }

        /* ── 主视图：先解析上下文（popup → 控件 / 歌词 → 列表），再按动作分发 ── */
        ActionId action;
        if (g_control_focus == 1) {
            /* 弹窗激活时 popup 动作优先；其余按键回退到控件上下文。 */
            if (g_popup.active) {
                if (ch == 27) {
                    handle_popup_action(ACTION_POPUP_CANCEL);
                    continue;
                }
                ActionId popup_action = keymap_resolve(CTX_POPUP, ch);
                if (actions_context(popup_action) == CTX_POPUP) {
                    handle_popup_action(popup_action);
                    continue;
                }
            }
            action = keymap_resolve(CTX_MAIN_CONTROL, ch);
        } else {
            KeyContext list_context = CTX_MAIN_LIST;
            if (g_lyric_cursor_mode && player_lyrics_total() > 0) {
                list_context = CTX_MAIN_LYRIC;
            } else if (g_search_state.active || g_search_state.in_progress) {
                list_context = CTX_MAIN_SEARCH;
            }
            action = keymap_resolve(list_context, ch);
        }

        int main_rc = handle_main_action(action);
        if (main_rc == 2) {
            break;
        }
        continue;
    }
}

/* ============================================================
 * Cleanup
 * ============================================================ */

void cleanup(void)
{
    log_info("ui", "cleanup() called");

    /* 菜单窗口必须在 endwin() 之前销毁，否则 delwin 会落在已结束的屏幕上。 */
    ui_context_menu_close(0);

    search_async_cancel();
    pthread_mutex_lock(&g_search_mutex);
    if (g_search_state.in_progress) {
        pthread_t old_thread = g_search_state.thread;
        pthread_mutex_unlock(&g_search_mutex);
        pthread_join(old_thread, NULL);
    } else {
        pthread_mutex_unlock(&g_search_mutex);
    }


    /* 退出语义按后端分流（Task 4.3）：
     *   - local：本进程即核心，持久化会话、停止本地播放、等播放线程收尾；
     *   - remote：前端不得 Control.Stop 核心、不得写会话恢复配置（持久化归核心）；
     *            Detach 或 Quit 已由 main 的 player_shutdown 按 stop_core_on_exit
     *            决定，且仅在该前端已接入核心（握手完成且持有 Attach 令牌）时才发送，
     *            这里不再重复停止。若本进程因历史/回归路径真的初始化过音频引擎，
     *            下面也只收掉本进程自己的引擎，绝不向核心发命令。 */
    if (player_backend() == PLAYER_BACKEND_LOCAL) {
        persist_playback_session_state(); /* config-ownership-allow: local(TUI-即-核心)分支才会执行到这里的会话持久化；remote 前端被上方 guard 挡离，持久化归核心 */
    }

    /* 收掉本进程的音频引擎：local 正常收；remote 只在检测到本进程被历史/
     * 回归路径意外初始化过音频引擎时才收（正常远端前端没有本地音频）。
     * 由门面决定，界面不直接碰 stop_audio()/audio_backend_shutdown()。
     * 必须早于下面 media_session_shutdown() 释放主总线名：否则会留下
     * “名字没了、PipeWire 流还在”的窗口，新 TUI/新核心与旧进程各一路播放
     * → 叠播（多路重音）。 */
    player_teardown_audio();
    media_session_shutdown();
    library_shutdown();

    destroy_main_windows();

    endwin();
    ui_restore_stdio();  /* 恢复真实 stderr，随后的退出错误才能显示在终端 */
    reset_album_cover_cache();
    info_release_cover_cache();
    remote_view_shutdown();
    remote_cleanup();
}
