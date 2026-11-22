/**
 * @file remote.c
 * @brief 远程音乐源协议实现（FTP/SFTP/WebDAV/HTTP 走 libcurl，SMB 走 smbclient）
 *
 * 安全/健壮性约定（对应审计报告）：
 *   - 空响应按空目录成功处理，绝不对 NULL 缓冲做字符串操作（F-01）；
 *   - SMB 用 fork+execvp 传递 argv 与 0600 认证文件，不经过 shell，密码也
 *     不出现在子进程 argv（F-02/F-05/F-06/F-24）；
 *   - URL 文本不含凭据，日志只打印脱敏 URL（F-05）；
 *   - TLS 由显式标志决定，非 443 的 https 仍是 https（F-08）；
 *   - 列目录/下载都有字节上限与总超时；传输期间可被取消钩子中断（F-09/F-10）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote.h"
#include "remote/remote_parse.h"
#include "remote/remote_url.h"

#include "logger/logger.h"

#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 列目录响应上限：超过按错误处理，避免恶意服务器把前端 OOM（F-10） */
#define REMOTE_LISTING_MAX_BYTES (32u * 1024u * 1024u)
/* 单曲下载上限：超过中止并清理 .part（F-10） */
#define REMOTE_FETCH_MAX_BYTES (1024LL * 1024LL * 1024LL)
/* 单曲下载总超时（秒）：低速率服务器不能无限拖住会话（F-10） */
#define REMOTE_FETCH_TIMEOUT 600L
#define REMOTE_LISTING_TIMEOUT 120L
#define REMOTE_CONNECT_TIMEOUT 10L
#define REMOTE_LOW_SPEED_TIME 60L
#define SMB_DEFAULT_PORT 445

/* ── 每线程错误缓冲（F-26：工作线程写、UI 线程读不再互相踩） ──────── */

static __thread char g_remote_errbuf[256];

void remote_set_error(const char *msg)
{
    snprintf(g_remote_errbuf, sizeof(g_remote_errbuf), "%s", msg ? msg : "");
    log_debug("remote", "Error set: %s", g_remote_errbuf[0] ? g_remote_errbuf : "(null)");
}

const char *remote_strerror(void)
{
    return g_remote_errbuf;
}

/* ── 中断钩子（F-09） ─────────────────────────────────────────────── */

static int (*g_abort_check)(void *ctx) = NULL;
static void *g_abort_ctx = NULL;

void remote_set_abort_check(int (*check)(void *ctx), void *ctx)
{
    g_abort_check = check;
    g_abort_ctx = ctx;
}

static int abort_requested(void)
{
    return g_abort_check ? g_abort_check(g_abort_ctx) : 0;
}

/* 在途传输/工作线程计数：remote_cleanup() 绝不在还有使用者时清理 libcurl。 */
static int g_transfers_in_flight = 0;
static int g_workers_active = 0;

void remote_set_worker_active(int active)
{
    if (active) {
        __sync_add_and_fetch(&g_workers_active, 1);
    } else {
        __sync_sub_and_fetch(&g_workers_active, 1);
    }
}

static void transfer_enter(void)
{
    __sync_add_and_fetch(&g_transfers_in_flight, 1);
}

static void transfer_leave(void)
{
    __sync_sub_and_fetch(&g_transfers_in_flight, 1);
}

/* ── 生命周期 ─────────────────────────────────────────────────────── */

void remote_init(void)
{
    log_info("remote", "Initializing remote subsystem (libcurl)");
    curl_global_init(CURL_GLOBAL_ALL);
}

void remote_cleanup(void)
{
    /* UI 退出时先 remote_cache_shutdown()（会 join worker）；这里再等一小会儿，
     * 确保没有任何传输仍在 libcurl 内部，然后才做全局清理（F-09）。 */
    for (int i = 0; i < 50 && g_transfers_in_flight > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (g_transfers_in_flight > 0 || g_workers_active > 0) {
        log_warn("remote", "Remote cleanup skipped: %d transfer(s) in flight, %d worker(s) active",
                 g_transfers_in_flight, g_workers_active);
        return;
    }
    curl_global_cleanup();
    log_info("remote", "Remote subsystem cleaned up");
}

int remote_is_remote_path(const char *path)
{
    return path && strstr(path, "://") != NULL;
}

const char *remote_protocol_name(int protocol)
{
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return "SMB";
        case REMOTE_PROTOCOL_SFTP:   return "SFTP";
        case REMOTE_PROTOCOL_FTP:    return "FTP";
        case REMOTE_PROTOCOL_WEBDAV: return "WebDAV";
        case REMOTE_PROTOCOL_HTTP:   return "HTTP";
        default:                     return "?";
    }
}

