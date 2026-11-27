/**
 * @file file_picker.c
 * @brief 文件选择框实现：桌面对话框（zenity/kdialog）优先，纯 TTY 回退手输
 *
 * 设计与"portal 偏好值"的取舍见 ui/file_picker.h 的头注释（接口已预留，
 * 不自己实现 D-Bus FileChooser）。本文件只做三件事：
 *
 *   1. 选后端（环境变量强制 / 按 PATH 探测）；
 *   2. 起外部对话框并收 stdout 里的一行路径（不用 shell，路径可含空格/UTF-8）；
 *   3. 纯 TTY 时用 ui/dialog.c 的 prompt_text_input 手输（空输入 = 取消）。
 *
 * 关键约束（都是实测/既有代码踩过的坑）：
 *  - **外部对话框打开期间不退出 curses 备用屏幕**：zenity/kdialog 是 GUI 进程，
 *    不读终端输入、不写 stdout（路径走管道），不需要也不应该 endwin()。旧实现
 *    的 def_prog_mode()+endwin() 会发 rmcup 离开备用屏幕，对话框背后露出启动
 *    TUI 前的 shell 内容（t8 用户报障"界面临时退出"的根因）。现在只在打开前/
 *    返回后各 refresh() 一次补差异，不 clearok（避免整屏闪烁）。TTY 后端本来就
 *    在 curses 里手输，由 prompt_text_input 自己刷新。
 *  - 子进程的 stdin/stdout/stderr 全部改道：stdout 是唯一结果通道（一行路径），
 *    stderr 丢 /dev/null（GTK/Qt 的告警绝不能画到 curses 屏幕上），stdin 接
 *    /dev/null（绝不让对话框偷走 TUI 的按键）。
 *  - 外部命令一律 argv 数组 execvp，**不拼 shell 命令行**：路径含空格、
 *    引号、UTF-8 时行为可预期，也没有注入面。
 *  - 退出码语义：0 = 选中（stdout 一行），1 = 用户取消；其它按失败报。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#include "ui/file_picker.h"

#include "i18n/i18n.h"
#include "logger/logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef TERM_MUSIC_FILE_PICKER_TEST_STUB_CURSES
/* 单测专用编译桩：某些 CI/开发容器只有 ncurses 运行库、没有开发头（本项目
 * 的容器就是：头在 /opt/host-tools 下而编译器默认搜索路径没有它）。这里只
 * 声明 file_picker.c 真正用到的符号；单测不初始化 curses，TTY 分支在
 * stdscr==NULL 处就返回"需要终端界面"的可读错误，真正的 TUI 输入由
 * scripts/test/folder-picker-e2e.sh 用 pty 覆盖。生产构建永远走真实头文件。 */
typedef struct _win_st WINDOW;
extern WINDOW *stdscr;
extern int isendwin(void);
extern int refresh(void);
extern int noecho(void);
extern int curs_set(int visibility);
extern int move(int y, int x);
extern int clrtoeol(void);
#define getmaxyx(win, y, x) \
    do { (void)(win); (y) = 0; (x) = 0; } while (0)
#else
#include <ncursesw/ncurses.h>
#endif

/* 本模块只用 ui/dialog.c 的 prompt_text_input 一个入口。这里直接声明而不是
 * include "ui/dialog.h"：那个头会把 ncurses 头带进编译面，单测（见上面的
 * 编译桩）没必要为它拖进整套系统头；签名与 ui/dialog.h 完全一致。 */
int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                      char *buffer, size_t buffer_size, int trim_whitespace,
                      int password_mode, int prefill);

/* 对话框返回路径的最大长度：与模型域的 KARAOKE_MODEL_PATH_CAP 同量级，
 * 超长一律当失败（不截断——截断出来的路径指向另一个文件，比报错更坏）。 */
#define PICKER_PATH_CAP 4096

