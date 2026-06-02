#include "remote/remote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

static void (*g_remote_progress_hook)(void) = NULL;

void remote_set_progress_hook(void (*hook)(void)) {
    g_remote_progress_hook = hook;
}

static int progress_cb(void *clientp,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (!g_remote_progress_hook) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    uint64_t *last_refresh = (uint64_t *)clientp;
    if (now - *last_refresh >= 500) {
        g_remote_progress_hook();
        *last_refresh = now;
    }
    return 0;
}

/* ---------- error reporting ---------- */

static char g_remote_errbuf[256];

void remote_set_error(const char *msg) {
    log_debug("remote", "Error set: %s", msg ? msg : "(null)");
    strncpy(g_remote_errbuf, msg ? msg : "", sizeof(g_remote_errbuf) - 1);
    g_remote_errbuf[sizeof(g_remote_errbuf) - 1] = '\0';
}

const char *remote_strerror(void) {
    return g_remote_errbuf;
}

/* ---------- lifecycle ---------- */

void remote_init(void) {
    log_info("remote", "Initializing remote subsystem (libcurl)");
    curl_global_init(CURL_GLOBAL_ALL);
}

void remote_cleanup(void) {
    log_info("remote", "Remote subsystem cleaned up");
    curl_global_cleanup();
}

/* ---------- path detection ---------- */

int remote_is_remote_path(const char *path) {
    return path && strstr(path, "://") != NULL;
}

/* ---------- URL construction ---------- */

static const char *protocol_scheme(int protocol) {
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return "smb";
        case REMOTE_PROTOCOL_SFTP:   return "sftp";
        case REMOTE_PROTOCOL_FTP:    return "ftp";
        case REMOTE_PROTOCOL_WEBDAV:
        case REMOTE_PROTOCOL_HTTP:   return "http";
        default:                     return "ftp";
    }
}

static int default_port(int protocol) {
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return 445;
        case REMOTE_PROTOCOL_SFTP:   return 22;
        case REMOTE_PROTOCOL_FTP:    return 21;
        case REMOTE_PROTOCOL_WEBDAV:
        case REMOTE_PROTOCOL_HTTP:   return 80;
        default:                     return 21;
    }
}

/* ---------- URL parsing ---------- */

int remote_parse_url(const char *url, RemoteConnectionConfig *conn) {
    if (!url || !conn) return -1;
    memset(conn, 0, sizeof(*conn));

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        log_debug("remote", "URL parse failed (no scheme): %s", url);
        return -1;
    }

    size_t scheme_len = scheme_end - url;
    if (scheme_len == 3 && strncmp(url, "ftp", 3) == 0)
        conn->protocol = REMOTE_PROTOCOL_FTP;
    else if (scheme_len == 4 && strncmp(url, "sftp", 4) == 0)
        conn->protocol = REMOTE_PROTOCOL_SFTP;
    else if (scheme_len == 3 && strncmp(url, "smb", 3) == 0)
        conn->protocol = REMOTE_PROTOCOL_SMB;
    else if (scheme_len == 4 && strncmp(url, "http", 4) == 0)
        conn->protocol = REMOTE_PROTOCOL_HTTP;
    else if (scheme_len == 5 && strncmp(url, "https", 5) == 0) {
        conn->protocol = REMOTE_PROTOCOL_HTTP;
        conn->port = 443;
    }
    else
        return -1;

    const char *p = scheme_end + 3;
    if (!*p) return -1;

    // Extract user:pass@ if present
    const char *at = strrchr(p, '@');
    char host_part[512] = "";
    if (at) {
        size_t auth_len = at - p;
        char auth[256];
        size_t cp = auth_len < sizeof(auth) - 1 ? auth_len : sizeof(auth) - 1;
        strncpy(auth, p, cp);
        auth[cp] = '\0';

        const char *colon = strchr(auth, ':');
        if (colon) {
            size_t ulen = colon - auth;
            strncpy(conn->username, auth, ulen < sizeof(conn->username) - 1 ? ulen : sizeof(conn->username) - 1);
            strncpy(conn->password, colon + 1, sizeof(conn->password) - 1);
        } else {
            strncpy(conn->username, auth, sizeof(conn->username) - 1);
        }

        strncpy(host_part, at + 1, sizeof(host_part) - 1);
    } else {
        strncpy(host_part, p, sizeof(host_part) - 1);
    }

    // Extract host:port/path from host_part
    char *path_start = strchr(host_part, '/');
    char path_buf[512] = "";
    if (path_start) {
        strncpy(path_buf, path_start, sizeof(path_buf) - 1);
        *path_start = '\0';
    }

    // Check for IPv6: [addr]:port
    char *port_str = NULL;
    if (host_part[0] == '[') {
        char *close_bracket = strchr(host_part, ']');
        if (!close_bracket) return -1;
        size_t addr_len = close_bracket - host_part - 1;
        strncpy(conn->host, host_part + 1,
                addr_len < sizeof(conn->host) - 1 ? addr_len : sizeof(conn->host) - 1);
        if (close_bracket[1] == ':') port_str = close_bracket + 2;
    } else {
        char *colon = strrchr(host_part, ':');
        if (colon) {
            *colon = '\0';
            port_str = colon + 1;
            strncpy(conn->host, host_part, sizeof(conn->host) - 1);
        } else {
            strncpy(conn->host, host_part, sizeof(conn->host) - 1);
        }
    }

    if (port_str && *port_str) conn->port = atoi(port_str);
    if (conn->port <= 0) conn->port = default_port(conn->protocol);

    // Store path as base_path (URL-decoded, since consumers like
    // smbclient expect raw characters rather than percent-encoding)
    if (path_buf[0]) {
        char decoded_path[sizeof(conn->base_path)];
        remote_url_decode(path_buf, decoded_path, sizeof(decoded_path));
        strncpy(conn->base_path, decoded_path, sizeof(conn->base_path) - 1);
    } else {
        conn->base_path[0] = '/';
        conn->base_path[1] = '\0';
    }

    snprintf(conn->name, sizeof(conn->name), "%.64s", host_part);
    log_debug("remote", "URL parsed: protocol=%d host=%s port=%d base='%s'",
              conn->protocol, conn->host, conn->port, conn->base_path);
    return 0;
}

