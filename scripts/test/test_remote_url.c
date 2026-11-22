/*
 * remote_url 单测：URL 解析/构造/百分号编码/脱敏
 *
 * 覆盖审计 F-05（凭据不进 URL）、F-08（非 443 的 https）、F-16（IPv6）、
 * F-17（保留字符/非 ASCII/'+'/非法 %xx/超长拒绝）、F-30（scheme 大小写与别名）。
 *
 * 运行：scripts/test/run-unit-tests.sh test_remote_url
 * 依赖（见 test_remote_url.srcs）：remote/remote_url.c + stub_logger.c
 */

#include "remote/remote.h"
#include "remote/remote_url.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static void check_parse_build(const char *url, int want_protocol, int want_tls,
                              const char *want_host, int want_port,
                              const char *want_base, const char *what)
{
    RemoteConnectionConfig conn;
    int rc = remote_parse_url(url, &conn);
    CHECK(rc == 0, what);
    if (rc != 0) {
        return;
    }
    CHECK(conn.protocol == want_protocol, what);
    CHECK(conn.use_tls == want_tls, what);
    CHECK(strcmp(conn.host, want_host) == 0, what);
    CHECK(conn.port == want_port, what);
    CHECK(strcmp(conn.base_path, want_base) == 0, what);
}

static void test_parse_schemes(void)
{
    check_parse_build("ftp://h/p", REMOTE_PROTOCOL_FTP, 0, "h", 21, "/p", "ftp 解析");
    check_parse_build("FTP://H/p", REMOTE_PROTOCOL_FTP, 0, "H", 21, "/p", "scheme 大小写");
    check_parse_build("ftps://h/p", REMOTE_PROTOCOL_FTP, 1, "h", 990, "/p", "ftps=TLS+FTP");
    check_parse_build("sftp://h/p", REMOTE_PROTOCOL_SFTP, 0, "h", 22, "/p", "sftp 解析");
    check_parse_build("smb://h/share", REMOTE_PROTOCOL_SMB, 0, "h", 445, "/share", "smb 解析");
    check_parse_build("smbs://h/share", REMOTE_PROTOCOL_SMB, 0, "h", 445, "/share", "smbs 别名");
    check_parse_build("HTTP://h", REMOTE_PROTOCOL_HTTP, 0, "h", 80, "/", "大写 HTTP");
    check_parse_build("webdav://h/dav/", REMOTE_PROTOCOL_WEBDAV, 0, "h", 80, "/dav/", "webdav scheme");
    check_parse_build("davs://h:8443/dav/", REMOTE_PROTOCOL_WEBDAV, 1, "h", 8443, "/dav/", "davs=TLS+WebDAV");

    RemoteConnectionConfig conn;
    CHECK(remote_parse_url("gopher://h/", &conn) != 0, "不支持的 scheme 被拒绝");
    CHECK(remote_parse_url("no-scheme", &conn) != 0, "缺 scheme 被拒绝");
    CHECK(remote_parse_url("http://", &conn) != 0, "空主机被拒绝");
    CHECK(remote_parse_url("http://h:abc/", &conn) != 0, "非数字端口被拒绝");
    CHECK(remote_parse_url("http://h:70000/", &conn) != 0, "越界端口被拒绝");
    CHECK(remote_parse_url("http://h:0/", &conn) != 0, "端口 0 被拒绝");
}

