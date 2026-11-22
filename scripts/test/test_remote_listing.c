/*
 * remote_listing 单测：FTP/WebDAV/HTTP/SMB/SFTP 目录列表解析
 *
 * 覆盖审计 F-01（空响应不崩）、F-03（WebDAV 命名空间/collection 泄漏）、
 * F-07（SMB 目录 size=0）、F-11（MLSD 文件名含 ';'）、F-12（HTML href/实体）、
 * F-13（SFTP 文件名含空格）。
 *
 * 运行：scripts/test/run-unit-tests.sh test_remote_listing
 * 依赖（见 test_remote_listing.srcs）：remote/remote_parse.c + stub_logger.c
 */

#include "remote/remote.h"
#include "remote/remote_parse.h"

#include <stdio.h>
#include <stdlib.h>
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

static RemoteDirEntry *find_entry(RemoteDirEntry *entries, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

/* ── FTP ──────────────────────────────────────────────────────────── */

static void test_ftp_mlsd(void)
{
    const char *payload =
        "type=cdir;modify=20241120120000; .\r\n"
        "type=pdir;modify=20241120120000; ..\r\n"
        "type=dir;size=0;modify=20241120120000; sub dir\r\n"
        "type=file;size=123;modify=20241120120000; a;b.mp3\r\n"
        "type=file;size=9; a b.mp3\r\n"
        "type=OS.unix=symlink;size=1; link.mp3\r\n"
        "type=file;size=7; plain.flac\r\n";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_ftp_listing(payload, strlen(payload), &entries, &count) == 0,
          "MLSD 解析成功");
    CHECK(count == 4, "MLSD 条目数（跳过 cdir/pdir/symlink）");
    RemoteDirEntry *dir = find_entry(entries, count, "sub dir");
    CHECK(dir != NULL && dir->is_dir == 1, "MLSD 空目录 size=0 仍识别为目录");
    CHECK(dir != NULL && dir->size == 0, "MLSD 目录 size=0");
    RemoteDirEntry *semi = find_entry(entries, count, "a;b.mp3");
    CHECK(semi != NULL, "MLSD 文件名含 ';' 不被截断");
    CHECK(semi != NULL && semi->size == 123, "MLSD size 解析");
    CHECK(find_entry(entries, count, "a b.mp3") != NULL, "MLSD 文件名含空格");
    CHECK(find_entry(entries, count, ".") == NULL, "MLSD 跳过 .");
    CHECK(find_entry(entries, count, "link.mp3") == NULL, "MLSD 跳过符号链接");
    remote_free_entries(entries, count);
}

static void test_ftp_list_and_empty(void)
{
    const char *payload =
        "total 12\r\n"
        "drwxr-xr-x    2 user group 4096 Nov 20 12:00 sub dir\r\n"
        "-rw-r--r--    1 user group  123 Nov 20  2024 a b.mp3\r\n"
        "lrwxrwxrwx    1 user group    4 Nov 20 12:00 link -> a b.mp3\r\n"
        "-rw-r--r--    1 user group   10 Nov 20 12:00 plain.wav\r\n";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_ftp_listing(payload, strlen(payload), &entries, &count) == 0,
          "LIST 解析成功");
    CHECK(count == 3, "LIST 条目数（跳过 total/符号链接）");
    RemoteDirEntry *dir = find_entry(entries, count, "sub dir");
    CHECK(dir != NULL && dir->is_dir == 1, "LIST 目录识别");
    CHECK(find_entry(entries, count, "a b.mp3") != NULL, "LIST 文件名含空格");
    remote_free_entries(entries, count);

    /* F-01：空响应（NULL / 0 字节）必须按空目录成功，不能崩 */
    entries = (RemoteDirEntry *)0x1;
    count = 123;
    CHECK(remote_parse_ftp_listing(NULL, 0, &entries, &count) == 0, "NULL 空响应不崩");
    CHECK(entries == NULL && count == 0, "NULL 空响应 = 0 条目");
    CHECK(remote_parse_ftp_listing("", 0, &entries, &count) == 0, "空串响应不崩");
    CHECK(count == 0, "空串响应 = 0 条目");
}

/* ── SFTP ─────────────────────────────────────────────────────────── */

