/**
 * @file remote_parse.h
 * @brief 远程目录列表文本解析（FTP/WebDAV/HTTP/SMB/SFTP）——不依赖 libcurl
 *
 * 这些解析器只处理内存中的响应文本，便于用 canned 夹具做单元测试：
 *   - FTP：MLSD（facts 以第一个空格结束，文件名可含 ';' 与空格）、LIST、
 *     NLST 三种形状，空响应按“空目录”处理（F-01/F-11）；
 *   - WebDAV：忽略命名空间前缀、以 response 为界判定 collection
 *     （F-03），href 做 XML 实体 + 百分号解码与相对/绝对路径归一；
 *   - HTTP autoindex：以 href 为准而不是显示文本，解 HTML 实体，
 *     跳过父目录/排序链接并去重（F-12）；
 *   - SMB：按 smbclient 真实列格式（name 宽 30、类型 7、size 8、日期）
 *     从右往左切分，目录 size=0 不再被丢掉（F-07/F-24）；
 *   - SFTP：ls -l 固定 8 列，文件名保留空格（F-13）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef REMOTE_PARSE_H
#define REMOTE_PARSE_H

#include <stddef.h>

#include "remote/remote.h"

/* 单次列目录的条目上限（防恶意服务器用超大目录把前端内存吃光，F-10） */
#define REMOTE_LISTING_MAX_ENTRIES 200000

/* 所有解析器：成功返回 0（count 可为 0 = 空目录），失败返回 -1。
 * 成功时 *out 为 malloc 的数组（可用 remote_free_entries 释放）。 */
int remote_parse_ftp_listing(const char *data, size_t len,
                             RemoteDirEntry **out, int *out_count);
int remote_parse_webdav_listing(const char *data, size_t len,
                                const char *request_path,
                                RemoteDirEntry **out, int *out_count);
int remote_parse_html_listing(const char *data, size_t len,
                              const char *request_path,
                              RemoteDirEntry **out, int *out_count);
int remote_parse_smb_listing(const char *data, size_t len,
                             RemoteDirEntry **out, int *out_count);
int remote_parse_sftp_listing(const char *data, size_t len,
                              RemoteDirEntry **out, int *out_count);

/* HTML/XML 实体解码（&amp; &#39; &#x27; &lt; ...）——供测试直接验证。 */
void remote_parse_unescape_entities(const char *in, char *out, size_t out_size);

#endif /* REMOTE_PARSE_H */
