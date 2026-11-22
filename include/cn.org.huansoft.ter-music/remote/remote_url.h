/**
 * @file remote_url.h
 * @brief 远程音乐源 URL 构造/解析（无 libcurl 依赖，便于单测）
 *
 * 设计约束：
 *   - **凭据永不进入 URL 文本**。解析时把 userinfo 解码一次存进
 *     RemoteConnectionConfig；构造 URL 只输出 scheme://host[:port]/path。
 *     这样日志、last_opened_path、dir_history、ps 都看不到密码（F-05）。
 *   - TLS 由显式的 use_tls 标志决定，绝不由端口号推断（F-08）。
 *   - IPv6 主机内部不带方括号存储，构造时补 []（F-16）。
 *   - 路径按 RFC3986 百分号编码一次、解码一次；'+' 不是空格；非法 %xx
 *     保持字面量而不是被截断（F-17）。
 *   - 超长路径直接拒绝（返回 -1 并清空输出），不静默截断（F-17）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef REMOTE_URL_H
#define REMOTE_URL_H

#include <stddef.h>

#include "remote/remote.h"

/* 远程 URL 的最大字节数（含结尾 0）。超过即拒绝，避免缓冲区里出现半截 URL。 */
#define REMOTE_URL_MAX 4096

/* 解析 "scheme://[user[:pass]@]host[:port][/path]"。
 * 支持的 scheme（大小写不敏感）：
 *   ftp / ftps / sftp / smb / smbs / http / https / webdav / dav / webdavs / davs
 * @return 0 成功；-1 失败（conn 已清零）。 */
int remote_parse_url(const char *url, RemoteConnectionConfig *conn);

/* 构造 URL：subpath 是服务器上的完整路径（通常就是 conn->base_path 或其
 * 子目录）；subpath 为空时使用 conn->base_path。
 * 不含凭据；超长/非法输入时 url 置空并返回 -1。
 * @return 0 成功；-1 失败 */
int remote_build_url(const RemoteConnectionConfig *conn, const char *subpath,
                     char *url, size_t url_size);

/* 同 remote_build_url，但保证输出以 '/' 结尾（列目录专用，F-04）。
 * subpath 为空时输出 "<base>/" 。 */
int remote_build_dir_url(const RemoteConnectionConfig *conn, const char *subpath,
                         char *url, size_t url_size);

/* 百分号编码路径：保留 RFC3986 unreserved 与 '/'，其余字节编码。
 * @return 0 成功；-1 输出缓冲不足（输出被截断在字符边界并置空）。 */
int remote_encode_url_path(const char *in, char *out, size_t out_size);

/* 同 remote_encode_url_path，但 '/' 也编码（用于 URL 查询参数/凭据展示）。 */
int remote_encode_url_component(const char *in, char *out, size_t out_size);

/* RFC3986 百分号解码：严格解 %HH；非法 %xx 与 '+' 均按字面量保留。
 * @return 0 成功；-1 输出缓冲不足。 */
int remote_url_decode(const char *in, char *out, size_t out_size);

/* 去掉 URL 的 userinfo（scheme://user:pass@host → scheme://host）。
 * 无 userinfo 时原样拷贝。用于日志/历史/错误信息。 */
void remote_redact_url(const char *in, char *out, size_t out_size);

/* URL 是否含 userinfo（含 '@'）。 */
int remote_url_has_userinfo(const char *url);

/* 协议 + TLS 标志对应的 scheme / 默认端口。 */
const char *remote_url_scheme(int protocol, int use_tls);
int remote_url_default_port(int protocol, int use_tls);

#endif /* REMOTE_URL_H */
