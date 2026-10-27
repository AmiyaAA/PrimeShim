#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct pc_count {
    uint32_t pc;
    unsigned int count;
};

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

static int compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;

    return (a > b) - (a < b);
}

static int compare_count_descending(const void *left, const void *right)
{
    const struct pc_count *a = left;
    const struct pc_count *b = right;

    if (a->count != b->count)
        return (a->count < b->count) - (a->count > b->count);
    return (a->pc > b->pc) - (a->pc < b->pc);
}

static size_t compress_samples(uint32_t *samples, size_t sample_count,
                               struct pc_count *counts, uint32_t mask)
{
    size_t count_used = 0;
    size_t index;

    for (index = 0; index < sample_count; ++index)
        samples[index] &= mask;
    qsort(samples, sample_count, sizeof(*samples), compare_u32);

    for (index = 0; index < sample_count; ++index) {
        if (count_used != 0 && counts[count_used - 1].pc == samples[index]) {
            ++counts[count_used - 1].count;
        } else {
            counts[count_used].pc = samples[index];
            counts[count_used].count = 1;
            ++count_used;
        }
    }
    qsort(counts, count_used, sizeof(*counts), compare_count_descending);
    return count_used;
}

static void print_counts(const char *heading, const struct pc_count *counts,
                         size_t count_used, size_t sample_count)
{
    size_t index;

    puts(heading);
    puts("address,count,percent");
    for (index = 0; index < count_used && index < 100; ++index) {
        printf("0x%08" PRIx32 ",%u,%.3f\n", counts[index].pc,
               counts[index].count,
               100.0 * (double)counts[index].count / (double)sample_count);
    }
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
    unsigned long duration_seconds;
    unsigned long interval_us;
    size_t capacity;
    size_t sample_count = 0;
    uint32_t *samples;
    uint32_t *bucket_samples;
    struct pc_count *counts;
    struct user_regs regs;
    struct timespec delay;
    double deadline;
    pid_t target;
    int status;
    int attached = 0;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s PID DURATION_SECONDS [INTERVAL_US]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    target = (pid_t)parse_unsigned(argv[1], "PID");
    duration_seconds = parse_unsigned(argv[2], "duration");
    interval_us = argc == 4
                      ? parse_unsigned(argv[3], "sampling interval")
                      : default_interval_us;
    if (target <= 0 || duration_seconds == 0 || interval_us < 1000) {
        fputs("PID and duration must be positive; interval must be >= 1000 us\n",
              stderr);
        return EXIT_FAILURE;
    }

    capacity = (size_t)((duration_seconds * 1000000UL) / interval_us + 16);
    samples = calloc(capacity, sizeof(*samples));
    bucket_samples = calloc(capacity, sizeof(*bucket_samples));
    counts = calloc(capacity, sizeof(*counts));
    if (samples == NULL || bucket_samples == NULL || counts == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    if (ptrace(PTRACE_SEIZE, target, 0, 0) != 0) {
        perror("PTRACE_SEIZE");
        return EXIT_FAILURE;
    }
    attached = 1;

    delay.tv_sec = (time_t)(interval_us / 1000000UL);
    delay.tv_nsec = (long)(interval_us % 1000000UL) * 1000L;
    deadline = monotonic_seconds() + (double)duration_seconds;

    while (!stop_requested && monotonic_seconds() < deadline &&
           sample_count < capacity) {
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            perror("nanosleep");
            break;
        }
        if (stop_requested)
            break;
        if (ptrace(PTRACE_INTERRUPT, target, 0, 0) != 0) {
            if (errno != ESRCH)
                perror("PTRACE_INTERRUPT");
            break;
        }
        if (waitpid(target, &status, __WALL) != target) {
            if (errno != ECHILD && errno != ESRCH)
                perror("waitpid");
            attached = 0;
            break;
        }
        if (!WIFSTOPPED(status)) {
            attached = 0;
            break;
        }
        if (ptrace(PTRACE_GETREGS, target, 0, &regs) != 0) {
            perror("PTRACE_GETREGS");
            break;
        }
        samples[sample_count++] = (uint32_t)regs.uregs[15];
        if (ptrace(PTRACE_CONT, target, 0, 0) != 0) {
            if (errno != ESRCH)
                perror("PTRACE_CONT");
            attached = 0;
            break;
        }
    }

    if (attached && ptrace(PTRACE_DETACH, target, 0, 0) != 0 && errno != ESRCH)
        perror("PTRACE_DETACH");

    if (sample_count == 0) {
        fputs("no samples collected\n", stderr);
        return EXIT_FAILURE;
    }

    memcpy(bucket_samples, samples, sample_count * sizeof(*samples));
    printf("samples=%zu interval_us=%lu duration_s=%.3f\n", sample_count,
           interval_us, (double)sample_count * (double)interval_us / 1000000.0);
    print_counts("EXACT_PC", counts,
                 compress_samples(samples, sample_count, counts, UINT32_MAX),
                 sample_count);
    memset(counts, 0, sample_count * sizeof(*counts));
    print_counts("BUCKET_256", counts,
                 compress_samples(bucket_samples, sample_count, counts,
                                  UINT32_C(0xffffff00)),
                 sample_count);

    free(counts);
    free(bucket_samples);
    free(samples);
    return EXIT_SUCCESS;
}