void remote_encode_url_path(const char *in, char *out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    if (!in) { out[0] = '\0'; return; }
    for (const char *p = in; *p && j < out_size - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '-' || c == '_' || c == '.' || c == '~' ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (j + 3 <= out_size - 1) {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0xf];
        } else {
            break;
        }
    }
    out[j] = '\0';
}

void remote_url_decode(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    if (!in) { out[0] = '\0'; return; }
    for (const char *p = in; *p && j < out_size - 1; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            out[j++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

void remote_build_url(const RemoteConnectionConfig *conn,
                      const char *subpath,
                      char *url, size_t url_size) {
    const char *scheme = protocol_scheme(conn->protocol);
    int port = conn->port > 0 ? conn->port : default_port(conn->protocol);
    int need_port = (port != default_port(conn->protocol));

    // strip leading slash from subpath for proper concatenation
    while (*subpath == '/') subpath++;

    // Percent-encode the subpath so libcurl accepts non-ASCII chars
    char encoded[4096];
    remote_encode_url_path(subpath, encoded, sizeof(encoded));

    char creds[1024] = "";
    if (conn->username[0]) {
        char encoded_user[192];
        char encoded_pass[768];
        remote_encode_url_path(conn->username, encoded_user, sizeof(encoded_user));
        remote_encode_url_path(conn->password, encoded_pass, sizeof(encoded_pass));
        snprintf(creds, sizeof(creds), "%s:%s@", encoded_user, encoded_pass);
    }

    if (conn->protocol == REMOTE_PROTOCOL_WEBDAV || conn->protocol == REMOTE_PROTOCOL_HTTP) {
        int use_https = (port == 443);
        const char *http_scheme = use_https ? "https" : "http";

        if (need_port) {
            snprintf(url, url_size, "%s://%s%s:%d/%s",
                     http_scheme, creds, conn->host, port, encoded);
        } else {
            snprintf(url, url_size, "%s://%s%s/%s",
                     http_scheme, creds, conn->host, encoded);
        }
    } else {
        if (need_port) {
            snprintf(url, url_size, "%s://%s%s:%d/%s",
                     scheme, creds, conn->host, port, encoded);
        } else {
            snprintf(url, url_size, "%s://%s%s/%s",
                     scheme, creds, conn->host, encoded);
        }
    }
}

/* ---------- directory listing ---------- */

struct write_buf {
    char *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct write_buf *buf = (struct write_buf *)userdata;

    if (buf->len + total + 1 > buf->cap) {
        buf->cap = buf->len + total + 8192;
        char *tmp = realloc(buf->data, buf->cap);
        if (!tmp) return 0;
        buf->data = tmp;
    }

    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static CURL *create_curl_handle(const RemoteConnectionConfig *conn,
                                const char *full_url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

    if (conn->username[0]) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, conn->username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, conn->password);
    }

    if (conn->protocol == REMOTE_PROTOCOL_SFTP && conn->private_key_path[0]) {
        curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, conn->private_key_path);
        curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES, CURLSSH_AUTH_PUBLICKEY);
    }

    return curl;
}

