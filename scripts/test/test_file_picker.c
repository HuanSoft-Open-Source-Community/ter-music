/**
 * @file test_file_picker.c
 * @brief 统一文件/目录选择器的单元测试（ui/file_picker.h + ui/file_picker.c）
 *
 * 覆盖两层：
 *   1. 头文件里的纯逻辑：后端环境值解析、后端名、`~/` 展开的边界；
 *   2. 后端选择与 argv 拼接：用临时目录里可控的假 zenity/kdialog 脚本起真实
 *      fork/execvp 路径，断言目录模式确实带 `--directory` /
 *      `--getexistingdirectory`、文件模式不带 `--directory`、退出码语义
 *      （0=选中 / 1=取消 / 其它=错误）、无后端/无 curses 的可读错误，以及
 *      "有可执行文件但无图形会话 → auto 回退 TTY" 的回退选择。
 *
 * 测试不需要真实桌面/终端：假脚本只把 argv 写进日志并回显一行路径；
 * TTY 分支故意不初始化 curses，用来断言"回退到 TTY 且给出可读错误"。
 * 真实 pty 下的 TTY 手输/取消由 scripts/test/folder-picker-e2e.sh 覆盖。
 */

#include "ui/file_picker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 与 file_picker.c 的测试编译桩（TERM_MUSIC_FILE_PICKER_TEST_STUB_CURSES，
 * 见 test_file_picker.env）保持同一个 WINDOW 前向声明：只为给
 * prompt_text_input 桩一个签名，本测试不初始化 curses，也不碰任何 curses
 * 符号。 */
typedef struct _win_st WINDOW;

/* ── 依赖桩：i18n / prompt_text_input（logger 桩在 stub_logger.c）────── */

/* 只把本测试断言用到的键翻成可读文本，其余回退键名（与真实 i18n 的兜底一致）。
 * 注意返回的格式串带占位符，file_picker.c 会按自己的参数填充。 */
const char *i18n_get(const char *key)
{
    static const struct {
        const char *key;
        const char *text;
    } k_map[] = {
        { "file_picker.title", "Select a file" },
        { "file_picker.folder_title", "Select a folder" },
        { "file_picker.filter_name", "File" },
        { "file_picker.tty_prompt_fmt", "File path (%s): " },
        { "file_picker.tty_prompt_dir", "File path (%s; start %s): " },
        { "file_picker.tty_folder_prompt_fmt", "Folder path (%s): " },
        { "file_picker.tty_folder_prompt_dir", "Folder path (%s; start %s): " },
        { "file_picker.tty_fallback_file_fmt", "Dialog unavailable (%s); file path: " },
        { "file_picker.tty_fallback_folder_fmt", "Dialog unavailable (%s); folder path: " },
        { "file_picker.err.gui_unavailable", "dialog could not start" },
        { "file_picker.err.no_backend", "backend unavailable: %s" },
        { "file_picker.err.spawn", "cannot start chooser: %s" },
        { "file_picker.err.exit", "chooser exited abnormally (code %d)" },
        { "file_picker.err.path_too_long", "path is too long" },
        { "file_picker.err.not_dir", "not an existing folder: %s" },
        { "file_picker.err.no_curses", "needs the terminal interface" },
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (key && strcmp(key, k_map[i].key) == 0) {
            return k_map[i].text;
        }
    }
    return key ? key : "";
}

/* TTY 分支在 stdscr==NULL 时根本不会走到这里（file_picker.c 先报 no_curses），
 * 定义这个符号只为满足链接；真被调到就说明回退语义坏了。 */
int prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                      char *buffer, size_t buffer_size, int trim_whitespace,
                      int password_mode, int prefill)
{
    (void)win; (void)row; (void)col; (void)prompt; (void)buffer;
    (void)buffer_size; (void)trim_whitespace; (void)password_mode; (void)prefill;
    return -1;
}

/* ── 断言与工具 ───────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
            printf("  ok   %s\n", (what));                                   \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);      \
        }                                                                    \
    } while (0)

static char g_root[512];      /* 测试临时根目录 */
static char g_fakebin[600];   /* 假 zenity/kdialog 所在目录 */
static char g_emptybin[600];  /* 空 PATH 目录 */
static char g_logfile[600];   /* 假对话框 argv 日志 */
static char g_specfile[600];  /* 假对话框"输出路径 + 退出码"指令 */
static char g_saved_path[4096];

