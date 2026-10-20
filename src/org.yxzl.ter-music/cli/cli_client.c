/**
 * @file cli_client.c
 * @brief CLI 的 D-Bus 客户端：把子命令翻译成 Info/Control 接口调用
 *
 * 本文件中的函数**不**初始化音频/ffmpeg/ncurses：CLI 客户端是短命进程，
 * 只连接会话总线并与当前主实例（daemon 或 TUI）通信。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/cli.h"

#include "app/open.h"
#include "config/config.h"
#include "config/config_fields.h"
#include "i18n/i18n.h"
#include "media/rpc.h"
#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"
#include "queue/backend_queue.h"
#include "info/info.h"
#include "types.h"
#include "util/json.h"
#include "util/utf8.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* 单文件标签读取：playlist.c 既有实现（与 library.c 同一复用方式，非公开头）。
 * queue append/insert 用它为本地文件补元数据，失败时退回 basename。 */
extern void get_audio_metadata(const char *path, char *title, char *artist, char *album);

#ifdef HAVE_DBUS

#include <dbus/dbus.h>
#include <signal.h>
#include <math.h>

/* 带格式参数的输出：格式串来自语言包，末尾统一补换行。语言包未就绪时
 * i18n_get() 返回键名本身，不会崩溃（与 cli.c 的 cli_out/cli_err 同约定）。 */
static void cli_client_out(const char *key, ...) {
    va_list args;
    va_start(args, key);
    vfprintf(stdout, i18n_get(key), args);
    va_end(args);
    fputc('\n', stdout);
}

static void cli_client_err(const char *key, ...) {
    va_list args;
    va_start(args, key);
    vfprintf(stderr, i18n_get(key), args);
    va_end(args);
    fputc('\n', stderr);
}

#define CLI_CALL_TIMEOUT_MS 5000

/* CLI 侧只用到接口名，不依赖核心侧 rpc.h 的宏定义（保持客户端/核心解耦）。 */
#define CLI_LYRICS_INTERFACE "org.yxzl.ter_music.Lyrics"
#define CLI_CONFIG_INTERFACE "org.yxzl.ter_music.Config"

typedef struct {
    DBusConnection *connection;
    char bus[128];
} CliClient;

static volatile sig_atomic_t g_cli_interrupted = 0;

/* 上一次 D-Bus 调用失败是否为“核心业务错误”（org.yxzl.ter_music.Error.*）。
 * 见 cli_send()：分页 TooLarge/InvalidArgs 归此类，由读类子命令映射成 REFUSED。 */
static int g_cli_last_error_is_core = 0;

static void cli_interrupt_handler(int sig) {
    (void)sig;
    g_cli_interrupted = 1;
}

/* ── 连接管理 ───────────────────────────────────────────────────── */

/* 部分被裁剪的执行环境不会传递会话总线地址，例如 Linyaps 沙箱里的
 * `ll-cli enter <app> -- <cmd>`（nsenter 进入容器但不带宿主环境变量）。
 * 此时按标准位置补全 DBUS_SESSION_BUS_ADDRESS，使 CLI 仍能找到会话总线；
 * 该位置与普通桌面会话一致（$XDG_RUNTIME_DIR/bus，兜底 /run/user/<uid>/bus）。 */
static void cli_ensure_session_bus_address(void) {
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address != NULL && address[0] != '\0') {
        return;
    }

    char path[PATH_MAX];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir != NULL && runtime_dir[0] != '\0') {
        snprintf(path, sizeof(path), "%s/bus", runtime_dir);
    } else {
        snprintf(path, sizeof(path), "/run/user/%u/bus", (unsigned)getuid());
    }

    struct stat info;
    if (stat(path, &info) != 0 || !S_ISSOCK(info.st_mode)) {
        return;
    }

    char fallback[PATH_MAX + 16];
    snprintf(fallback, sizeof(fallback), "unix:path=%s", path);
    setenv("DBUS_SESSION_BUS_ADDRESS", fallback, 1);
}

/* CLI 是短命进程，且一条命令内会多次发起调用：使用私有连接，
 * 由本进程显式关闭，避免共享连接在最后一次 unref 时被库关闭、
 * 后续调用却拿到已关闭对象（表现为调用静默失败或 NoReply）。 */
static DBusConnection *cli_connect(void) {
    DBusError error;
    dbus_error_init(&error);

    cli_ensure_session_bus_address();

    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        cli_client_err("cli.err.bus_connect_fmt", error.message);
        dbus_error_free(&error);
        return NULL;
    }
    if (!connection) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.bus_unavailable"));
        return NULL;
    }

    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    return connection;
}

static void cli_disconnect(DBusConnection *connection) {
    if (!connection) {
        return;
    }
    if (dbus_connection_get_is_connected(connection)) {
        dbus_connection_close(connection);
    }
    dbus_connection_unref(connection);
}

static int cli_client_open(CliClient *client, const char *bus_override) {
    memset(client, 0, sizeof(*client));
    client->connection = cli_connect();
    if (!client->connection) {
        return CLI_EXIT_DBUS;
    }
    snprintf(client->bus, sizeof(client->bus), "%s",
             (bus_override && bus_override[0]) ? bus_override : CLI_PRIMARY_BUS_NAME);
    return CLI_EXIT_OK;
}

static void cli_client_close(CliClient *client) {
    cli_disconnect(client->connection);
    client->connection = NULL;
}

static int cli_name_has_owner(DBusConnection *connection, const char *name) {
    DBusError error;
    dbus_error_init(&error);
    int has_owner = dbus_bus_name_has_owner(connection, name, &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return 0;
    }
    return has_owner ? 1 : 0;
}

int cli_client_primary_available(const char *bus) {
    DBusConnection *connection = cli_connect();
    if (!connection) {
        return 0;
    }
    const char *name = (bus && bus[0]) ? bus : CLI_PRIMARY_BUS_NAME;
    int available = cli_name_has_owner(connection, name);
    cli_disconnect(connection);
    return available;
}

static int cli_owner_pid(DBusConnection *connection, const char *name) {
    DBusMessage *message = dbus_message_new_method_call("org.freedesktop.DBus",
                                                        "/org/freedesktop/DBus",
                                                        "org.freedesktop.DBus",
                                                        "GetConnectionUnixProcessID");
    if (!message) {
        return 0;
    }

    DBusError error;
    dbus_error_init(&error);
    dbus_message_append_args(message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, CLI_CALL_TIMEOUT_MS, &error);
    dbus_message_unref(message);

    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return 0;
    }
    if (!reply) {
        return 0;
    }

    dbus_uint32_t pid = 0;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return 0;
    }
    dbus_message_unref(reply);
    return (int)pid;
}

int cli_client_wait_for_online(const char *bus, int timeout_ms, int *pid_out) {
    const char *name = (bus && bus[0]) ? bus : CLI_PRIMARY_BUS_NAME;

    DBusConnection *connection = cli_connect();
    if (!connection) {
        return CLI_EXIT_DBUS;
    }

    int waited = 0;
    while (waited <= timeout_ms) {
        if (cli_name_has_owner(connection, name)) {
            if (pid_out) {
                *pid_out = cli_owner_pid(connection, name);
            }
            cli_disconnect(connection);
            return CLI_EXIT_OK;
        }
        usleep(100000);
        waited += 100;
    }

    cli_disconnect(connection);
    return CLI_EXIT_NO_INSTANCE;
}

/* ── 调用与回复解析 ─────────────────────────────────────────────── */

/* Quit 专用：对端断开（正在退出）不算错误，用 got_reply 区分“收到回复” */
static DBusMessage *cli_call0_quit(CliClient *client, const char *interface_name,
                                   const char *method, int *got_reply) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        client->connection, message, CLI_CALL_TIMEOUT_MS, &error);
    dbus_message_unref(message);

    if (dbus_error_is_set(&error)) {
        int going_away = dbus_error_has_name(&error, DBUS_ERROR_NO_REPLY) ||
                         dbus_error_has_name(&error, DBUS_ERROR_DISCONNECTED) ||
                         dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN) ||
                         dbus_error_has_name(&error, DBUS_ERROR_NAME_HAS_NO_OWNER);
        dbus_error_free(&error);
        if (going_away) {
            if (got_reply) {
                *got_reply = 0;
            }
            return NULL;
        }
        cli_client_err("cli.err.dbus_call_method_fmt", method);
        return NULL;
    }

    if (got_reply) {
        *got_reply = 1;
    }
    return reply;
}

