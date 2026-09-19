/* 单测用的日志桩：把 logger 的四个入口变成空实现，避免为测试链接整个 logger。 */
#include <stdarg.h>

void log_debug(const char *module, const char *fmt, ...) { (void)module; (void)fmt; }
void log_info(const char *module, const char *fmt, ...)  { (void)module; (void)fmt; }
void log_warn(const char *module, const char *fmt, ...)  { (void)module; (void)fmt; }
void log_error(const char *module, const char *fmt, ...) { (void)module; (void)fmt; }