static void path_join(char *out, size_t out_size, const char *a, const char *b)
{
    snprintf(out, out_size, "%s/%s", a, b);
}

static int write_text(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    if (fputs(text, f) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    chmod(path, mode);
    return 0;
}

static int read_all(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 0;
}

static void log_reset(void)
{
    unlink(g_logfile);
}

static int log_has(const char *needle)
{
    char buf[8192];
    if (read_all(g_logfile, buf, sizeof(buf)) != 0) {
        return 0;
    }
    return strstr(buf, needle) != NULL;
}

static int log_count(const char *needle)
{
    char buf[8192];
    if (read_all(g_logfile, buf, sizeof(buf)) != 0) {
        return 0;
    }
    int count = 0;
    const char *p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

/* 假对话框指令：第 1 行 = 要回显的路径（空 = 不回显），第 2 行 = 退出码。 */
static void spec_set(const char *out_path, int code)
{
    char text[1024];
    snprintf(text, sizeof(text), "%s\n%d\n", out_path ? out_path : "", code);
    write_text(g_specfile, text, 0600);
}

static void env_set(const char *name, const char *value)
{
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

/* 递归删掉本次测试自己建的临时目录（不碰其它任何路径）。 */
static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[1024];
        path_join(child, sizeof(child), path, entry->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            unlink(child);
        }
    }
    closedir(dir);
    rmdir(path);
}

static const char *k_fake_dialog_body =
    "#!/bin/sh\n"
    "{\n"
    "  printf 'prog=%s\\n' \"${0##*/}\"\n"
    "  printf 'cwd=%s\\n' \"$(pwd)\"\n"
    "  printf 'argc=%s\\n' \"$#\"\n"
    "  for a in \"$@\"; do printf 'arg=%s\\n' \"$a\"; done\n"
    "  printf 'env_LD_LIBRARY_PATH=%s\\n' \"${LD_LIBRARY_PATH:-}\"\n"
    "  printf 'env_LD_PRELOAD=%s\\n' \"${LD_PRELOAD:-}\"\n"
    "  printf 'env_LD_AUDIT=%s\\n' \"${LD_AUDIT:-}\"\n"
    "} >> \"${TM_PICKER_TEST_LOG:?}\"\n"
    "# t8：子进程 stderr 必须被选择器丢弃；测试会捕获调用方 stderr 验证。\n"
    "printf 'TM_FAKE_DIALOG_STDERR_NOISE prog=%s\\n' \"${0##*/}\" >&2\n"
    "out=''\n"
    "code=0\n"
    "if [ -n \"${TM_PICKER_TEST_SPEC:-}\" ] && [ -f \"$TM_PICKER_TEST_SPEC\" ]; then\n"
    "  out=$(sed -n '1p' \"$TM_PICKER_TEST_SPEC\")\n"
    "  code=$(sed -n '2p' \"$TM_PICKER_TEST_SPEC\")\n"
    "  [ -n \"$code\" ] || code=0\n"
    "fi\n"
    "[ -n \"$out\" ] && printf '%s\\n' \"$out\"\n"
    "exit \"$code\"\n";

/* ── 1. 纯逻辑：环境值解析 / 后端名 / ~ 展开 ──────────────────────── */

static void test_pure_logic(void)
{
    printf("--- 纯逻辑：后端解析与 ~ 展开\n");

    CHECK(file_picker_backend_from_env(NULL) == FILE_PICKER_BACKEND_AUTO,
          "NULL 环境值 → AUTO");
    CHECK(file_picker_backend_from_env("") == FILE_PICKER_BACKEND_AUTO,
          "空环境值 → AUTO");
    CHECK(file_picker_backend_from_env("portal") == FILE_PICKER_BACKEND_PORTAL &&
          file_picker_backend_from_env("PORTAL") == FILE_PICKER_BACKEND_PORTAL,
          "portal/PORTAL → PORTAL（大小写不敏感）");
    CHECK(file_picker_backend_from_env("zenity") == FILE_PICKER_BACKEND_ZENITY &&
          file_picker_backend_from_env("ZENITY") == FILE_PICKER_BACKEND_ZENITY,
          "zenity/ZENITY → ZENITY");
    CHECK(file_picker_backend_from_env("kdialog") == FILE_PICKER_BACKEND_KDIALOG &&
          file_picker_backend_from_env("KDIALOG") == FILE_PICKER_BACKEND_KDIALOG,
          "kdialog/KDIALOG → KDIALOG");
    CHECK(file_picker_backend_from_env("tty") == FILE_PICKER_BACKEND_TTY &&
          file_picker_backend_from_env("TTY") == FILE_PICKER_BACKEND_TTY,
          "tty/TTY → TTY");
    CHECK(file_picker_backend_from_env("no-such-backend") == FILE_PICKER_BACKEND_AUTO,
          "未知值回退 AUTO（不拒绝打开选择框）");
    CHECK(strcmp(file_picker_backend_name(FILE_PICKER_BACKEND_PORTAL), "portal") == 0 &&
          strcmp(file_picker_backend_name(FILE_PICKER_BACKEND_ZENITY), "zenity") == 0 &&
          strcmp(file_picker_backend_name(FILE_PICKER_BACKEND_KDIALOG), "kdialog") == 0 &&
          strcmp(file_picker_backend_name(FILE_PICKER_BACKEND_TTY), "tty") == 0 &&
          strcmp(file_picker_backend_name(FILE_PICKER_BACKEND_AUTO), "auto") == 0,
          "后端名稳定（日志/错误文案用）");

    /* t8：GUI 对话框后端不需要真实终端，打开期间 TUI 必须保持备用屏幕；
     * 只有 TTY 后端在 curses 内手输。 */
    CHECK(file_picker_backend_uses_gui_dialog(FILE_PICKER_BACKEND_PORTAL) == 1 &&
          file_picker_backend_uses_gui_dialog(FILE_PICKER_BACKEND_ZENITY) == 1 &&
          file_picker_backend_uses_gui_dialog(FILE_PICKER_BACKEND_KDIALOG) == 1,
          "portal/zenity/kdialog 是 GUI 对话框后端（不需要终端让渡）");
    CHECK(file_picker_backend_uses_gui_dialog(FILE_PICKER_BACKEND_TTY) == 0 &&
          file_picker_backend_uses_gui_dialog(FILE_PICKER_BACKEND_AUTO) == 0 &&
          file_picker_backend_uses_gui_dialog(99) == 0,
          "TTY/AUTO/未知后端不按 GUI 对话框处理（TTY 在 curses 内手输）");

    char buf[256];
    CHECK(file_picker_expand_tilde("~/music", "/home/u", buf, sizeof(buf)) == 0 &&
          strcmp(buf, "/home/u/music") == 0,
          "~/music 展开为 home/music");
    CHECK(file_picker_expand_tilde("~", "/home/u", buf, sizeof(buf)) == 0 &&
          strcmp(buf, "/home/u") == 0,
          "裸 ~ 展开为 home");
    CHECK(file_picker_expand_tilde("/data/My Models/模型 1.onnx", "/home/u",
                                   buf, sizeof(buf)) == 0 &&
          strcmp(buf, "/data/My Models/模型 1.onnx") == 0,
          "绝对路径含空格/UTF-8 原样返回");
    CHECK(file_picker_expand_tilde("rel/a b", "/home/u", buf, sizeof(buf)) == 0 &&
          strcmp(buf, "rel/a b") == 0,
          "相对路径原样返回");
    CHECK(file_picker_expand_tilde("~user/a", "/home/u", buf, sizeof(buf)) == 0 &&
          strcmp(buf, "~user/a") == 0,
          "~user 不展开（只认 ~ 与 ~/）");
    CHECK(file_picker_expand_tilde("~/x", NULL, buf, sizeof(buf)) == 0 &&
          strcmp(buf, "~/x") == 0,
          "home 缺失时 ~/x 原样返回");
    CHECK(file_picker_expand_tilde(NULL, "/home/u", buf, sizeof(buf)) == -1,
          "in=NULL → -1");
    char small[4];
    CHECK(file_picker_expand_tilde("/very/long", "/home/u", small, sizeof(small)) == -1 &&
          small[0] == '\0',
          "输出缓冲不足 → -1 且清空输出");
}

/* ── 2. 后端选择与可用性（PATH + 图形会话，不执行对话框）────────────── */

static void test_backend_selection(void)
{
    printf("--- 后端选择：PATH + 图形会话 + 强制值\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);

    env_set(FILE_PICKER_ENV, NULL);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");
    env_set("WAYLAND_DISPLAY", NULL);
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_ZENITY,
          "auto：PATH 有 zenity 且有 DISPLAY → zenity");
    CHECK(file_picker_backend_available(FILE_PICKER_BACKEND_ZENITY) == 1,
          "zenity 可用（可执行 + 图形会话）");
    CHECK(file_picker_backend_available(FILE_PICKER_BACKEND_PORTAL) == 1,
          "portal 偏好值可用（由 zenity 承接）");

    env_set("DISPLAY", NULL);
    env_set("WAYLAND_DISPLAY", NULL);
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_TTY,
          "auto：有 zenity 但无 DISPLAY/WAYLAND → 回退 tty");
    CHECK(file_picker_backend_available(FILE_PICKER_BACKEND_ZENITY) == 0,
          "无图形会话时 zenity 不可用（不硬上）");
    CHECK(file_picker_backend_available(FILE_PICKER_BACKEND_TTY) == 1,
          "TTY 后端恒可用");

    env_set("DISPLAY", ":0");
    env_set("TERM_MUSIC_FILE_CHOOSER", "kdialog");
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_KDIALOG &&
          file_picker_backend_available(FILE_PICKER_BACKEND_KDIALOG) == 1,
          "强制 kdialog 生效且可用");

    env_set("TERM_MUSIC_FILE_CHOOSER", "zenity");
    env_set("DISPLAY", NULL);
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_ZENITY &&
          file_picker_backend_available(FILE_PICKER_BACKEND_ZENITY) == 0,
          "强制 zenity + 无图形会话：后端原样返回但不可用（由入口报错）");

    env_set("PATH", g_emptybin);
    env_set("DISPLAY", ":0");
    env_set("TERM_MUSIC_FILE_CHOOSER", NULL);
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_TTY,
          "auto：PATH 没有 zenity/kdialog → TTY");
    CHECK(file_picker_backend_available(FILE_PICKER_BACKEND_KDIALOG) == 0,
          "PATH 无 kdialog → 不可用");

    env_set("TERM_MUSIC_FILE_CHOOSER", "bogus");
    CHECK(file_picker_resolve_backend() == FILE_PICKER_BACKEND_TTY,
          "未知强制值按 auto 处理（无对话框时落到 TTY）");
    env_set("TERM_MUSIC_FILE_CHOOSER", NULL);
}

