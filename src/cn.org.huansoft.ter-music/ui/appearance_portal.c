/**
 * @file appearance_portal.c
 * @brief XDG desktop portal color-scheme 读取器（独立 private DBusConnection）
 *
 * 刻意不使用独立线程：libdbus 的连接/过滤器只在拥有它的线程 dispatch，
 * 本模块由 ui/appearance.c 的 appearance_tick()（输入路径每约 100ms 一次）
 * 单线程泵消息，避免与 ncurses/配置镜像的竞争，也无需 join。
 *
 * 错误/降级策略：无 DBUS_SESSION_BUS_ADDRESS 且无 XDG_RUNTIME_DIR 时**不**
 * 调用 libdbus（避免 X11 autolaunch 挂起）；连接失败/无 portal/超时都只回退，
 * 绝不 exit、不阻塞首帧；断线后 30s 退避重连。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/appearance_portal.h"

#include "config/appearance.h"
#include "logger/logger.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define PORTAL_BUS_NAME   "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT     "/org/freedesktop/portal/desktop"
#define PORTAL_INTERFACE  "org.freedesktop.portal.Settings"
#define PORTAL_NS         "org.freedesktop.appearance"
#define PORTAL_KEY        "color-scheme"
#define PORTAL_RETRY_FIRST_MS   500
#define PORTAL_RETRY_SECOND_MS  2000
#define PORTAL_RETRY_THIRD_MS   10000
#define PORTAL_RETRY_MAX        3
#define PORTAL_RECONNECT_MS     30000
#define PORTAL_ASYNC_TIMEOUT_MS 1500

static DBusConnection *g_conn = NULL;
static int g_cached_bg = APPEARANCE_BG_UNKNOWN;
static int g_changed = 0;
static unsigned long long g_next_retry_ms = 0;
static unsigned long long g_next_reconnect_ms = 0;
static int g_retry_count = 0;
static int g_read_in_flight = 0;
static int g_async_used_read = 0;   /* 异步重试已从 ReadOne 回退到 Read */
/* 非阻塞“回退读取”请求的结果（主题适配器失效时使用）。 */
static int g_async_result_requested = 0;
static int g_async_result_ready = 0;
static int g_async_result_bg = APPEARANCE_BG_UNKNOWN;
static DBusPendingCall *g_pending = NULL;

static unsigned long long portal_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

static void portal_clear_pending(void)
{
    if (g_pending) {
        dbus_pending_call_cancel(g_pending);
        dbus_pending_call_unref(g_pending);
        g_pending = NULL;
    }
    g_read_in_flight = 0;
}

static void portal_schedule_retry(int attempt)
{
    unsigned long long now = portal_now_ms();
    if (attempt <= 0) {
        g_next_retry_ms = now + PORTAL_RETRY_FIRST_MS;
    } else if (attempt == 1) {
        g_next_retry_ms = now + PORTAL_RETRY_SECOND_MS;
    } else if (attempt == 2) {
        g_next_retry_ms = now + PORTAL_RETRY_THIRD_MS;
    } else {
        g_next_retry_ms = 0;   /* 放弃自动重试，等下一次手动重探/重连 */
    }
}

static void portal_on_disconnect(void)
{
    portal_clear_pending();
    if (g_conn) {
        dbus_connection_close(g_conn);
        dbus_connection_unref(g_conn);
        g_conn = NULL;
    }
    g_next_reconnect_ms = portal_now_ms() + PORTAL_RECONNECT_MS;
    /* 保留最近一次已知值，避免观感跳变；重连成功后刷新。 */
    log_debug("appearance", "XDG portal connection lost; reconnect in %dms",
              PORTAL_RECONNECT_MS);
}

/* 从消息里递归解析最多两层 variant，取 uint32/int32。 */
static int portal_parse_scheme(DBusMessage *msg, unsigned int *scheme_out)
{
    if (!msg || !scheme_out) {
        return -1;
    }
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) {
        return -1;
    }
    for (int depth = 0; depth < 3; depth++) {
        int type = dbus_message_iter_get_arg_type(&iter);
        if (type == DBUS_TYPE_VARIANT) {
            DBusMessageIter sub;
            dbus_message_iter_recurse(&iter, &sub);
            iter = sub;
            continue;
        }
        if (type == DBUS_TYPE_UINT32) {
            dbus_uint32_t value = 0;
            dbus_message_iter_get_basic(&iter, &value);
            *scheme_out = (unsigned int)value;
            return 0;
        }
        if (type == DBUS_TYPE_INT32) {
            dbus_int32_t value = 0;
            dbus_message_iter_get_basic(&iter, &value);
            *scheme_out = (unsigned int)(value < 0 ? 0 : value);
            return 0;
        }
        return -1;
    }
    return -1;
}

