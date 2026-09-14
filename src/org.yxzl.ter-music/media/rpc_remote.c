/**
 * @file rpc_remote.c
 * @brief org.yxzl.ter_music.Remote —— 远程服务器配置与浏览
 *
 * 服务器条目存在 config 的 remote_connections 里（配置唯一写入口是同层的
 * Config 接口，这里只做读写同一份数据的薄封装，落盘仍走 save_config）。
 * 目录列举与连接播放列表构建都是阻塞网络调用，交给 media/rpc_job.c 的
 * 后台线程，结果经 Status 查询与 PlaylistChanged 通知。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"
#include "media/session.h"

#include "config/config.h"
#include "config/config_json.h"
#include "config/crypto.h"
#include "logger/logger.h"
#include "remote/remote.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define REMOTE_API_INTERFACE RPC_IFACE_REMOTE

/* 当前浏览会话（List/Connect 的上下文） */
static int g_remote_connection_index = -1;
static char g_remote_subpath[MAX_PATH_LEN] = "";

static const char *rpc_remote_protocol_id(int protocol)
{
    switch (protocol) {
        case REMOTE_PROTOCOL_SMB:     return "smb";
        case REMOTE_PROTOCOL_SFTP:    return "sftp";
        case REMOTE_PROTOCOL_FTP:     return "ftp";
        case REMOTE_PROTOCOL_WEBDAV:  return "webdav";
        case REMOTE_PROTOCOL_HTTP:    return "http";
        default:                      return "unknown";
    }
}

static int rpc_remote_protocol_from_id(const char *id)
{
    if (!id) return -1;
    if (strcmp(id, "smb") == 0)    return REMOTE_PROTOCOL_SMB;
    if (strcmp(id, "sftp") == 0)   return REMOTE_PROTOCOL_SFTP;
    if (strcmp(id, "ftp") == 0)    return REMOTE_PROTOCOL_FTP;
    if (strcmp(id, "webdav") == 0) return REMOTE_PROTOCOL_WEBDAV;
    if (strcmp(id, "http") == 0)   return REMOTE_PROTOCOL_HTTP;
    return -1;
}

static DBusMessage *rpc_remote_reply_servers(DBusMessage *message)
{
    size_t capacity = RPC_PAYLOAD_MAX / 4;
    char *json = malloc(capacity);
    if (!json) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    size_t pos = 0;
    pos = json_append_char(json, capacity, pos, '{');
    pos = json_append_key(json, capacity, pos, "count");
    pos = json_append_int(json, capacity, pos, g_app_config.remote_connection_count);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "servers");
    pos = json_append_char(json, capacity, pos, '[');

    for (int i = 0; i < g_app_config.remote_connection_count &&
                    i < MAX_REMOTE_CONNECTIONS; i++) {
        const RemoteConnectionConfig *rc = &g_app_config.remote_connections[i];
        if (i > 0) pos = json_append_char(json, capacity, pos, ',');
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "index");
        pos = json_append_int(json, capacity, pos, i);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "name");
        pos = json_append_escaped(json, capacity, pos, rc->name);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "protocol");
        pos = json_append_escaped(json, capacity, pos, rpc_remote_protocol_id(rc->protocol));
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "host");
        pos = json_append_escaped(json, capacity, pos, rc->host);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "port");
        pos = json_append_int(json, capacity, pos, rc->port);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "username");
        pos = json_append_escaped(json, capacity, pos, rc->username);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "base_path");
        pos = json_append_escaped(json, capacity, pos, rc->base_path);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "private_key_path");
        pos = json_append_escaped(json, capacity, pos, rc->private_key_path);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "has_password");
        pos = json_append_bool(json, capacity, pos, rc->password[0] != '\0');
        pos = json_append_char(json, capacity, pos, '}');
    }

    pos = json_append_char(json, capacity, pos, ']');
    pos = json_append_char(json, capacity, pos, '}');
    json[pos] = '\0';

    DBusMessage *reply = rpc_reply_string(message, json);
    free(json);
    return reply;
}

