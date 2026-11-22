/**
 * @file remote_url.c
 * @brief 远程音乐源 URL 解析/构造 —— 不依赖 libcurl，可直接单测
 *
 * 关键规则（对应审计 F-05/F-08/F-16/F-17/F-30）：
 *   - 凭据只在内存里出现，URL 文本永不包含 userinfo；
 *   - TLS 由 scheme 决定（https/ftps/webdavs），不由端口推断；
 *   - IPv6 主机内部无方括号，输出时补上；
 *   - 编码/解码严格按 RFC3986，'+' 不转空格，非法 %xx 保留字面量；
 *   - 超长输入拒绝（返回 -1，输出置空），不产生半截 URL。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote_url.h"

#include "logger/logger.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── 小工具 ───────────────────────────────────────────────────────── */

static int url_is_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int encode_internal(const char *in, char *out, size_t out_size, int encode_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;

    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (url_is_unreserved(c) || (c == '/' && !encode_slash)) {
            if (j + 1 >= out_size) {
                out[0] = '\0';
                return -1;
            }
            out[j++] = (char)c;
        } else {
            if (j + 3 >= out_size) {
                out[0] = '\0';
                return -1;
            }
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0x0f];
        }
    }
    out[j] = '\0';
    return 0;
}

int remote_encode_url_path(const char *in, char *out, size_t out_size)
{
    return encode_internal(in, out, out_size, 0);
}

int remote_encode_url_component(const char *in, char *out, size_t out_size)
{
    return encode_internal(in, out, out_size, 1);
}

int remote_url_decode(const char *in, char *out, size_t out_size)
{
    size_t j = 0;

    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }

    for (const char *p = in; *p; p++) {
        unsigned char produced;
        if (p[0] == '%' && p[1] && p[2]) {
            int hi = hex_value((unsigned char)p[1]);
            int lo = hex_value((unsigned char)p[2]);
            if (hi >= 0 && lo >= 0) {
                produced = (unsigned char)((hi << 4) | lo);
                p += 2;
            } else {
                produced = '%';          /* 非法转义：保留字面量，不静默截断 */
            }
        } else {
            produced = (unsigned char)*p; /* '+' 就是 '+'，不是空格 */
        }
        if (produced == 0) {
            /* %00 会把 C 字符串静默截断（/a%00/b -> /a）：拒绝而不是接受。 */
            out[0] = '\0';
            return -1;
        }
        if (j + 1 >= out_size) {
            out[0] = '\0';
            return -1;
        }
        out[j++] = (char)produced;
    }
    out[j] = '\0';
    return 0;
}

/* ── scheme / 默认端口 ────────────────────────────────────────────── */

const char *remote_url_scheme(int protocol, int use_tls)
{
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return "smb";
        case REMOTE_PROTOCOL_SFTP:   return "sftp";
        case REMOTE_PROTOCOL_FTP:    return use_tls ? "ftps" : "ftp";
        case REMOTE_PROTOCOL_WEBDAV:
        case REMOTE_PROTOCOL_HTTP:   return use_tls ? "https" : "http";
        default:                     return "ftp";
    }
}

int remote_url_default_port(int protocol, int use_tls)
{
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return 445;
        case REMOTE_PROTOCOL_SFTP:   return 22;
        case REMOTE_PROTOCOL_FTP:    return use_tls ? 990 : 21;
        case REMOTE_PROTOCOL_WEBDAV:
        case REMOTE_PROTOCOL_HTTP:   return use_tls ? 443 : 80;
        default:                     return 21;
    }
}

