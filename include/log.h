#ifndef PRIMESHIM_LOG_H
#define PRIMESHIM_LOG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum primeshim_log_level {
    PRIMESHIM_LOG_ERROR = 0,
    PRIMESHIM_LOG_WARN,
    PRIMESHIM_LOG_INFO,
    PRIMESHIM_LOG_DEBUG,
    PRIMESHIM_LOG_TRACE,
    PRIMESHIM_LOG_OFF
} primeshim_log_level_t;

/* The default level is WARN. PRIMESHIM_LOG_LEVEL (or PRIMESHIM_LOG) may be
 * set to error, warn, info, debug, trace, or off. */
void primeshim_log_init(void);
bool primeshim_log_level_parse(const char *text, primeshim_log_level_t *level);
const char *primeshim_log_level_name(primeshim_log_level_t level);
void primeshim_log_set_level(primeshim_log_level_t level);
primeshim_log_level_t primeshim_log_get_level(void);
bool primeshim_log_enabled(primeshim_log_level_t level);

/* Configured once during startup, before worker threads are created. */
extern primeshim_log_level_t primeshim_log_level;

static inline bool primeshim_log_would_write(primeshim_log_level_t level)
{
    return primeshim_log_level != PRIMESHIM_LOG_OFF &&
           level >= PRIMESHIM_LOG_ERROR && level < PRIMESHIM_LOG_OFF &&
           level <= primeshim_log_level;
}

void primeshim_log_write(primeshim_log_level_t level, const char *component,
                         const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void primeshim_log_write_errno(primeshim_log_level_t level,
                               const char *component, int error_number,
                               const char *format, ...)
    __attribute__((format(printf, 4, 5)));
void primeshim_log_hexdump(primeshim_log_level_t level, const char *component,
                           const void *data, size_t size, const char *label);

#define PS_LOG_AT(level, component, ...)                                    \
    do {                                                                    \
        if (__builtin_expect(primeshim_log_would_write((level)), 0))        \
            primeshim_log_write((level), (component), __VA_ARGS__);         \
    } while (0)

#define PS_LOG_ERROR(component, ...)                                        \
    PS_LOG_AT(PRIMESHIM_LOG_ERROR, (component), __VA_ARGS__)
#define PS_LOG_WARN(component, ...)                                         \
    PS_LOG_AT(PRIMESHIM_LOG_WARN, (component), __VA_ARGS__)
#define PS_LOG_INFO(component, ...)                                         \
    PS_LOG_AT(PRIMESHIM_LOG_INFO, (component), __VA_ARGS__)
#define PS_LOG_DEBUG(component, ...)                                        \
    PS_LOG_AT(PRIMESHIM_LOG_DEBUG, (component), __VA_ARGS__)
#define PS_LOG_TRACE(component, ...)                                        \
    PS_LOG_AT(PRIMESHIM_LOG_TRACE, (component), __VA_ARGS__)

#define PS_LOG_ERRNO_AT(level, component, error_number, ...)                \
    do {                                                                    \
        if (__builtin_expect(primeshim_log_would_write((level)), 0))        \
            primeshim_log_write_errno((level), (component), (error_number), \
                                      __VA_ARGS__);                          \
    } while (0)

#define PS_LOG_ERRNO_ERROR(component, error_number, ...)                    \
    PS_LOG_ERRNO_AT(PRIMESHIM_LOG_ERROR, (component), (error_number),       \
                    __VA_ARGS__)
#define PS_LOG_ERRNO_WARN(component, error_number, ...)                     \
    PS_LOG_ERRNO_AT(PRIMESHIM_LOG_WARN, (component), (error_number),        \
                    __VA_ARGS__)
#define PS_LOG_ERRNO_DEBUG(component, error_number, ...)                    \
    PS_LOG_ERRNO_AT(PRIMESHIM_LOG_DEBUG, (component), (error_number),       \
                    __VA_ARGS__)

#endif