static DBusMessage *rpc_remote_reply_status(DBusMessage *message)
{
    int count = 0;
    int error = 0;
    const char *path = NULL;
    RemoteDirEntry *entries = rpc_job_entries(&count, &error, &path);

    const char *state = "idle";
    if (rpc_job_state() == RPC_JOB_RUNNING) {
        state = "loading";
    } else if (rpc_job_state() == RPC_JOB_FAILED || error) {
        state = "error";
    }

    size_t capacity = RPC_PAYLOAD_MAX / 2;
    char *json = malloc(capacity);
    if (!json) {
        return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
    }

    size_t pos = 0;
    pos = json_append_char(json, capacity, pos, '{');
    pos = json_append_key(json, capacity, pos, "state");
    pos = json_append_escaped(json, capacity, pos, state);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "connection_index");
    pos = json_append_int(json, capacity, pos, g_remote_connection_index);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "path");
    pos = json_append_string_or_null(json, capacity, pos,
                                     g_remote_subpath[0] ? g_remote_subpath : NULL);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "total");
    pos = json_append_int(json, capacity, pos, count);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "error");
    pos = json_append_string_or_null(json, capacity, pos,
                                     error ? remote_strerror() : NULL);
    pos = json_append_raw(json, capacity, pos, ",");
    pos = json_append_key(json, capacity, pos, "entries");
    pos = json_append_char(json, capacity, pos, '[');

    for (int i = 0; i < count; i++) {
        if (i > 0) pos = json_append_char(json, capacity, pos, ',');
        pos = json_append_char(json, capacity, pos, '{');
        pos = json_append_key(json, capacity, pos, "name");
        pos = json_append_escaped(json, capacity, pos, entries[i].name);
        pos = json_append_raw(json, capacity, pos, ",");
        pos = json_append_key(json, capacity, pos, "is_dir");
        pos = json_append_bool(json, capacity, pos, entries[i].is_dir);
        pos = json_append_char(json, capacity, pos, '}');
    }

    pos = json_append_char(json, capacity, pos, ']');
    pos = json_append_char(json, capacity, pos, '}');
    json[pos] = '\0';

    DBusMessage *reply;
    if (pos + 1 > RPC_PAYLOAD_MAX) {
        reply = rpc_error(message, RPC_ERROR_TOO_LARGE,
                          "too many entries; request a subdirectory");
    } else {
        reply = rpc_reply_string(message, json);
    }
    free(json);
    return reply;
}