/* ── 通用响应缓冲（带上限、可中断） ───────────────────────────────── */

struct write_buf {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    int overflow;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    struct write_buf *buf = (struct write_buf *)userdata;

    if (abort_requested()) {
        return 0;
    }
    if (total > buf->limit || buf->len > buf->limit - total) {
        buf->overflow = 1;
        return 0;                        /* 触发 CURLE_WRITE_ERROR */
    }
    if (buf->len + total + 1 > buf->cap) {
        size_t next = buf->len + total + 8192;
        if (next > buf->limit + 1) {
            next = buf->limit + 1;
        }
        char *tmp = realloc(buf->data, next);
        if (!tmp) {
            return 0;
        }
        buf->data = tmp;
        buf->cap = next;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static void write_buf_free(struct write_buf *buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

/* ── curl 句柄与错误映射 ──────────────────────────────────────────── */

static void set_http_error(long code)
{
    char message[128];
    switch (code) {
        case 401: snprintf(message, sizeof(message), "Authentication failed (HTTP 401)"); break;
        case 403: snprintf(message, sizeof(message), "Forbidden (HTTP 403)"); break;
        case 404: snprintf(message, sizeof(message), "Not found (HTTP 404)"); break;
        case 405: snprintf(message, sizeof(message), "Method not allowed (HTTP 405)"); break;
        case 500: snprintf(message, sizeof(message), "Server error (HTTP 500)"); break;
        case 501: snprintf(message, sizeof(message), "Not implemented (HTTP 501)"); break;
        case 507: snprintf(message, sizeof(message), "Insufficient storage (HTTP 507)"); break;
        default:
            snprintf(message, sizeof(message), "HTTP error %ld", code);
            break;
    }
    remote_set_error(message);
}

static void set_curl_error(CURLcode res)
{
    const char *message = NULL;
    switch (res) {
        case CURLE_COULDNT_RESOLVE_HOST: message = "Cannot resolve host"; break;
        case CURLE_COULDNT_CONNECT:      message = "Cannot connect to server"; break;
        case CURLE_LOGIN_DENIED:         message = "Authentication failed"; break;
        case CURLE_REMOTE_FILE_NOT_FOUND:
        case CURLE_FTP_COULDNT_RETR_FILE: message = "Remote file not found"; break;
        case CURLE_OPERATION_TIMEDOUT:   message = "Operation timed out"; break;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:    message = "TLS certificate verification failed"; break;
        case CURLE_UNSUPPORTED_PROTOCOL: message = "Unsupported remote protocol"; break;
        case CURLE_FILESIZE_EXCEEDED:    message = "Remote file is too large"; break;
        case CURLE_WRITE_ERROR:          message = "Transfer aborted (write limit or cancelled)"; break;
        case CURLE_ABORTED_BY_CALLBACK:  message = "Transfer cancelled"; break;
        default:                         message = curl_easy_strerror(res); break;
    }
    remote_set_error(message);
}

static int ssl_insecure_requested(const RemoteConnectionConfig *conn)
{
    return conn->insecure_tls || getenv("TER_MUSIC_REMOTE_INSECURE_TLS") != NULL;
}

static CURL *create_curl_handle(const RemoteConnectionConfig *conn, const char *full_url,
                                int for_listing)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        remote_set_error("Cannot initialise libcurl");
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REMOTE_CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     for_listing ? REMOTE_LISTING_TIMEOUT : REMOTE_FETCH_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (conn->username[0]) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, conn->username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, conn->password);
        if (conn->protocol == REMOTE_PROTOCOL_HTTP ||
            conn->protocol == REMOTE_PROTOCOL_WEBDAV) {
            /* Basic/Digest/NTLM 都交给 libcurl 协商（F-27） */
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
        }
    }

    if (conn->protocol == REMOTE_PROTOCOL_HTTP ||
        conn->protocol == REMOTE_PROTOCOL_WEBDAV) {
        long redir_protocols = conn->use_tls ? CURLPROTO_HTTPS
                                             : (CURLPROTO_HTTP | CURLPROTO_HTTPS);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         conn->use_tls ? "https" : "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, redir_protocols);
#endif
    }