static void test_sftp(void)
{
    const char *payload =
        "-rw-r--r--    1 user group 1234 Nov 20 12:00 01 - Song.mp3\n"
        "drwxr-xr-x    2 user group 4096 Nov 20 12:00 专辑 目录\n"
        "lrwxrwxrwx    1 user group    4 Nov 20 12:00 link -> x\n";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_sftp_listing(payload, strlen(payload), &entries, &count) == 0,
          "SFTP 解析成功");
    CHECK(count == 2, "SFTP 条目数（跳过符号链接）");
    CHECK(find_entry(entries, count, "01 - Song.mp3") != NULL,
          "SFTP 文件名含空格不被截成最后一个词（F-13）");
    RemoteDirEntry *dir = find_entry(entries, count, "专辑 目录");
    CHECK(dir != NULL && dir->is_dir == 1, "SFTP 目录（含空格中文名）");
    remote_free_entries(entries, count);
}

/* ── WebDAV ───────────────────────────────────────────────────────── */

static void test_webdav(void)
{
    const char *namespaced =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<D:multistatus xmlns:D=\"DAV:\">\n"
        "<D:response><D:href>/music/</D:href><D:propstat><D:prop>"
        "<D:resourcetype><D:collection/></D:resourcetype>"
        "</D:prop></D:propstat></D:response>\n"
        "<D:response><D:href>/music/tone1.wav</D:href><D:propstat><D:prop>"
        "<D:resourcetype/><D:getcontentlength>1234</D:getcontentlength>"
        "</D:prop></D:propstat></D:response>\n"
        "<D:response><D:href>/music/tone%20two.mp3</D:href><D:propstat><D:prop>"
        "<D:resourcetype/></D:prop></D:propstat></D:response>\n"
        "<D:response><D:href>/music/sub%20dir/</D:href><D:propstat><D:prop>"
        "<D:resourcetype><D:collection/></D:resourcetype>"
        "</D:prop></D:propstat></D:response>\n"
        "</D:multistatus>";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_webdav_listing(namespaced, strlen(namespaced), "/music/",
                                      &entries, &count) == 0, "WebDAV 命名空间解析成功");
    CHECK(count == 3, "WebDAV 命名空间条目数（自身目录不列出）");
    RemoteDirEntry *file = find_entry(entries, count, "tone1.wav");
    CHECK(file != NULL && file->is_dir == 0, "WebDAV 文件不是目录");
    CHECK(file != NULL && file->size == 1234, "WebDAV getcontentlength");
    CHECK(find_entry(entries, count, "tone two.mp3") != NULL, "WebDAV href 百分号解码");
    RemoteDirEntry *dir = find_entry(entries, count, "sub dir");
    CHECK(dir != NULL && dir->is_dir == 1, "WebDAV 目录识别");
    remote_free_entries(entries, count);

    /* 无命名空间前缀 */
    const char *plain =
        "<?xml version=\"1.0\"?><multistatus>"
        "<response><href>/m/a.mp3</href><propstat><prop><resourcetype/>"
        "</prop></propstat></response>"
        "<response><href>/m/b.mp3</href><propstat><prop><resourcetype/>"
        "</prop></propstat></response>"
        "</multistatus>";
    CHECK(remote_parse_webdav_listing(plain, strlen(plain), "/m/", &entries, &count) == 0,
          "WebDAV plain 解析");
    CHECK(count == 2, "WebDAV plain 条目数");
    CHECK(find_entry(entries, count, "a.mp3") != NULL &&
          find_entry(entries, count, "a.mp3")->is_dir == 0, "plain 文件不是目录");
    remote_free_entries(entries, count);

    /* collection 标记不得跨 response 泄漏（F-03） */
    const char *leak =
        "<multistatus>"
        "<response><href>/music/file.mp3</href><propstat><prop><resourcetype/>"
        "</prop></propstat></response>"
        "<response><href>/music/sub/</href><propstat><prop><resourcetype>"
        "<collection/></resourcetype></prop></propstat></response>"
        "</multistatus>";
    CHECK(remote_parse_webdav_listing(leak, strlen(leak), "/music/",
                                      &entries, &count) == 0, "WebDAV leak 解析");
    RemoteDirEntry *leaked = find_entry(entries, count, "file.mp3");
    CHECK(leaked != NULL && leaked->is_dir == 0, "collection 标记不跨 response 泄漏");
    RemoteDirEntry *sub = find_entry(entries, count, "sub");
    CHECK(sub != NULL && sub->is_dir == 1, "collection 目录仍识别");
    remote_free_entries(entries, count);

    /* 空 multistatus = 空目录；HTML 错误页 = 解析失败 */
    const char *empty =
        "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/music/</D:href>"
        "<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype>"
        "</D:prop></D:propstat></D:response></D:multistatus>";
    CHECK(remote_parse_webdav_listing(empty, strlen(empty), "/music/",
                                      &entries, &count) == 0, "WebDAV 空目录成功");
    CHECK(count == 0, "WebDAV 空目录 0 条目");
    remote_free_entries(entries, count);

    const char *not_dav = "<html><body>404</body></html>";
    CHECK(remote_parse_webdav_listing(not_dav, strlen(not_dav), "/music/",
                                      &entries, &count) != 0, "非 WebDAV 响应报错");

    /* 绝对 URL href */
    const char *absolute =
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>http://host:8080/music/abs.mp3</D:href>"
        "<D:propstat><D:prop><D:resourcetype/></D:prop></D:propstat></D:response>"
        "</D:multistatus>";
    CHECK(remote_parse_webdav_listing(absolute, strlen(absolute), "/music/",
                                      &entries, &count) == 0, "WebDAV 绝对 href 解析");
    CHECK(find_entry(entries, count, "abs.mp3") != NULL, "WebDAV 绝对 href 取文件名");
    remote_free_entries(entries, count);
}