/* Parse FTP MLSD / NLST response */
static int parse_ftp_listing(const char *data, size_t len,
                             RemoteDirEntry **out, int *out_count) {
    int cap = 64;
    int count = 0;
    *out = malloc(sizeof(RemoteDirEntry) * cap);
    if (!*out) return -1;

    const char *p = data;
    while (p && *p && (size_t)(p - data) < len) {
        // skip empty lines
        while (*p == '\r' || *p == '\n') p++;
        if (!*p) break;

        const char *end = strchr(p, '\n');
        if (!end) end = strchr(p, '\r');
        if (!end) end = p + strlen(p);

        size_t line_len = end - p;
        if (line_len == 0) { p = end; continue; }

        char line[1024];
        size_t copy = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
        strncpy(line, p, copy);
        line[copy] = '\0';

        int is_dir = 0;
        char fname[256] = "";

        // MLSD format: "type=dir;size=123;modify=...; /path/name"
        if (strstr(line, "type=") != NULL) {
            if (strstr(line, "type=dir")) is_dir = 1;
            if (strstr(line, "type=OS.unix=symlink") || strstr(line, "type=OS.unix=slink")) {
                p = end + 1;
                continue;
            }
            const char *semi = strrchr(line, ';');
            const char *name_start = semi ? semi + 1 : line;
            while (*name_start == ' ') name_start++;
            strncpy(fname, name_start, sizeof(fname) - 1);
        } else if (line[0] == '-' || line[0] == 'd' || line[0] == 'l') {
            // UNIX ls -l format: "permissions links owner group size month day HH:MM name"
            if (line[0] == 'l') {
                // Symlink — skip this entry
                p = end + 1;
                continue;
            }
            is_dir = (line[0] == 'd');
            // Skip the first 8 space-delimited fields (perms, links, owner, group,
            // size, month, day, time/year) — then everything remaining is the filename.
            int fields = 0;
            const char *fp = line;
            while (*fp && fields < 8) {
                while (*fp == ' ') fp++;
                if (!*fp) break;
                fields++;
                while (*fp && *fp != ' ') fp++;
            }
            if (fields >= 8) {
                while (*fp == ' ') fp++;
                // Handle symlinks: strip " -> target"
                const char *arrow = strstr(fp, " -> ");
                if (arrow) {
                    size_t nlen = arrow - fp;
                    if (nlen < sizeof(fname))
                        strncpy(fname, fp, nlen);
                    fname[nlen] = '\0';
                } else {
                    strncpy(fname, fp, sizeof(fname) - 1);
                }
            } else {
                strncpy(fname, line, sizeof(fname) - 1);
            }
        } else {
            // NLST: just a filename per line
            strncpy(fname, line, sizeof(fname) - 1);
        }

        // trim trailing whitespace
        size_t flen = strlen(fname);
        while (flen > 0 && (fname[flen-1] == ' ' || fname[flen-1] == '\r')) {
            fname[--flen] = '\0';
        }

        if (fname[0] && strcmp(fname, ".") != 0 && strcmp(fname, "..") != 0) {
            if (count >= cap) {
                cap *= 2;
                RemoteDirEntry *tmp = realloc(*out, sizeof(RemoteDirEntry) * cap);
                if (!tmp) break;
                *out = tmp;
            }
            strncpy((*out)[count].name, fname, sizeof((*out)[count].name) - 1);
            (*out)[count].name[sizeof((*out)[count].name) - 1] = '\0';
            (*out)[count].is_dir = is_dir;
            count++;
        }

        p = end + 1;
    }

    *out_count = count;
    return 0;
}