static DBusMessage *cli_send(CliClient *client, DBusMessage *message) {
    if (!message) {
        return NULL;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        client->connection, message, CLI_CALL_TIMEOUT_MS, &error);
    dbus_message_unref(message);

    /* 记录上一次失败是否为“核心业务错误”（org.yxzl.ter_music.Error.*）：
     * 分页 TooLarge / InvalidArgs 这类应由调用方映射成 CLI_EXIT_REFUSED，
     * 而不是和真正的总线故障（CLI_EXIT_DBUS）混为一谈。成功调用会清掉它。 */
    g_cli_last_error_is_core = 0;

    if (dbus_error_is_set(&error)) {
        if (error.name && strncmp(error.name, "org.yxzl.ter_music.Error.", 25) == 0) {
            g_cli_last_error_is_core = 1;
        }
        if (dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN) ||
            dbus_error_has_name(&error, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
            fprintf(stderr, "%s\n", i18n_get("cli.err.no_instance_cli"));
        } else if (dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_METHOD) ||
                   dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_INTERFACE)) {
            cli_client_err("cli.err.unsupported_interface_fmt", error.message);
        } else {
            cli_client_err("cli.err.dbus_call_fmt", error.message);
        }
        dbus_error_free(&error);
        return NULL;
    }
    return reply;
}

static DBusMessage *cli_call0(CliClient *client, const char *interface_name,
                              const char *method) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    return cli_send(client, message);
}

static DBusMessage *cli_call_s(CliClient *client, const char *interface_name,
                               const char *method, const char *value) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    const char *safe = value ? value : "";
    dbus_message_append_args(message, DBUS_TYPE_STRING, &safe, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

static DBusMessage *cli_call_i(CliClient *client, const char *interface_name,
                               const char *method, int value) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    dbus_int32_t arg = (dbus_int32_t)value;
    dbus_message_append_args(message, DBUS_TYPE_INT32, &arg, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

static DBusMessage *cli_call_x(CliClient *client, const char *interface_name,
                               const char *method, int64_t value) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    dbus_int64_t arg = (dbus_int64_t)value;
    dbus_message_append_args(message, DBUS_TYPE_INT64, &arg, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

static DBusMessage *cli_call_d(CliClient *client, const char *interface_name,
                               const char *method, double value) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    dbus_message_append_args(message, DBUS_TYPE_DOUBLE, &value, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

static DBusMessage *cli_call_sb(CliClient *client, const char *interface_name,
                                const char *method, const char *value, int flag) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    const char *safe = value ? value : "";
    dbus_bool_t boolean = flag ? TRUE : FALSE;
    dbus_message_append_args(message, DBUS_TYPE_STRING, &safe,
                             DBUS_TYPE_BOOLEAN, &boolean, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

/* 两个 int32 入参：Queue.Get(offset, count) / Lyrics.GetDocument(offset, count) */
static DBusMessage *cli_call_ii(CliClient *client, const char *interface_name,
                                const char *method, int a, int b) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    dbus_int32_t first = (dbus_int32_t)a;
    dbus_int32_t second = (dbus_int32_t)b;
    dbus_message_append_args(message, DBUS_TYPE_INT32, &first,
                             DBUS_TYPE_INT32, &second, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

/* int32 + string 入参：Queue.InsertAfter(position, json) */
static DBusMessage *cli_call_is(CliClient *client, const char *interface_name,
                                const char *method, int position, const char *value) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    dbus_int32_t arg = (dbus_int32_t)position;
    const char *safe = value ? value : "";
    dbus_message_append_args(message, DBUS_TYPE_INT32, &arg,
                             DBUS_TYPE_STRING, &safe, DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

/* string + int32 + int32 入参：Info.GetCoverArt(charset, cols, rows) */
static DBusMessage *cli_call_sii(CliClient *client, const char *interface_name,
                                 const char *method, const char *value,
                                 int cols, int rows) {
    DBusMessage *message = dbus_message_new_method_call(client->bus, CLI_OBJECT_PATH,
                                                        interface_name, method);
    if (!message) {
        return NULL;
    }
    const char *safe = value ? value : "";
    dbus_int32_t c = (dbus_int32_t)cols;
    dbus_int32_t r = (dbus_int32_t)rows;
    dbus_message_append_args(message, DBUS_TYPE_STRING, &safe,
                             DBUS_TYPE_INT32, &c, DBUS_TYPE_INT32, &r,
                             DBUS_TYPE_INVALID);
    return cli_send(client, message);
}

static int cli_reply_bool(DBusMessage *reply, int *ok) {
    if (!reply) {
        return CLI_EXIT_DBUS;
    }
    DBusError error;
    dbus_error_init(&error);
    dbus_bool_t value = FALSE;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &value,
                               DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return CLI_EXIT_DBUS;
    }
    dbus_message_unref(reply);
    if (ok) {
        *ok = value ? 1 : 0;
    }
    return CLI_EXIT_OK;
}

static int cli_reply_int(DBusMessage *reply, int *value) {
    if (!reply) {
        return CLI_EXIT_DBUS;
    }
    DBusError error;
    dbus_error_init(&error);
    dbus_int32_t out = 0;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_INT32, &out, DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return CLI_EXIT_DBUS;
    }
    dbus_error_free(&error);
    dbus_message_unref(reply);
    if (value) {
        *value = out;
    }
    return CLI_EXIT_OK;
}

static int cli_reply_double(DBusMessage *reply, double *value) {
    if (!reply) {
        return CLI_EXIT_DBUS;
    }
    DBusError error;
    dbus_error_init(&error);
    double out = 0.0;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_DOUBLE, &out, DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return CLI_EXIT_DBUS;
    }
    dbus_error_free(&error);
    dbus_message_unref(reply);
    if (value) {
        *value = out;
    }
    return CLI_EXIT_OK;
}

static int cli_reply_string(DBusMessage *reply, char *out, size_t out_size) {
    if (!reply) {
        return CLI_EXIT_DBUS;
    }
    DBusError error;
    dbus_error_init(&error);
    const char *value = NULL;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_STRING, &value,
                               DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return CLI_EXIT_DBUS;
    }
    dbus_error_free(&error);
    if (out && out_size) {
        snprintf(out, out_size, "%s", value ? value : "");
    }
    dbus_message_unref(reply);
    return CLI_EXIT_OK;
}

/* ── 极简 JSON 取值（仅用于 CLI 展示，不做通用解析） ────────────── */

static int cli_json_int(const char *json, const char *key, long *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *position = strstr(json, pattern);
    if (!position) {
        return -1;
    }
    position = strchr(position, ':');
    if (!position) {
        return -1;
    }
    position++;
    while (*position == ' ') {
        position++;
    }
    if (strncmp(position, "null", 4) == 0) {
        return -1;
    }
    *out = strtol(position, NULL, 10);
    return 0;
}

static int cli_json_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) {
        return -1;
    }
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *position = strstr(json, pattern);
    if (!position) {
        return -1;
    }
    position = strchr(position, ':');
    if (!position) {
        return -1;
    }
    position++;
    while (*position == ' ') {
        position++;
    }
    if (*position != '"') {
        return -1;
    }
    position++;
    size_t written = 0;
    while (*position != '\0' && *position != '"' && written + 1 < out_size) {
        out[written++] = *position++;
    }
    out[written] = '\0';
    return 0;
}

/* ── 实例信息 ───────────────────────────────────────────────────── */

/* 在**既有连接**上问某实例的 Info.InstanceInfo（避免每个名字各开一条连接）。
 * 调用方负责连接的关闭；client 只借用它，不能按普通 CliClient 释放。 */
static int cli_instance_info_on(DBusConnection *connection, const char *bus,
                                char *json_out, size_t json_size, int *pid_out) {
    CliClient client;
    memset(&client, 0, sizeof(client));
    client.connection = connection;
    /* 与 cli_client_open() 同一口径：NULL/空串 = 主实例名。少了这一步，
     * 目标名会变成 "(null)"，libdbus 的 bus name 断言会直接 abort。 */
    snprintf(client.bus, sizeof(client.bus), "%s",
             (bus && bus[0]) ? bus : CLI_PRIMARY_BUS_NAME);

    int result = cli_reply_string(cli_call0(&client, CLI_INFO_INTERFACE, "InstanceInfo"),
                                  json_out, json_size);
    if (result == CLI_EXIT_OK && pid_out) {
        long pid = 0;
        if (cli_json_int(json_out, "pid", &pid) == 0) {
            *pid_out = (int)pid;
        } else {
            *pid_out = 0;
        }
    }
    return result;
}

static int cli_instance_info(const char *bus, char *json_out, size_t json_size,
                             int *pid_out) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    int result = cli_instance_info_on(client.connection, bus, json_out, json_size, pid_out);
    cli_client_close(&client);
    return result;
}

int cli_client_instance_mode(const char *bus, char *mode_out, size_t mode_size,
                             int *pid_out) {
    char json[512] = "";
    int pid = 0;

    if (!cli_client_primary_available(bus)) {
        return CLI_EXIT_NO_INSTANCE;
    }

    int rc = cli_instance_info(bus, json, sizeof(json), &pid);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (mode_out && mode_size) {
        if (cli_json_string(json, "mode", mode_out, mode_size) != 0) {
            snprintf(mode_out, mode_size, "unknown");
        }
    }
    if (pid_out) {
        *pid_out = pid;
    }
    return CLI_EXIT_OK;
}