    /* TLS 校验选项与协议无关：HTTP/WebDAV 的 https、FTP 的 ftps 都要用
     * insecure_tls / CURL_CA_BUNDLE（R-04/F-27）。 */
    if ((conn->protocol == REMOTE_PROTOCOL_HTTP ||
         conn->protocol == REMOTE_PROTOCOL_WEBDAV ||
         conn->protocol == REMOTE_PROTOCOL_FTP) && conn->use_tls) {
        if (ssl_insecure_requested(conn)) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        } else {
            /* 显式尊重标准 CA 环境变量：某些发行版/打包的 libcurl 不读
             * CURL_CA_BUNDLE，用户（与 E2E 的自签名 CA）需要它。 */
            const char *ca_bundle = getenv("CURL_CA_BUNDLE");
            if (!ca_bundle || !ca_bundle[0]) {
                ca_bundle = getenv("SSL_CERT_FILE");
            }
            if (ca_bundle && ca_bundle[0]) {
                curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
                log_debug("remote", "Using CA bundle: %s", ca_bundle);
            }
        }
    }

    if (conn->protocol == REMOTE_PROTOCOL_SFTP) {
        if (conn->private_key_path[0]) {
            curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, conn->private_key_path);
            curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES,
                             (long)(CURLSSH_AUTH_PUBLICKEY | CURLSSH_AUTH_PASSWORD |
                                    CURLSSH_AUTH_AGENT));
            if (conn->has_key_passphrase && conn->key_passphrase[0]) {
                curl_easy_setopt(curl, CURLOPT_KEYPASSWD, conn->key_passphrase);
            }
        } else {
            curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES, (long)CURLSSH_AUTH_ANY);
        }
        /* host key 校验（F-13）：**fail-closed**。
         * libcurl 只有在设置了 CURLOPT_SSH_KNOWNHOSTS 时才校验 host key；
         * 缺文件时静默跳过等于允许 MITM。因此这里先解析文件：
         *   - 连接显式配置 known_hosts_path 时用它；
         *   - 否则用 ~/.ssh/known_hosts；
         *   - 文件缺失/不可读 → 拒绝连接并给出可操作提示（绝不静默连）。 */
        char known_hosts[MAX_PATH_LEN];
        known_hosts[0] = '\0';
        if (conn->known_hosts_path[0]) {
            snprintf(known_hosts, sizeof(known_hosts), "%s", conn->known_hosts_path);
        } else {
            const char *home = getenv("HOME");
            if (home && home[0]) {
                snprintf(known_hosts, sizeof(known_hosts), "%s/.ssh/known_hosts", home);
            }
        }
        if (known_hosts[0] == '\0' || access(known_hosts, R_OK) != 0) {
            char message[256];
            snprintf(message, sizeof(message),
                     "SFTP refused: host key verification requires a readable known_hosts "
                     "file (configured: %s). Set known_hosts_path in the connection or create "
                     "~/.ssh/known_hosts, e.g. ssh-keyscan HOST >> ~/.ssh/known_hosts",
                     known_hosts[0] ? known_hosts : "(none)");
            remote_set_error(message);
            log_error("remote", "SFTP rejected: no readable known_hosts file (%s)",
                      known_hosts[0] ? known_hosts : "(none)");
            curl_easy_cleanup(curl);
            return NULL;
        }
        curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS, known_hosts);
        log_debug("remote", "SFTP host key verification enabled: %s", known_hosts);
    }

    return curl;
}

/* 进度回调：只用于取消（F-09）。libcurl < 7.32 没有 XFERINFO，退回旧接口。 */
#if LIBCURL_VERSION_NUM >= 0x072000
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return abort_requested() ? 1 : 0;
}
#else
static int progress_cb(void *clientp, double dltotal, double dlnow,
                       double ultotal, double ulnow)
{
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return abort_requested() ? 1 : 0;
}
#endif

static void enable_abort_callback(CURL *curl)
{
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
#else
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, NULL);
#endif
}

/* ── SMB：fork+execvp（无 shell、无密码 argv） ────────────────────── */

/* smbclient 的 -c 参数有自己的解析器（支持 ';' 命令分隔），因此远端名字里出现
 * 双引号/控制字符时拒绝构造命令，避免把远端可控文本变成 smbclient 指令。 */
/* smbclient 的 -c 参数由 Samba process_command_string() 解析：它先按 ';'
 * 切分命令再处理引号，因此远端可控的 ';' 等于命令分隔符（F-02/R-01）。
 * '!' 在 smbclient 交互解析里也有特殊含义，防御性一并拒绝；引号/反斜杠/
 * 控制字符同样会破坏命令结构。合法文件名不受影响（仅这些字符被拒绝）。 */