/* Parse WebDAV PROPFIND XML response (minimal parser) */
static int parse_webdav_listing(const char *data, size_t len,
                                RemoteDirEntry **out, int *out_count) {
    int cap = 64;
    int count = 0;
    *out = malloc(sizeof(RemoteDirEntry) * cap);
    if (!*out) return -1;

    // Simple state machine over the XML
    const char *p = data;
    while (p && *p && (size_t)(p - data) < len) {
        // Look for <response> or <d:response> or <D:response>
        const char *resp_start = NULL;
        if ((resp_start = strstr(p, "<response>")) == NULL &&
            (resp_start = strstr(p, "<d:response>")) == NULL &&
            (resp_start = strstr(p, "<D:response>")) == NULL) {
            break;
        }
        p = resp_start + 1;

        const char *resp_end = strstr(p, "</response>");
        if (!resp_end &&
            (resp_end = strstr(p, "</d:response>")) == NULL &&
            (resp_end = strstr(p, "</D:response>")) == NULL) {
            break;
        }

        // Extract href (between <href> and </href>)
        const char *href_start = strstr(p, "<href>");
        if (!href_start || href_start > resp_end) { p = resp_end + 1; continue; }
        href_start += 6;
        const char *href_end = strstr(href_start, "</href>");
        if (!href_end) { p = resp_end + 1; continue; }

        size_t href_len = href_end - href_start;
        char href[1024];
        size_t copy = href_len < sizeof(href) - 1 ? href_len : sizeof(href) - 1;
        strncpy(href, href_start, copy);
        href[copy] = '\0';

        // Extract collection flag from <collection/> or <d:collection/>
        int is_dir = 0;
        if (strstr(p, "<collection/>") || strstr(p, "<collection />") ||
            strstr(p, "<d:collection/>") || strstr(p, "<d:collection />") ||
            strstr(p, "<D:collection/>")) {
            is_dir = 1;
        }

        // Extract just the filename from href
        const char *name = href;
        const char *last_slash = strrchr(href, '/');
        if (last_slash) name = last_slash + 1;

        // URL-decode (simple version)
        char fname[256];
        int fi = 0, si = 0;
        while (name[si] && fi < (int)sizeof(fname) - 1) {
            if (name[si] == '%' && name[si+1] && name[si+2]) {
                char hex[3] = {name[si+1], name[si+2], 0};
                fname[fi++] = (char)strtol(hex, NULL, 16);
                si += 3;
            } else if (name[si] == '+') {
                fname[fi++] = ' ';
                si++;
            } else {
                fname[fi++] = name[si++];
            }
        }
        fname[fi] = '\0';

        if (fname[0] && strcmp(fname, ".") != 0 && strcmp(fname, "..") != 0) {
            if (count >= cap) {
                cap *= 2;
                RemoteDirEntry *tmp = realloc(*out, sizeof(RemoteDirEntry) * cap);
                if (!tmp) break;
                *out = tmp;
            }
            strncpy((*out)[count].name, fname, sizeof((*out)[count].name) - 1);
            (*out)[count].name[sizeof((*out)[count].name) - 1] = '\0';
            (*out)[count].is_dir = is_dir;
            count++;
        }

        p = resp_end + 1;
    }

    *out_count = count;
    return 0;
}

/* ---------- HTML autoindex parser (nginx, Apache, python http.server) ---------- */

static int parse_html_listing(const char *data, size_t len,
                               RemoteDirEntry **out, int *out_count) {
    int cap = 64;
    int count = 0;
    *out = malloc(sizeof(RemoteDirEntry) * cap);
    if (!*out) return -1;

    const char *p = data;
    while (p && *p && (size_t)(p - data) < len) {
        // Find next <a href="
        const char *href_start = strstr(p, "<a href=\"");
        if (!href_start) break;

        href_start += 9; // skip past '<a href="'
        const char *href_end = strchr(href_start, '"');
        if (!href_end) break;

        size_t href_len = href_end - href_start;
        if (href_len == 0 || href_len >= 1024) { p = href_end + 1; continue; }

        char href[1024];
        strncpy(href, href_start, href_len);
        href[href_len] = '\0';

        // Skip parent directory and self links
        if (strcmp(href, "../") == 0 || strcmp(href, "./") == 0 ||
            strcmp(href, "..") == 0) {
            p = href_end + 1;
            continue;
        }
        // Skip query strings and anchor links
        if (strchr(href, '?') || strchr(href, '#')) {
            p = href_end + 1;
            continue;
        }

        // Find the display name: >text</a>
        const char *close_tag = strstr(href_end, "</a>");
        if (!close_tag) break;

        // Find the '>' that precedes the text
        const char *gt = href_end + 1;
        while (gt < close_tag && *gt != '>') gt++;
        if (gt >= close_tag) { p = close_tag + 4; continue; }

        const char *text_start = gt + 1;
        const char *text_end = close_tag;
        size_t text_len = text_end - text_start;

        // Skip if no visible text
        if (text_len == 0 || text_len >= 256) { p = close_tag + 4; continue; }

        char fname[256];
        strncpy(fname, text_start, text_len);
        fname[text_len] = '\0';

        // trim trailing whitespace
        size_t flen = strlen(fname);
        while (flen > 0 && (fname[flen-1] == ' ' || fname[flen-1] == '\r' || fname[flen-1] == '\n'))
            fname[--flen] = '\0';

        // Determine if directory: href ends with '/'
        int is_dir = (href_len > 0 && href[href_len - 1] == '/');

        if (fname[0]) {
            if (count >= cap) {
                cap *= 2;
                RemoteDirEntry *tmp = realloc(*out, sizeof(RemoteDirEntry) * cap);
                if (!tmp) break;
                *out = tmp;
            }
            strncpy((*out)[count].name, fname, sizeof((*out)[count].name) - 1);
            (*out)[count].name[sizeof((*out)[count].name) - 1] = '\0';
            (*out)[count].is_dir = is_dir;
            count++;
        }

        p = close_tag + 4;
    }

    *out_count = count;
    return (count > 0 || *out) ? 0 : -1;
}