/* 名字是否属于本基名下的实例：主名本身，或 `<主名>.instance<pid>` */
static int cli_instance_name_matches(const char *base, const char *name) {
    if (!base || !name) {
        return 0;
    }
    if (strcmp(name, base) == 0) {
        return 1;
    }
    size_t base_len = strlen(base);
    static const char k_instance_suffix[] = ".instance";
    if (strncmp(name, base, base_len) != 0 ||
        strncmp(name + base_len, k_instance_suffix, sizeof(k_instance_suffix) - 1) != 0) {
        return 0;
    }
    /* 只接受 `.instance<数字>`：避免把别人的名字前缀撞进来 */
    const char *digits = name + base_len + sizeof(k_instance_suffix) - 1;
    if (digits[0] == '\0') {
        return 0;
    }
    for (const char *p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

int cli_client_list_instances(const char *base_bus, CliInstanceInfo *out, int out_cap) {
    if (!out || out_cap <= 0) {
        return -1;
    }

    const char *base = (base_bus && base_bus[0]) ? base_bus : CLI_PRIMARY_BUS_NAME;

    DBusConnection *connection = cli_connect();
    if (!connection) {
        return -1;
    }

    /* 本机 libdbus 头文件未提供 dbus_bus_list_names，故与 cli_owner_pid()
     * 一样手写方法调用；回复是 "as"，用显式迭代器解析。 */
    DBusMessage *message = dbus_message_new_method_call("org.freedesktop.DBus",
                                                        "/org/freedesktop/DBus",
                                                        "org.freedesktop.DBus",
                                                        "ListNames");
    if (!message) {
        cli_disconnect(connection);
        return -1;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, CLI_CALL_TIMEOUT_MS, &error);
    dbus_message_unref(message);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
    }
    if (!reply) {
        cli_disconnect(connection);
        return -1;
    }

    int written = 0;
    DBusMessageIter iter;
    DBusMessageIter array;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
            const char *name = NULL;
            dbus_message_iter_get_basic(&array, &name);
            if (name && cli_instance_name_matches(base, name) && written < out_cap) {
                CliInstanceInfo *slot = &out[written];
                memset(slot, 0, sizeof(*slot));
                snprintf(slot->bus, sizeof(slot->bus), "%s", name);
                slot->primary = (strcmp(name, base) == 0);
                snprintf(slot->mode, sizeof(slot->mode), "unknown");

                char json[512] = "";
                int pid = 0;
                if (cli_instance_info_on(connection, name, json, sizeof(json), &pid) ==
                    CLI_EXIT_OK) {
                    slot->pid = pid;
                    if (cli_json_string(json, "mode", slot->mode, sizeof(slot->mode)) != 0) {
                        snprintf(slot->mode, sizeof(slot->mode), "unknown");
                    }
                }
                written++;
            }
            dbus_message_iter_next(&array);
        }
    }
    dbus_message_unref(reply);
    cli_disconnect(connection);

    /* 主实例在前，其余按 pid 升序：输出与测试都稳定 */
    for (int i = 1; i < written; i++) {
        CliInstanceInfo key = out[i];
        int j = i - 1;
        while (j >= 0 && (out[j].primary < key.primary ||
                          (out[j].primary == key.primary && out[j].pid > key.pid))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return written;
}

/* ── 握手：核心 api_version 校验 ───────────────────────────────────
 * 短命 CLI 进程一条命令即退出，这里最多做一次 GetInfo 往返；用静态缓存把
 * 同进程内多次校验（如 play 分块下发前后的多次调用）收敛成一次，避免重复往返。 */
static char g_api_checked_bus[128] = "";
static int g_api_checked_rc = CLI_EXIT_OK;

/* 在既有连接上读 core.api_version；返回 CLI_EXIT_OK/DBUS，并把版本号写入 *api。 */
static int cli_core_api_version(CliClient *client, long *api) {
    char *json = malloc(INFO_JSON_MAX);
    if (!json) {
        return CLI_EXIT_DBUS;
    }
    int rc = cli_reply_string(cli_call0(client, CLI_INFO_INTERFACE, "GetInfo"),
                              json, INFO_JSON_MAX);
    if (rc != CLI_EXIT_OK) {
        free(json);
        return rc;
    }
    JsonReader reader;
    json_reader_init(&reader, json, strlen(json));
    if (!json_reader_ok(&reader)) {
        free(json);
        return CLI_EXIT_DBUS;
    }
    /* 旧核心（api_version<2）没有 core 对象：json_get_path 落空 → 视为不兼容 */
    *api = (long)json_get_int(&reader, "core.api_version", -1);
    free(json);
    return CLI_EXIT_OK;
}

int cli_client_check_api(const char *bus) {
    const char *name = (bus && bus[0]) ? bus : CLI_PRIMARY_BUS_NAME;

    /* 同进程、同总线：沿用上一次结果，不再往返 */
    if (g_api_checked_bus[0] && strcmp(g_api_checked_bus, name) == 0) {
        return g_api_checked_rc;
    }

    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;   /* 连不上总线：DBUS（cli_send 已对无实例给出可读提示） */
    }

    long api = -1;
    rc = cli_core_api_version(&client, &api);
    cli_client_close(&client);

    if (rc != CLI_EXIT_OK) {
        g_api_checked_rc = rc;
        snprintf(g_api_checked_bus, sizeof(g_api_checked_bus), "%s", name);
        return rc;
    }
    if (api < TER_MUSIC_API_VERSION) {
        cli_client_err("cli.err.core_too_old_fmt", TER_MUSIC_API_VERSION, api);
        fprintf(stderr, "%s\n", i18n_get("cli.err.core_too_old_hint"));
        g_api_checked_rc = CLI_EXIT_REFUSED;
        snprintf(g_api_checked_bus, sizeof(g_api_checked_bus), "%s", name);
        return CLI_EXIT_REFUSED;
    }

    g_api_checked_rc = CLI_EXIT_OK;
    snprintf(g_api_checked_bus, sizeof(g_api_checked_bus), "%s", name);
    return CLI_EXIT_OK;
}

/* ── 传输控制 ───────────────────────────────────────────────────── */

