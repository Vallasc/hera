#ifndef HERA_LOG_H
#define HERA_LOG_H

#include <stdarg.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} log_level_t;

/* Verbosity uses the protocol's own scale (PROTOCOL.md §9, CMD LOG_LEVEL):
 * 0 = off, 1 = errors, 2 = info, 3 = debug. */
void log_set_level(int proto_level);       /* stderr verbosity */

/* Mirror log lines into LOG frames (0x30) for the connected client. The sink
 * is called from whichever thread logged, so it must be thread-safe and must
 * never log itself. `proto_level` starts at 1 (errors only), per §9. */
typedef void (*log_sink_fn)(void *ud, const char *line);
void log_set_sink(log_sink_fn fn, void *ud);
void log_set_sink_level(int proto_level);

/* Thread-safe formatted logging to stderr with a timestamp and level tag. */
void log_msg(log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define log_debug(tag, ...) log_msg(LOG_DEBUG, (tag), __VA_ARGS__)
#define log_info(tag, ...)  log_msg(LOG_INFO,  (tag), __VA_ARGS__)
#define log_warn(tag, ...)  log_msg(LOG_WARN,  (tag), __VA_ARGS__)
#define log_error(tag, ...) log_msg(LOG_ERROR, (tag), __VA_ARGS__)

#endif /* HERA_LOG_H */