/* ── 3. 目录选择：zenity --directory / 取消 / 错误 / 校验 ───────────── */

static void test_folder_zenity(void)
{
    printf("--- 目录选择：zenity 后端（真实 fork+exec 假对话框）\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");
    env_set("WAYLAND_DISPLAY", NULL);
    env_set(FILE_PICKER_ENV, "zenity");

    char root_dir[600];
    path_join(root_dir, sizeof(root_dir), g_root, "root dir");
    mkdir(root_dir, 0755);

    char out[4096];
    char err[512];

    /* 正常选中：初始目录也传进去，验证 --filename 与实际 cwd。 */
    log_reset();
    spec_set(root_dir, 0);
    memset(out, 0xAA, sizeof(out));
    err[0] = '\0';
    int rc = file_picker_pick_folder("Pick Folder", root_dir,
                                     out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK, "选中目录 → FILE_PICKER_OK");
    CHECK(strcmp(out, root_dir) == 0, "返回路径与对话框输出一致");
    CHECK(err[0] == '\0', "成功时 err 为空");
    CHECK(log_has("prog=zenity"), "确实起的是 zenity");
    CHECK(log_has("arg=--file-selection"), "zenity 参数含 --file-selection");
    CHECK(log_has("arg=--directory"), "目录模式 zenity 参数含 --directory");
    CHECK(log_has("arg=--title=Pick Folder"), "标题原样传给 zenity");
    CHECK(log_count("arg=--file-filter=") == 0, "目录模式不再传文件过滤器");
    char filename_arg[700];
    snprintf(filename_arg, sizeof(filename_arg), "arg=--filename=%s/", root_dir);
    CHECK(log_has(filename_arg), "初始目录以 --filename=<dir>/ 传给 zenity");
    CHECK(log_has("cwd=") && log_has(root_dir), "子进程 cwd 切到初始目录");

    /* UTF-8 + 空格目录：路径原样返回，不做任何改写。 */
    char utf_dir[700];
    path_join(utf_dir, sizeof(utf_dir), g_root, "午休 音乐");
    mkdir(utf_dir, 0755);
    log_reset();
    spec_set(utf_dir, 0);
    rc = file_picker_pick_folder(NULL, NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK, "UTF-8/空格目录 → OK");
    CHECK(strcmp(out, utf_dir) == 0, "UTF-8/空格路径原样返回");
    CHECK(log_has("arg=--title=Select a folder"),
          "未给标题时用 file_picker.folder_title");
    CHECK(log_count("arg=--filename=") == 0, "无初始目录时不传 --filename");

    /* 取消：退出码 1，不写 err、不返回半截路径。 */
    log_reset();
    spec_set("", 1);
    memset(out, 0xAA, sizeof(out));
    err[0] = '\0';
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_CANCEL, "对话框取消 → FILE_PICKER_CANCEL");
    CHECK(out[0] == '\0', "取消时输出清空");
    CHECK(err[0] == '\0', "取消不当作错误（err 为空）");

    /* 后端"成功"但输出为空：同样按取消处理（后端异常但没报错）。 */
    log_reset();
    spec_set("", 0);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_CANCEL, "退出码 0 + 空输出 → CANCEL");

    /* 后端异常退出：错误可读，输出清空。 */
    log_reset();
    spec_set("", 3);
    err[0] = '\0';
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "对话框异常退出 → FILE_PICKER_ERROR");
    CHECK(out[0] == '\0', "错误时输出清空");
    CHECK(strstr(err, "exited abnormally") != NULL, "err 写可读原因（退出码）");

    /* 不存在 / 不是目录：入口统一校验，错误里带路径。 */
    char missing[700];
    path_join(missing, sizeof(missing), g_root, "no such dir");
    log_reset();
    spec_set(missing, 0);
    err[0] = '\0';
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "不存在的目录 → FILE_PICKER_ERROR");
    CHECK(strstr(err, "not an existing folder") != NULL && strstr(err, missing) != NULL,
          "err 指明缺少的目录（含路径）");
    CHECK(out[0] == '\0', "非法目录不返回路径");

    char regular_file[700];
    path_join(regular_file, sizeof(regular_file), g_root, "not-a-dir.txt");
    write_text(regular_file, "x", 0600);
    spec_set(regular_file, 0);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "普通文件当目录 → FILE_PICKER_ERROR");
    CHECK(strstr(err, "not an existing folder") != NULL, "普通文件也报可读原因");

    /* shell 风格 ~/ 输入（外部对话框一般不会回传，但保持统一展开语义）。 */
    char home_saved[1024];
    snprintf(home_saved, sizeof(home_saved), "%s", getenv("HOME") ? getenv("HOME") : "");
    env_set("HOME", g_root);
    log_reset();
    spec_set("~/root dir", 0);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK && strcmp(out, root_dir) == 0,
          "对话框回传 ~/root dir 也展开后校验通过");
    env_set("HOME", home_saved[0] ? home_saved : NULL);

    /* 输出缓冲不够：路径过长错误（不截断成另一个目录）。 */
    log_reset();
    spec_set(root_dir, 0);
    char tiny[8];
    rc = file_picker_pick_folder("Pick Folder", NULL, tiny, sizeof(tiny),
                                 err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR && strstr(err, "too long") != NULL,
          "输出缓冲不足 → 路径过长错误（不截断）");
    CHECK(tiny[0] == '\0', "路径过长时输出清空");

    /* 强制后端不可用：报可读错误，且不执行对话框。 */
    log_reset();
    env_set("PATH", g_emptybin);
    err[0] = '\0';
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "强制 zenity 但 PATH 无 zenity → ERROR");
    CHECK(strstr(err, "backend unavailable") != NULL && strstr(err, "zenity") != NULL,
          "错误说明是哪个后端不可用");
    CHECK(log_count("prog=") == 0, "后端不可用时不执行任何对话框");
}