static void picker_set_err(char *err, size_t err_size, const char *fmt, ...)
{
    if (!err || err_size == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

/* ── PATH 探测 ────────────────────────────────────────────────────── */

/* 名字里带 '/' 时按路径判可执行，否则在 PATH 里逐个目录找。
 * PATH 的空项（连续冒号）按规范表示当前目录，这里跳过——不让"当前目录里
 * 碰巧有个 zenity"改变行为。 */
static int picker_has_exec(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return 0;
    }

    char candidate[1024];
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t dir_len = end ? (size_t)(end - p) : strlen(p);
        if (dir_len > 0 &&
            dir_len + 1 + strlen(name) + 1 <= sizeof(candidate)) {
            memcpy(candidate, p, dir_len);
            candidate[dir_len] = '/';
            snprintf(candidate + dir_len + 1, sizeof(candidate) - dir_len - 1,
                     "%s", name);
            if (access(candidate, X_OK) == 0) {
                return 1;
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 0;
}

/* 图形会话是否在线：zenity/kdialog 都需要一个可连的显示。headless/SSH 下
 * PATH 里即使装了它们也弹不出窗口，必须当作"不可用"（否则用户会等一个
 * 永远不会出现的对话框，或在超时后收到一条看不懂的错误）。 */
static int picker_has_display(void)
{
    const char *display = getenv("DISPLAY");
    if (display && display[0]) {
        return 1;
    }
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0]) {
        return 1;
    }
    return 0;
}

int file_picker_backend_available(int backend)
{
    switch (backend) {
        case FILE_PICKER_BACKEND_TTY:
            /* 纯终端后端只需要 curses 已初始化（调用方在设置页里，一定是）。
             * 这里返回"可用"，真正的 stdscr 判空在 picker_pick_tty() 里。 */
            return 1;
        case FILE_PICKER_BACKEND_ZENITY:
            return picker_has_display() && picker_has_exec("zenity");
        case FILE_PICKER_BACKEND_KDIALOG:
            return picker_has_display() && picker_has_exec("kdialog");
        case FILE_PICKER_BACKEND_PORTAL:
            /* portal 偏好值由桌面对话框承接（见头注释）：任一可用即算可用。 */
            return picker_has_display() &&
                   (picker_has_exec("zenity") || picker_has_exec("kdialog"));
        case FILE_PICKER_BACKEND_AUTO:
        default:
            return 0;
    }
}

int file_picker_resolve_backend(void)
{
    int forced = file_picker_backend_from_env(getenv(FILE_PICKER_ENV));
    if (forced != FILE_PICKER_BACKEND_AUTO) {
        return forced;   /* 强制值原样返回：不可用由调用方报明确错误 */
    }
    /* auto：可执行文件与图形会话都齐了才选桌面对话框，否则纯 TTY 手输。 */
    if (picker_has_display() && picker_has_exec("zenity")) {
        return FILE_PICKER_BACKEND_ZENITY;
    }
    if (picker_has_display() && picker_has_exec("kdialog")) {
        return FILE_PICKER_BACKEND_KDIALOG;
    }
    /* 无桌面：纯 TTY 手输路径（"纯 TTY 回退"是契约要求的最后一级） */
    return FILE_PICKER_BACKEND_TTY;
}

/* ── curses 画面保持 ─────────────────────────────────────────────── */

/* curses 是否正在使用中：stdscr 为空表示调用方根本没 initscr（纯 CLI 场景）。 */
static int picker_curses_active(void)
{
    return stdscr != NULL;
}

/* 外部 GUI 对话框打开前/返回后各刷一次 curses 缓冲：
 *   - 打开前：确保对话框出现时终端上的 TUI 画面已是最新（没有挂起的更新）；
 *   - 返回后：补上窗口系统/焦点切换可能触发的终端重绘差异。
 * **绝不 endwin()/clearok()**：前者会发 rmcup 退出备用屏幕（对话框背后露出
 * shell 内容，t8 根因），后者整屏重画会闪。TTY 后端不需要（prompt 自己刷新）。 */
static void picker_sync_tui(void)
{
    if (!picker_curses_active() || isendwin()) {
        return;
    }
    refresh();
}

/* ── 外部对话框 ───────────────────────────────────────────────────── */

/* 起一个外部对话框，把它的 stdout 当作路径读回来。
 *   - argv 由调用方给（不含 NULL 结尾，count 给出）；
 *   - 子进程 cwd 切到 initial_dir（失败忽略，用后端默认目录）；
 *   - 退出码 0 = 选中、1 = 取消（zenity/kdialog 的约定），其它 = 失败。 */
static int picker_run_capture(char *const argv[], int argc,
                              const char *initial_dir,
                              char *out, size_t out_size,
                              char *err, size_t err_size)
{
    if (argc <= 0 || !argv || !argv[0]) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.no_backend"));
        return FILE_PICKER_ERROR;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.spawn"),
                       strerror(errno));
        return FILE_PICKER_ERROR;
    }

    pid_t pid = fork();
    if (pid < 0) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.spawn"),
                       strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return FILE_PICKER_ERROR;
    }

    if (pid == 0) {
        /* 子进程：只保留 stdout 管道，stdin/stderr 全部改道，绝不碰 TUI。
         * stderr 必须丢弃而不是继承终端：TUI 现在**不退出备用屏幕**（t8），
         * GTK/Qt 的告警若落到同一个 tty 会直接画花 curses 画面。 */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        } else {
            /* /dev/null 打不开（几乎不可能）：宁可关掉 stderr（写入得到
             * EBADF）也不能让它继承 curses 的终端。 */
            close(STDERR_FILENO);
        }
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close(fds[0]);
        close(fds[1]);

        if (initial_dir && initial_dir[0]) {
            if (chdir(initial_dir) != 0) {
                /* 初始目录不可用不是致命错误：用后端自己的默认位置 */
                log_debug("file_picker", "initial dir '%s' unusable: %s",
                          initial_dir, strerror(errno));
            }
        }

        execvp(argv[0], argv);
        _exit(127);   /* 走到这里 = 执行失败（PATH 竞态等） */
    }

    /* 父进程：读结果（足够小，不会填满管道），再收尸。 */
    close(fds[1]);
    char buf[PICKER_PATH_CAP + 2];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fds[0], buf + total, sizeof(buf) - 1 - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        if (total + 1 >= sizeof(buf)) {
            break;   /* 超长：下面按"路径过长"报错 */
        }
    }
    close(fds[0]);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.spawn"),
                       strerror(errno));
        return FILE_PICKER_ERROR;
    }

    if (WIFSIGNALED(status)) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.exit"),
                       WTERMSIG(status));
        return FILE_PICKER_ERROR;
    }
    if (!WIFEXITED(status)) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.exit"), -1);
        return FILE_PICKER_ERROR;
    }

    int code = WEXITSTATUS(status);
    if (code == 1 || code == 5) {
        /* zenity: 1 = 取消、5 = 超时（等价"用户没选"，不报错刷屏） */
        return FILE_PICKER_CANCEL;
    }
    if (code != 0) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.exit"), code);
        return FILE_PICKER_ERROR;
    }

    /* 去掉结尾换行；空输出按取消处理（后端异常但没报错的情况） */
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r')) {
        total--;
    }
    buf[total] = '\0';
    if (total == 0) {
        return FILE_PICKER_CANCEL;
    }
    if (total >= out_size) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.path_too_long"));
        return FILE_PICKER_ERROR;
    }
    memcpy(out, buf, total + 1);
    return FILE_PICKER_OK;
}