static int smb_path_is_safe(const char *path)
{
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == ';' || *p == '!' || *p == '"' || *p == '\\' ||
            *p < 0x20 || *p == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static const char *smb_share_of(const RemoteConnectionConfig *conn, char *out, size_t out_size)
{
    const char *base = conn->base_path;
    while (*base == '/') base++;
    size_t len = strcspn(base, "/");
    if (len == 0 || len >= out_size) {
        return NULL;
    }
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

struct smb_run_result {
    char *output;
    size_t length;
    int exit_status;
    int spawn_failed;
    int read_failed;
    int cancelled;
    int timed_out;
    int timeout_sec;
};

static void smb_result_free(struct smb_run_result *result)
{
    free(result->output);
    memset(result, 0, sizeof(*result));
}

/* 运行 smbclient，captured stdout。@return 0 已运行（exit_status 有意义），
 * -1 连进程都没起来（缺 smbclient/资源不足）。 */
static int smb_run(const RemoteConnectionConfig *conn, const char *command,
                   int timeout_sec, struct smb_run_result *result)
{
    memset(result, 0, sizeof(*result));
    result->timeout_sec = timeout_sec;

    char share[256];
    if (!smb_share_of(conn, share, sizeof(share))) {
        remote_set_error("SMB URL must contain a share name");
        return -1;
    }

    char auth_path[] = "/tmp/ter-music-smb-auth-XXXXXX";
    int auth_fd = mkstemp(auth_path);
    if (auth_fd < 0) {
        remote_set_error("Cannot create temporary SMB auth file");
        return -1;
    }
    fchmod(auth_fd, 0600);
    char auth[1024];
    int auth_len = snprintf(auth, sizeof(auth), "username = %s\npassword = %s\n",
                            conn->username, conn->password);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(auth) ||
        write(auth_fd, auth, (size_t)auth_len) != auth_len) {
        close(auth_fd);
        unlink(auth_path);
        remote_set_error("Cannot write temporary SMB auth file");
        return -1;
    }
    close(auth_fd);

    char host[sizeof(conn->host) + 4];
    if (strchr(conn->host, ':')) {
        snprintf(host, sizeof(host), "[%s]", conn->host);
    } else {
        snprintf(host, sizeof(host), "%s", conn->host);
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", conn->port > 0 ? conn->port : SMB_DEFAULT_PORT);

    char *argv[12];
    int argc = 0;
    argv[argc++] = (char *)"smbclient";
    char unc[600];
    snprintf(unc, sizeof(unc), "//%s/%s", host, share);
    argv[argc++] = unc;
    argv[argc++] = (char *)"-A";
    argv[argc++] = auth_path;
    argv[argc++] = (char *)"-p";
    argv[argc++] = port;
    argv[argc++] = (char *)"-c";
    argv[argc++] = (char *)command;
    argv[argc] = NULL;

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        unlink(auth_path);
        remote_set_error("Cannot create pipe for smbclient");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        unlink(auth_path);
        remote_set_error("Cannot fork smbclient");
        return -1;
    }
    if (pid == 0) {
        /* 子进程：只做 exec，不碰父进程状态 */
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        setenv("LC_ALL", "C.UTF-8", 1);
        setenv("LANG", "C.UTF-8", 1);
        execvp("smbclient", argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    int pipe_flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (pipe_flags >= 0) {
        fcntl(pipe_fds[0], F_SETFL, pipe_flags | O_NONBLOCK);
    }
    size_t cap = 8192;
    size_t length = 0;
    char *output = malloc(cap);
    if (!output) {
        close(pipe_fds[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        unlink(auth_path);
        remote_set_error("Out of memory reading smbclient output");
        return -1;
    }

    int status = 0;
    int child_done = 0;
    time_t deadline = time(NULL) + timeout_sec;
    while (!child_done) {
        for (;;) {
            if (length + 4096 + 1 > cap) {
                size_t next = cap * 2;
                if (next > REMOTE_LISTING_MAX_BYTES) {
                    next = REMOTE_LISTING_MAX_BYTES + 1;
                }
                if (next <= cap) {
                    result->read_failed = 1;
                    break;
                }
                char *grown = realloc(output, next);
                if (!grown) {
                    result->read_failed = 1;
                    break;
                }
                output = grown;
                cap = next;
            }
            ssize_t got = read(pipe_fds[0], output + length, cap - length - 1);
            if (got > 0) {
                length += (size_t)got;
                if (length > REMOTE_LISTING_MAX_BYTES) {
                    result->read_failed = 1;
                    break;
                }
                continue;
            }
            if (got == 0) {
                child_done = 1;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            result->read_failed = 1;
            break;
        }
        if (result->read_failed) {
            kill(pid, SIGTERM);
            break;
        }
        if (child_done) {
            break;
        }
        if (abort_requested()) {
            result->cancelled = 1;
            kill(pid, SIGTERM);
            break;
        }
        if (time(NULL) >= deadline) {
            result->timed_out = 1;
            kill(pid, SIGTERM);
            break;
        }

        struct pollfd pfd;
        pfd.fd = pipe_fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno != EINTR) {
            result->read_failed = 1;
            kill(pid, SIGTERM);
            break;
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            child_done = 1;
        }
    }
    close(pipe_fds[0]);

    if (!child_done) {
        /* 取消/超时/读失败：先给 SIGTERM 机会，再 SIGKILL，并 wait 回收，
         * 绝不留孤儿 smbclient（R-03）。 */
        kill(pid, SIGTERM);
        int reaped = 0;
        for (int i = 0; i < 20; i++) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { reaped = 1; break; }
            usleep(50 * 1000);
        }
        if (!reaped) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        }
    } else {
        /* stdout EOF 不等于进程已退出：限时回收，不阻塞 UI 退出。 */
        int reaped = 0;
        for (int i = 0; i < 20; i++) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { reaped = 1; break; }
            if (w < 0 && errno == ECHILD) { reaped = 1; break; }
            usleep(10 * 1000);
        }
        if (!reaped) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 20; i++) {
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid) { reaped = 1; break; }
                usleep(50 * 1000);
            }
            if (!reaped) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            }
        }
    }
    output[length] = '\0';
    unlink(auth_path);

    result->output = output;
    result->length = length;
    result->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (result->exit_status == 127 && length == 0) {
        result->spawn_failed = 1;        /* execvp 找不到 smbclient（F-06） */
    }
    if (result->cancelled || result->timed_out || result->read_failed) {
        result->exit_status = -1;
    }
    return 0;
}