/* ── 4. 目录选择：kdialog --getexistingdirectory ───────────────────── */

static void test_folder_kdialog(void)
{
    printf("--- 目录选择：kdialog 后端\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");
    env_set(FILE_PICKER_ENV, "kdialog");

    char root_dir[600];
    path_join(root_dir, sizeof(root_dir), g_root, "root dir");

    char out[4096];
    char err[512];
    log_reset();
    spec_set(root_dir, 0);
    int rc = file_picker_pick_folder("Pick Folder", g_root,
                                     out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK && strcmp(out, root_dir) == 0,
          "kdialog 目录选择 → OK");
    CHECK(log_has("prog=kdialog"), "确实起的是 kdialog");
    CHECK(log_has("arg=--getexistingdirectory"),
          "目录模式 kdialog 参数含 --getexistingdirectory");
    CHECK(log_count("arg=--getopenfilename") == 0,
          "目录模式不传 --getopenfilename");
    CHECK(log_count("arg=--file-filter=") == 0, "目录模式不传过滤器");

    /* portal 偏好值：由 zenity 承接（与 pick_file 同一语义）。 */
    env_set(FILE_PICKER_ENV, "portal");
    log_reset();
    spec_set(root_dir, 0);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK && strcmp(out, root_dir) == 0,
          "portal 偏好值由 zenity 承接目录选择");
    CHECK(log_has("prog=zenity") && log_has("arg=--directory"),
          "portal → zenity 目录模式 argv 正确");
    env_set(FILE_PICKER_ENV, "kdialog");

    /* 取消与错误语义和 zenity 一致。 */
    log_reset();
    spec_set("", 1);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_CANCEL, "kdialog 取消 → CANCEL");
    log_reset();
    spec_set("", 2);
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "kdialog 非 0/1 退出 → ERROR");
}

