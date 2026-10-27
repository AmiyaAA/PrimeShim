#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
primeshim_log_level_t primeshim_log_level = PRIMESHIM_LOG_WARN;

static bool text_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

bool primeshim_log_level_parse(const char *text, primeshim_log_level_t *level)
{
    primeshim_log_level_t parsed;

    if (text_equal(text, "error")) parsed = PRIMESHIM_LOG_ERROR;
    else if (text_equal(text, "warn") || text_equal(text, "warning"))
        parsed = PRIMESHIM_LOG_WARN;
    else if (text_equal(text, "info")) parsed = PRIMESHIM_LOG_INFO;
    else if (text_equal(text, "debug")) parsed = PRIMESHIM_LOG_DEBUG;
    else if (text_equal(text, "trace")) parsed = PRIMESHIM_LOG_TRACE;
    else if (text_equal(text, "off") || text_equal(text, "none"))
        parsed = PRIMESHIM_LOG_OFF;
    else return false;

    if (level) *level = parsed;
    return true;
}

const char *primeshim_log_level_name(primeshim_log_level_t level)
{
    static const char *const names[] = {
        "ERROR", "WARN", "INFO", "DEBUG", "TRACE", "OFF"
    };

    if ((unsigned)level >= sizeof(names) / sizeof(names[0])) return "UNKNOWN";
    return names[level];
}

void primeshim_log_init(void)
{
    const char *text = getenv("PRIMESHIM_LOG_LEVEL");
    primeshim_log_level_t parsed = PRIMESHIM_LOG_WARN;

    if (!text || !*text) text = getenv("PRIMESHIM_LOG");
    if (!text || !*text) {
        const char *legacy_trace = getenv("PRIMESHIM_TRACE_INPUT");
        if (legacy_trace && *legacy_trace && strcmp(legacy_trace, "0") != 0)
            parsed = PRIMESHIM_LOG_TRACE;
        primeshim_log_level = parsed;
        return;
    }

    if (primeshim_log_level_parse(text, &parsed)) {
        primeshim_log_level = parsed;
        return;
    }

    fprintf(stderr,
            "WARN  [log] invalid log level '%s'; using WARN\n", text);
    primeshim_log_level = PRIMESHIM_LOG_WARN;
}

void primeshim_log_set_level(primeshim_log_level_t level)
{
    if (level < PRIMESHIM_LOG_ERROR || level > PRIMESHIM_LOG_OFF) return;
    primeshim_log_level = level;
}

primeshim_log_level_t primeshim_log_get_level(void)
{
    return primeshim_log_level;
}

bool primeshim_log_enabled(primeshim_log_level_t level)
{
    return primeshim_log_would_write(level);
}

static long current_thread_id(void)
{
#ifdef SYS_gettid
    return syscall(SYS_gettid);
#else
    return (long)getpid();
#endif
}

static void write_prefix(primeshim_log_level_t level, const char *component)
{
    struct timespec now;
    struct tm local;
    char timestamp[32];

    if (clock_gettime(CLOCK_REALTIME, &now) == 0 &&
        localtime_r(&now.tv_sec, &local)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
        fprintf(stderr, "%s.%03ld %-5s [%-10s] [tid=%ld] ", timestamp,
                now.tv_nsec / 1000000L, primeshim_log_level_name(level),
                component ? component : "core", current_thread_id());
    } else {
        fprintf(stderr, "%-5s [%-10s] [tid=%ld] ",
                primeshim_log_level_name(level),
                component ? component : "core", current_thread_id());
    }
}

static void write_message(primeshim_log_level_t level, const char *component,
                          const char *format, va_list arguments,
                          int error_number)
{
    size_t length;

    if (!primeshim_log_enabled(level)) return;

    pthread_mutex_lock(&log_lock);
    write_prefix(level, component);
    vfprintf(stderr, format, arguments);
    if (error_number) fprintf(stderr, ": %s", strerror(error_number));
    length = format ? strlen(format) : 0;
    if (!length || format[length - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

void primeshim_log_write(primeshim_log_level_t level, const char *component,
                         const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    write_message(level, component, format, arguments, 0);
    va_end(arguments);
}

void primeshim_log_write_errno(primeshim_log_level_t level,
                               const char *component, int error_number,
                               const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    write_message(level, component, format, arguments,
                  error_number ? error_number : EINVAL);
    va_end(arguments);
}

void primeshim_log_hexdump(primeshim_log_level_t level, const char *component,
                           const void *data, size_t size, const char *label)
{
    const uint8_t *bytes = data;
    size_t offset;

    if (!primeshim_log_enabled(level)) return;
    if (!data && size) {
        primeshim_log_write(level, component, "%s: <null>",
                            label ? label : "data");
        return;
    }

    primeshim_log_write(level, component, "%s: %zu byte(s)",
                        label ? label : "data", size);
    for (offset = 0; offset < size; offset += 16) {
        char line[128];
        size_t cursor = 0;
        size_t i;

        cursor += (size_t)snprintf(line + cursor, sizeof(line) - cursor,
                                   "%04zx  ", offset);
        for (i = 0; i < 16; ++i) {
            if (offset + i < size)
                cursor += (size_t)snprintf(line + cursor,
                                           sizeof(line) - cursor, "%02x ",
                                           bytes[offset + i]);
            else
                cursor += (size_t)snprintf(line + cursor,
                                           sizeof(line) - cursor, "   ");
        }
        cursor += (size_t)snprintf(line + cursor, sizeof(line) - cursor, " ");
        for (i = 0; i < 16 && offset + i < size && cursor + 1 < sizeof(line);
             ++i) {
            unsigned char byte = bytes[offset + i];
            line[cursor++] = isprint(byte) ? (char)byte : '.';
        }
        line[cursor] = '\0';
        primeshim_log_write(level, component, "%s", line);
    }
}