/* smbclient 的错误常打在 stdout；提取第一条 NT_STATUS_* 供 UI 显示。 */
static void smb_set_error_from_output(const struct smb_run_result *result)
{
    if (result->cancelled) {
        remote_set_error("SMB operation cancelled");
        return;
    }
    if (result->timed_out) {
        char message[96];
        snprintf(message, sizeof(message), "SMB operation timed out after %d s",
                 result->timeout_sec);
        remote_set_error(message);
        return;
    }
    if (result->spawn_failed) {
        remote_set_error("SMB support requires the 'smbclient' program "
                         "(install the samba client package and retry)");
        return;
    }
    if (result->read_failed) {
        remote_set_error("SMB directory listing is too large");
        return;
    }
    if (result->output) {
        const char *hit = strstr(result->output, "NT_STATUS_");
        if (hit) {
            char line[160];
            size_t len = strcspn(hit, "\r\n");
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, hit, len);
            line[len] = '\0';
            char message[220];
            snprintf(message, sizeof(message), "SMB error: %s", line);
            remote_set_error(message);
            return;
        }
    }
    char message[96];
    snprintf(message, sizeof(message), "smbclient failed (exit %d)", result->exit_status);
    remote_set_error(message);
}

/* 把 subpath（含 share 前缀）换成共享内的相对路径 */
static void smb_relative_path(const RemoteConnectionConfig *conn, const char *subpath,
                              char *out, size_t out_size)
{
    const char *base = conn->base_path;
    while (*base == '/') base++;
    size_t base_len = strcspn(base, "/");   /* share 名长度 */
    const char *sub = subpath ? subpath : "";
    while (*sub == '/') sub++;
    if (strncmp(sub, base, base_len) == 0) {
        sub += base_len;
        while (*sub == '/') sub++;
    }
    if (*sub == '\0') {
        snprintf(out, out_size, ".");
    } else {
        snprintf(out, out_size, "%s", sub);
    }
}

