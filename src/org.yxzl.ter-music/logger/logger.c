#include "logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static FILE *g_log_file = NULL;
static int g_log_enabled = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char path[256];
    strftime(path, sizeof(path), "./ter-music-debug-%Y-%m-%d-%H%M%S.log", t);

    pthread_mutex_lock(&g_log_mutex);
    g_log_file = fopen(path, "a");
    if (g_log_file) {
        setvbuf(g_log_file, NULL, _IONBF, 0);
        g_log_enabled = 1;
    }
    pthread_mutex_unlock(&g_log_mutex);

    if (g_log_enabled) {
        pthread_mutex_lock(&g_log_mutex);
        g_log_enabled = 1;
        fprintf(g_log_file, "=== Logging started ===\n");
        fflush(g_log_file);
        pthread_mutex_unlock(&g_log_mutex);
    }
}

void logger_shutdown(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file && g_log_enabled) {
        fprintf(g_log_file, "=== Logging stopped ===\n");
        fflush(g_log_file);
    }
    g_log_enabled = 0;
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void logger_set_enabled(int enabled) {
    pthread_mutex_lock(&g_log_mutex);
    g_log_enabled = enabled;
    pthread_mutex_unlock(&g_log_mutex);
}

int logger_is_enabled(void) {
    int enabled;
    pthread_mutex_lock(&g_log_mutex);
    enabled = g_log_enabled;
    pthread_mutex_unlock(&g_log_mutex);
    return enabled;
}

static void log_write(LogLevel level, const char *module, const char *fmt, va_list args) {
    if (!fmt || !module) return;

    pthread_mutex_lock(&g_log_mutex);
    if (!g_log_file || !g_log_enabled) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *tm_local = localtime(&ts.tv_sec);

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_local);
    int ms = (int)(ts.tv_nsec / 1000000L);

    fprintf(g_log_file, "[%s.%03d] ", time_buf, ms);

    switch (level) {
        case LOG_LEVEL_DEBUG: fprintf(g_log_file, "[DEBUG] "); break;
        case LOG_LEVEL_INFO:  fprintf(g_log_file, "[INFO]  "); break;
        case LOG_LEVEL_WARN:  fprintf(g_log_file, "[WARN]  "); break;
        case LOG_LEVEL_ERROR: fprintf(g_log_file, "[ERROR] "); break;
        default:              fprintf(g_log_file, "[?]     "); break;
    }

    fprintf(g_log_file, "[%s] ", module);
    vfprintf(g_log_file, fmt, args);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);

    pthread_mutex_unlock(&g_log_mutex);
}

void log_debug(const char *module, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LOG_LEVEL_DEBUG, module, fmt, args);
    va_end(args);
}

void log_info(const char *module, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LOG_LEVEL_INFO, module, fmt, args);
    va_end(args);
}

void log_warn(const char *module, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LOG_LEVEL_WARN, module, fmt, args);
    va_end(args);
}

void log_error(const char *module, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write(LOG_LEVEL_ERROR, module, fmt, args);
    va_end(args);
}