static void test_tls_never_inferred_from_port(void)
{
    /* F-08：https 非 443 必须是 https；http:443 不能自动升级 */
    RemoteConnectionConfig conn;
    char url[REMOTE_URL_MAX];

    CHECK(remote_parse_url("https://nas.example:8443/dav/", &conn) == 0, "https:8443 解析");
    CHECK(conn.use_tls == 1, "https:8443 带 TLS 标志");
    CHECK(remote_build_url(&conn, conn.base_path, url, sizeof(url)) == 0, "https:8443 构造");
    CHECK(strncmp(url, "https://nas.example:8443/", 25) == 0, "https:8443 不降级为 http");

    CHECK(remote_parse_url("http://nas.example:443/dav/", &conn) == 0, "http:443 解析");
    CHECK(conn.use_tls == 0, "http:443 没有 TLS 标志");
    CHECK(remote_build_url(&conn, conn.base_path, url, sizeof(url)) == 0, "http:443 构造");
    CHECK(strncmp(url, "http://nas.example:443/", 23) == 0, "http:443 不被升级为 https");

    CHECK(remote_parse_url("https://nas.example/dav/", &conn) == 0, "https 默认端口");
    CHECK(conn.port == 443, "https 默认 443");
    CHECK(remote_build_url(&conn, conn.base_path, url, sizeof(url)) == 0, "https 默认构造");
    CHECK(strcmp(url, "https://nas.example/dav/") == 0, "https 默认端口省略");
}

static void test_credentials(void)
{
    /* 凭据含 % @ : / 空格 —— 解析解码一次、URL 不再含凭据 */
    RemoteConnectionConfig conn;
    const char *url =
        "sftp://u%40x%3Ay%2Fz%20q:p%40ss%3Aw%2Frd%20%25@host.example:2222/music/%E4%B8%AD%E6%96%87%20a.mp3";
    CHECK(remote_parse_url(url, &conn) == 0, "保留字符凭据解析");
    CHECK(strcmp(conn.username, "u@x:y/z q") == 0, "用户名解码一次（无二次编码）");
    CHECK(strcmp(conn.password, "p@ss:w/rd %") == 0, "密码解码一次且 % 保留");
    CHECK(strcmp(conn.base_path, "/music/中文 a.mp3") == 0, "非 ASCII 路径解码");

    char built[REMOTE_URL_MAX];
    CHECK(remote_build_url(&conn, conn.base_path, built, sizeof(built)) == 0, "凭据连接构造");
    CHECK(strstr(built, "u@x") == NULL && strstr(built, "p@ss") == NULL,
          "构造的 URL 不含 userinfo（F-05）");
    CHECK(strstr(built, "%E4%B8%AD%E6%96%87%20a.mp3") != NULL, "非 ASCII 路径百分号编码");

    /* 往返幂等：parse(build(parse(x))) 的 host/port/path/凭据一致 */
    RemoteConnectionConfig round;
    CHECK(remote_parse_url(built, &round) == 0, "构造结果可再解析");
    CHECK(strcmp(round.username, "") == 0, "URL 里没有用户名（凭据已移出 URL）");
    CHECK(strcmp(round.host, "host.example") == 0, "往返主机一致");
    CHECK(round.port == 2222, "往返端口一致");
    CHECK(strcmp(round.base_path, conn.base_path) == 0, "往返路径一致");

    char plain[REMOTE_URL_MAX];
    CHECK(remote_build_url(&conn, "sub dir/中文.mp3", plain, sizeof(plain)) == 0, "子路径构造");
    CHECK(strstr(plain, "/sub%20dir/%E4%B8%AD%E6%96%87.mp3") != NULL, "子路径编码");
}