static int scheme_lookup(const char *scheme, size_t len, int *protocol, int *use_tls)
{
    char lowered[16];
    if (len == 0 || len >= sizeof(lowered)) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        lowered[i] = (char)tolower((unsigned char)scheme[i]);
    }
    lowered[len] = '\0';

    *use_tls = 0;
    if (strcmp(lowered, "ftp") == 0) {
        *protocol = REMOTE_PROTOCOL_FTP;
    } else if (strcmp(lowered, "ftps") == 0) {
        *protocol = REMOTE_PROTOCOL_FTP;
        *use_tls = 1;
    } else if (strcmp(lowered, "sftp") == 0) {
        *protocol = REMOTE_PROTOCOL_SFTP;
    } else if (strcmp(lowered, "smb") == 0 || strcmp(lowered, "smbs") == 0) {
        *protocol = REMOTE_PROTOCOL_SMB;
    } else if (strcmp(lowered, "http") == 0) {
        *protocol = REMOTE_PROTOCOL_HTTP;
    } else if (strcmp(lowered, "https") == 0) {
        *protocol = REMOTE_PROTOCOL_HTTP;
        *use_tls = 1;
    } else if (strcmp(lowered, "webdav") == 0 || strcmp(lowered, "dav") == 0) {
        *protocol = REMOTE_PROTOCOL_WEBDAV;
    } else if (strcmp(lowered, "webdavs") == 0 || strcmp(lowered, "davs") == 0) {
        *protocol = REMOTE_PROTOCOL_WEBDAV;
        *use_tls = 1;
    } else {
        return -1;
    }
    return 0;
}

/* ── 解析 ─────────────────────────────────────────────────────────── */

