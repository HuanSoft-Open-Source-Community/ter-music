#ifndef LOGGER_H
#define LOGGER_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} LogLevel;

void logger_init(void);
void logger_shutdown(void);

void logger_set_enabled(int enabled);
int  logger_is_enabled(void);

/* 调试日志文件的底层 fd（--debug 且日志可用时），否则返回 -1。
 * 供 stderr 守卫（ui.c）把 fd2 改道到调试日志使用，调用方无需
 * 直接访问内部的 g_log_file。 */
int  logger_output_fd(void);

void log_debug(const char *module, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_info(const char *module, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_warn(const char *module, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_error(const char *module, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