int cli_client_transport(const char *bus, const char *method) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* 握手：传输控制也先校验核心 api_version（同进程缓存，一次往返） */
    rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        cli_client_close(&client);
        return rc;
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call0(&client, CLI_CONTROL_INTERFACE, method), &ok);
    cli_client_close(&client);

    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        cli_client_err("cli.err.operation_refused_fmt", method);
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_seek(const char *bus, const char *argument) {
    if (!argument || argument[0] == '\0') {
        fprintf(stderr, "%s\n", i18n_get("cli.err.seek_usage"));
        return CLI_EXIT_USAGE;
    }

    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    int ok = 0;
    if (argument[0] == '+' || argument[0] == '-') {
        double seconds = atof(argument);
        rc = cli_reply_bool(cli_call_x(&client, CLI_CONTROL_INTERFACE, "SeekBy",
                                       (int64_t)llround(seconds * 1000000.0)), &ok);
    } else if (strchr(argument, '%') != NULL) {
        double percent = atof(argument);
        char json[INFO_JSON_MAX];
        rc = cli_reply_string(cli_call0(&client, CLI_INFO_INTERFACE, "GetProgress"),
                              json, sizeof(json));
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        long duration_ms = 0;
        if (cli_json_int(json, "duration_ms", &duration_ms) != 0 || duration_ms <= 0) {
            fprintf(stderr, "%s\n", i18n_get("cli.err.seek_duration_unknown"));
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
        if (percent < 0.0) percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        rc = cli_reply_bool(cli_call_x(&client, CLI_CONTROL_INTERFACE, "SeekTo",
                                       (int64_t)llround((double)duration_ms * percent * 10.0)),
                            &ok);
    } else {
        int minutes = 0;
        int seconds = 0;
        if (sscanf(argument, "%d:%d", &minutes, &seconds) == 2) {
            rc = cli_reply_bool(cli_call_x(&client, CLI_CONTROL_INTERFACE, "SeekTo",
                                           (int64_t)(minutes * 60 + seconds) * 1000000LL),
                                &ok);
        } else {
            double seconds_value = atof(argument);
            rc = cli_reply_bool(cli_call_x(&client, CLI_CONTROL_INTERFACE, "SeekTo",
                                           (int64_t)llround(seconds_value * 1000000.0)),
                                &ok);
        }
    }

    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.seek_failed"));
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_volume(const char *bus, const char *argument) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (!argument || argument[0] == '\0') {
        int volume = 0;
        rc = cli_reply_int(cli_call0(&client, CLI_CONTROL_INTERFACE, "GetVolume"), &volume);
        cli_client_close(&client);
        if (rc == CLI_EXIT_OK) {
            printf("%d%%\n", volume);
        }
        return rc;
    }

    int target = 0;
    if (argument[0] == '+' || argument[0] == '-') {
        int current = 0;
        rc = cli_reply_int(cli_call0(&client, CLI_CONTROL_INTERFACE, "GetVolume"), &current);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        target = current + atoi(argument);
    } else {
        target = atoi(argument);
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call_i(&client, CLI_CONTROL_INTERFACE, "SetVolume", target), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

int cli_client_speed(const char *bus, const char *argument) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (!argument || argument[0] == '\0') {
        double speed = 1.0;
        rc = cli_reply_double(cli_call0(&client, CLI_CONTROL_INTERFACE, "GetSpeed"), &speed);
        cli_client_close(&client);
        if (rc == CLI_EXIT_OK) {
            printf("%.2fx\n", speed);
        }
        return rc;
    }

    double speed = atof(argument);
    if (speed < 0.5 || speed > 3.0) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.speed_range"));
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call_d(&client, CLI_CONTROL_INTERFACE, "SetSpeed", speed), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

int cli_client_mode(const char *bus, const char *argument) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (!argument || argument[0] == '\0') {
        int mode = 0;
        char name[256] = "";
        rc = cli_reply_int(cli_call0(&client, CLI_CONTROL_INTERFACE, "GetPlayMode"), &mode);
        if (rc == CLI_EXIT_OK) {
            DBusMessage *reply = cli_call0(&client, CLI_CONTROL_INTERFACE, "GetPlayModeName");
            if (cli_reply_string(reply, name, sizeof(name)) != CLI_EXIT_OK) {
                /* 取显示名失败不再吞掉后留空串：标注 (unknown)，用户能看出是
                 * 名称查询失败而不是“模式没有名字”。 */
                snprintf(name, sizeof(name), "(unknown)");
            }
        }
        cli_client_close(&client);
        if (rc == CLI_EXIT_OK) {
            printf("%s (%d) %s\n", info_play_mode_id((PlayMode)mode), mode, name);
        }
        return rc;
    }

    int mode = info_play_mode_from_id(argument);
    if (mode < 0) {
        cli_client_err("cli.err.mode_unknown_fmt", argument);
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call_i(&client, CLI_CONTROL_INTERFACE, "SetPlayMode", mode), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

/* ── show ───────────────────────────────────────────────────────── */

static int cli_show_once(CliClient *client, const char *options, int want_json) {
    char *text = malloc(INFO_JSON_MAX);
    if (!text) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.oom"));
        return CLI_EXIT_DBUS;
    }

    int rc;
    if (want_json) {
        rc = cli_reply_string(cli_call0(client, CLI_INFO_INTERFACE, "GetInfo"),
                              text, INFO_JSON_MAX);
    } else {
        rc = cli_reply_string(cli_call_s(client, CLI_INFO_INTERFACE, "GetDisplay", options),
                              text, INFO_JSON_MAX);
    }

    if (rc == CLI_EXIT_OK) {
        fputs(text, stdout);
        size_t length = strlen(text);
        if (!want_json && (length == 0 || text[length - 1] != '\n')) {
            fputc('\n', stdout);
        }
    }
    free(text);
    return rc;
}

static void cli_watch_paint(const char *text, int first_frame) {
    if (first_frame) {
        fputs("\033[2J\033[H", stdout);
    } else {
        fputs("\033[H", stdout);
    }

    const char *cursor = text;
    while (cursor && *cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        if (newline) {
            fwrite(cursor, 1, (size_t)(newline - cursor), stdout);
            fputs("\033[K\n", stdout);
            cursor = newline + 1;
        } else {
            fputs(cursor, stdout);
            fputs("\033[K\n", stdout);
            cursor += strlen(cursor);
        }
    }
    fputs("\033[J", stdout);
    fflush(stdout);
}

int cli_client_show(const char *bus, const char *options, int want_json,
                    int watch_interval_ms) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (watch_interval_ms <= 0) {
        rc = cli_show_once(&client, options, want_json);
        cli_client_close(&client);
        return rc;
    }

    if (want_json) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.watch_json_conflict"));
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.watch_requires_tty"));
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = cli_interrupt_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    fputs("\033[?25l", stdout);
    fflush(stdout);

    char *text = malloc(INFO_JSON_MAX);
    if (!text) {
        fputs("\033[?25h", stdout);
        cli_client_close(&client);
        return CLI_EXIT_DBUS;
    }

    int first_frame = 1;
    int disconnect_rc = CLI_EXIT_OK;
    while (!g_cli_interrupted) {
        rc = cli_reply_string(cli_call_s(&client, CLI_INFO_INTERFACE, "GetDisplay", options),
                              text, INFO_JSON_MAX);
        if (rc != CLI_EXIT_OK) {
            /* 核心失联 / D-Bus 错误：记下 rc，循环结束后据此非零退出，
             * 不能像旧实现那样无条件 return OK 把故障粉饰成“正常结束”。 */
            disconnect_rc = rc;
            break;
        }
        cli_watch_paint(text, first_frame);
        first_frame = 0;

        int slept = 0;
        while (slept < watch_interval_ms && !g_cli_interrupted) {
            usleep(50000);
            slept += 50;
        }
    }

    free(text);
    fputs("\033[?25h\n", stdout);
    fflush(stdout);
    cli_client_close(&client);

    /* 信号退出（Ctrl+C / SIGTERM）仍是 0；非信号导致的循环中止返回失联码 */
    if (g_cli_interrupted) {
        return CLI_EXIT_OK;
    }
    return disconnect_rc;
}

/* ── play / quit / reload / playlist ────────────────────────────── */

/* 说明：cli_client_load_local_content / cli_client_build_queue_json 只触碰
 * 内容侧（app_open_path + g_playlist），不依赖 D-Bus，故放在文件末尾的“两种
 * 构建都编译”区，供 `play --foreground` 在无 D-Bus 构建里也能装载内容。 */

/* ── 分块下发：把 g_playlist 内容交给核心 ─────────────────────────
 * 核心对单发 Queue.Set/Append 超过 BQ_SET_MAX(500) 条整体拒绝并提示分块。
 * 这里复用前端 playlist_queue_foreach_chunk（与 TUI/远端整表下发同一条装配
 * 路径），首块 Set、余块 Append。任一块失败立即中止：核心停在已下发的合法前缀。 */
typedef struct {
    CliClient *client;
    int total;       /* 已成功下发的条目总数（用于 --index 范围校验） */
    int error_rc;    /* 失败时的退出码 */
} PlayChunkPush;

static int play_push_chunk_cb(const char *chunk_json, int chunk_index,
                              int entries_in_chunk, void *user)
{
    PlayChunkPush *push = (PlayChunkPush *)user;
    const char *method = (chunk_index == 0) ? "Set" : "Append";

    int written = 0;
    DBusMessage *reply = cli_call_s(push->client, CLI_QUEUE_INTERFACE, method, chunk_json);
    int rc = cli_reply_int(reply, &written);
    if (rc != CLI_EXIT_OK) {
        push->error_rc = rc;
        return -1;
    }
    if (written < 0) {
        cli_client_err("cli.err.queue_chunk_refused_fmt",
                       chunk_index + 1, entries_in_chunk);
        push->error_rc = CLI_EXIT_REFUSED;
        return -1;
    }
    push->total += written;
    return 0;
}

static int cli_client_deliver_queue_chunked(CliClient *client, int *out_total,
                                            int *out_error_rc)
{
    PlayChunkPush push = {0};
    push.client = client;
    push.error_rc = CLI_EXIT_REFUSED;

    int dispatched = playlist_queue_foreach_chunk(play_push_chunk_cb, &push);
    if (dispatched < 0) {
        if (out_error_rc) {
            *out_error_rc = push.error_rc ? push.error_rc : CLI_EXIT_REFUSED;
        }
        return -1;
    }
    if (out_total) {
        *out_total = dispatched;
    }
    return 0;
}

int cli_client_play(const char *bus, const char *path, int index, int mode) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* 握手：装载/下发前一次性校验核心 api_version（同进程缓存，不重复往返）。 */
    rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        cli_client_close(&client);
        return rc;
    }

    const char *load_path = NULL;
    char default_path[MAX_PATH_LEN] = "";

    if (path && path[0]) {
        load_path = path;
    } else if (index >= 0) {
        /* 无 path 但显式给 --index：核心队列为空时无从播放，故装载配置默认音乐
         * 目录（default_startup_path → remember_last_path 的 last_opened_path），
         * 再分块下发并 PlayAt(index)。两个都没配 / 装载失败即明确报错。 */
        if (g_app_config.default_startup_path[0]) {
            snprintf(default_path, sizeof(default_path), "%s",
                     g_app_config.default_startup_path);
        } else if (g_app_config.remember_last_path && g_app_config.last_opened_path[0]) {
            snprintf(default_path, sizeof(default_path), "%s",
                     g_app_config.last_opened_path);
        }
        if (!default_path[0]) {
            fprintf(stderr, "%s\n", i18n_get("cli.err.play_no_default_path"));
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
        load_path = default_path;
    } else {
        /* 无 path 无 index：保持 Control.Play 语义（继续/从头播放当前队列）不变 */
        int ok = 0;
        rc = cli_reply_bool(cli_call0(&client, CLI_CONTROL_INTERFACE, "Play"), &ok);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        if (!ok) {
            fprintf(stderr, "%s\n", i18n_get("cli.err.play_no_tracks"));
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
        cli_client_close(&client);
        return CLI_EXIT_OK;
    }

    /* ── 有装载路径：本进程扫描装载，再分块下发，最后 PlayAt + 设模式 ── */
    int selected_track = -1;
    if (cli_client_load_local_content(load_path, &selected_track) < 0) {
        cli_client_err("cli.err.open_path_fmt", load_path);
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }

    int written = 0;
    int deliver_error = CLI_EXIT_REFUSED;
    if (cli_client_deliver_queue_chunked(&client, &written, &deliver_error) != 0) {
        cli_client_close(&client);
        return deliver_error;
    }
    if (written <= 0) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.loaded_no_tracks"));
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }

    /* 未显式给 --index 时从内容侧装载的那一首开始（单文件即该文件，目录即首曲） */
    int position = (index >= 0) ? index : selected_track;
    if (position < 0) {
        position = 0;
    }
    /* 分块后 written 是全量条数（不再被截成 500），范围校验以它为准 */
    if (position >= written) {
        cli_client_err("cli.err.position_out_of_range_fmt", position, written);
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call_i(&client, CLI_QUEUE_INTERFACE, "PlayAt", position), &ok);
    if (rc != CLI_EXIT_OK) {
        cli_client_close(&client);
        return rc;
    }
    if (!ok) {
        cli_client_err("cli.err.play_at_failed_fmt", position + 1);
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }

    /* 模式改到队列下发**之后**设置：旧实现在 Set 之前调 SetPlayMode，此刻核心队列
     * 为空，核心 set_play_mode 的 play_queue_rebuild 直接跳过重排、Queue.Set 又不
     * 应用模式，于是 `play --mode shuffle` 不洗牌。
     * 核心语义：Control.SetPlayMode → set_play_mode → play_queue_rebuild →
     * bq_apply_mode 会就地按模式重排执行顺序（洗牌/文件夹/专辑/艺术家组模式），
     * 洗牌时并把当前条目挪到队首。因此这里**不需要**再补一次 Queue.Shuffle——
     * 补了就是双重洗牌。SetPlayMode 的业务返回 bool 不能再吞（漏洞17）。 */
    if (mode >= 0) {
        int ok_mode = 0;
        rc = cli_reply_bool(cli_call_i(&client, CLI_CONTROL_INTERFACE, "SetPlayMode", mode),
                            &ok_mode);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        if (!ok_mode) {
            cli_client_err("cli.err.mode_refused_fmt", mode);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
    }

    cli_client_close(&client);
    return CLI_EXIT_OK;
}

/* ── 回复映射助手：区分“核心业务错误(REFUSED)”与“总线故障(DBUS)” ────
 * cli_send() 把 D-Bus 错误折叠成 NULL 回复；org.yxzl.ter_music.Error.*（分页
 * TooLarge / OutOfRange / InvalidArgs）语义上属于“核心拒绝”，其余才是 DBUS。 */
static int cli_reply_string_ex(DBusMessage *reply, char *out, size_t out_size) {
    if (reply) {
        return cli_reply_string(reply, out, out_size);
    }
    return g_cli_last_error_is_core ? CLI_EXIT_REFUSED : CLI_EXIT_DBUS;
}

static int cli_reply_bool_ex(DBusMessage *reply, int *ok) {
    if (reply) {
        return cli_reply_bool(reply, ok);
    }
    return g_cli_last_error_is_core ? CLI_EXIT_REFUSED : CLI_EXIT_DBUS;
}

/* 读类调用统一收尾：reply 非空则填 text，否则按错误类别返回退出码 */
static int cli_call_string(CliClient *client, DBusMessage *reply,
                           char *text, size_t text_size) {
    (void)client;   /* 预留：与写类调用同签名，便于日后统一收尾 */
    int rc = cli_reply_string_ex(reply, text, text_size);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    return CLI_EXIT_OK;
}

/* 为单个本地文件构造 {"entries":[{...}]} 载荷：元数据尽量用 playlist.c 的
 * get_audio_metadata()（含 APE/GBK 修正），标题缺失时退回文件名 basename。
 * 时长/CUE/歌词来源不下发，由核心按文件本身解析（与前端整表装配同口径）。 */
static char *cli_build_single_entry_json(const char *path) {
    char title[MAX_META_LEN] = "";
    char artist[MAX_META_LEN] = "";
    char album[MAX_META_LEN] = "";

    get_audio_metadata(path, title, artist, album);
    if (!title[0]) {
        const char *base = strrchr(path, '/');
        snprintf(title, sizeof(title), "%s", base && base[1] ? base + 1 : path);
    }

    size_t capacity = (size_t)(MAX_PATH_LEN * 2 + MAX_META_LEN * 6 + 256);
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    size_t pos = 0;
    pos = json_append_char(buffer, capacity, pos, '{');
    pos = json_append_key(buffer, capacity, pos, "entries");
    pos = json_append_char(buffer, capacity, pos, '[');
    pos = json_append_char(buffer, capacity, pos, '{');
    pos = json_append_key(buffer, capacity, pos, "path");
    pos = json_append_escaped(buffer, capacity, pos, path);
    pos = json_append_raw(buffer, capacity, pos, ",");
    pos = json_append_key(buffer, capacity, pos, "title");
    pos = json_append_escaped(buffer, capacity, pos, title);
    pos = json_append_raw(buffer, capacity, pos, ",");
    pos = json_append_key(buffer, capacity, pos, "artist");
    pos = json_append_escaped(buffer, capacity, pos, artist);
    pos = json_append_raw(buffer, capacity, pos, ",");
    pos = json_append_key(buffer, capacity, pos, "album");
    pos = json_append_escaped(buffer, capacity, pos, album);
    pos = json_append_char(buffer, capacity, pos, '}');
    pos = json_append_char(buffer, capacity, pos, ']');
    pos = json_append_char(buffer, capacity, pos, '}');
    buffer[pos] = '\0';

    if (json_writer_overflowed(capacity, pos)) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

/* ── queue 子命令 ───────────────────────────────────────────────── */

/* 表格式渲染一页队列：序号 / 标题 / 艺术家 / 路径，当前播放行用 '>' 标记。 */
static void cli_print_queue_page(const char *json) {
    JsonReader reader;
    json_reader_init(&reader, json, strlen(json));

    long long total = json_get_int(&reader, "count", 0);
    long long current = json_get_int(&reader, "current_position", -1);
    long long page_offset = json_get_int(&reader, "offset", 0);

    JsonValue rows;
    if (json_get_path(&reader, "rows", &rows) != 0 ||
        rows.type != JSON_VALUE_ARRAY) {
        cli_client_out("cli.table.total_fmt", total);
        return;
    }

    cli_client_out("cli.table.total_current_fmt", total, current + 1, page_offset + 1);
    printf(" %4s  %-24s  %-18s  %s\n",
           i18n_get("cli.table.index"), i18n_get("cli.table.title"),
           i18n_get("cli.table.artist"), i18n_get("cli.table.path"));

    JsonReader array = reader;
    if (json_reader_enter(&array, &rows) != 0) {
        return;
    }

    JsonValue element;
    while (json_array_next(&array, &element) == 1) {
        if (element.type != JSON_VALUE_OBJECT) {
            continue;
        }
        JsonReader row;
        json_reader_init(&row, element.start, element.length);

        long long position = json_get_int(&row, "position", -1);
        char title[MAX_META_LEN] = "";
        char artist[MAX_META_LEN] = "";
        char path[MAX_PATH_LEN] = "";
        json_get_string(&row, "title", title, sizeof(title));
        json_get_string(&row, "artist", artist, sizeof(artist));
        json_get_string(&row, "path", path, sizeof(path));

        char title_pad[64], artist_pad[40];
        utf8_str_pad(title_pad, sizeof(title_pad), title, 24);
        utf8_str_pad(artist_pad, sizeof(artist_pad), artist, 18);

        printf("%c%3d  %s  %s  %s\n",
               (position == current) ? '>' : ' ',
               (int)(position >= 0 ? position + 1 : 0),
               title_pad, artist_pad, path);
    }
}

int cli_client_queue_list(const char *bus, int offset, int count) {
    if (count < 0 || count > RPC_PAGE_MAX) {
        cli_client_err("cli.err.queue_count_range_fmt", RPC_PAGE_MAX, RPC_PAGE_DEFAULT);
        return CLI_EXIT_USAGE;
    }
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* 单页可能很大（≤1000 行 × ~几百字节），堆分配，避免大栈帧 */
    size_t capacity = (size_t)RPC_PAYLOAD_MAX / 2;
    char *text = malloc(capacity);
    if (!text) {
        cli_client_close(&client);
        return CLI_EXIT_DBUS;
    }

    rc = cli_call_string(&client,
                         cli_call_ii(&client, CLI_QUEUE_INTERFACE, "Get", offset, count),
                         text, capacity);
    if (rc != CLI_EXIT_OK) {
        if (rc == CLI_EXIT_REFUSED) {
            fprintf(stderr, "%s\n", i18n_get("cli.msg.queue_page_refused"));
        }
        free(text);
        cli_client_close(&client);
        return rc;
    }

    cli_print_queue_page(text);
    free(text);
    cli_client_close(&client);
    return CLI_EXIT_OK;
}

int cli_client_queue_clear(const char *bus) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    rc = cli_reply_bool_ex(cli_call0(&client, CLI_QUEUE_INTERFACE, "Clear"), &ok);
    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_clear_refused"));
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_queue_shuffle(const char *bus) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    rc = cli_reply_bool_ex(cli_call0(&client, CLI_QUEUE_INTERFACE, "Shuffle"), &ok);
    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_shuffle_refused"));
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_queue_remove(const char *bus, int position) {
    if (position < 0) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_remove_position"));
        return CLI_EXIT_USAGE;
    }
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    /* 越界位置核心回 Error.OutOfRange → 由 cli_send 折叠 → REFUSED */
    rc = cli_reply_bool_ex(cli_call_i(&client, CLI_QUEUE_INTERFACE, "RemoveAt", position), &ok);
    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        if (rc == CLI_EXIT_REFUSED) {
            cli_client_err("cli.err.queue_remove_failed_fmt", position + 1);
        }
        return rc;
    }
    if (!ok) {
        cli_client_err("cli.err.queue_remove_refused_fmt", position + 1);
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_queue_append(const char *bus, char **paths, int n) {
    if (n <= 0 || !paths) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_append_no_path"));
        return CLI_EXIT_USAGE;
    }
    for (int i = 0; i < n; i++) {
        if (!paths[i] || !paths[i][0] || !bq_path_is_local(paths[i])) {
            cli_client_err("cli.err.not_local_path_fmt",
                           paths[i] ? paths[i] : "");
            return CLI_EXIT_USAGE;
        }
    }

    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    int appended = 0;
    for (int i = 0; i < n; i++) {
        char *payload = cli_build_single_entry_json(paths[i]);
        if (!payload) {
            cli_client_err("cli.err.build_entry_failed_fmt", paths[i]);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
        int written = 0;
        DBusMessage *reply = cli_call_s(&client, CLI_QUEUE_INTERFACE, "Append", payload);
        free(payload);
        rc = cli_reply_int(reply, &written);
        if (rc != CLI_EXIT_OK) {
            cli_client_err("cli.err.queue_append_failed_fmt", paths[i], rc);
            cli_client_close(&client);
            return g_cli_last_error_is_core ? CLI_EXIT_REFUSED : rc;
        }
        if (written <= 0) {
            cli_client_err("cli.err.queue_append_refused_fmt", paths[i]);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
        appended += written;
    }

    cli_client_out("cli.msg.queue_appended_fmt", appended);
    cli_client_close(&client);
    return CLI_EXIT_OK;
}

int cli_client_queue_insert(const char *bus, int position, const char *path) {
    if (position < 0) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_insert_position"));
        return CLI_EXIT_USAGE;
    }
    if (!path || !path[0] || !bq_path_is_local(path)) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.queue_insert_path"));
        return CLI_EXIT_USAGE;
    }

    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* InsertAfter 锚点语义（见 rpc_queue.c）：position 为“插在哪个队列位置之后”，
     * 越界（>= 队列长度）由核心回 OutOfRange。 */
    char *payload = cli_build_single_entry_json(path);
    if (!payload) {
        cli_client_err("cli.err.build_entry_failed_fmt", path);
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }
    int written = 0;
    DBusMessage *reply = cli_call_is(&client, CLI_QUEUE_INTERFACE, "InsertAfter",
                                     position, payload);
    free(payload);
    rc = cli_reply_int(reply, &written);
    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        cli_client_err("cli.err.queue_insert_failed_fmt",
                       position + 1, rc);
        return g_cli_last_error_is_core ? CLI_EXIT_REFUSED : rc;
    }
    if (written <= 0) {
        cli_client_err("cli.err.queue_insert_refused_fmt", path);
        return CLI_EXIT_REFUSED;
    }
    cli_client_out("cli.msg.queue_inserted_fmt", position + 1);
    return CLI_EXIT_OK;
}

/* ── config 子命令 ──────────────────────────────────────────────── */

/* 在标量字段表里查键：返回其分区名与类型；未知键返回 0。
 * equalizer 是复合分区，不在 k_fields，调用方另作处理。 */
static int cli_config_field_lookup(const char *key, const char **section,
                                   ConfigFieldType *type) {
    for (int i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(k_fields[i].field.key, key) == 0) {
            if (section) *section = k_fields[i].section;
            if (type) *type = k_fields[i].field.type;
            return 1;
        }
    }
    return 0;
}