int remote_parse_url(const char *url, RemoteConnectionConfig *conn)
{
    if (!url || !conn) {
        return -1;
    }
    memset(conn, 0, sizeof(*conn));

    size_t url_len = strlen(url);
    if (url_len == 0 || url_len >= REMOTE_URL_MAX) {
        log_debug("remote", "URL parse failed (length %zu not in 1..%d)", url_len, REMOTE_URL_MAX - 1);
        return -1;
    }

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        log_debug("remote", "URL parse failed (no scheme)");
        return -1;
    }
    if (scheme_lookup(url, (size_t)(scheme_end - url), &conn->protocol, &conn->use_tls) != 0) {
        log_debug("remote", "URL parse failed (unsupported scheme '%.*s')",
                  (int)(scheme_end - url), url);
        return -1;
    }

    const char *auth = scheme_end + 3;
    const char *auth_end = strpbrk(auth, "/?#");
    if (!auth_end) {
        auth_end = auth + strlen(auth);
    }
    if (auth_end == auth) {
        return -1;                       /* 无主机名 */
    }

    /* userinfo：按最后一个 '@' 切分（密码里的 '@' 必须编码，故不会歧义） */
    const char *host_start = auth;
    const char *at = NULL;
    for (const char *scan = auth; scan < auth_end; scan++) {
        if (*scan == '@') at = scan;
    }
    if (at) {
        const char *userinfo_end = at;
        const char *colon = NULL;
        for (const char *scan = auth; scan < userinfo_end; scan++) {
            if (*scan == ':') { colon = scan; break; }
        }
        char decoded[512];
        size_t raw_len = (size_t)((colon ? colon : userinfo_end) - auth);
        char raw[512];
        if (raw_len >= sizeof(raw)) {
            return -1;
        }
        memcpy(raw, auth, raw_len);
        raw[raw_len] = '\0';
        if (remote_url_decode(raw, decoded, sizeof(decoded)) != 0 ||
            strlen(decoded) >= sizeof(conn->username)) {
            return -1;
        }
        size_t decoded_len = strlen(decoded);
        memcpy(conn->username, decoded, decoded_len + 1);

        if (colon) {
            raw_len = (size_t)(userinfo_end - colon - 1);
            if (raw_len >= sizeof(raw)) {
                return -1;
            }
            memcpy(raw, colon + 1, raw_len);
            raw[raw_len] = '\0';
            if (remote_url_decode(raw, decoded, sizeof(decoded)) != 0 ||
                strlen(decoded) >= sizeof(conn->password)) {
                return -1;
            }
            size_t decoded_len = strlen(decoded);
            memcpy(conn->password, decoded, decoded_len + 1);
        }
        host_start = at + 1;
    }

    /* host[:port]；IPv6 用 [addr]:port。裸多冒号按 IPv6 主机处理（宽松）。 */
    const char *host_end = host_start;
    while (host_end < auth_end && *host_end != ':') {
        host_end++;
    }
    int bracketed = (host_start < auth_end && *host_start == '[');
    if (bracketed) {
        const char *close = memchr(host_start, ']', (size_t)(auth_end - host_start));
        if (!close) {
            return -1;
        }
        size_t host_len = (size_t)(close - host_start - 1);
        if (host_len == 0 || host_len >= sizeof(conn->host)) {
            return -1;
        }
        char raw_host[sizeof(conn->host)];
        memcpy(raw_host, host_start + 1, host_len);
        raw_host[host_len] = '\0';
        /* RFC 6874：IPv6 zone id 里的 '%' 在 URL 中写作 %25，解析后还原 */
        if (remote_url_decode(raw_host, conn->host, sizeof(conn->host)) != 0) {
            return -1;
        }
        host_end = close + 1;
    } else {
        /* 不括方括号时：最后一个 ':' 之后全是数字才当端口，否则整体是 IPv6 主机 */
        const char *last_colon = NULL;
        for (const char *scan = host_start; scan < auth_end; scan++) {
            if (*scan == ':') last_colon = scan;
        }
        const char *host_stop = auth_end;
        if (last_colon && last_colon > host_start) {
            int all_digits = 1;
            for (const char *scan = last_colon + 1; scan < auth_end; scan++) {
                if (!isdigit((unsigned char)*scan)) { all_digits = 0; break; }
            }
            if (all_digits) {
                host_stop = last_colon;
                host_end = last_colon;
            }
        }
        size_t host_len = (size_t)(host_stop - host_start);
        if (host_len == 0 || host_len >= sizeof(conn->host)) {
            return -1;
        }
        memcpy(conn->host, host_start, host_len);
        conn->host[host_len] = '\0';
        if (host_end == host_start) {
            host_end = host_stop;
        }
    }

    /* 端口 */
    if (host_end < auth_end) {
        if (*host_end != ':') {
            return -1;
        }
        const char *port_start = host_end + 1;
        if (port_start >= auth_end) {
            return -1;
        }
        long value = 0;
        for (const char *scan = port_start; scan < auth_end; scan++) {
            if (!isdigit((unsigned char)*scan)) {
                return -1;
            }
            value = value * 10 + (*scan - '0');
            if (value > 65535) {
                return -1;
            }
        }
        if (value <= 0) {
            return -1;
        }
        conn->port = (int)value;
    }
    if (conn->port <= 0) {
        conn->port = remote_url_default_port(conn->protocol, conn->use_tls);
    }

    /* 路径（去掉 query/fragment；解码一次存放） */
    const char *path = auth_end;
    size_t path_len = 0;
    while (path[path_len] && path[path_len] != '?' && path[path_len] != '#') {
        path_len++;
    }
    char decoded_path[sizeof(conn->base_path) * 3];
    char raw_path[REMOTE_URL_MAX];
    if (path_len >= sizeof(raw_path)) {
        return -1;
    }
    memcpy(raw_path, path, path_len);
    raw_path[path_len] = '\0';
    if (remote_url_decode(raw_path, decoded_path, sizeof(decoded_path)) != 0) {
        return -1;
    }
    size_t decoded_path_len = strlen(decoded_path);
    if (decoded_path_len + 2 >= sizeof(conn->base_path)) {
        return -1;                       /* 超长路径：拒绝而不是静默截断 */
    }
    if (decoded_path_len == 0) {
        conn->base_path[0] = '/';
        conn->base_path[1] = '\0';
    } else if (decoded_path[0] != '/') {
        conn->base_path[0] = '/';
        memcpy(conn->base_path + 1, decoded_path, decoded_path_len + 1);
    } else {
        memcpy(conn->base_path, decoded_path, decoded_path_len + 1);
    }

    snprintf(conn->name, sizeof(conn->name), "%.63s", conn->host);
    log_debug("remote", "URL parsed: protocol=%d tls=%d host=%s port=%d base='%s'",
              conn->protocol, conn->use_tls, conn->host, conn->port, conn->base_path);
    return 0;
}

/* ── 构造 ─────────────────────────────────────────────────────────── */