static int smb_list_via_client(const RemoteConnectionConfig *conn,
                               const char *subpath,
                               RemoteDirEntry **out_entries,
                               int *out_count)
{
    char rel_path[1024];
    smb_relative_path(conn, subpath, rel_path, sizeof(rel_path));
    if (!smb_path_is_safe(rel_path)) {
        remote_set_error("SMB path contains ';', '!' or another character unsafe "
                         "for smbclient command mode; rename it on the server and retry");
        return -1;
    }

    char command[2048];
    if (strcmp(rel_path, ".") == 0) {
        snprintf(command, sizeof(command), "ls");
    } else {
        snprintf(command, sizeof(command), "cd \"%s\"; ls", rel_path);
    }

    struct smb_run_result result;
    if (smb_run(conn, command, REMOTE_LISTING_TIMEOUT, &result) != 0) {
        return -1;
    }
    if (result.spawn_failed || result.exit_status != 0 || result.read_failed ||
        result.cancelled || result.timed_out) {
        smb_set_error_from_output(&result);
        smb_result_free(&result);
        return -1;
    }

    int parsed = remote_parse_smb_listing(result.output, result.length,
                                          out_entries, out_count);
    smb_result_free(&result);
    if (parsed != 0) {
        remote_set_error("Failed to parse SMB directory listing");
        return -1;
    }
    return 0;
}

/* ── 远程文件下载（SMB 分支） ─────────────────────────────────────── */

static void smb_split_share_path(const char *base_path, char *share, size_t share_size,
                                 char *file_path, size_t file_path_size)
{
    const char *p = base_path;
    while (*p == '/') p++;
    size_t share_len = strcspn(p, "/");
    if (share_len >= share_size) share_len = share_size - 1;
    memcpy(share, p, share_len);
    share[share_len] = '\0';
    const char *rest = p + share_len;
    while (*rest == '/') rest++;
    snprintf(file_path, file_path_size, "%s", rest);
}

static int smb_fetch_to_file(const RemoteConnectionConfig *conn,
                             const char *subpath, const char *dest_path)
{
    char share[256];
    if (!smb_share_of(conn, share, sizeof(share))) {
        remote_set_error("SMB URL must contain a share name");
        return -1;
    }

    /* subpath 是共享内的完整路径（可能带 share 前缀）：去掉前缀得到相对路径 */
    const char *file_path = subpath ? subpath : "";
    while (*file_path == '/') file_path++;
    size_t share_len = strlen(share);
    if (strncmp(file_path, share, share_len) == 0) {
        file_path += share_len;
        while (*file_path == '/') file_path++;
    }
    if (file_path[0] == '\0') {
        remote_set_error("SMB URL missing file path");
        return -1;
    }
    if (!smb_path_is_safe(file_path) || !smb_path_is_safe(dest_path)) {
        remote_set_error("SMB path contains ';', '!' or another character unsafe "
                         "for smbclient command mode; rename it on the server and retry");
        return -1;
    }

    char dir_part[1024] = "";
    char file_part[256] = "";
    const char *last_slash = strrchr(file_path, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - file_path);
        if (dir_len >= sizeof(dir_part)) {
            remote_set_error("SMB path is too long");
            return -1;
        }
        memcpy(dir_part, file_path, dir_len);
        dir_part[dir_len] = '\0';
        snprintf(file_part, sizeof(file_part), "%s", last_slash + 1);
    } else {
        snprintf(file_part, sizeof(file_part), "%s", file_path);
    }
    if (!file_part[0]) {
        remote_set_error("SMB URL missing file name");
        return -1;
    }

    char command[4096];
    if (dir_part[0]) {
        snprintf(command, sizeof(command),
                 "cd \"%s\"; get \"%s\" \"%s\"", dir_part, file_part, dest_path);
    } else {
        snprintf(command, sizeof(command), "get \"%s\" \"%s\"", file_part, dest_path);
    }

    struct smb_run_result result;
    if (smb_run(conn, command, REMOTE_FETCH_TIMEOUT, &result) != 0) {
        return -1;
    }
    int failed = result.spawn_failed || result.exit_status != 0 || result.read_failed ||
                 result.cancelled || result.timed_out;
    if (failed) {
        smb_set_error_from_output(&result);
    }
    smb_result_free(&result);
    if (failed) {
        unlink(dest_path);
        return -1;
    }

    struct stat st;
    if (stat(dest_path, &st) != 0 || st.st_size <= 0) {
        unlink(dest_path);
        remote_set_error("smbclient download failed (empty or missing file)");
        return -1;
    }
    /* 远端列表里的 size 可以被恶意服务器谎报；下载结束再做一次硬上限，
     * 超限立即删除，避免 smbclient 直接写满磁盘（R-03/F-10）。 */
    if (st.st_size > REMOTE_FETCH_MAX_BYTES) {
        unlink(dest_path);
        remote_set_error("SMB download exceeds the 1 GiB limit");
        return -1;
    }
    return 0;
}

