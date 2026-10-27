#ifndef PRIMESHIM_LOG_CAPTURE_H
#define PRIMESHIM_LOG_CAPTURE_H

#include <stddef.h>

/* Captures the runtime's stderr (log.c records and the crash reporter both
 * write there) into a fixed ring buffer so the UI can display it.  The pipe is
 * drained by a dedicated thread, so a slow reader never blocks the guest. */
int primeshim_logcapture_start(void);
void primeshim_logcapture_stop(void);

/* Copies the most recent `capacity` bytes, oldest first, NUL terminated.
 * Returns the number of bytes written. */
size_t primeshim_logcapture_snapshot(char *destination, size_t capacity);
void primeshim_logcapture_clear(void);

#endif