/* ── 5. 文件选择语义不变（语言包入口依赖）──────────────────────────── */

static void test_file_mode_semantics(void)
{
    printf("--- 文件选择：既有语义保持（不校验存在性 / 不传 --directory）\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");

    char out[4096];
    char err[512];

    env_set(FILE_PICKER_ENV, "zenity");
    log_reset();
    spec_set("/tmp/does-not-exist/我的 语言包.tar.gz", 0);
    int rc = file_picker_pick_file("Pick Pack", "Language pack",
                                   "*.tar.gz;*.tgz", NULL,
                                   out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK, "文件选择不校验文件存在性（语义保持）");
    CHECK(strcmp(out, "/tmp/does-not-exist/我的 语言包.tar.gz") == 0,
          "文件路径含空格/UTF-8 原样返回");
    CHECK(log_count("arg=--directory") == 0, "文件模式不传 --directory");
    CHECK(log_has("arg=--file-filter=Language pack | *.tar.gz *.tgz"),
          "文件模式过滤器 ';' → ' ' 传给 zenity");

    env_set(FILE_PICKER_ENV, "kdialog");
    log_reset();
    spec_set("/tmp/x.tar.gz", 0);
    rc = file_picker_pick_file("Pick Pack", "Language pack",
                               "*.tar.gz;*.tgz", g_root,
                               out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK && strcmp(out, "/tmp/x.tar.gz") == 0,
          "kdialog 文件选择 → OK");
    CHECK(log_has("arg=--getopenfilename"), "文件模式 kdialog 用 --getopenfilename");
    CHECK(log_count("arg=--getexistingdirectory") == 0,
          "文件模式不传 --getexistingdirectory");
    CHECK(log_has("arg=*.tar.gz *.tgz|Language pack"), "kdialog 文件过滤器格式不变");
}