/* ---------- smbclient-based SMB support ----------
 *
 * libcurl's SMB protocol implementation is incompatible with many Samba
 * servers (SMB protocol negotiation fails after TCP connect succeeds).
 * We use smbclient via subprocess for all SMB operations instead.
 */

static int smb_list_via_client(const RemoteConnectionConfig *conn,
                                const char *subpath,
                                RemoteDirEntry **out_entries,
                                int *out_count) {
    char cmd[4096];

    // conn->base_path is the share name (e.g. "音乐"), possibly with a
    // leading '/' when parsed from a URL. Strip for smbclient use.
    const char *share = conn->base_path;
    while (*share == '/') share++;

    // subpath includes the share name as a prefix (e.g. "音乐" or "音乐/subdir").
    // Strip the prefix to get the relative path within the share.
    const char *rel_path = ".";
    if (subpath && subpath[0]) {
        size_t base_len = strlen(conn->base_path);
        if (strncmp(subpath, conn->base_path, base_len) == 0) {
            rel_path = subpath + base_len;
            while (*rel_path == '/') rel_path++;
        }
        if (!rel_path[0]) rel_path = ".";
    }

    if (strcmp(rel_path, ".") != 0) {
        snprintf(cmd, sizeof(cmd),
                 "smbclient '//%s/%s' -U '%s%%%s' -c 'cd \"%s\"; ls' </dev/null 2>/dev/null",
                 conn->host, share, conn->username,
                 conn->password, rel_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "smbclient '//%s/%s' -U '%s%%%s' -c ls </dev/null 2>/dev/null",
                 conn->host, share, conn->username,
                 conn->password);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        remote_set_error("smbclient not found or not executable");
        *out_entries = NULL;
        *out_count = 0;
        return -1;
    }

    char line[2048];
    int cap = 64;
    int count = 0;
    *out_entries = malloc(sizeof(RemoteDirEntry) * cap);
    if (!*out_entries) { pclose(fp); return -1; }

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'
               || line[len-1] == ' '))
            line[--len] = '\0';

        if (len < 10) continue;

        // Skip summary line (contains "blocks")
        if (strstr(line, "blocks")) continue;

        // Parse: filename<spaces>D/N<spaces>size<spaces>date
        // Find the type field: a single letter (D/N/A/H/S/R) surrounded
        // by spaces, followed by %8d (right-padded size field).
        int is_dir = 0;
        char fname[256] = "";
        int found = 0;

        for (size_t i = 3; i + 8 < len; i++) {
            if (line[i-1] != ' ' ||
                !(line[i] == 'D' || line[i] == 'N' || line[i] == 'A' ||
                  line[i] == 'H' || line[i] == 'S' || line[i] == 'R') ||
                line[i+1] != ' ')
                continue;

            // After type char comes %8d (right-padded size field),
            // e.g. " 3917305" or "    1195" or "       0".
            // Verify at least one digit exists in the next 8 chars.
            int has_size = 0;
            for (size_t j = i + 1; j <= i + 8 && j < len && !has_size; j++) {
                if (line[j] >= '0' && line[j] <= '9') has_size = 1;
            }
            if (!has_size) continue;

            // Extract filename (trim leading and trailing spaces)
            const char *fn_start = line;
            while (*fn_start == ' ') fn_start++;

            size_t fn_end = i - 1;
            while (fn_end > 0 && (fn_start < line + fn_end)
                   && line[fn_end-1] == ' ')
                fn_end--;

            size_t fn_len = (line + fn_end) - fn_start;
            if (fn_len == 0) continue;
            if (fn_len >= sizeof(fname)) fn_len = sizeof(fname) - 1;
            memcpy(fname, fn_start, fn_len);
            fname[fn_len] = '\0';

            if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0)
                break;

            is_dir = (line[i] == 'D');
            found = 1;
            break;
        }

    if (!found || !fname[0]) continue;

        if (count >= cap) {
            cap *= 2;
            RemoteDirEntry *tmp = realloc(*out_entries,
                                          sizeof(RemoteDirEntry) * cap);
            if (!tmp) break;
            *out_entries = tmp;
        }
        strncpy((*out_entries)[count].name, fname,
                sizeof((*out_entries)[count].name) - 1);
        (*out_entries)[count].name[sizeof((*out_entries)[count].name) - 1] = '\0';
        (*out_entries)[count].is_dir = is_dir;
        count++;
    }

    int status = pclose(fp);

    if (count == 0 && status != 0) {
        free(*out_entries);
        *out_entries = NULL;
        *out_count = 0;
        remote_set_error("smbclient failed - check credentials and server");
        return -1;
    }

    *out_count = count;
    return 0;
}

