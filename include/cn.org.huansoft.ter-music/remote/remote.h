#ifndef REMOTE_H
#define REMOTE_H

#include "types.h"
#include <stddef.h>
#include <stdlib.h>

/* 远程音乐源（SMB/SFTP/FTP/WebDAV/HTTP）——**前端专用**模块。
 *
 * 核心不认识远程：远程服务器列表、凭据、目录浏览与下载全部由前端完成，
 * 前端把下载好的本地文件路径交给核心播放。故本头文件与其实现只允许被
 * 前端代码（ui/、main.c、player 门面）包含，核心侧目录（audio/ playlist/
 * media/ config/ library/ search/ info/ core/ cli/ app/）出现任何 remote
 * 符号即违规——由 scripts/test/check-core-purity.sh 守门。 */

#define MAX_REMOTE_CONNECTIONS 20
#define MAX_REMOTE_NAME_LEN 64

typedef enum {
    REMOTE_PROTOCOL_SMB = 0,
    REMOTE_PROTOCOL_SFTP = 1,
    REMOTE_PROTOCOL_FTP = 2,
    REMOTE_PROTOCOL_WEBDAV = 3,
    REMOTE_PROTOCOL_HTTP = 4
} RemoteProtocol;

typedef struct {
    char name[MAX_REMOTE_NAME_LEN];
    int protocol;               // RemoteProtocol value
    char host[256];
    int port;
    char username[64];
    char password[256];
    char private_key_path[MAX_PATH_LEN];
    char base_path[512];

    /* 以下字段为协议/安全能力扩展（老配置文件缺省即 0/空，行为保持旧版） */
    int use_tls;                            // 显式 TLS（https/ftps/webdavs），不由端口推断
    int insecure_tls;                       // 跳过证书校验（仅用户显式配置/测试 CA 时）
    int has_key_passphrase;                 // key_passphrase 是否有效
    char key_passphrase[256];               // SFTP 私钥口令（remote.xml 加密保存）
    char known_hosts_path[MAX_PATH_LEN];    // SFTP known_hosts；空 = 默认 ~/.ssh/known_hosts
} RemoteConnectionConfig;

typedef struct {
    char name[256];
    int is_dir;
    long long size;             // 远端字节数；-1 = 协议未提供（如 HTML autoindex）
    long mtime;                 // 远端修改时间（epoch 秒）；0 = 未知
} RemoteDirEntry;

// Lifecycle
void remote_init(void);
void remote_cleanup(void);

// Path detection
int remote_is_remote_path(const char *path);

// Directory listing – returns 0 on success (count may be 0 = empty directory),
// -1 on error. Call remote_free_entries() when done. 空响应必须按“空目录”
// 成功处理，不得对 NULL 缓冲做字符串操作（见审计 F-01）。
int remote_list_directory(const RemoteConnectionConfig *conn,
                          const char *subpath,
                          RemoteDirEntry **out_entries,
                          int *out_count);
void remote_free_entries(RemoteDirEntry *entries, int count);

// Protocol name for display
const char *remote_protocol_name(int protocol);

// URL parsing/construction —— 实现见 remote_url.c（无 libcurl 依赖）
int remote_parse_url(const char *url, RemoteConnectionConfig *conn);
int remote_build_url(const RemoteConnectionConfig *conn, const char *subpath,
                     char *url, size_t url_size);
int remote_build_dir_url(const RemoteConnectionConfig *conn, const char *subpath,
                         char *url, size_t url_size);
int remote_encode_url_path(const char *in, char *out, size_t out_size);
int remote_url_decode(const char *in, char *out, size_t out_size);
void remote_redact_url(const char *in, char *out, size_t out_size);

// Error reporting – returns last error message (empty string if none).
// 每线程一份，工作线程写、UI 线程读不会互相踩（见审计 F-26）。
const char *remote_strerror(void);
void remote_set_error(const char *msg);

// Download one remote file to a local file.
// subpath 是服务器上的完整路径（含文件名）；凭据来自 conn，绝不出现在
// URL 文本里（F-05）。失败时删除目标文件（调用方通常传 <final>.part，
// 成功后自行 rename）。
// Returns 0 on success, -1 on error.
int remote_fetch_file(const RemoteConnectionConfig *conn, const char *subpath,
                      const char *dest_path);

/* 中断钩子：curl 传输期间周期性回调；返回非 0 立即中止传输（F-09）。
 * 由 remote_cache 的取消逻辑安装；传 NULL 卸载。 */
void remote_set_abort_check(int (*check)(void *ctx), void *ctx);

/* 工作线程存活标记：remote_cleanup() 在仍有 worker 时不会做 curl 全局清理，
 * 避免“退出后 worker 继续用 libcurl”的竞态（F-09）。 */
void remote_set_worker_active(int active);

#endif