static void test_plus_and_invalid_escape(void)
{
    RemoteConnectionConfig conn;
    char url[REMOTE_URL_MAX];

    /* '+' 是字面量，不当空格；构造时编码为 %2B */
    CHECK(remote_parse_url("http://h/a+b.mp3", &conn) == 0, "'+' URL 解析");
    CHECK(strcmp(conn.base_path, "/a+b.mp3") == 0, "'+' 不转空格");
    CHECK(remote_build_url(&conn, conn.base_path, url, sizeof(url)) == 0, "'+' 构造");
    CHECK(strstr(url, "/a%2Bb.mp3") != NULL, "'+' 输出为 %2B");

    /* 非法 %xx 保留字面量，不被 strtol(0) 截断 */
    CHECK(remote_parse_url("http://h/x%zzb.mp3", &conn) == 0, "非法转义 URL 解析");
    CHECK(strcmp(conn.base_path, "/x%zzb.mp3") == 0, "非法 %xx 保留字面量");

    char out[64];
    CHECK(remote_url_decode("100%25%20ok", out, sizeof(out)) == 0, "合法转义解码");
    CHECK(strcmp(out, "100% ok") == 0, "解码结果");
    CHECK(remote_url_decode("100%2", out, sizeof(out)) == 0, "截断转义解码");
    CHECK(strcmp(out, "100%2") == 0, "截断转义按字面量");
    CHECK(remote_url_decode("a+b", out, sizeof(out)) == 0, "'+' 解码");
    CHECK(strcmp(out, "a+b") == 0, "'+' 保持字面量");
    CHECK(remote_url_decode("bad%GG", out, sizeof(out)) == 0, "非法十六进制解码");
    CHECK(strcmp(out, "bad%GG") == 0, "非法十六进制按字面量");
    /* R-08：%00 会造成 C 字符串静默截断，必须拒绝 */
    CHECK(remote_url_decode("a%00b", out, sizeof(out)) == -1, "%00 解码被拒绝");
    CHECK(out[0] == '\0', "%00 失败时输出为空");
}

static void test_ipv6(void)
{
    RemoteConnectionConfig conn;
    char url[REMOTE_URL_MAX];

    CHECK(remote_parse_url("http://[::1]:8080/music/", &conn) == 0, "IPv6 解析");
    CHECK(strcmp(conn.host, "::1") == 0, "IPv6 主机去方括号存储");
    CHECK(conn.port == 8080, "IPv6 端口");
    CHECK(remote_build_url(&conn, conn.base_path, url, sizeof(url)) == 0, "IPv6 构造");
    CHECK(strcmp(url, "http://[::1]:8080/music/") == 0, "IPv6 构造补方括号");

    /* 表单方式：host 直接是 ::1 */
    memset(&conn, 0, sizeof(conn));
    conn.protocol = REMOTE_PROTOCOL_HTTP;
    snprintf(conn.host, sizeof(conn.host), "::1");
    conn.port = 9000;
    snprintf(conn.base_path, sizeof(conn.base_path), "/");
    CHECK(remote_build_url(&conn, "/", url, sizeof(url)) == 0, "裸 IPv6 主机构造");
    CHECK(strcmp(url, "http://[::1]:9000/") == 0, "裸 IPv6 主机补方括号");

    CHECK(remote_parse_url("http://[fe80::1%25eth0]/", &conn) == 0, "带 zone 的 IPv6 解析");
    CHECK(strcmp(conn.host, "fe80::1%eth0") == 0, "zone 解码");
}

static void test_long_paths(void)
{
    RemoteConnectionConfig conn;
    char url[REMOTE_URL_MAX];
    char long_path[4096];

    memset(&conn, 0, sizeof(conn));
    conn.protocol = REMOTE_PROTOCOL_HTTP;
    snprintf(conn.host, sizeof(conn.host), "host");
    conn.port = 80;
    snprintf(conn.base_path, sizeof(conn.base_path), "/");

    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    url[0] = 'x';
    CHECK(remote_build_url(&conn, long_path, url, sizeof(url)) != 0, "超长路径构造被拒绝");
    CHECK(url[0] == '\0', "拒绝时输出被清空（不产生半截 URL）");

    /* 超长输入 URL 拒绝，不静默截断 base_path */
    char big_url[8192];
    memset(big_url, 0, sizeof(big_url));
    snprintf(big_url, sizeof(big_url), "http://host/");
    size_t used = strlen(big_url);
    memset(big_url + used, 'b', sizeof(big_url) - used - 1);
    CHECK(remote_parse_url(big_url, &conn) != 0, "超长 URL 被拒绝");

    /* 目录 URL 构造补尾斜杠（F-04） */
    memset(&conn, 0, sizeof(conn));
    conn.protocol = REMOTE_PROTOCOL_FTP;
    snprintf(conn.host, sizeof(conn.host), "h");
    conn.port = 21;
    snprintf(conn.base_path, sizeof(conn.base_path), "/sub dir");
    CHECK(remote_build_dir_url(&conn, "/sub dir", url, sizeof(url)) == 0, "目录 URL 构造");
    CHECK(strcmp(url, "ftp://h/sub%20dir/") == 0, "目录 URL 以 / 结尾");
    CHECK(remote_build_dir_url(&conn, "/sub dir/", url, sizeof(url)) == 0, "已带斜杠目录");
    CHECK(strcmp(url, "ftp://h/sub%20dir/") == 0, "不会出现双斜杠");
    CHECK(remote_build_url(&conn, "/sub dir/x.mp3", url, sizeof(url)) == 0, "文件 URL 不补斜杠");
    CHECK(strcmp(url, "ftp://h/sub%20dir/x.mp3") == 0, "文件 URL 输出");
}

