/**
 * @file test_tty_guard_keypad_pty.c
 * @brief t25/t31 回归：F2 之后方向键必须仍被 ncurses 解码成 KEY_UP，而不是裸 ESC
 *
 * 做法（真 pty，不用 mock）：
 *   - 子进程：initscr() + keypad(stdscr, TRUE)（与 ter-music 主界面一致），
 *     把要验证的“F2 序列”原样 write(2) 到自己的终端，然后 getch() 解码一次按键，
 *     用退出码报告解码结果（0=KEY_UP，1=裸 ESC(27)，2=其它）。
 *   - 父进程：扮演终端。用 terminfo 的 `kcuu1` 判断该终端的方向键是 SS3(`ESC O A`)
 *     还是 CSI(`ESC [ A`)；有 smkx 的终端（xterm/screen）再按自己模拟的 DECCKM
 *     状态选择 SS3/CSI，没有 smkx 的终端（linux/vt220，kcuu1 本身就是 CSI）永远发 CSI。
 *
 * 三个臂：
 *   1) TERM=xterm-256color + 修复后序列（core + terminfo smkx）→ 应用模式 → KEY_UP；
 *   2) TERM=xterm-256color + 修复前序列（core + ESC[?1l）→ 普通模式 → 裸 ESC(27)
 *      （这就是“设置页按方向键退回主界面”的回归，钉死对照）；
 *   3) TERM=linux（无 smkx，kcuu1=ESC[A）+ 修复后序列（core + 空串 = 什么都不补发）
 *      → CSI → KEY_UP；并且序列里不得凭空出现 `?1h`/`?1l`。
 *
 * 运行：scripts/test/run-unit-tests.sh test_tty_guard_keypad_pty
 * 依赖：同名 .env 提供 `-lncursesw`；序列构造来自 ui/tty_guard.h（纯头文件）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/tty_guard.h"

#include <fcntl.h>
#include <ncursesw/ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* 修复前的尾部（t25 之前的 UI_TTY_MODE_RESET_SEQ 以它结尾） */
#define OLD_RMKX_TAIL "\033[?1l"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                            \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_failures++;                                            \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

/* 子进程：在 pty 上初始化 ncurses，写出 seq，再解码一次按键。 */
static int child_run(const char *term, const char *seq, size_t len)
{
    setenv("TERM", term, 1);
    setenv("ESCDELAY", "100", 1);   /* 未匹配的 ESC 前缀尽快返回，别等 1s */

    if (!initscr()) {
        _exit(3);
    }
    keypad(stdscr, TRUE);
    set_escdelay(100);

    /* 走裸 fd：模拟 putp() 直接把序列写到终端（不经过 ncurses 缓冲）。 */
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, seq + off, len - off);
        if (n <= 0) {
            endwin();
            _exit(4);
        }
        off += (size_t)n;
    }

    /* 哨兵：告诉父进程“序列已完整写出”，并告知该终端方向键的编码形态
     * （terminfo kcuu1 以 ESC O 开头 = SS3，否则 CSI）。只走输出流，
     * 不会进入 getch 的输入。 */
    const char *up = tigetstr("kcuu1");
    char sentinel = (up && up[0] == '\033' && up[1] == 'O') ? '\001' : '\002';
    (void)!write(STDOUT_FILENO, &sentinel, 1);

    int ch = getch();
    endwin();
    if (ch == KEY_UP) {
        _exit(0);
    }
    if (ch == 27) {
        _exit(1);
    }
    _exit(2);
}

/* 父进程：开 pty、起子进程、跟 DECCKM 状态、按终端类型发方向键、回收退出码。
 * 返回 0 表示用例正常跑完（*out_key 为子进程解码结果），-1 表示 harness 失败。 */