/* 选择模式：同一个对话框后端既能挑文件也能挑目录，argv 只在几处不同。 */
typedef enum {
    PICKER_MODE_FILE   = 0,
    PICKER_MODE_FOLDER = 1
} PickerMode;

static int picker_pick_zenity(PickerMode mode, const char *title,
                              const char *filter_name, const char *filter_glob,
                              const char *initial_dir,
                              char *out, size_t out_size,
                              char *err, size_t err_size)
{
    char title_arg[512];
    char filter_arg[512];
    char filename_arg[PICKER_PATH_CAP + 32];

    snprintf(title_arg, sizeof(title_arg), "--title=%s",
             (title && title[0]) ? title
                                 : i18n_get(mode == PICKER_MODE_FOLDER
                                                ? "file_picker.folder_title"
                                                : "file_picker.title"));

    char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)"zenity";
    argv[argc++] = (char *)"--file-selection";
    if (mode == PICKER_MODE_FOLDER) {
        /* zenity 的目录模式：--file-selection --directory */
        argv[argc++] = (char *)"--directory";
    } else {
        /* zenity 3 的过滤器语法："显示名 | *.onnx *.ONNX"（空格分隔多个模式） */
        char globs[256];
        snprintf(globs, sizeof(globs), "%s",
                 (filter_glob && filter_glob[0]) ? filter_glob : "*");
        for (char *p = globs; *p; p++) {
            if (*p == ';') {
                *p = ' ';
            }
        }
        snprintf(filter_arg, sizeof(filter_arg), "--file-filter=%s | %s",
                 (filter_name && filter_name[0]) ? filter_name
                                                 : i18n_get("file_picker.filter_name"),
                 globs);
        argv[argc++] = filter_arg;
    }
    argv[argc++] = title_arg;
    if (initial_dir && initial_dir[0]) {
        snprintf(filename_arg, sizeof(filename_arg), "--filename=%s/", initial_dir);
        argv[argc++] = filename_arg;
    }
    argv[argc] = NULL;

    return picker_run_capture(argv, argc, initial_dir, out, out_size, err, err_size);
}