int cli_client_config_get(const char *bus, const char *key) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    size_t capacity = (size_t)RPC_PAYLOAD_MAX / 2;
    char *text = malloc(capacity);
    if (!text) {
        cli_client_close(&client);
        return CLI_EXIT_DBUS;
    }

    rc = cli_call_string(&client,
                         cli_call0(&client, CLI_CONFIG_INTERFACE, "GetAll"),
                         text, capacity);
    if (rc != CLI_EXIT_OK) {
        if (rc == CLI_EXIT_REFUSED) {
            fprintf(stderr, "%s\n", i18n_get("cli.msg.config_payload_too_large"));
        }
        free(text);
        cli_client_close(&client);
        return rc;
    }

    /* 无 key：原文输出完整 JSON */
    if (!key || !key[0]) {
        fputs(text, stdout);
        size_t len = strlen(text);
        if (len == 0 || text[len - 1] != '\n') {
            fputc('\n', stdout);
        }
        free(text);
        cli_client_close(&client);
        return CLI_EXIT_OK;
    }

    /* 有 key：按 schema 分区 + 类型提取 */
    JsonReader reader;
    json_reader_init(&reader, text, strlen(text));

    if (strcmp(key, "version") == 0) {
        printf("%lld\n", json_get_int(&reader, "version", 0));
        free(text);
        cli_client_close(&client);
        return CLI_EXIT_OK;
    }

    const char *section = NULL;
    ConfigFieldType type = CFG_STRING;
    if (!cli_config_field_lookup(key, &section, &type)) {
        cli_client_err("cli.err.config_unknown_key_fmt", key);
        free(text);
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s.%s", section, key);
    JsonValue value;
    if (json_get_path(&reader, path, &value) != 0) {
        cli_client_err("cli.err.config_key_missing_fmt", key);
        free(text);
        cli_client_close(&client);
        return CLI_EXIT_REFUSED;
    }

    if (type == CFG_STRING) {
        char out[MAX_PATH_LEN] = "";
        json_value_string(&value, out, sizeof(out));
        printf("%s\n", out);
    } else if (type == CFG_FLOAT) {
        printf("%g\n", json_value_double(&value, 0.0));
    } else {
        printf("%lld\n", json_value_int(&value, 0));
    }

    free(text);
    cli_client_close(&client);
    return CLI_EXIT_OK;
}