static int run_case(const char *term, const char *seq, int *out_key)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    const char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        cfmakeraw(&tio);          /* pty 只是字节管道，不做行规程处理 */
        tcsetattr(slave, TCSANOW, &tio);
    }

    size_t len = strlen(seq);
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }
    if (pid == 0) {
        close(master);
        setsid();
        (void)!ioctl(slave, TIOCSCTTY, 0);   /* 没有控制 tty 也能跑（stdin/stdout 即可） */
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        child_run(term, seq, len);
        _exit(5);
    }
    close(slave);

    /* 读子进程输出，跟踪 DECCKM（有 smkx 的终端才有意义）；收到哨兵 \001
     * 之后按“终端类型 + 当时的 DECCKM”发送方向键。 */
    char buf[4096];
    char window[64] = {0};
    size_t wlen = 0;
    int app_mode = 1;             /* ncurses 启动时 keypad(TRUE) 会先发 smkx（若有） */
    int up_is_ss3 = 0;            /* 子进程按 kcuu1 报告 */
    int sent = 0;

    for (int guard = 0; guard < 400 && !sent; guard++) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (wlen < sizeof(window) - 1) {
                window[wlen++] = buf[i];
                window[wlen] = '\0';
            } else {
                memmove(window, window + 1, sizeof(window) - 2);
                window[sizeof(window) - 2] = buf[i];
                window[sizeof(window) - 1] = '\0';
            }
            /* 最近一次 DECCKM 状态（"ESC[?1h"=应用模式 / "ESC[?1l"=普通模式） */
            if (wlen >= 3 && strncmp(window + wlen - 3, "?1h", 3) == 0) {
                app_mode = 1;
            } else if (wlen >= 3 && strncmp(window + wlen - 3, "?1l", 3) == 0) {
                app_mode = 0;
            }
            if (buf[i] == '\001' || buf[i] == '\002') {
                up_is_ss3 = (buf[i] == '\001');
                const char *arrow = (up_is_ss3 && app_mode) ? "\033OA" : "\033[A";
                if (write(master, arrow, 3) != 3) {
                    break;
                }
                sent = 1;
            }
        }
    }

    if (!sent) {
        /* harness 失败：不兜底乱发按键（那会把失败掩盖成“箭头问题”）。 */
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(master);
        printf("  harness: sentinel not observed (seq not flushed?)\n");
        return -1;
    }

    int status = 0;
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            break;
        }
        usleep(20000);
    }
    if (!WIFEXITED(status)) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        close(master);
        return -1;
    }
    *out_key = WEXITSTATUS(status);
    close(master);
    return 0;
}

int main(void)
{
    if (getenv("TERM") == NULL) {
        setenv("TERM", "xterm-256color", 1);
    }

    /* ① xterm-256color（有 smkx）：核心序列 + terminfo keypad_xmit */
    char with_smkx[128];
    snprintf(with_smkx, sizeof(with_smkx), "%s%s", ui_tty_guard_mode_reset_core(),
             ui_tty_guard_keypad_restore_seq("\033[?1h\033="));
    CHECK(strstr(with_smkx, "?1l") == NULL, "F2 (smkx) has no rmkx");
    CHECK(strstr(with_smkx, "?1h") != NULL, "F2 (smkx) restores DECCKM app mode");

    int key = -1;
    int rc = run_case("xterm-256color", with_smkx, &key);
    CHECK(rc == 0, "pty run (xterm-256color + smkx) completed");
    printf("  [xterm-256color + smkx] rc=%d keycode=%d (0=KEY_UP 1=ESC)\n", rc, key);
    CHECK(rc == 0 && key == 0, "xterm + smkx -> arrow decoded as KEY_UP");

    /* ② 回归对照：修复前序列（core + ESC[?1l）→ 普通模式 → 裸 ESC */
    char old_seq[128];
    snprintf(old_seq, sizeof(old_seq), "%s%s", ui_tty_guard_mode_reset_core(),
             OLD_RMKX_TAIL);
    CHECK(strstr(old_seq, "?1l") != NULL, "old F2 control contains rmkx");
    key = -1;
    rc = run_case("xterm-256color", old_seq, &key);
    CHECK(rc == 0, "pty run (old F2 control) completed");
    printf("  [xterm-256color + old rmkx] rc=%d keycode=%d\n", rc, key);
    CHECK(rc == 0 && key == 1, "old F2 -> arrow degrades to bare ESC (regression)");

    /* ③ 无 smkx 的终端（linux：kcuu1=ESC[A，无 keypad_xmit）：
     *    恢复序列是空串（什么都不发），F2 不得让方向键退化成裸 ESC。 */
    char no_smkx[128];
    const char *absent = ui_tty_guard_keypad_restore_seq(NULL);
    snprintf(no_smkx, sizeof(no_smkx), "%s%s", ui_tty_guard_mode_reset_core(), absent);
    CHECK(absent[0] == '\0', "no terminfo smkx -> nothing to restore");
    CHECK(strstr(no_smkx, "?1h") == NULL, "TERM=linux F2 must NOT invent DECCKM on");
    CHECK(strstr(no_smkx, "?1l") == NULL, "TERM=linux F2 must NOT send rmkx either");
    key = -1;
    rc = run_case("linux", no_smkx, &key);
    CHECK(rc == 0, "pty run (TERM=linux) completed");
    printf("  [linux] rc=%d keycode=%d (0=KEY_UP 1=ESC)\n", rc, key);
    CHECK(rc == 0 && key == 0, "TERM=linux -> arrow still decoded as KEY_UP");

    if (g_failures == 0) {
        printf("tty_guard_keypad_pty: %d checks passed\n", g_checks);
        return 0;
    }
    printf("tty_guard_keypad_pty: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
