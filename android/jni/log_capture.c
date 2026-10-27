#include "log_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CAPTURE_BYTES (128u * 1024u)

static char ring[CAPTURE_BYTES];
static size_t ring_head;       /* next byte to overwrite */
static size_t ring_used;       /* valid bytes, saturating at CAPTURE_BYTES */
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;

static int read_fd = -1;
static int wake_fd = -1;   /* duplicate of the write end, used to unblock read() */
static int saved_stderr = -1;
static pthread_t reader;
static volatile int running;

static void append(const char *data, size_t length) {
    size_t i;

    pthread_mutex_lock(&ring_mutex);
    for (i = 0; i < length; ++i) {
        ring[ring_head] = data[i];
        ring_head = (ring_head + 1u) % CAPTURE_BYTES;
        if (ring_used < CAPTURE_BYTES) ++ring_used;
    }
    pthread_mutex_unlock(&ring_mutex);
}

static void *reader_main(void *unused) {
    char buffer[4096];
    (void)unused;

    while (running) {
        ssize_t count = read(read_fd, buffer, sizeof(buffer));
        if (count > 0) {
            append(buffer, (size_t)count);
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    return NULL;
}

int primeshim_logcapture_start(void) {
    int fds[2];

    if (read_fd >= 0) return 0;
    if (pipe(fds) != 0) return -1;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (dup2(fds[1], STDERR_FILENO) < 0) {
        close(saved_stderr);
        saved_stderr = -1;
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    wake_fd = dup(fds[1]);
    close(fds[1]);
    read_fd = fds[0];
    running = 1;
    if (pthread_create(&reader, NULL, reader_main, NULL) != 0) {
        running = 0;
        if (wake_fd >= 0) close(wake_fd);
        wake_fd = -1;
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
        saved_stderr = -1;
        close(read_fd);
        read_fd = -1;
        return -1;
    }
    return 0;
}

void primeshim_logcapture_stop(void) {
    if (read_fd < 0) return;
    running = 0;
    /* bionic does not declare pthread_cancel without feature macros; a single
     * byte on the write side wakes the blocked read() instead. */
    if (wake_fd >= 0) {
        ssize_t ignored = write(wake_fd, "\n", 1);
        (void)ignored;
    }
    pthread_join(reader, NULL);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    saved_stderr = -1;
    close(read_fd);
    read_fd = -1;
}

size_t primeshim_logcapture_snapshot(char *destination, size_t capacity) {
    size_t start;
    size_t first;
    size_t count;

    if (!destination || capacity == 0) return 0;
    pthread_mutex_lock(&ring_mutex);
    count = ring_used < capacity - 1 ? ring_used : capacity - 1;
    start = (ring_head + CAPTURE_BYTES - count) % CAPTURE_BYTES;
    first = CAPTURE_BYTES - start;
    if (first > count) first = count;
    memcpy(destination, ring + start, first);
    if (count > first) memcpy(destination + first, ring, count - first);
    pthread_mutex_unlock(&ring_mutex);
    destination[count] = '\0';
    return count;
}

void primeshim_logcapture_clear(void) {
    pthread_mutex_lock(&ring_mutex);
    ring_head = 0;
    ring_used = 0;
    pthread_mutex_unlock(&ring_mutex);
}