/* ── 6. 桌面不可达时的回退选择与可读错误 ───────────────────────────── */

static void test_fallbacks(void)
{
    printf("--- 回退：无桌面 → TTY；无 curses → 可读错误\n");

    char out[4096];
    char err[512];

    /* 没有 DISPLAY/WAYLAND、PATH 里也没有对话框：auto 必须落到 TTY。
     * 单测里没有初始化 curses，所以这里得到"需要终端界面"的可读错误，
     * 而不是空等外部对话框——真正的 TTY 手输由 E2E 覆盖。 */
    env_set("PATH", g_emptybin);
    env_set("DISPLAY", NULL);
    env_set("WAYLAND_DISPLAY", NULL);
    env_set(FILE_PICKER_ENV, NULL);
    err[0] = '\0';
    int rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out),
                                     err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR, "无桌面且无 curses → ERROR（不挂起）");
    CHECK(strstr(err, "terminal interface") != NULL,
          "错误说明需要终端界面（TTY 回退路径）");
    CHECK(out[0] == '\0', "失败时不输出路径");

    env_set(FILE_PICKER_ENV, "tty");
    err[0] = '\0';
    rc = file_picker_pick_folder("Pick Folder", NULL, out, sizeof(out),
                                 err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR && strstr(err, "terminal interface") != NULL,
          "强制 tty 且无 curses → 同一可读错误");
    env_set(FILE_PICKER_ENV, NULL);

    /* 文件选择同样回退 TTY（不是直接报"没有后端"）。 */
    env_set(FILE_PICKER_ENV, NULL);
    err[0] = '\0';
    rc = file_picker_pick_file("Pick File", NULL, NULL, NULL,
                               out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_ERROR && strstr(err, "terminal interface") != NULL,
          "文件选择在无桌面时也回退 TTY（可读错误）");
}