static void test_redact(void)
{
    char out[256];
    remote_redact_url("ftp://user:p%40ss@host:21/music/", out, sizeof(out));
    CHECK(strcmp(out, "ftp://host:21/music/") == 0, "脱敏去掉 userinfo");
    CHECK(remote_url_has_userinfo("ftp://u:p@host/") == 1, "识别 userinfo");
    CHECK(remote_url_has_userinfo("ftp://host/") == 0, "无 userinfo");
    remote_redact_url("ftp://host/music/", out, sizeof(out));
    CHECK(strcmp(out, "ftp://host/music/") == 0, "无 userinfo 原样保留");
    remote_redact_url("/local/path", out, sizeof(out));
    CHECK(strcmp(out, "/local/path") == 0, "非 URL 原样保留");
}

static void test_encode_bounds(void)
{
    char out[8];
    CHECK(remote_encode_url_path("abcdefg", out, sizeof(out)) == 0, "刚好放得下");
    CHECK(strcmp(out, "abcdefg") == 0, "编码结果");
    CHECK(remote_encode_url_path("abcdefgh", out, sizeof(out)) != 0, "放不下时拒绝");
    CHECK(out[0] == '\0', "拒绝时输出清空");

    char encoded[64];
    CHECK(remote_encode_url_path("a b/中文", encoded, sizeof(encoded)) == 0, "空格+中文编码");
    CHECK(strcmp(encoded, "a%20b/%E4%B8%AD%E6%96%87") == 0, "编码保留 '/'");
    char component[64];
    CHECK(remote_encode_url_component("a/b", component, sizeof(component)) == 0, "分量编码");
    CHECK(strcmp(component, "a%2Fb") == 0, "分量编码转义 '/'");
}

static void test_defaults(void)
{
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_HTTP, 0) == 80, "HTTP 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_HTTP, 1) == 443, "HTTPS 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_WEBDAV, 1) == 443, "WebDAVS 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_FTP, 0) == 21, "FTP 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_FTP, 1) == 990, "FTPS 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_SFTP, 0) == 22, "SFTP 默认端口");
    CHECK(remote_url_default_port(REMOTE_PROTOCOL_SMB, 0) == 445, "SMB 默认端口");
    CHECK(strcmp(remote_url_scheme(REMOTE_PROTOCOL_HTTP, 1), "https") == 0, "HTTPS scheme");
    CHECK(strcmp(remote_url_scheme(REMOTE_PROTOCOL_FTP, 1), "ftps") == 0, "FTPS scheme");
    CHECK(strcmp(remote_url_scheme(REMOTE_PROTOCOL_WEBDAV, 0), "http") == 0, "WebDAV 走 http");
}

int main(void)
{
    test_parse_schemes();
    test_tls_never_inferred_from_port();
    test_credentials();
    test_plus_and_invalid_escape();
    test_ipv6();
    test_long_paths();
    test_redact();
    test_encode_bounds();
    test_defaults();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_remote_url\n");
        return 0;
    }
    printf("FAIL test_remote_url\n");
    return 1;
}