static int picker_pick_kdialog(PickerMode mode, const char *title,
                               const char *filter_name, const char *filter_glob,
                               const char *initial_dir,
                               char *out, size_t out_size,
                               char *err, size_t err_size)
{
    /* kdialog 的目录模式：--getexistingdirectory <起始目录>
     * kdialog 的文件模式：--getopenfilename <起始目录> <过滤器>
     * 过滤器语法："*.onnx|显示名"（多个模式用空格分隔） */
    char title_value[512];
    char filter_arg[384];
    const char *start_dir = (initial_dir && initial_dir[0]) ? initial_dir : NULL;

    if (!start_dir) {
        start_dir = getenv("HOME");
    }
    if (!start_dir || !start_dir[0]) {
        start_dir = "/";
    }
    snprintf(title_value, sizeof(title_value), "%s",
             (title && title[0]) ? title
                                 : i18n_get(mode == PICKER_MODE_FOLDER
                                                ? "file_picker.folder_title"
                                                : "file_picker.title"));

    char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)"kdialog";
    argv[argc++] = (char *)"--title";
    argv[argc++] = title_value;
    if (mode == PICKER_MODE_FOLDER) {
        argv[argc++] = (char *)"--getexistingdirectory";
        argv[argc++] = (char *)start_dir;
    } else {
        char globs[256];
        snprintf(globs, sizeof(globs), "%s",
                 (filter_glob && filter_glob[0]) ? filter_glob : "*");
        for (char *p = globs; *p; p++) {
            if (*p == ';') {
                *p = ' ';
            }
        }
        snprintf(filter_arg, sizeof(filter_arg), "%s|%s", globs,
                 (filter_name && filter_name[0]) ? filter_name
                                                 : i18n_get("file_picker.filter_name"));
        argv[argc++] = (char *)"--getopenfilename";
        argv[argc++] = (char *)start_dir;
        argv[argc++] = filter_arg;
    }
    argv[argc] = NULL;

    return picker_run_capture(argv, argc, start_dir, out, out_size, err, err_size);
}

/* ── 纯 TTY：手输路径 ─────────────────────────────────────────────── */