/* ── 6b. 外部对话框不得继承 AppImage 的动态链接器环境（t18）────────── */

static void test_child_env_sanitized(void)
{
    printf("--- 外部对话框：LD_LIBRARY_PATH/LD_PRELOAD/LD_AUDIT 必须清理\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");
    env_set(FILE_PICKER_ENV, "zenity");

    char dir[600];
    path_join(dir, sizeof(dir), g_root, "env dir");
    mkdir(dir, 0755);

    char saved_ld[1024] = "";
    char saved_pre[1024] = "";
    char saved_audit[1024] = "";
    const char *v = getenv("LD_LIBRARY_PATH");
    if (v) snprintf(saved_ld, sizeof(saved_ld), "%s", v);
    v = getenv("LD_PRELOAD");
    if (v) snprintf(saved_pre, sizeof(saved_pre), "%s", v);
    v = getenv("LD_AUDIT");
    if (v) snprintf(saved_audit, sizeof(saved_audit), "%s", v);

    env_set("LD_LIBRARY_PATH", "/tmp/appimage-old-libs");
    env_set("LD_PRELOAD", "/tmp/appimage-old-libs/libstdc++.so.6");
    env_set("LD_AUDIT", "/tmp/appimage-old-libs/libaudit.so");

    char out[4096];
    char err[512];
    log_reset();
    spec_set(dir, 0);
    int rc = file_picker_pick_folder("env", NULL, out, sizeof(out), err, sizeof(err));
    CHECK(rc == FILE_PICKER_OK && strcmp(out, dir) == 0,
          "清理动态链接器环境后仍能正常选中（PATH 仍可解析 zenity）");
    CHECK(log_has("env_LD_LIBRARY_PATH=\n") &&
          !log_has("env_LD_LIBRARY_PATH=/tmp/appimage-old-libs"),
          "子进程看不到 LD_LIBRARY_PATH（AppImage 旧库不泄漏给宿主对话框）");
    CHECK(log_has("env_LD_PRELOAD=\n") &&
          !log_has("env_LD_PRELOAD=/tmp/appimage-old-libs"),
          "子进程看不到 LD_PRELOAD");
    CHECK(log_has("env_LD_AUDIT=\n") &&
          !log_has("env_LD_AUDIT=/tmp/appimage-old-libs"),
          "子进程看不到 LD_AUDIT");

    env_set("LD_LIBRARY_PATH", saved_ld[0] ? saved_ld : NULL);
    env_set("LD_PRELOAD", saved_pre[0] ? saved_pre : NULL);
    env_set("LD_AUDIT", saved_audit[0] ? saved_audit : NULL);
    env_set(FILE_PICKER_ENV, NULL);
}

/* ── 7. 外部对话框的 stderr 不得落到调用方终端（t8）────────────────── */