int cli_client_config_set(const char *bus, const char *key, const char *value) {
    if (!key || !key[0] || !value) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.config_set_usage"));
        return CLI_EXIT_USAGE;
    }

    const char *section = NULL;
    ConfigFieldType type = CFG_STRING;
    if (!cli_config_field_lookup(key, &section, &type)) {
        cli_client_err("cli.err.config_set_unknown_key_fmt", key);
        return CLI_EXIT_USAGE;
    }

    /* 构造分区补丁 {"section":{"key":value}}；值按 schema 类型转成 JSON。
     * 数字键做基本校验：非数字即 USAGE（核心侧也会再校验/钳制）。 */
    size_t capacity = (size_t)(MAX_PATH_LEN * 2 + MAX_META_LEN * 2 + 256);
    char *patch = malloc(capacity);
    if (!patch) {
        return CLI_EXIT_DBUS;
    }
    size_t pos = 0;
    pos = json_append_char(patch, capacity, pos, '{');
    pos = json_append_key(patch, capacity, pos, section);
    pos = json_append_char(patch, capacity, pos, '{');
    pos = json_append_key(patch, capacity, pos, key);
    if (type == CFG_STRING) {
        pos = json_append_escaped(patch, capacity, pos, value);
    } else {
        char *end = NULL;
        double num = strtod(value, &end);
        if (!end || end == value) {
            cli_client_err("cli.err.config_value_not_number_fmt", key, value);
            free(patch);
            return CLI_EXIT_USAGE;
        }
        if (type == CFG_INT) {
            pos = json_append_int(patch, capacity, pos, (long long)num);
        } else {
            pos = json_append_double(patch, capacity, pos, num, 6);
        }
    }
    pos = json_append_char(patch, capacity, pos, '}');
    pos = json_append_char(patch, capacity, pos, '}');
    patch[pos] = '\0';

    if (json_writer_overflowed(capacity, pos)) {
        free(patch);
        return CLI_EXIT_USAGE;
    }

    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        free(patch);
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        free(patch);
        return rc;
    }

    int ok = 0;
    rc = cli_reply_bool_ex(cli_call_s(&client, CLI_CONFIG_INTERFACE, "Set", patch), &ok);
    free(patch);
    cli_client_close(&client);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.config_set_refused"));
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_config_reload(const char *bus) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    rc = cli_reply_bool_ex(cli_call0(&client, CLI_CONFIG_INTERFACE, "Reload"), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

int cli_client_config_reset(const char *bus) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    rc = cli_reply_bool_ex(cli_call0(&client, CLI_CONFIG_INTERFACE, "Reset"), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

/* ── lyrics / track / cover 子命令 ──────────────────────────────── */

/* 当前歌词行上下文（Info.GetLyricsLines）：核心已只读化，直接读快照。 */
static void cli_print_lyrics_lines(const char *json) {
    JsonReader reader;
    json_reader_init(&reader, json, strlen(json));

    int has_lyrics = json_get_bool(&reader, "has_lyrics", 0);
    if (!has_lyrics) {
        printf("%s\n", i18n_get("cli.msg.lyrics_none"));
        return;
    }

    char current[MAX_LYRIC_TEXT_LEN] = "";
    char next[MAX_LYRIC_TEXT_LEN] = "";
    json_get_string(&reader, "current_text", current, sizeof(current));
    json_get_string(&reader, "next_text", next, sizeof(next));

    printf("%s%s\n", i18n_get("info.lyrics.current"), current);
    if (next[0]) {
        printf("%s%s\n", i18n_get("cli.field.next"), next);
    }
}

/* 带时间戳全量歌词（Lyrics.GetDocument 分页）；返回退出码。 */
static int cli_print_lyrics_document(CliClient *client, int want_lines) {
    size_t capacity = (size_t)RPC_PAYLOAD_MAX / 2;
    char *text = malloc(capacity);
    if (!text) {
        return CLI_EXIT_DBUS;
    }

    int printed = 0;
    int rc = CLI_EXIT_OK;
    for (int offset = 0; ; ) {
        /* 每页取一页；want_lines>0 时不超过它 */
        int page_count = RPC_PAGE_DEFAULT;
        if (want_lines > 0 && want_lines - printed < page_count) {
            page_count = want_lines - printed;
        }
        if (page_count <= 0) {
            break;
        }

        DBusMessage *reply = cli_call_ii(client, CLI_LYRICS_INTERFACE, "GetDocument",
                                        offset, page_count);
        rc = cli_call_string(client, reply, text, capacity);
        if (rc != CLI_EXIT_OK) {
            if (rc == CLI_EXIT_REFUSED) {
                fprintf(stderr, "%s\n", i18n_get("cli.msg.lyrics_page_refused"));
            }
            break;
        }

        JsonReader reader;
        json_reader_init(&reader, text, strlen(text));
        int has_lyrics = json_get_bool(&reader, "has_lyrics", 0);
        if (!has_lyrics) {
            printf("%s\n", i18n_get("cli.msg.lyrics_none"));
            break;
        }

        JsonValue lines;
        if (json_get_path(&reader, "lines", &lines) != 0 ||
            lines.type != JSON_VALUE_ARRAY) {
            break;
        }
        int current_index = (int)json_get_int(&reader, "current_index", -1);
        int has_timestamps = json_get_bool(&reader, "has_timestamps", 0);

        JsonReader array = reader;
        if (json_reader_enter(&array, &lines) != 0) {
            break;
        }
        JsonValue element;
        while (json_array_next(&array, &element) == 1 && printed < want_lines) {
            if (element.type != JSON_VALUE_OBJECT) {
                continue;
            }
            JsonReader lr;
            json_reader_init(&lr, element.start, element.length);
            long long index = json_get_int(&lr, "index", -1);
            double timestamp = json_get_double(&lr, "timestamp", 0.0);
            char line[MAX_LYRIC_TEXT_LEN] = "";
            json_get_string(&lr, "text", line, sizeof(line));

            int minute = (int)(timestamp / 60.0);
            double second = timestamp - minute * 60.0;
            printf("%c%s [%02d:%05.2f] %s\n",
                   (index == current_index) ? '>' : ' ',
                   has_timestamps ? "" : "  ",
                   minute, second, line);
            printed++;
        }

        long long total = json_get_int(&reader, "total", 0);
        offset += page_count;
        if (offset >= total) {
            break;
        }
    }

    free(text);
    return rc;
}

int cli_client_lyrics(const char *bus, int lines) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (lines <= 0) {
        /* 默认：当前歌词行上下文（核心只读 Info.GetLyricsLines） */
        size_t capacity = 4096;
        char *text = malloc(capacity);
        if (!text) {
            cli_client_close(&client);
            return CLI_EXIT_DBUS;
        }
        rc = cli_call_string(&client,
                             cli_call0(&client, CLI_INFO_INTERFACE, "GetLyricsLines"),
                             text, capacity);
        if (rc == CLI_EXIT_OK) {
            cli_print_lyrics_lines(text);
        }
        free(text);
        cli_client_close(&client);
        return rc;
    }

    /* --lines：走 Lyrics.GetDocument 分页拉全量、带时间戳 */
    rc = cli_print_lyrics_document(&client, lines);
    cli_client_close(&client);
    return rc;
}

int cli_client_track(const char *bus) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    size_t capacity = (size_t)INFO_JSON_MAX;
    char *text = malloc(capacity);
    if (!text) {
        cli_client_close(&client);
        return CLI_EXIT_DBUS;
    }
    rc = cli_call_string(&client,
                         cli_call0(&client, CLI_INFO_INTERFACE, "GetTrackInfo"),
                         text, capacity);
    if (rc != CLI_EXIT_OK) {
        free(text);
        cli_client_close(&client);
        return rc;
    }

    JsonReader reader;
    json_reader_init(&reader, text, strlen(text));

    int valid = json_get_bool(&reader, "valid", 0);
    if (!valid) {
        printf("%s\n", i18n_get("cli.msg.no_track"));
        free(text);
        cli_client_close(&client);
        return CLI_EXIT_OK;
    }

    char title[MAX_META_LEN] = "", artist[MAX_META_LEN] = "",
         album[MAX_META_LEN] = "", path[MAX_PATH_LEN] = "";
    json_get_string(&reader, "title", title, sizeof(title));
    json_get_string(&reader, "artist", artist, sizeof(artist));
    json_get_string(&reader, "album", album, sizeof(album));
    json_get_string(&reader, "path", path, sizeof(path));
    long long number = json_get_int(&reader, "number", 0);
    long long queue_position = json_get_int(&reader, "queue_position", -1);
    long long queue_count = json_get_int(&reader, "queue_count", 0);

    /* 位置/时长来自 playback 段（这里只补队列位与曲目编号，避免二次往返） */
    printf("%s%s\n", i18n_get("player.title"), title[0] ? title : i18n_get("cli.msg.unknown"));
    printf("%s%s\n", i18n_get("player.artist"), artist[0] ? artist : i18n_get("cli.msg.unknown"));
    printf("%s%s\n", i18n_get("player.album"), album[0] ? album : i18n_get("cli.msg.unknown"));
    cli_client_out("cli.field.track_number_fmt", number);
    if (queue_position >= 0) {
        cli_client_out("cli.field.queue_position_fmt", queue_position, queue_count);
    }
    printf("%s%s\n", i18n_get("info.field.path"), path[0] ? path : i18n_get("cli.msg.unknown"));

    free(text);
    cli_client_close(&client);
    return CLI_EXIT_OK;
}