/* ── 远程文件下载（libcurl 分支） ─────────────────────────────────── */

struct write_file_ctx {
    FILE *fp;
    long long written;
    int overflow;
};

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct write_file_ctx *ctx = (struct write_file_ctx *)userdata;
    size_t total = size * nmemb;
    if (abort_requested()) {
        return 0;
    }
    if (ctx->written > REMOTE_FETCH_MAX_BYTES - (long long)total) {
        ctx->overflow = 1;
        return 0;
    }
    size_t wrote = fwrite(ptr, size, nmemb, ctx->fp);
    if (wrote != nmemb) {
        return 0;
    }
    ctx->written += (long long)total;
    return total;
}

int remote_fetch_file(const RemoteConnectionConfig *conn, const char *subpath,
                      const char *dest_path)
{
    if (!conn || !dest_path || dest_path[0] == '\0') {
        remote_set_error("Invalid remote fetch request");
        return -1;
    }
    if (conn->protocol == REMOTE_PROTOCOL_SMB) {
        return smb_fetch_to_file(conn, subpath, dest_path);
    }

    char url[REMOTE_URL_MAX];
    if (remote_build_url(conn, subpath, url, sizeof(url)) != 0) {
        remote_set_error("Remote URL is too long");
        return -1;
    }

    log_info("remote", "Fetching remote file (protocol=%s host=%s)",
             remote_protocol_name(conn->protocol), conn->host);

    CURL *curl = create_curl_handle(conn, url, 0);
    if (!curl) {
        return -1;
    }

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        remote_set_error("Cannot open cache file for writing");
        return -1;
    }

    struct write_file_ctx ctx = { .fp = fp };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, REMOTE_LOW_SPEED_TIME);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)REMOTE_FETCH_MAX_BYTES);
    enable_abort_callback(curl);

    transfer_enter();
    CURLcode res = curl_easy_perform(curl);
    transfer_leave();

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK || ctx.overflow || http_code >= 400) {
        if (ctx.overflow) {
            remote_set_error("Remote file is too large");
        } else if (res != CURLE_OK) {
            if (abort_requested()) {
                remote_set_error("Transfer cancelled");
            } else {
                set_curl_error(res);
            }
        } else {
            set_http_error(http_code);
        }
        unlink(dest_path);
        log_warn("remote", "Fetch failed: protocol=%s curl=%d http=%ld: %s",
                 remote_protocol_name(conn->protocol), (int)res, http_code, remote_strerror());
        return -1;
    }
    if (ctx.written <= 0) {
        unlink(dest_path);
        remote_set_error("Remote file is empty");
        return -1;
    }

    log_info("remote", "Fetch complete (protocol=%s, %lld bytes)",
             remote_protocol_name(conn->protocol), ctx.written);
    return 0;
}

/* ── 目录列举 ─────────────────────────────────────────────────────── */

static int finish_listing(const RemoteConnectionConfig *conn,
                          struct write_buf *buf, int parse_result,
                          RemoteDirEntry **out_entries, int *out_count)
{
    write_buf_free(buf);
    if (parse_result != 0) {
        *out_entries = NULL;
        *out_count = 0;
        remote_set_error("Failed to parse the directory listing");
        return -1;
    }
    log_info("remote", "Listed %d entries (protocol=%s)",
             *out_count, remote_protocol_name(conn->protocol));
    return 0;
}