int remote_list_directory(const RemoteConnectionConfig *conn,
                          const char *subpath,
                          RemoteDirEntry **out_entries,
                          int *out_count) {
    g_remote_errbuf[0] = '\0';  // clear previous error
    char url[4096];
    remote_build_url(conn, subpath, url, sizeof(url));

    log_info("remote", "Listing directory: protocol=%d url='%s'", conn->protocol, url);

    CURL *curl = create_curl_handle(conn, url);
    if (!curl) {
        log_error("remote", "Failed to create curl handle for listing url='%s'", url);
        return -1;
    }

    struct write_buf buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res;

    if (conn->protocol == REMOTE_PROTOCOL_WEBDAV) {
        // PROPFIND for WebDAV
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Depth: 1");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");

        res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            remote_set_error(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            free(buf.data);
            return -1;
        }
        // Check HTTP response code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            curl_easy_cleanup(curl);
            free(buf.data);
            remote_set_error(http_code == 401 ? "Authentication failed" :
                             http_code == 404 ? "Not found" :
                             http_code == 403 ? "Forbidden" :
                             http_code == 500 ? "Server error" : "HTTP error");
            return -1;
        }
        curl_easy_cleanup(curl);

        int ret = parse_webdav_listing(buf.data, buf.len, out_entries, out_count);
        free(buf.data);
        return ret;

    } else if (conn->protocol == REMOTE_PROTOCOL_FTP) {
        // Try MLSD first, fall back to NLST
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MLSD");
        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            // fall back to NLST
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
            curl_easy_setopt(curl, CURLOPT_URL, url); // re-set URL
            res = curl_easy_perform(curl);
        }

        if (res != CURLE_OK) {
            remote_set_error(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            free(buf.data);
            return -1;
        }
        curl_easy_cleanup(curl);

        // Parse result into entries
        // With MLSD: lines like "type=dir;modify=...; /dirname"
        // With NLST: just filenames
        // We need to try to determine which is which - check if lines contain "type="
        int has_mlsd = (strstr(buf.data, "type=") != NULL);
        int ret = parse_ftp_listing(buf.data, buf.len, out_entries, out_count);

        // For NLST, we can't tell dirs from files without extra requests.
        // If we used NLST, try to detect dirs by appending "/" to each entry
        // and checking with CURLOPT_NOBODY. Skip this for now - treat all as files.
        if (!has_mlsd && *out_entries && *out_count > 0) {
            // All entries from NLST are marked as files initially
            // Users will see dir entries too, which is fine for browsing
        }

        free(buf.data);
        return ret;

    } else if (conn->protocol == REMOTE_PROTOCOL_HTTP) {
        // Plain HTTP GET for nginx/Apache autoindex
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            remote_set_error(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            free(buf.data);
            return -1;
        }

        // Check HTTP response code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            curl_easy_cleanup(curl);
            free(buf.data);
            remote_set_error(http_code == 401 ? "Authentication failed" :
                             http_code == 403 ? "Forbidden" :
                             http_code == 404 ? "Not found" : "HTTP error");
            return -1;
        }
        curl_easy_cleanup(curl);

        int ret = parse_html_listing(buf.data, buf.len, out_entries, out_count);
        free(buf.data);
        return ret;

    } else if (conn->protocol == REMOTE_PROTOCOL_SMB) {
        // SMB – libcurl's SMB implementation is incompatible with many
        // Samba servers, so we use smbclient via subprocess instead.
        curl_easy_cleanup(curl);
        free(buf.data);
        return smb_list_via_client(conn, subpath, out_entries, out_count);
    } else {
        // SFTP – libcurl returns raw listing lines
        curl_easy_setopt(curl, CURLOPT_URL, url);
        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            remote_set_error(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            free(buf.data);
            return -1;
        }
        curl_easy_cleanup(curl);

        // Parse: for SFTP, lines look like:
        // "drwxr-xr-x 3 user group 4096 Jan 1 00:00 dirname"
        // "-rw-r--r-- 1 user group 1234 Jan 1 00:00 filename.mp3"
        int cap = 64;
        int count = 0;
        *out_entries = malloc(sizeof(RemoteDirEntry) * cap);
        if (!*out_entries) { free(buf.data); return -1; }

        const char *p = buf.data;
        while (p && *p && (size_t)(p - buf.data) < buf.len) {
            while (*p == '\r' || *p == '\n') p++;
            if (!*p) break;

            const char *end = strchr(p, '\n');
            if (!end) end = strchr(p, '\r');
            if (!end) end = p + strlen(p);

            size_t line_len = end - p;
            if (line_len == 0 || line_len >= 2048) { p = end; continue; }

            char line[2048];
            strncpy(line, p, line_len);
            line[line_len] = '\0';

            int is_dir = 0;
            char fname[256] = "";

            // SFTP ls -l format
            if (line_len > 10 && (line[0] == 'd' || line[0] == '-')) {
                is_dir = (line[0] == 'd');
                // Find the filename after the date/time fields
                // Format: "permissions links user group size month day time name"
                // or "permissions links user group size month day year name"
                // Find the 8th space-delimited field... actually just take last field
                if (line_len > 50) {
                    const char *last_space = NULL;
                    const char *scan = line + line_len - 1;
                    while (scan > line) {
                        if (*scan == ' ') { last_space = scan; break; }
                        scan--;
                    }
                    // Find the field before last_space (the time/date field)
                    if (last_space && last_space > line + 40) {
                        const char *time_field_start = last_space - 1;
                        while (time_field_start > line && *time_field_start != ' ') time_field_start--;
                        if (*time_field_start == ' ') time_field_start++;
                        // Now name is everything after last_space
                        strncpy(fname, last_space + 1, sizeof(fname) - 1);
                    } else {
                        // Fallback: whole line as name
                        strncpy(fname, line, sizeof(fname) - 1);
                    }
                } else {
                    strncpy(fname, line, sizeof(fname) - 1);
                }
            } else if (line_len > 10 && line[0] == 'l') {
                // SFTP symlink — skip this entry (don't add to listing)
                p = end + 1;
                continue;
            } else {
                // Fallback: treat whole line as filename
                strncpy(fname, line, sizeof(fname) - 1);
            }

            // trim
            size_t flen = strlen(fname);
            while (flen > 0 && (fname[flen-1] == ' ' || fname[flen-1] == '\r' || fname[flen-1] == '\n')) {
                fname[--flen] = '\0';
            }

            if (fname[0] && strcmp(fname, ".") != 0 && strcmp(fname, "..") != 0) {
                if (count >= cap) {
                    cap *= 2;
                    RemoteDirEntry *tmp = realloc(*out_entries, sizeof(RemoteDirEntry) * cap);
                    if (!tmp) break;
                    *out_entries = tmp;
                }
                strncpy((*out_entries)[count].name, fname, sizeof((*out_entries)[count].name) - 1);
                (*out_entries)[count].name[sizeof((*out_entries)[count].name) - 1] = '\0';
                (*out_entries)[count].is_dir = is_dir;
                count++;
            }

            p = end + 1;
        }

        *out_count = count;
        free(buf.data);
        log_info("remote", "Listed %d entries from directory", count);
        return 0;
    }
}