int cli_client_cover(const char *bus, int width, int height, const char *charset) {
    int rc = cli_client_check_api(bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (width < 0 || height < 0 ||
        width > INFO_COVER_COLS_MAX || height > INFO_COVER_ROWS_MAX) {
        cli_client_err("cli.err.cover_size_range_fmt",
                       INFO_COVER_COLS_MIN, INFO_COVER_COLS_MAX,
                       INFO_COVER_ROWS_MIN, INFO_COVER_ROWS_MAX);
        return CLI_EXIT_USAGE;
    }
    if (charset && charset[0] &&
        strcmp(charset, "braille") != 0 && strcmp(charset, "ascii") != 0 &&
        strcmp(charset, "half") != 0) {
        fprintf(stderr, "%s\n", i18n_get("cli.err.cover_charset"));
        return CLI_EXIT_USAGE;
    }

    CliClient client;
    rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* width/height <=0、charset 空串 = 沿用配置（核心侧处理） */
    size_t capacity = (size_t)INFO_COVER_TEXT_MAX;
    char *text = malloc(capacity);
    if (!text) {
        cli_client_close(&client);
        return CLI_EXIT_DBUS;
    }
    rc = cli_call_string(&client,
                         cli_call_sii(&client, CLI_INFO_INTERFACE, "GetCoverArt",
                                      charset, width, height),
                         text, capacity);
    if (rc != CLI_EXIT_OK) {
        free(text);
        cli_client_close(&client);
        return rc;
    }

    /* 原样输出字符画（与 show 的封面同风格） */
    fputs(text, stdout);
    size_t len = strlen(text);
    if (len == 0 || text[len - 1] != '\n') {
        fputc('\n', stdout);
    }
    if (len == 0) {
        printf("%s\n", i18n_get("cli.msg.cover_none"));
    }

    free(text);
    cli_client_close(&client);
    return CLI_EXIT_OK;
}

int cli_client_activate_instance(const char *bus) {
    const char *name = (bus && bus[0]) ? bus : CLI_PRIMARY_BUS_NAME;

    DBusConnection *connection = cli_connect();
    if (!connection) {
        return CLI_EXIT_DBUS;
    }

    /* Linyaps（如意玲珑）把包内的 dbus-1/services 导出到宿主
     * $XDG_DATA_DIRS/dbus-1/services，因此可以通过总线按需拉起后台播放：
     * 总会在宿主机执行 `ll-cli run <appid> -- ter-music daemon foreground`。 */
    DBusError error;
    dbus_error_init(&error);
    dbus_uint32_t reply = 0;
    dbus_bool_t ok = dbus_bus_start_service_by_name(connection, name, 0, &reply, &error);

    int result;
    if (dbus_error_is_set(&error)) {
        /* SERVICE_UNKNOWN / NAME_HAS_NO_OWNER：没有可激活的服务文件（非沙箱安装或
         * 未安装 dbus 激活文件）→ 失败但不当作错误打印 */
        dbus_error_free(&error);
        result = CLI_EXIT_REFUSED;
    } else if (!ok ||
               (reply != DBUS_START_REPLY_SUCCESS &&
                reply != DBUS_START_REPLY_ALREADY_RUNNING)) {
        result = CLI_EXIT_REFUSED;
    } else {
        /* SUCCESS 或 ALREADY_RUNNING 都视为“实例可用” */
        result = CLI_EXIT_OK;
    }

    cli_disconnect(connection);
    return result;
}

int cli_client_quit(const char *bus) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    int got_reply = 0;
    rc = cli_reply_bool(cli_call0_quit(&client, CLI_CONTROL_INTERFACE, "Quit",
                                       &got_reply), &ok);
    cli_client_close(&client);

    /* 实例可能在回复送达前就关闭了连接（它正在退出），这同样意味着停止成功 */
    if (!got_reply) {
        return CLI_EXIT_OK;
    }
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

int cli_client_reload(const char *bus) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ok = 0;
    rc = cli_reply_bool(cli_call0(&client, CLI_CONTROL_INTERFACE, "ReloadConfig"), &ok);
    cli_client_close(&client);
    return (rc == CLI_EXIT_OK && ok) ? CLI_EXIT_OK : CLI_EXIT_REFUSED;
}