DBusMessage *rpc_remote_handle(DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);
    if (!member) {
        return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Missing method name");
    }

    if (strcmp(member, "ListServers") == 0) {
        return rpc_remote_reply_servers(message);
    }

    if (strcmp(member, "SaveServer") == 0) {
        dbus_int32_t index = -1;
        const char *patch = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_STRING, &patch,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (!patch || patch[0] != '{') {
            return rpc_error(message, RPC_ERROR_INVALID_ARGS, "server patch must be a JSON object");
        }

        int count = g_app_config.remote_connection_count;
        int target = index;
        if (target < 0) {
            if (count >= MAX_REMOTE_CONNECTIONS) {
                return rpc_error(message, RPC_ERROR_BUSY, "no free remote connection slot");
            }
            target = count;
            memset(&g_app_config.remote_connections[target], 0,
                   sizeof(RemoteConnectionConfig));
        } else if (target >= count || target >= MAX_REMOTE_CONNECTIONS) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "index is out of range");
        }

        /* 复用 Config 的补丁解析：把单个条目包成 patch 的 remote_connections 数组。
         * 为此构造一个临时 AppConfig，成功后再写回，保证原子性。 */
        AppConfig *draft = malloc(sizeof(AppConfig));
        if (!draft) {
            return rpc_error(message, DBUS_ERROR_NO_MEMORY, "Out of memory");
        }
        memcpy(draft, &g_app_config, sizeof(AppConfig));

        RemoteConnectionConfig entry;
        memset(&entry, 0, sizeof(entry));
        entry.protocol = REMOTE_PROTOCOL_SFTP;
        memcpy(&entry, &draft->remote_connections[target], sizeof(entry));

        JsonReader reader;
        json_reader_init(&reader, patch, strlen(patch));
        json_get_string(&reader, "name", entry.name, sizeof(entry.name));
        json_get_string(&reader, "host", entry.host, sizeof(entry.host));
        json_get_string(&reader, "username", entry.username, sizeof(entry.username));
        json_get_string(&reader, "base_path", entry.base_path, sizeof(entry.base_path));
        json_get_string(&reader, "private_key_path", entry.private_key_path,
                        sizeof(entry.private_key_path));

        char protocol_id[32];
        if (json_get_string(&reader, "protocol", protocol_id, sizeof(protocol_id)) > 0) {
            int protocol = rpc_remote_protocol_from_id(protocol_id);
            if (protocol < 0) {
                free(draft);
                return rpc_error(message, RPC_ERROR_INVALID_ARGS,
                                 "protocol must be smb|sftp|ftp|webdav|http");
            }
            entry.protocol = protocol;
        }

        JsonValue port;
        if (json_get_path(&reader, "port", &port) == 0) {
            int value = (int)json_value_int(&port, entry.port);
            if (value < 0 || value > 65535) {
                free(draft);
                return rpc_error(message, RPC_ERROR_INVALID_ARGS, "port is out of range");
            }
            entry.port = value;
        }

        JsonValue password;
        if (json_get_path(&reader, "password", &password) == 0 &&
            password.type == JSON_VALUE_STRING && password.length > 0) {
            json_value_string(&password, entry.password, sizeof(entry.password));
        } else if (json_get_path(&reader, "password_encrypted", &password) == 0 &&
                   password.type == JSON_VALUE_STRING && password.length > 0) {
            char hex[512];
            json_value_string(&password, hex, sizeof(hex));
            crypto_decrypt(hex, entry.password, sizeof(entry.password));
        }

        memcpy(&draft->remote_connections[target], &entry, sizeof(entry));
        if (index < 0) {
            draft->remote_connection_count = count + 1;
        }

        memcpy(&g_app_config, draft, sizeof(AppConfig));
        free(draft);

        save_config();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "DeleteServer") == 0) {
        dbus_int32_t index = -1;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        int count = g_app_config.remote_connection_count;
        if (index < 0 || index >= count) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "index is out of range");
        }

        for (int i = index; i < count - 1; i++) {
            g_app_config.remote_connections[i] = g_app_config.remote_connections[i + 1];
        }
        memset(&g_app_config.remote_connections[count - 1], 0,
               sizeof(RemoteConnectionConfig));
        g_app_config.remote_connection_count = count - 1;
        save_config();
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "List") == 0) {
        dbus_int32_t index = -1;
        const char *subpath = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_STRING, &subpath,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (index < 0 || index >= g_app_config.remote_connection_count) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "index is out of range");
        }
        if (rpc_job_state() == RPC_JOB_RUNNING) {
            return rpc_error(message, RPC_ERROR_BUSY, "another remote operation is running");
        }

        rpc_job_set_connection(&g_app_config.remote_connections[index]);
        if (rpc_job_start(RPC_JOB_REMOTE_LIST, NULL, subpath ? subpath : "", 0) != 0) {
            return rpc_error(message, RPC_ERROR_FAILED, "failed to start the listing job");
        }
        g_remote_connection_index = index;
        snprintf(g_remote_subpath, sizeof(g_remote_subpath), "%s", subpath ? subpath : "");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Connect") == 0) {
        dbus_int32_t index = -1;
        const char *subpath = NULL;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_STRING, &subpath,
                                   DBUS_TYPE_INVALID)) {
            DBusMessage *reply = rpc_error(message, DBUS_ERROR_INVALID_ARGS, error.message);
            dbus_error_free(&error);
            return reply;
        }
        dbus_error_free(&error);

        if (index < 0 || index >= g_app_config.remote_connection_count) {
            return rpc_error(message, RPC_ERROR_OUT_OF_RANGE, "index is out of range");
        }
        if (rpc_job_state() == RPC_JOB_RUNNING) {
            return rpc_error(message, RPC_ERROR_BUSY, "another remote operation is running");
        }

        rpc_job_set_connection(&g_app_config.remote_connections[index]);
        if (rpc_job_start(RPC_JOB_REMOTE_CONNECT, NULL, subpath ? subpath : "", 0) != 0) {
            return rpc_error(message, RPC_ERROR_FAILED, "failed to start the connect job");
        }
        g_remote_connection_index = index;
        snprintf(g_remote_subpath, sizeof(g_remote_subpath), "%s", subpath ? subpath : "");
        rpc_playlist_emit_changed("loading");
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Disconnect") == 0) {
        g_remote_connection_index = -1;
        g_remote_subpath[0] = '\0';
        return rpc_reply_bool(message, 1);
    }

    if (strcmp(member, "Status") == 0) {
        return rpc_remote_reply_status(message);
    }

    return rpc_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown remote method");
}

/* ── 自省片段（由 session.c 在启动时拼装） ─────────────────────── */

static const char *const k_remote_introspection =
    "  <interface name=\"org.yxzl.ter_music.Remote\">\n"
    "    <method name=\"ListServers\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"SaveServer\">\n"
    "      <arg name=\"index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"json\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"DeleteServer\">\n"
    "      <arg name=\"index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"List\">\n"
    "      <arg name=\"index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"subpath\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Status\"><arg name=\"json\" type=\"s\" direction=\"out\"/></method>\n"
    "    <method name=\"Connect\">\n"
    "      <arg name=\"index\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"subpath\" type=\"s\" direction=\"in\"/>\n"
    "      <arg type=\"b\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Disconnect\"><arg type=\"b\" direction=\"out\"/></method>\n"
    "  </interface>\n";

const char *rpc_remote_introspection(void)
{
    return k_remote_introspection;
}

#endif /* HAVE_DBUS */