static int list_with_curl(const RemoteConnectionConfig *conn, const char *url,
                          const char *listing_path,
                          RemoteDirEntry **out_entries, int *out_count)
{
    if (conn->protocol == REMOTE_PROTOCOL_WEBDAV) {
        struct write_buf buf = { .limit = REMOTE_LISTING_MAX_BYTES };
        CURL *curl = create_curl_handle(conn, url, 1);
        if (!curl) return -1;
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Depth: 1");
        headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
        static const char propfind_body[] =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<propfind xmlns=\"DAV:\"><prop>"
            "<resourcetype/><getcontentlength/><getlastmodified/>"
            "</prop></propfind>";
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, propfind_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(sizeof(propfind_body) - 1));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        enable_abort_callback(curl);

        transfer_enter();
        CURLcode res = curl_easy_perform(curl);
        transfer_leave();

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (buf.overflow) {
            write_buf_free(&buf);
            remote_set_error("Directory listing is too large");
            return -1;
        }
        if (res != CURLE_OK) {
            write_buf_free(&buf);
            if (abort_requested()) remote_set_error("Listing cancelled");
            else set_curl_error(res);
            return -1;
        }
        if (http_code >= 400) {
            write_buf_free(&buf);
            set_http_error(http_code);
            return -1;
        }
        int parsed = remote_parse_webdav_listing(buf.data, buf.len, listing_path,
                                                 out_entries, out_count);
        return finish_listing(conn, &buf, parsed, out_entries, out_count);
    }

    /* FTP：先 MLSD，失败后再用默认 LIST，两次都用全新句柄/缓冲 */
    if (conn->protocol == REMOTE_PROTOCOL_FTP) {
        struct write_buf buf = { .limit = REMOTE_LISTING_MAX_BYTES };
        CURL *curl = create_curl_handle(conn, url, 1);
        if (!curl) return -1;
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MLSD");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        enable_abort_callback(curl);

        transfer_enter();
        CURLcode res = curl_easy_perform(curl);
        transfer_leave();
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || buf.overflow) {
            write_buf_free(&buf);
            if (abort_requested()) {
                remote_set_error("Listing cancelled");
                return -1;
            }
            buf.limit = REMOTE_LISTING_MAX_BYTES;
            buf.data = NULL;
            buf.len = buf.cap = buf.overflow = 0;
            curl = create_curl_handle(conn, url, 1);
            if (!curl) return -1;
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
            enable_abort_callback(curl);
            transfer_enter();
            res = curl_easy_perform(curl);
            transfer_leave();
            curl_easy_cleanup(curl);
            if (buf.overflow) {
                write_buf_free(&buf);
                remote_set_error("Directory listing is too large");
                return -1;
            }
            if (res != CURLE_OK) {
                write_buf_free(&buf);
                set_curl_error(res);
                return -1;
            }
        }
        int parsed = remote_parse_ftp_listing(buf.data, buf.len, out_entries, out_count);
        return finish_listing(conn, &buf, parsed, out_entries, out_count);
    }

    /* HTTP autoindex / SFTP */
    struct write_buf buf = { .limit = REMOTE_LISTING_MAX_BYTES };
    CURL *curl = create_curl_handle(conn, url, 1);
    if (!curl) return -1;
    if (conn->protocol == REMOTE_PROTOCOL_HTTP) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    enable_abort_callback(curl);

    transfer_enter();
    CURLcode res = curl_easy_perform(curl);
    transfer_leave();

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (buf.overflow) {
        write_buf_free(&buf);
        remote_set_error("Directory listing is too large");
        return -1;
    }
    if (res != CURLE_OK) {
        write_buf_free(&buf);
        if (abort_requested()) remote_set_error("Listing cancelled");
        else set_curl_error(res);
        return -1;
    }
    if (http_code >= 400) {
        write_buf_free(&buf);
        set_http_error(http_code);
        return -1;
    }

    int parsed;
    if (conn->protocol == REMOTE_PROTOCOL_HTTP) {
        parsed = remote_parse_html_listing(buf.data, buf.len,
                                           listing_path && listing_path[0]
                                               ? listing_path : "/",
                                           out_entries, out_count);
    } else {
        parsed = remote_parse_sftp_listing(buf.data, buf.len, out_entries, out_count);
    }
    return finish_listing(conn, &buf, parsed, out_entries, out_count);
}

int remote_list_directory(const RemoteConnectionConfig *conn,
                          const char *subpath,
                          RemoteDirEntry **out_entries,
                          int *out_count)
{
    if (!conn || !out_entries || !out_count) {
        return -1;
    }
    *out_entries = NULL;
    *out_count = 0;
    remote_set_error("");

    if (conn->protocol == REMOTE_PROTOCOL_SMB) {
        return smb_list_via_client(conn, subpath, out_entries, out_count);
    }

    char url[REMOTE_URL_MAX];
    if (remote_build_dir_url(conn, subpath, url, sizeof(url)) != 0) {
        remote_set_error("Remote URL is too long");
        return -1;
    }
    /* URL 不含凭据：可以安全写日志（F-05）；列目录统一补尾斜杠（F-04）。 */
    log_info("remote", "Listing directory: protocol=%s url='%s'",
             remote_protocol_name(conn->protocol), url);

    char listing_path[sizeof(conn->base_path) + 1024];
    const char *path = (subpath && subpath[0]) ? subpath : conn->base_path;
    if (!path || path[0] == '\0') {
        path = "/";
    }
    snprintf(listing_path, sizeof(listing_path), "%s", path);

    return list_with_curl(conn, url, listing_path, out_entries, out_count);
}

