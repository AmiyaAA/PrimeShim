#ifndef PRIMESHIM_HOST_IPC_H
#define PRIMESHIM_HOST_IPC_H

#include <stdint.h>

#define PRIMESHIM_LCD_WIDTH 320
#define PRIMESHIM_LCD_HEIGHT 240
#define PRIMESHIM_FRAMEBUFFER_BYTES \
    (PRIMESHIM_LCD_WIDTH * PRIMESHIM_LCD_HEIGHT * sizeof(uint32_t))

#define PRIMESHIM_FRAMEBUFFER_FD_ENV "PRIMESHIM_FRAMEBUFFER_FD"
#define PRIMESHIM_INPUT_FD_ENV "PRIMESHIM_INPUT_FD"

enum primeshim_host_event_type {
    PRIMESHIM_HOST_EVENT_KEY = 1,
    PRIMESHIM_HOST_EVENT_TOUCH_BEGIN,
    PRIMESHIM_HOST_EVENT_TOUCH_MOVE,
    PRIMESHIM_HOST_EVENT_TOUCH_END
};

typedef struct primeshim_host_event {
    uint32_t type;
    int32_t value0;
    int32_t value1;
    int32_t value2;
} primeshim_host_event_t;

_Static_assert(sizeof(primeshim_host_event_t) == 16,
               "host IPC event ABI must remain 16 bytes");

#endif
