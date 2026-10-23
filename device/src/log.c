#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Protocol level -> minimum log_level_t that is emitted. Level 0 maps past
 * LOG_ERROR so nothing passes. */
static const int LEVEL_MIN[4] = { LOG_ERROR + 1, LOG_WARN, LOG_INFO, LOG_DEBUG };

static int g_min       = LOG_INFO;
static int g_sink_min  = LOG_WARN;   /* protocol default is 1 = errors */
static log_sink_fn g_sink;
static void *g_sink_ud;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_tag(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "?";
    }
}

static int clamp_proto(int p)
{
    return p < 0 ? 0 : (p > 3 ? 3 : p);
}

void log_set_level(int proto_level)
{
    __atomic_store_n(&g_min, LEVEL_MIN[clamp_proto(proto_level)],
                     __ATOMIC_RELAXED);
}

void log_set_sink(log_sink_fn fn, void *ud)
{
    pthread_mutex_lock(&g_lock);
    g_sink = fn;
    g_sink_ud = ud;
    pthread_mutex_unlock(&g_lock);
}

void log_set_sink_level(int proto_level)
{
    __atomic_store_n(&g_sink_min, LEVEL_MIN[clamp_proto(proto_level)],
                     __ATOMIC_RELAXED);
}

void log_msg(log_level_t level, const char *tag, const char *fmt, ...)
{
    pthread_mutex_lock(&g_lock);
    int has_sink = g_sink != NULL;
    pthread_mutex_unlock(&g_lock);
    int to_stderr = (int)level >= __atomic_load_n(&g_min, __ATOMIC_RELAXED);
    int to_sink   = has_sink &&
                    (int)level >= __atomic_load_n(&g_sink_min, __ATOMIC_RELAXED);
    if (!to_stderr && !to_sink)
        return;

    /* LOG payloads are limited to 256 bytes (§5.2). */
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    if (to_stderr) {
        char ts[16];
        struct tm tm;
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

        pthread_mutex_lock(&g_lock);
        fprintf(stderr, "[%s] %-5s %s: %s\n", ts, level_tag(level),
                tag ? tag : "-", body);
        fflush(stderr);
        pthread_mutex_unlock(&g_lock);
    }

    if (to_sink) {
        /* Level (5), space, tag (at most 63), ": ", body, NUL. The sink
         * truncates to the LOG payload limit. */
        char line[5 + 1 + 63 + 2 + sizeof body + 1];
        snprintf(line, sizeof(line), "%s %.63s: %s", level_tag(level),
                 tag ? tag : "-", body);
        /* Call the sink outside the lock. */
        pthread_mutex_lock(&g_lock);
        log_sink_fn fn = g_sink;
        void *ud = g_sink_ud;
        pthread_mutex_unlock(&g_lock);
        if (fn)
            fn(ud, line);
    }
}