static void test_child_stderr_not_inherited(void)
{
    printf("--- 外部对话框：stdout 回传路径、stderr 必须丢弃\n");

    char path_env[8192];
    snprintf(path_env, sizeof(path_env), "%s:%s", g_fakebin, g_saved_path);
    env_set("PATH", path_env);
    env_set("DISPLAY", ":0");
    env_set(FILE_PICKER_ENV, "zenity");

    char dir[600];
    path_join(dir, sizeof(dir), g_root, "stderr dir");
    mkdir(dir, 0755);

    char capture_path[700];
    path_join(capture_path, sizeof(capture_path), g_root, "child-stderr-capture.txt");

    /* 把本测试进程的 stderr 临时改到文件：若选择器让子进程继承 stderr，
     * 假脚本写的噪音就会落进这个文件。先写一条对照组证明捕获窗口有效。 */
    fflush(stderr);
    int saved_fd = dup(STDERR_FILENO);
    int cap_fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(saved_fd >= 0 && cap_fd >= 0, "可建立 stderr 捕获文件");
    if (saved_fd < 0 || cap_fd < 0) {
        if (saved_fd >= 0) {
            close(saved_fd);
        }
        if (cap_fd >= 0) {
            close(cap_fd);
        }
        return;
    }
    dup2(cap_fd, STDERR_FILENO);
    close(cap_fd);

    char out[4096];
    char err[512];
    log_reset();
    spec_set(dir, 0);
    int rc = file_picker_pick_folder("stderr", NULL, out, sizeof(out),
                                     err, sizeof(err));

    fputs("PARENT_STDERR_CONTROL\n", stderr);
    fflush(stderr);
    dup2(saved_fd, STDERR_FILENO);
    close(saved_fd);

    CHECK(rc == FILE_PICKER_OK && strcmp(out, dir) == 0,
          "stdout 仍只回传选中的路径（stderr 改道不影响结果）");

    char captured[4096];
    CHECK(read_all(capture_path, captured, sizeof(captured)) == 0 &&
          strstr(captured, "PARENT_STDERR_CONTROL") != NULL,
          "stderr 捕获对照组命中（捕获窗口有效）");
    CHECK(strstr(captured, "TM_FAKE_DIALOG_STDERR_NOISE") == NULL,
          "子进程 stderr 没有继承到调用方终端（噪音被丢弃）");
}

int main(void)
{
    /* 临时根目录 + 假后端脚本；测试自建自删，不依赖仓库内任何夹具。 */
    snprintf(g_root, sizeof(g_root), "/tmp/tm-file-picker-test-XXXXXX");
    if (!mkdtemp(g_root)) {
        fprintf(stderr, "FAIL cannot create temp dir: %s\n", strerror(errno));
        return 1;
    }
    path_join(g_fakebin, sizeof(g_fakebin), g_root, "bin");
    path_join(g_emptybin, sizeof(g_emptybin), g_root, "empty-bin");
    path_join(g_logfile, sizeof(g_logfile), g_root, "dialog.log");
    path_join(g_specfile, sizeof(g_specfile), g_root, "dialog.spec");
    mkdir(g_fakebin, 0755);
    mkdir(g_emptybin, 0755);

    char script_path[700];
    path_join(script_path, sizeof(script_path), g_fakebin, "zenity");
    write_text(script_path, k_fake_dialog_body, 0755);
    path_join(script_path, sizeof(script_path), g_fakebin, "kdialog");
    write_text(script_path, k_fake_dialog_body, 0755);

    const char *saved_path = getenv("PATH");
    snprintf(g_saved_path, sizeof(g_saved_path), "%s", saved_path ? saved_path : "");
    setenv("TM_PICKER_TEST_LOG", g_logfile, 1);
    setenv("TM_PICKER_TEST_SPEC", g_specfile, 1);

    test_pure_logic();
    test_backend_selection();
    test_folder_zenity();
    test_folder_kdialog();
    test_child_stderr_not_inherited();
    test_child_env_sanitized();
    test_file_mode_semantics();
    test_fallbacks();

    unsetenv("TM_PICKER_TEST_LOG");
    unsetenv("TM_PICKER_TEST_SPEC");
    env_set("PATH", g_saved_path[0] ? g_saved_path : NULL);
    remove_tree(g_root);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