#else /* !HAVE_DBUS */

int cli_client_primary_available(const char *bus) {
    (void)bus;
    fprintf(stderr, "%s\n", i18n_get("cli.err.no_dbus_build"));
    return 0;
}

int cli_client_wait_for_online(const char *bus, int timeout_ms, int *pid_out) {
    (void)bus; (void)timeout_ms; (void)pid_out;
    return CLI_EXIT_DBUS;
}

int cli_client_instance_mode(const char *bus, char *mode_out, size_t mode_size,
                             int *pid_out) {
    (void)bus; (void)mode_out; (void)mode_size; (void)pid_out;
    return CLI_EXIT_DBUS;
}

int cli_client_list_instances(const char *base, CliInstanceInfo *out, int out_cap) {
    (void)base; (void)out; (void)out_cap;
    return -1;
}

int cli_client_transport(const char *bus, const char *method) {
    (void)bus; (void)method;
    return CLI_EXIT_DBUS;
}

int cli_client_seek(const char *bus, const char *argument) {
    (void)bus; (void)argument;
    return CLI_EXIT_DBUS;
}

int cli_client_volume(const char *bus, const char *argument) {
    (void)bus; (void)argument;
    return CLI_EXIT_DBUS;
}

int cli_client_speed(const char *bus, const char *argument) {
    (void)bus; (void)argument;
    return CLI_EXIT_DBUS;
}

int cli_client_mode(const char *bus, const char *argument) {
    (void)bus; (void)argument;
    return CLI_EXIT_DBUS;
}

int cli_client_show(const char *bus, const char *options, int want_json,
                    int watch_interval_ms) {
    (void)bus; (void)options; (void)want_json; (void)watch_interval_ms;
    return CLI_EXIT_DBUS;
}

int cli_client_play(const char *bus, const char *path, int index, int mode) {
    (void)bus; (void)path; (void)index; (void)mode;
    return CLI_EXIT_DBUS;
}

int cli_client_quit(const char *bus) {
    (void)bus;
    return CLI_EXIT_DBUS;
}

int cli_client_activate_instance(const char *bus) {
    (void)bus;
    return CLI_EXIT_DBUS;
}

int cli_client_reload(const char *bus) {
    (void)bus;
    return CLI_EXIT_DBUS;
}

int cli_client_check_api(const char *bus) {
    (void)bus;
    fprintf(stderr, "%s\n", i18n_get("cli.err.no_dbus_build"));
    return CLI_EXIT_DBUS;
}

int cli_client_queue_list(const char *bus, int offset, int count) {
    (void)bus; (void)offset; (void)count; return CLI_EXIT_DBUS;
}
int cli_client_queue_clear(const char *bus) { (void)bus; return CLI_EXIT_DBUS; }
int cli_client_queue_shuffle(const char *bus) { (void)bus; return CLI_EXIT_DBUS; }
int cli_client_queue_remove(const char *bus, int position) {
    (void)bus; (void)position; return CLI_EXIT_DBUS;
}
int cli_client_queue_append(const char *bus, char **paths, int n) {
    (void)bus; (void)paths; (void)n; return CLI_EXIT_DBUS;
}
int cli_client_queue_insert(const char *bus, int position, const char *path) {
    (void)bus; (void)position; (void)path; return CLI_EXIT_DBUS;
}

int cli_client_config_get(const char *bus, const char *key) {
    (void)bus; (void)key; return CLI_EXIT_DBUS;
}
int cli_client_config_set(const char *bus, const char *key, const char *value) {
    (void)bus; (void)key; (void)value; return CLI_EXIT_DBUS;
}
int cli_client_config_reload(const char *bus) { (void)bus; return CLI_EXIT_DBUS; }
int cli_client_config_reset(const char *bus) { (void)bus; return CLI_EXIT_DBUS; }

int cli_client_lyrics(const char *bus, int lines) {
    (void)bus; (void)lines; return CLI_EXIT_DBUS;
}
int cli_client_track(const char *bus) { (void)bus; return CLI_EXIT_DBUS; }
int cli_client_cover(const char *bus, int width, int height, const char *charset) {
    (void)bus; (void)width; (void)height; (void)charset; return CLI_EXIT_DBUS;
}

#endif /* HAVE_DBUS */

/* ── 前端内容装载（不依赖 D-Bus，两种构建都编译） ──────────────── */

int cli_client_load_local_content(const char *path, int *out_track_index)
{
    if (out_track_index) {
        *out_track_index = -1;
    }
    if (!path || path[0] == '\0') {
        return -1;
    }

    /* 内容侧：目录递归扫描或单文件装载（与前端的 app_open_path 同一实现）。
     * 起始下标必须由 app_open_path 给出：它已把 `~`/相对路径展开，并在“打开单
     * 个文件”时定位到该文件在所在目录列表里的物理下标。若像旧实现那样丢回 NULL
     * 再拿**未扩展**的原始 path 反查，`play ~/Music/foo.flac` 会 found=-1 回落 0
     * 播成第一首。 */
    int track_index = -1;
    AppOpenResult result = app_open_path(path, NULL, 0, &track_index, NULL);
    if (result != APP_OPEN_OK) {
        return -1;
    }
    if (playlist_count() <= 0) {
        return -1;
    }

    if (out_track_index) {
        *out_track_index = track_index >= 0 ? track_index : 0;
    }
    return 0;
}

char *cli_client_build_queue_json(void)
{
    return playlist_queue_render();
}

/* 供 cli.c 计算默认宽度（与 D-Bus 无关，两种构建都需要）：
 * 终端实际宽度（stdout 为 TTY 时）→ $COLUMNS → 80 */
int cli_default_width(void) {
    if (isatty(STDOUT_FILENO)) {
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
            int width = size.ws_col;
            if (width < INFO_WIDTH_MIN) width = INFO_WIDTH_MIN;
            if (width > INFO_WIDTH_MAX) width = INFO_WIDTH_MAX;
            return width;
        }
    }

    const char *columns = getenv("COLUMNS");
    if (columns && columns[0]) {
        int width = atoi(columns);
        if (width >= INFO_WIDTH_MIN && width <= INFO_WIDTH_MAX) {
            return width;
        }
    }
    return INFO_DEFAULT_WIDTH;
}