/* ── HTTP autoindex ───────────────────────────────────────────────── */

static void test_html(void)
{
    const char *python =
        "<!DOCTYPE HTML><html><body><h1>Directory listing for /</h1><ul>"
        "<li><a href=\"a%26b.wav\">a&amp;b.wav</a></li>"
        "<li><a href=\"tone1.wav\">tone1.wav</a></li>"
        "<li><a href=\"sub%20dir/\">sub dir/</a></li>"
        "<li><a href=\"../\">../</a></li>"
        "<li><a href=\"tone1.wav\">tone1.wav (duplicate)</a></li>"
        "</ul></body></html>";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_html_listing(python, strlen(python), "/",
                                    &entries, &count) == 0, "HTTP autoindex 解析成功");
    CHECK(count == 3, "HTTP 条目数（父目录/重复项跳过）");
    RemoteDirEntry *amp = find_entry(entries, count, "a&b.wav");
    CHECK(amp != NULL && amp->is_dir == 0, "以 href 为准并解 HTML 实体（F-12）");
    RemoteDirEntry *sub = find_entry(entries, count, "sub dir");
    CHECK(sub != NULL && sub->is_dir == 1, "HTTP 目录标记");
    remote_free_entries(entries, count);

    /* Apache 形状：绝对 href + 显示文本被截断 + 排序链接 */
    const char *apache =
        "<html><body><table>"
        "<tr><th><a href=\"?C=N;O=D\">Name</a></th></tr>"
        "<tr><td><a href=\"/\">Parent Directory</a></td></tr>"
        "<tr><td><a href=\"/music/\">music/</a></td></tr>"
        "<tr><td><a href=\"/music/tone1.mp3\">tone1.mp3</a></td></tr>"
        "<tr><td><a href=\"/music/a%26b.mp3\">a&amp;b.mp3</a></td></tr>"
        "<tr><td><a href=\"/music/long-name-that-gets-truncated-in-display.mp3\">"
        "long-name-that-gets-truncated-in-disp..&gt;</a></td></tr>"
        "</table></body></html>";
    CHECK(remote_parse_html_listing(apache, strlen(apache), "/music/",
                                    &entries, &count) == 0, "Apache 形状解析");
    CHECK(count == 3, "Apache 条目数（去掉排序/父目录）");
    CHECK(find_entry(entries, count, "a&b.mp3") != NULL, "Apache 实体/相对路径");
    CHECK(find_entry(entries, count,
                     "long-name-that-gets-truncated-in-display.mp3") != NULL,
          "Apache 用 href 取全名而不是截断文本");
    CHECK(find_entry(entries, count, "music") == NULL, "自身目录链接跳过");
    remote_free_entries(entries, count);

    /* R-06：.. 归一必须同步 out_len；sub/../file 不能丢，../file 不越界 */
    const char *dots =
        "<html><body>"
        "<a href=\"sub/../tone-dot.wav\">tone-dot.wav</a>"
        "<a href=\"../outside.wav\">outside.wav</a>"
        "<a href=\"a/b/../../tone-deep.wav\">tone-deep.wav</a>"
        "</body></html>";
    CHECK(remote_parse_html_listing(dots, strlen(dots), "/music/",
                                    &entries, &count) == 0, "含 .. 的 autoindex 解析");
    CHECK(find_entry(entries, count, "tone-dot.wav") != NULL,
          "sub/../file 归一后保留（R-06 out_len）");
    CHECK(find_entry(entries, count, "tone-deep.wav") != NULL,
          "多层 ../ 归一后保留");
    CHECK(find_entry(entries, count, "outside.wav") == NULL,
          "越过 listing base 的 ../file 被拒绝");
    remote_free_entries(entries, count);

    /* 二进制内容（把文件 URL 当目录）返回错误，不是空目录 */
    const char *binary = "RIFF\x00\x01\x02WAVE";
    CHECK(remote_parse_html_listing(binary, 12, "/", &entries, &count) != 0,
          "非 HTML 内容判定为失败");
    CHECK(remote_parse_html_listing("", 0, "/", &entries, &count) == 0, "空 HTML = 空目录");
    CHECK(count == 0, "空 HTML 0 条目");

    char unescaped[128];
    remote_parse_unescape_entities("a&amp;b &#39;q&#x27; &lt;x&gt;", unescaped,
                                   sizeof(unescaped));
    CHECK(strcmp(unescaped, "a&b 'q' <x>") == 0, "HTML/XML 实体解码");
}