static int picker_pick_tty(PickerMode mode, const char *title, const char *initial_dir,
                           char *out, size_t out_size,
                           char *err, size_t err_size)
{
    if (!picker_curses_active()) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.no_curses"));
        return FILE_PICKER_ERROR;
    }

    char input[PICKER_PATH_CAP];
    input[0] = '\0';

    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    int prompt_row = rows - 3;
    if (prompt_row < 1) {
        prompt_row = 1;
    }
    int prompt_col = cols / 4;
    if (prompt_col < 2) {
        prompt_col = 2;
    }

    /* 提示里带上标题与初始目录（如果有），让用户知道该填什么。
     * 目录模式换一组文案（"目录路径"而不是"文件路径"）。 */
    char prompt[768];
    const char *hint = (title && title[0])
        ? title
        : i18n_get(mode == PICKER_MODE_FOLDER ? "file_picker.folder_title"
                                              : "file_picker.title");
    const char *fmt_key = (mode == PICKER_MODE_FOLDER)
        ? "file_picker.tty_folder_prompt_fmt" : "file_picker.tty_prompt_fmt";
    const char *dir_key = (mode == PICKER_MODE_FOLDER)
        ? "file_picker.tty_folder_prompt_dir" : "file_picker.tty_prompt_dir";
    if (initial_dir && initial_dir[0]) {
        snprintf(prompt, sizeof(prompt), i18n_get(dir_key), hint, initial_dir);
    } else {
        snprintf(prompt, sizeof(prompt), i18n_get(fmt_key), hint);
    }

    noecho();
    curs_set(1);
    prompt_text_input(stdscr, prompt_row, prompt_col, prompt,
                      input, sizeof(input), 1, 0, 0);
    /* 无论回车还是中断都要把终端/光标属性收回原状（调用方随后整体重绘）。 */
    noecho();
    curs_set(0);
    move(prompt_row, 0);
    clrtoeol();
    refresh();

    if (input[0] == '\0') {
        return FILE_PICKER_CANCEL;   /* 空输入 = 取消 */
    }

    if (file_picker_expand_tilde(input, getenv("HOME"), out, out_size) != 0) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.path_too_long"));
        return FILE_PICKER_ERROR;
    }
    return FILE_PICKER_OK;
}

/* ── 入口 ─────────────────────────────────────────────────────────── */

/* 文件/目录两种模式共用的后端分发：选后端 → 可用性检查 → 起对应的拾取器。
 * 返回后端原始结果（目录模式的 stat 校验由 file_picker_pick_folder 做，
 * 这样"文件选择不校验存在性"的既有语义不被改变）。 */