static int build_common(const RemoteConnectionConfig *conn, const char *subpath,
                        int want_dir, char *url, size_t url_size)
{
    if (!conn || !url || url_size == 0) {
        return -1;
    }
    url[0] = '\0';

    if (conn->host[0] == '\0') {
        return -1;
    }

    const char *scheme = remote_url_scheme(conn->protocol, conn->use_tls);
    int default_port = remote_url_default_port(conn->protocol, conn->use_tls);
    int port = conn->port > 0 ? conn->port : default_port;
    int need_port = (port != default_port);

    /* 路径：subpath 就是服务器上的完整路径（调用方传 base_path 或它下面
     * 的子目录）；为空时回落到 conn->base_path。与旧实现保持同一约定。 */
    char joined[REMOTE_URL_MAX];
    const char *path = (subpath && subpath[0]) ? subpath : conn->base_path;
    if (!path || path[0] == '\0') {
        path = "/";
    }
    while (*path == '/') {
        path++;
    }
    int written = snprintf(joined, sizeof(joined), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(joined)) {
        return -1;
    }

    /* 列目录时补尾斜杠（F-04）；根路径由下面的 "/%s" 骨架提供唯一斜杠。 */
    size_t joined_len = strlen(joined);
    if (want_dir && joined_len > 0 && joined[joined_len - 1] != '/') {
        if (joined_len + 1 >= sizeof(joined)) {
            return -1;
        }
        joined[joined_len++] = '/';
        joined[joined_len] = '\0';
    }

    char encoded[REMOTE_URL_MAX];
    if (remote_encode_url_path(joined, encoded, sizeof(encoded)) != 0) {
        return -1;
    }

    /* IPv6：host 含 ':' 但未带方括号时补上（F-16） */
    char host_encoded[sizeof(conn->host) * 3 + 1];
    size_t host_pos = 0;
    for (const char *scan = conn->host; *scan; scan++) {
        if (*scan == '%') {
            if (host_pos + 3 >= sizeof(host_encoded)) return -1;
            memcpy(host_encoded + host_pos, "%25", 3);
            host_pos += 3;
        } else {
            if (host_pos + 1 >= sizeof(host_encoded)) return -1;
            host_encoded[host_pos++] = *scan;
        }
    }
    host_encoded[host_pos] = '\0';

    char host_buf[sizeof(host_encoded) + 4];
    if (strchr(host_encoded, ':') && host_encoded[0] != '[') {
        written = snprintf(host_buf, sizeof(host_buf), "[%s]", host_encoded);
    } else {
        written = snprintf(host_buf, sizeof(host_buf), "%s", host_encoded);
    }
    if (written < 0 || (size_t)written >= sizeof(host_buf)) {
        return -1;
    }

    if (need_port) {
        written = snprintf(url, url_size, "%s://%s:%d/%s",
                           scheme, host_buf, port, encoded);
    } else {
        written = snprintf(url, url_size, "%s://%s/%s",
                           scheme, host_buf, encoded);
    }
    if (written < 0 || (size_t)written >= url_size) {
        url[0] = '\0';
        return -1;                       /* 超长：拒绝，不产出半截 URL */
    }
    return 0;
}

int remote_build_url(const RemoteConnectionConfig *conn, const char *subpath,
                     char *url, size_t url_size)
{
    return build_common(conn, subpath, 0, url, url_size);
}

int remote_build_dir_url(const RemoteConnectionConfig *conn, const char *subpath,
                         char *url, size_t url_size)
{
    return build_common(conn, subpath, 1, url, url_size);
}

/* ── 脱敏 ─────────────────────────────────────────────────────────── */

int remote_url_has_userinfo(const char *url)
{
    if (!url) {
        return 0;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return 0;
    }
    const char *auth = scheme_end + 3;
    const char *auth_end = strpbrk(auth, "/?#");
    if (!auth_end) {
        auth_end = auth + strlen(auth);
    }
    for (const char *scan = auth; scan < auth_end; scan++) {
        if (*scan == '@') {
            return 1;
        }
    }
    return 0;
}

void remote_redact_url(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    const char *scheme_end = strstr(in, "://");
    if (!scheme_end) {
        snprintf(out, out_size, "%s", in);
        return;
    }
    const char *auth = scheme_end + 3;
    const char *auth_end = strpbrk(auth, "/?#");
    if (!auth_end) {
        auth_end = auth + strlen(auth);
    }
    const char *at = NULL;
    for (const char *scan = auth; scan < auth_end; scan++) {
        if (*scan == '@') at = scan;
    }
    if (!at) {
        snprintf(out, out_size, "%s", in);
        return;
    }
    size_t prefix_len = (size_t)(auth - in);
    if (prefix_len >= out_size) {
        return;
    }
    memcpy(out, in, prefix_len);
    size_t rest_len = strlen(at + 1);
    if (prefix_len + rest_len >= out_size) {
        return;
    }
    memcpy(out + prefix_len, at + 1, rest_len + 1);
}