void remote_free_entries(RemoteDirEntry *entries, int count) {
    (void)count;
    free(entries);
}

/* ---------- protocol name ---------- */

const char *remote_protocol_name(int protocol) {
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:    return "SMB";
        case REMOTE_PROTOCOL_SFTP:   return "SFTP";
        case REMOTE_PROTOCOL_FTP:    return "FTP";
        case REMOTE_PROTOCOL_WEBDAV: return "WebDAV";
        case REMOTE_PROTOCOL_HTTP:   return "HTTP";
        default:                     return "?";
    }
}

/* ---------- remote file fetching ---------- */

static int smb_fetch_to_file(const char *url, const char *dest_path);

int remote_fetch_to_buffer(const char *url, unsigned char **data, size_t *size) {
    if (!url || !data || !size) return -1;

    *data = NULL;
    *size = 0;

    // SMB URLs need smbclient — download to temp file then read it.
    if (strncmp(url, "smb://", 6) == 0) {
        char tmp_path[] = "/tmp/ter-music-smb-XXXXXX";
        int fd = mkstemp(tmp_path);
        if (fd < 0) return -1;
        close(fd);

        int ret = smb_fetch_to_file(url, tmp_path);
        if (ret != 0) {
            unlink(tmp_path);
            return -1;
        }

        FILE *fp = fopen(tmp_path, "rb");
        if (!fp) { unlink(tmp_path); return -1; }
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        if (fsize < 0) { fclose(fp); unlink(tmp_path); return -1; }
        rewind(fp);

        *data = malloc((size_t)fsize + 1);
        if (!*data) { fclose(fp); unlink(tmp_path); return -1; }
        *size = fread(*data, 1, (size_t)fsize, fp);
        (*data)[*size] = '\0';
        fclose(fp);
        unlink(tmp_path);
        return 0;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct write_buf buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    uint64_t last_refresh = 0;
    if (g_remote_progress_hook) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &last_refresh);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        free(buf.data);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400) {
        free(buf.data);
        return -1;
    }

    *data = (unsigned char *)buf.data;
    *size = buf.len;
    return 0;
}