static int picker_pick(PickerMode mode, const char *title,
                       const char *filter_name, const char *filter_glob,
                       const char *initial_dir,
                       char *out, size_t out_size,
                       char *err, size_t err_size)
{
    if (!out || out_size == 0) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.no_curses"));
        return FILE_PICKER_ERROR;
    }
    out[0] = '\0';
    if (err && err_size) {
        err[0] = '\0';
    }

    int backend = file_picker_resolve_backend();
    log_debug("file_picker", "backend=%s mode=%s title='%s' filter='%s'",
              file_picker_backend_name(backend),
              mode == PICKER_MODE_FOLDER ? "folder" : "file",
              title ? title : "", filter_glob ? filter_glob : "");

    /* 强制后端不可用：如实报错，不偷偷换成另一个（脚本测试要能断言）。 */
    if (!file_picker_backend_available(backend)) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.no_backend"),
                       file_picker_backend_name(backend));
        return FILE_PICKER_ERROR;
    }

    /* GUI 对话框打开期间**保持 curses 备用屏幕**（不 endwin）：只需在拉起前
     * 把挂起的界面更新刷到终端，返回后再刷一次补差异。TTY 后端在 curses 内
     * 手输，prompt_text_input 自己负责刷新，这里不插手。 */
    const int gui_dialog = file_picker_backend_uses_gui_dialog(backend);
    if (gui_dialog) {
        picker_sync_tui();
    }

    int rc = FILE_PICKER_ERROR;

    switch (backend) {
        case FILE_PICKER_BACKEND_TTY:
            rc = picker_pick_tty(mode, title, initial_dir, out, out_size,
                                 err, err_size);
            break;

        case FILE_PICKER_BACKEND_ZENITY:
            rc = picker_pick_zenity(mode, title, filter_name, filter_glob,
                                    initial_dir, out, out_size, err, err_size);
            break;

        case FILE_PICKER_BACKEND_KDIALOG:
            rc = picker_pick_kdialog(mode, title, filter_name, filter_glob,
                                     initial_dir, out, out_size, err, err_size);
            break;

        case FILE_PICKER_BACKEND_PORTAL:
            /* portal 偏好值：由桌面对话框承接（见头注释）；zenity 优先。 */
            if (picker_has_exec("zenity")) {
                rc = picker_pick_zenity(mode, title, filter_name, filter_glob,
                                        initial_dir, out, out_size, err, err_size);
            } else if (picker_has_exec("kdialog")) {
                rc = picker_pick_kdialog(mode, title, filter_name, filter_glob,
                                         initial_dir, out, out_size, err, err_size);
            } else {
                picker_set_err(err, err_size, i18n_get("file_picker.err.no_backend"),
                               file_picker_backend_name(backend));
                rc = FILE_PICKER_ERROR;
            }
            break;

        case FILE_PICKER_BACKEND_AUTO:
        default:
            picker_set_err(err, err_size, i18n_get("file_picker.err.no_backend"),
                           file_picker_backend_name(backend));
            rc = FILE_PICKER_ERROR;
            break;
    }

    if (gui_dialog) {
        picker_sync_tui();
    }

    if (rc == FILE_PICKER_OK) {
        log_info("file_picker", "picked '%s' via %s",
                 out, file_picker_backend_name(backend));
    } else if (rc == FILE_PICKER_CANCEL) {
        log_debug("file_picker", "cancelled via %s",
                  file_picker_backend_name(backend));
        out[0] = '\0';
    } else {
        log_warn("file_picker", "failed via %s: %s",
                 file_picker_backend_name(backend), err ? err : "");
        out[0] = '\0';
    }
    return rc;
}

int file_picker_pick_file(const char *title, const char *filter_name,
                          const char *filter_glob, const char *initial_dir,
                          char *out, size_t out_size,
                          char *err, size_t err_size)
{
    return picker_pick(PICKER_MODE_FILE, title, filter_name, filter_glob,
                       initial_dir, out, out_size, err, err_size);
}

int file_picker_pick_folder(const char *title, const char *initial_dir,
                            char *out, size_t out_size,
                            char *err, size_t err_size)
{
    char raw[PICKER_PATH_CAP];

    int rc = picker_pick(PICKER_MODE_FOLDER, title, NULL, NULL, initial_dir,
                         raw, sizeof(raw), err, err_size);
    if (rc != FILE_PICKER_OK) {
        if (out && out_size) {
            out[0] = '\0';
        }
        return rc;
    }

    /* 目录选择的统一收口：展开 ~/ 后必须存在且是目录。放在入口处而不是各
     * 后端里，四种后端（含 portal 承接的桌面对话框）语义才完全一致。 */
    char expanded[PICKER_PATH_CAP];
    if (file_picker_expand_tilde(raw, getenv("HOME"), expanded,
                                 sizeof(expanded)) != 0) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.path_too_long"));
        if (out && out_size) {
            out[0] = '\0';
        }
        return FILE_PICKER_ERROR;
    }

    struct stat st;
    if (stat(expanded, &st) != 0 || !S_ISDIR(st.st_mode)) {
        picker_set_err(err, err_size, i18n_get("file_picker.err.not_dir"), expanded);
        log_warn("file_picker", "folder rejected (not a directory): %s", expanded);
        if (out && out_size) {
            out[0] = '\0';
        }
        return FILE_PICKER_ERROR;
    }

    size_t len = strlen(expanded);
    if (!out || out_size == 0 || len >= out_size) {
        picker_set_err(err, err_size, "%s", i18n_get("file_picker.err.path_too_long"));
        if (out && out_size) {
            out[0] = '\0';
        }
        return FILE_PICKER_ERROR;
    }
    memcpy(out, expanded, len + 1);
    log_info("file_picker", "folder '%s' accepted", out);
    return FILE_PICKER_OK;
}