/* ── SMB ──────────────────────────────────────────────────────────── */

static void test_smb(void)
{
    /* 宿主 smbclient 的真实列格式：name 宽 30、类型宽 7、size 宽 8、日期 */
    const char *payload =
        "  .                              D          0  Thu Nov 20 12:00:00 2025\n"
        "  ..                             D          0  Thu Nov 20 12:00:00 2025\n"
        "  my song.mp3                    A    3917305  Thu Nov 20 12:00:00 2025\n"
        "  sub dir                        D          0  Thu Nov 20 12:00:00 2025\n"
        "  x D 123.mp3                    A       1234  Thu Nov 20 12:00:00 2025\n"
        "\n"
        "                3917305 blocks of size 1024. 123456 blocks available\n";

    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_smb_listing(payload, strlen(payload), &entries, &count) == 0,
          "SMB 列表解析成功");
    CHECK(count == 3, "SMB 条目数（./.. 与汇总行跳过）");
    RemoteDirEntry *file = find_entry(entries, count, "my song.mp3");
    CHECK(file != NULL && file->is_dir == 0 && file->size == 3917305, "SMB 文件+size");
    RemoteDirEntry *dir = find_entry(entries, count, "sub dir");
    CHECK(dir != NULL && dir->is_dir == 1,
          "SMB 目录 size=0 不被漏掉（F-07）");
    CHECK(dir != NULL && dir->size == 0, "SMB 目录 size 0");
    CHECK(find_entry(entries, count, "x D 123.mp3") != NULL,
          "SMB 文件名含 ' D 123' 不被误判");
    remote_free_entries(entries, count);

    /* NT_STATUS 错误行 → 失败（不再把错误当空目录） */
    const char *error = "NT_STATUS_LOGON_FAILURE listing \\\\host\\share\n";
    CHECK(remote_parse_smb_listing(error, strlen(error), &entries, &count) != 0,
          "SMB 错误输出判定为失败");

    RemoteDirEntry *empty_entries = (RemoteDirEntry *)0x1;
    int empty_count = 9;
    CHECK(remote_parse_smb_listing(NULL, 0, &empty_entries, &empty_count) == 0,
          "SMB 空输出不崩");
    CHECK(empty_entries == NULL && empty_count == 0, "SMB 空输出 0 条目");
}

/* F-10：条目数超过上限时明确失败，而不是无限吃内存 */
static void test_entry_cap(void)
{
    size_t per_line = 16;
    size_t total = (size_t)REMOTE_LISTING_MAX_ENTRIES * per_line;
    char *payload = malloc(total);
    if (!payload) {
        CHECK(0, "分配大目录夹具");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i <= REMOTE_LISTING_MAX_ENTRIES; i++) {
        pos += (size_t)snprintf(payload + pos, total - pos, "f%06d.wav\n", i);
    }
    RemoteDirEntry *entries = NULL;
    int count = 0;
    CHECK(remote_parse_ftp_listing(payload, pos, &entries, &count) != 0,
          "超大目录被拒绝（F-10 上限）");
    free(payload);
}

int main(void)
{
    test_ftp_mlsd();
    test_ftp_list_and_empty();
    test_sftp();
    test_webdav();
    test_html();
    test_smb();
    test_entry_cap();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_remote_listing\n");
        return 0;
    }
    printf("FAIL test_remote_listing\n");
    return 1;
}
