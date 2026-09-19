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
#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"
#include "info/info.h"
#include "types.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_DBUS

#include <dbus/dbus.h>
#include <signal.h>
#include <math.h>

#define CLI_CALL_TIMEOUT_MS 5000

typedef struct {
    DBusConnection *connection;
    char bus[128];
} CliClient;

static volatile sig_atomic_t g_cli_interrupted = 0;

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
        fprintf(stderr, "错误：无法连接会话总线：%s\n", error.message);
        dbus_error_free(&error);
        return NULL;
    }
    if (!connection) {
        fprintf(stderr, "错误：无法连接会话总线（D-Bus 不可用）。\n");
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
        fprintf(stderr, "错误：D-Bus 调用失败（%s）。\n", method);
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

    if (dbus_error_is_set(&error)) {
        if (dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN) ||
            dbus_error_has_name(&error, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
            fprintf(stderr, "错误：没有正在运行的 ter-music 实例（用 `ter-music play <路径>` 或 `ter-music daemon start` 启动）。\n");
        } else if (dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_METHOD) ||
                   dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_INTERFACE)) {
            fprintf(stderr, "错误：运行中的实例不支持该接口（可能是旧版本）：%s\n", error.message);
        } else {
            fprintf(stderr, "错误：D-Bus 调用失败：%s\n", error.message);
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

static int cli_instance_info(const char *bus, char *json_out, size_t json_size,
                             int *pid_out) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

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

/* ── 传输控制 ───────────────────────────────────────────────────── */

int cli_client_transport(const char *bus, const char *method) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    int ok = 0;
    rc = cli_reply_bool(cli_call0(&client, CLI_CONTROL_INTERFACE, method), &ok);
    cli_client_close(&client);

    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (!ok) {
        fprintf(stderr, "错误：操作被实例拒绝（%s）。\n", method);
        return CLI_EXIT_REFUSED;
    }
    return CLI_EXIT_OK;
}

int cli_client_seek(const char *bus, const char *argument) {
    if (!argument || argument[0] == '\0') {
        fprintf(stderr, "错误：seek 需要参数，例如 `ter-music seek +10`、`ter-music seek 1:23`、`ter-music seek 50%%`。\n");
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
            fprintf(stderr, "错误：当前曲目时长未知，无法按百分比跳转。\n");
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
        fprintf(stderr, "错误：跳转失败（无曲目或时长未知）。\n");
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
        fprintf(stderr, "错误：倍速需在 0.5 - 3.0 之间。\n");
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
                name[0] = '\0';
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
        fprintf(stderr, "错误：未知播放模式 '%s'。可用值：sequential, single_repeat, list_repeat, "
                        "shuffle_once, shuffle_repeat, folder_*, album_*, artist_* 或 0-16。\n", argument);
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
        fprintf(stderr, "错误：内存不足。\n");
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
        fprintf(stderr, "错误：--watch 与 --json 不能同时使用。\n");
        cli_client_close(&client);
        return CLI_EXIT_USAGE;
    }
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "错误：--watch 需要在终端里直接运行（请勿重定向输出）。\n");
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
    while (!g_cli_interrupted) {
        rc = cli_reply_string(cli_call_s(&client, CLI_INFO_INTERFACE, "GetDisplay", options),
                              text, INFO_JSON_MAX);
        if (rc != CLI_EXIT_OK) {
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
    return CLI_EXIT_OK;
}

/* ── play / quit / reload / playlist ────────────────────────────── */

int cli_client_load_local_content(const char *path, int *out_track_index)
{
    if (out_track_index) {
        *out_track_index = -1;
    }
    if (!path || path[0] == '\0') {
        return -1;
    }

    /* 内容侧：目录递归扫描或单文件装载（与前端的 app_open_path 同一实现） */
    AppOpenResult result = app_open_path(path, NULL, 0, NULL, NULL);
    if (result != APP_OPEN_OK) {
        return -1;
    }
    if (playlist_count() <= 0) {
        return -1;
    }

    /* 单文件：定位该文件在内容列表里的**物理下标**。
     * 目录：从第一首开始。
     * 注意 g_selected_index 在树模式下是**可见行**下标，不能当曲目下标用。 */
    if (out_track_index) {
        int found = playlist_find_track_index_by_path(path);
        *out_track_index = found >= 0 ? found : 0;
    }
    return 0;
}

char *cli_client_build_queue_json(void)
{
    return playlist_queue_render();
}

int cli_client_play(const char *bus, const char *path, int index, int mode) {
    CliClient client;
    int rc = cli_client_open(&client, bus);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (mode >= 0) {
        int ok_mode = 0;
        rc = cli_reply_bool(cli_call_i(&client, CLI_CONTROL_INTERFACE, "SetPlayMode", mode),
                            &ok_mode);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
    }

    int ok = 0;
    if (path && path[0]) {
        /* 内容归前端：本进程扫描路径、装配队列，再把路径队列下发给核心 */
        int selected_track = -1;
        int loaded = cli_client_load_local_content(path, &selected_track);
        if (loaded < 0) {
            fprintf(stderr, "错误：无法打开 '%s'（路径不存在或没有可播放的音频）。\n", path);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }

        char *queue_json = cli_client_build_queue_json();
        if (!queue_json) {
            fprintf(stderr, "错误：装载内容后没有可播放的曲目。\n");
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }

        int written = 0;
        rc = cli_reply_int(cli_call_s(&client, CLI_QUEUE_INTERFACE, "Set", queue_json), &written);
        free(queue_json);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        if (written <= 0) {
            fprintf(stderr, "错误：核心拒绝了队列。\n");
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }

        /* 未显式给 --index 时从内容侧装载的那一首开始（单文件即该文件） */
        int position = (index >= 0) ? index : selected_track;
        if (position < 0) {
            position = 0;
        }
        if (position >= written) {
            fprintf(stderr, "错误：曲目序号 %d 超出范围（共 %d 首）。\n", position, written);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }

        rc = cli_reply_bool(cli_call_i(&client, CLI_QUEUE_INTERFACE, "PlayAt", position), &ok);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        if (!ok) {
            fprintf(stderr, "错误：核心未能开始播放第 %d 首。\n", position + 1);
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
    } else {
        rc = cli_reply_bool(cli_call0(&client, CLI_CONTROL_INTERFACE, "Play"), &ok);
        if (rc != CLI_EXIT_OK) {
            cli_client_close(&client);
            return rc;
        }
        if (!ok) {
            fprintf(stderr, "错误：实例中没有可播放的曲目，请先 `ter-music play <路径>`。\n");
            cli_client_close(&client);
            return CLI_EXIT_REFUSED;
        }
    }

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
    fprintf(stderr, "错误：本版本编译时未启用 D-Bus 支持，CLI 控制命令不可用。\n");
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

#endif /* HAVE_DBUS */

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