/* ---------- write-to-file callback ---------- */

struct write_file_ctx {
    FILE *fp;
};

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct write_file_ctx *ctx = (struct write_file_ctx *)userdata;
    return fwrite(ptr, size, nmemb, ctx->fp);
}

static int smb_fetch_to_file(const char *url, const char *dest_path) {
    // Parse the SMB URL to extract connection details
    RemoteConnectionConfig conn;
    if (remote_parse_url(url, &conn) != 0) {
        remote_set_error("Failed to parse SMB URL");
        return -1;
    }

    // URL-decode the path portion
    char decoded[MAX_PATH_LEN];
    remote_url_decode(conn.base_path, decoded, sizeof(decoded));

    // Split decoded path into share name and file path
    // Format: "/share_name/file_path" or "/share_name/dir/file.mp3"
    char share[256] = "";
    char file_path[1024] = "";
    const char *p = decoded;
    while (*p == '/') p++;

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t share_len = slash - p;
        if (share_len >= sizeof(share)) share_len = sizeof(share) - 1;
        memcpy(share, p, share_len);
        share[share_len] = '\0';
        strncpy(file_path, slash + 1, sizeof(file_path) - 1);
    } else {
        strncpy(share, p, sizeof(share) - 1);
        file_path[0] = '\0';
    }

    if (!share[0] || !file_path[0]) {
        remote_set_error("SMB URL missing share or file path");
        return -1;
    }

    // Split file_path into directory part and filename
    char dir_part[1024] = "";
    char file_part[256] = "";
    const char *last_slash = strrchr(file_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - file_path;
        memcpy(dir_part, file_path, dir_len);
        dir_part[dir_len] = '\0';
        strncpy(file_part, last_slash + 1, sizeof(file_part) - 1);
    } else {
        strncpy(file_part, file_path, sizeof(file_part) - 1);
    }

    if (!file_part[0]) {
        remote_set_error("SMB URL missing file name");
        return -1;
    }

    // Build smbclient command
    char cmd[4096];
    if (dir_part[0]) {
        snprintf(cmd, sizeof(cmd),
                 "smbclient '//%s/%s' -U '%s%%%s' -c 'cd \"%s\"; get \"%s\" \"%s\"' </dev/null 2>/dev/null",
                 conn.host, share, conn.username, conn.password,
                 dir_part, file_part, dest_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "smbclient '//%s/%s' -U '%s%%%s' -c 'get \"%s\" \"%s\"' </dev/null 2>/dev/null",
                 conn.host, share, conn.username, conn.password,
                 file_part, dest_path);
    }

    log_debug("remote", "smbclient fetch: '%s'", cmd);

    int ret = system(cmd);
    if (ret != 0 || access(dest_path, F_OK) != 0) {
        remote_set_error("smbclient download failed");
        return -1;
    }

    return 0;
}

int remote_fetch_to_file(const char *url, const char *dest_path) {
    if (!url || !dest_path) return -1;

    // SMB URLs need smbclient — libcurl's SMB implementation is
    // incompatible with many Samba servers.
    if (strncmp(url, "smb://", 6) == 0) {
        return smb_fetch_to_file(url, dest_path);
    }

    log_info("remote", "Fetching remote file: '%s' -> '%s'", url, dest_path);

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("remote", "Failed to create curl handle for fetch: '%s'", url);
        return -1;
    }

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        log_error("remote", "Failed to open dest file '%s' for writing", dest_path);
        curl_easy_cleanup(curl);
        return -1;
    }

    struct write_file_ctx ctx = { .fp = fp };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    uint64_t last_refresh = 0;
    if (g_remote_progress_hook) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &last_refresh);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code >= 400) {
        log_error("remote", "Fetch failed: url='%s' curl=%d http=%ld", url, res, http_code);
        unlink(dest_path);
        return -1;
    }

    log_info("remote", "Fetch complete: '%s' -> '%s'", url, dest_path);
    return 0;
}
