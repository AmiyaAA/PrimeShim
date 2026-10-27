#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int read_process_stat(const char *path, char *state,
                             unsigned long *utime, unsigned long *stime)
{
    char buffer[1024];
    char *close_parenthesis;
    char *cursor;
    char *save;
    char *token;
    int field = 4;
    int fd;
    ssize_t length;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    length = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (length <= 0) {
        if (length == 0)
            errno = ESRCH;
        return -1;
    }
    buffer[length] = '\0';
    close_parenthesis = strrchr(buffer, ')');
    if (close_parenthesis == NULL || close_parenthesis[1] != ' ' ||
        close_parenthesis[2] == '\0' || close_parenthesis[3] != ' ') {
        errno = EINVAL;
        return -1;
    }
    *state = close_parenthesis[2];
    cursor = close_parenthesis + 4;
    save = NULL;
    for (token = strtok_r(cursor, " ", &save); token != NULL;
         token = strtok_r(NULL, " ", &save), ++field) {
        if (field == 14)
            *utime = strtoul(token, NULL, 10);
        else if (field == 15) {
            *stime = strtoul(token, NULL, 10);
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static unsigned long parse_unsigned(const char *text, const char *name)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return value;
}

int main(int argc, char **argv)
{
    const unsigned long default_interval_us = 20000;
    unsigned long interval_us;
    struct timespec delay;
    unsigned long duration;
    unsigned long utime = 0;
    unsigned long stime = 0;
    unsigned long last_utime = 0;
    unsigned long last_stime = 0;
    char path[64];
    char state = '?';
    char last_state = '?';
    double started;
    double last_report = -1.0;
    pid_t target;

    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "usage: %s PID DURATION_SECONDS [INTERVAL_US]\n", argv[0]);
        return EXIT_FAILURE;
    }
    target = (pid_t)parse_unsigned(argv[1], "PID");
    duration = parse_unsigned(argv[2], "duration");
    interval_us = argc == 4
                      ? parse_unsigned(argv[3], "sampling interval")
                      : default_interval_us;
    if (target <= 0 || duration == 0 || interval_us < 1000) {
        fputs("PID and duration must be positive; interval must be >= 1000 us\n",
              stderr);
        return EXIT_FAILURE;
    }
    delay.tv_sec = (time_t)(interval_us / 1000000UL);
    delay.tv_nsec = (long)(interval_us % 1000000UL) * 1000L;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)target) >=
        (int)sizeof(path)) {
        fputs("PID is too long\n", stderr);
        return EXIT_FAILURE;
    }

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    started = monotonic_seconds();
    puts("elapsed_s,state,utime_ticks,stime_ticks");

    while (!stop_requested) {
        double elapsed = monotonic_seconds() - started;

        if (elapsed >= (double)duration)
            break;
        if (read_process_stat(path, &state, &utime, &stime) != 0) {
            if (errno != ENOENT && errno != ESRCH)
                perror("read process stat");
            break;
        }
        if (state != last_state || elapsed - last_report >= 1.0 ||
            utime < last_utime || stime < last_stime) {
            printf("%.3f,%c,%lu,%lu\n", elapsed, state, utime, stime);
            fflush(stdout);
            last_state = state;
            last_report = elapsed;
            last_utime = utime;
            last_stime = stime;
        }
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            perror("nanosleep");
            break;
        }
    }
    return EXIT_SUCCESS;
}
