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
} RemoteConnectionConfig;

typedef struct {
    char name[256];
    int is_dir;
} RemoteDirEntry;

// Lifecycle
void remote_init(void);
void remote_cleanup(void);

// Path detection
int remote_is_remote_path(const char *path);

// URL construction
void remote_build_url(const RemoteConnectionConfig *conn,
                      const char *subpath,
                      char *url, size_t url_size);

// Percent-encode non-ASCII and URL-special characters in a path.
// Preserves '/' for path structure.  Output buffer must be at least
// available_input_len * 3 + 1 bytes for correct encoding of all inputs.
void remote_encode_url_path(const char *in, char *out, size_t out_size);
void remote_url_decode(const char *in, char *out, size_t out_size);

// Directory listing – returns number of entries, or -1 on error.
// Call remote_free_entries() when done.
int remote_list_directory(const RemoteConnectionConfig *conn,
                          const char *subpath,
                          RemoteDirEntry **out_entries,
                          int *out_count);
void remote_free_entries(RemoteDirEntry *entries, int count);

// Protocol name for display
const char *remote_protocol_name(int protocol);

// URL parsing – parse "protocol://user:pass@host:port/path" into config
// Returns 0 on success, -1 on parse failure.
int remote_parse_url(const char *url, RemoteConnectionConfig *conn);

// Error reporting – returns last error message (empty string if none)
const char *remote_strerror(void);

// Download a remote URL into a malloc'd buffer (caller must free).
// Returns 0 on success, -1 on error.
int remote_fetch_to_buffer(const char *url, unsigned char **data, size_t *size);

// Download a remote URL directly to a local file.
// Returns 0 on success, -1 on error (partial file is cleaned up).
int remote_fetch_to_file(const char *url, const char *dest_path);

// Set a hook that is called periodically during remote download to keep the
// UI responsive.  Pass NULL to unset.  The hook is called from inside curl's
// blocking I/O, so it must be safe to call from any context (typically just
// calls refresh() or similar).
void remote_set_progress_hook(void (*hook)(void));

#endif