static DBusMessage *portal_build_read(const char *method, DBusError *error)
{
    DBusMessage *msg = dbus_message_new_method_call(
        PORTAL_BUS_NAME, PORTAL_OBJECT, PORTAL_INTERFACE, method);
    if (!msg) {
        return NULL;
    }
    const char *ns = PORTAL_NS;
    const char *key = PORTAL_KEY;
    if (!dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &ns,
                                  DBUS_TYPE_STRING, &key,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return NULL;
    }
    (void)error;
    return msg;
}

static const char *portal_address(char *buffer, size_t buffer_size)
{
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address && address[0]) {
        return address;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(buffer, buffer_size, "unix:path=%s/bus", runtime);
        return buffer;
    }
    /* 两者都没有：绝不调用 libdbus（可能触发 X11 autolaunch 子进程）。 */
    return NULL;
}

static DBusHandlerResult portal_filter(DBusConnection *connection,
                                       DBusMessage *message, void *user_data);

static int portal_connect(void)
{
    if (g_conn) {
        return 1;
    }
    char buffer[512];
    const char *address = portal_address(buffer, sizeof(buffer));
    if (!address) {
        return 0;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (!conn) {
        log_debug("appearance", "Cannot open private session bus: %s",
                  dbus_error_is_set(&error) ? error.message : "unknown");
        dbus_error_free(&error);
        return 0;
    }
    if (!dbus_bus_register(conn, &error)) {
        log_debug("appearance", "Cannot register private session bus: %s",
                  dbus_error_is_set(&error) ? error.message : "unknown");
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 0;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    dbus_connection_set_route_peer_messages(conn, FALSE);
    if (!dbus_connection_add_filter(conn, portal_filter, NULL, NULL)) {
        log_debug("appearance", "Cannot add XDG portal filter");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 0;
    }

    g_conn = conn;
    log_debug("appearance", "XDG portal private session bus connected");
    return 1;
}

static int portal_add_match(void)
{
    if (!g_conn) {
        return -1;
    }
    DBusError error;
    dbus_error_init(&error);
    dbus_bus_add_match(g_conn,
        "type='signal',"
        "sender='" PORTAL_BUS_NAME "',"
        "path='" PORTAL_OBJECT "',"
        "interface='" PORTAL_INTERFACE "',"
        "member='SettingChanged'", &error);
    if (dbus_error_is_set(&error)) {
        log_debug("appearance", "Cannot add XDG portal match: %s", error.message);
        dbus_error_free(&error);
        return -1;
    }
    return 0;
}

static void portal_handle_scheme(unsigned int scheme)
{
    int bg = appearance_xdg_color_scheme_to_bg(scheme);
    if (bg == g_cached_bg) {
        return;
    }
    g_cached_bg = bg;
    g_changed = 1;
}

static DBusHandlerResult portal_filter(DBusConnection *connection,
                                       DBusMessage *message, void *user_data)
{
    (void)connection;
    (void)user_data;
    if (!appearance_portal_signal_matches(
            dbus_message_get_interface(message),
            dbus_message_get_member(message),
            dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL)) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter iter;
    if (!dbus_message_iter_init(message, &iter)) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    const char *ns = NULL;
    const char *key = NULL;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_message_iter_get_basic(&iter, &ns);
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_message_iter_get_basic(&iter, &key);
    dbus_message_iter_next(&iter);
    if (!ns || !key || strcmp(ns, PORTAL_NS) != 0 || strcmp(key, PORTAL_KEY) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter sub;
    dbus_message_iter_recurse(&iter, &sub);
    if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_uint32_t scheme = 0;
    dbus_message_iter_get_basic(&sub, &scheme);
    portal_handle_scheme((unsigned int)scheme);
    log_debug("appearance", "XDG portal SettingChanged: color-scheme=%u",
              (unsigned int)scheme);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void portal_on_read_reply(DBusPendingCall *pending, void *user_data);

static int portal_send_async_read(const char *method, int used_fallback)
{
    if (!g_conn || g_read_in_flight) {
        return -1;
    }
    DBusMessage *msg = portal_build_read(method, NULL);
    if (!msg) {
        return -1;
    }
    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(g_conn, msg, &pending,
                                         PORTAL_ASYNC_TIMEOUT_MS)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_message_unref(msg);
    if (!pending) {
        return -1;
    }
    g_pending = pending;
    g_read_in_flight = 1;
    g_async_used_read = used_fallback;
    dbus_pending_call_set_notify(pending, portal_on_read_reply, NULL, NULL);
    return 0;
}

static void portal_finish_async_request(int bg, int ready)
{
    if (!g_async_result_requested) {
        return;
    }
    g_async_result_requested = 0;
    g_async_result_ready = ready;
    g_async_result_bg = bg;
}

static void portal_async_read_again(void)
{
    if (g_async_result_requested) {
        /* 主题适配器失效回退只需要一次有界结果；失败就按 unknown 回退，
         * 不进入启动重试退避（避免 tick 长期挂 pending）。 */
        portal_finish_async_request(APPEARANCE_BG_UNKNOWN, 1);
        portal_clear_pending();
        return;
    }
    portal_clear_pending();
    if (!g_async_used_read) {
        if (portal_send_async_read("Read", 1) == 0) {
            return;
        }
    }
    g_retry_count++;
    portal_schedule_retry(g_retry_count);
}

static void portal_on_read_reply(DBusPendingCall *pending, void *user_data)
{
    (void)user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    int ok = 0;
    if (reply) {
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
            unsigned int scheme = 0;
            if (portal_parse_scheme(reply, &scheme) == 0) {
                portal_handle_scheme(scheme);
                g_retry_count = 0;
                g_next_retry_ms = 0;
                portal_finish_async_request(g_cached_bg, 1);
                ok = 1;
            }
        } else if (!g_async_used_read) {
            const char *name = dbus_message_get_error_name(reply);
            if (name && (strcmp(name, "org.freedesktop.DBus.Error.UnknownMethod") == 0 ||
                         strcmp(name, "org.freedesktop.DBus.Error.UnknownInterface") == 0)) {
                dbus_message_unref(reply);
                portal_clear_pending();
                if (portal_send_async_read("Read", 1) == 0) {
                    return;
                }
                if (g_async_result_requested) {
                    portal_finish_async_request(APPEARANCE_BG_UNKNOWN, 1);
                    portal_clear_pending();
                    return;
                }
                g_retry_count++;
                portal_schedule_retry(g_retry_count);
                return;
            }
        }
        dbus_message_unref(reply);
    }
    if (!ok) {
        portal_async_read_again();
    } else {
        portal_clear_pending();
    }
}

int appearance_portal_init(void)
{
    if (g_conn) {
        return 1;
    }
    if (!portal_connect()) {
        return 0;
    }
    if (portal_add_match() != 0) {
        portal_on_disconnect();
        return 0;
    }
    return 1;
}

int appearance_portal_read(int timeout_ms, int *bg_out)
{
    if (!bg_out) {
        return 0;
    }
    *bg_out = APPEARANCE_BG_UNKNOWN;
    if (timeout_ms <= 0) {
        timeout_ms = 120;
    }
    if (!appearance_portal_init()) {
        return 0;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = NULL;
    DBusMessage *msg = portal_build_read("ReadOne", &error);
    if (msg) {
        reply = dbus_connection_send_with_reply_and_block(g_conn, msg,
                                                          timeout_ms, &error);
        dbus_message_unref(msg);
    }
    if (!reply) {
        int fallback = 1;
        if (dbus_error_is_set(&error)) {
            const char *name = error.name;
            if (name &&
                strcmp(name, "org.freedesktop.DBus.Error.UnknownMethod") != 0 &&
                strcmp(name, "org.freedesktop.DBus.Error.UnknownInterface") != 0 &&
                strcmp(name, "org.freedesktop.DBus.Error.UnknownObject") != 0) {
                /* 超时/无服务/无总线：不再额外阻塞一次 Read。 */
                fallback = 0;
            }
            log_debug("appearance", "XDG portal ReadOne failed: %s",
                      name ? name : "unknown");
            dbus_error_free(&error);
            dbus_error_init(&error);
        }
        if (fallback) {
            msg = portal_build_read("Read", &error);
            if (msg) {
                reply = dbus_connection_send_with_reply_and_block(g_conn, msg,
                                                                 timeout_ms, &error);
                dbus_message_unref(msg);
            }
        }
    }
    if (!reply) {
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
        }
        /* 启动读失败：安排 500ms 后的非阻塞重试（最多 3 次）。 */
        if (g_retry_count == 0 && g_next_retry_ms == 0) {
            portal_schedule_retry(0);
        }
        return 0;
    }

    unsigned int scheme = 0;
    int rc = portal_parse_scheme(reply, &scheme);
    dbus_message_unref(reply);
    if (rc != 0) {
        return -1;
    }
    int bg = appearance_xdg_color_scheme_to_bg(scheme);
    g_cached_bg = bg;
    *bg_out = bg;
    g_retry_count = 0;
    g_next_retry_ms = 0;
    return 1;
}

void appearance_portal_poll(void)
{
    unsigned long long now = portal_now_ms();

    if (!g_conn) {
        if (g_next_reconnect_ms && now >= g_next_reconnect_ms) {
            g_next_reconnect_ms = 0;
            if (appearance_portal_init()) {
                (void)portal_send_async_read("ReadOne", 0);
            } else {
                g_next_reconnect_ms = now + PORTAL_RECONNECT_MS;
            }
        }
        return;
    }
    if (!dbus_connection_get_is_connected(g_conn)) {
        portal_on_disconnect();
        return;
    }

    /* 非阻塞 dispatch：只处理已到达的消息，不等待。 */
    (void)dbus_connection_read_write_dispatch(g_conn, 0);

    if (!g_read_in_flight && g_retry_count < PORTAL_RETRY_MAX &&
        g_next_retry_ms && now >= g_next_retry_ms) {
        g_next_retry_ms = 0;
        if (portal_send_async_read("ReadOne", 0) != 0) {
            g_retry_count++;
            portal_schedule_retry(g_retry_count);
        }
    }
}

int appearance_portal_request_async_read(void)
{
    /* 只复用已建立的连接：tick 属输入路径，绝不在这里做可能阻塞的连接/Hallo。 */
    if (!g_conn) {
        return 0;
    }
    g_async_result_requested = 1;
    g_async_result_ready = 0;
    g_async_result_bg = APPEARANCE_BG_UNKNOWN;
    if (!g_read_in_flight) {
        if (portal_send_async_read("ReadOne", 0) != 0) {
            portal_finish_async_request(APPEARANCE_BG_UNKNOWN, 1);
            return 0;
        }
    }
    return 1;
}

int appearance_portal_take_async_result(int *bg_out)
{
    if (!g_async_result_ready) {
        return 0;
    }
    g_async_result_ready = 0;
    g_async_result_requested = 0;
    if (bg_out) {
        *bg_out = g_async_result_bg;
    }
    return 1;
}

int appearance_portal_take_changed(int *bg_out)
{
    if (!g_changed) {
        return 0;
    }
    g_changed = 0;
    if (bg_out) {
        *bg_out = g_cached_bg;
    }
    return 1;
}

void appearance_portal_shutdown(void)
{
    portal_clear_pending();
    if (g_conn) {
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_remove_match(g_conn,
            "type='signal',"
            "sender='" PORTAL_BUS_NAME "',"
            "path='" PORTAL_OBJECT "',"
            "interface='" PORTAL_INTERFACE "',"
            "member='SettingChanged'", &error);
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
        }
        dbus_connection_remove_filter(g_conn, portal_filter, NULL);
        dbus_connection_close(g_conn);
        dbus_connection_unref(g_conn);
        g_conn = NULL;
    }
    g_cached_bg = APPEARANCE_BG_UNKNOWN;
    g_changed = 0;
    g_next_retry_ms = 0;
    g_next_reconnect_ms = 0;
    g_retry_count = 0;
    g_read_in_flight = 0;
    g_async_used_read = 0;
    g_async_result_requested = 0;
    g_async_result_ready = 0;
    g_async_result_bg = APPEARANCE_BG_UNKNOWN;
}

int appearance_portal_is_active(void)
{
    return g_conn != NULL;
}

#else /* !HAVE_DBUS */

/*
 * 无 D-Bus 构建：整个 XDG portal 探测链不可用，语义与运行时“无会话总线/
 * 无 portal”一致——所有查询都回退 UNKNOWN，由 ui/appearance.c 的统一优先级
 * 链继续走 COLORFGBG → TERM 提示 → 暗色，绝不阻塞、不退出。
 * 各入口保持与真实实现相同的可调用契约，UI 侧无需条件编译。
 */

int appearance_portal_init(void)
{
    return 0;   /* 无 D-Bus：没有可建立的会话总线连接 */
}

int appearance_portal_read(int timeout_ms, int *bg_out)
{
    (void)timeout_ms;
    if (bg_out) {
        *bg_out = APPEARANCE_BG_UNKNOWN;
    }
    return 0;   /* 不可用/超时语义（调用方按 unknown 回退） */
}

void appearance_portal_poll(void)
{
    /* 无连接可泵，无重连可安排。 */
}

int appearance_portal_request_async_read(void)
{
    return 0;   /* 连接不可用：调用方按 unknown 回退 */
}

int appearance_portal_take_async_result(int *bg_out)
{
    (void)bg_out;
    return 0;   /* 从未发起异步读取，永远没有结果可取 */
}

int appearance_portal_take_changed(int *bg_out)
{
    (void)bg_out;
    return 0;   /* 无 SettingChanged 订阅，不会出现新值 */
}

void appearance_portal_shutdown(void)
{
    /* 无状态可清理。 */
}

int appearance_portal_is_active(void)
{
    return 0;
}

#endif /* HAVE_DBUS */
