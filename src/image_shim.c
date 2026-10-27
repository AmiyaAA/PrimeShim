/*
 * Image / blit syscalls.
 *
 * Every image entry in tools/interruptmap.txt is a weak stub today, which means
 * the firmware's text and icon drawing silently disappears: glyphs are blitted
 * as small bitmaps, they are not drawn as vectors.  This module records the
 * call arguments so the real ABI can be recovered from an actual run.
 *
 * Set PRIMESHIM_TRACE_IMAGE=1 (or use --log-level trace) to see the calls.
 */
#include "log.h"

#include <stdint.h>
#include <stdlib.h>

static int tracing(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("PRIMESHIM_TRACE_IMAGE");
        enabled = (value && *value && value[0] != '0') ? 1 : 0;
    }
    return enabled;
}

#define TRACE_IMAGE(...)                                                    \
    do {                                                                    \
        if (tracing()) PS_LOG_INFO("image", __VA_ARGS__);                   \
    } while (0)

uint32_t SDKLIB__BitBlt(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                        uint32_t e, uint32_t f) {
    TRACE_IMAGE("_BitBlt(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", a, b, c, d, e, f);
    return 0;
}

uint32_t SDKLIB_PutImage(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                         uint32_t e, uint32_t f) {
    TRACE_IMAGE("PutImage(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", a, b, c, d, e, f);
    return 0;
}

uint32_t SDKLIB_GetImage(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    TRACE_IMAGE("GetImage(0x%x, 0x%x, 0x%x, 0x%x)", a, b, c, d);
    return 0;
}

uint32_t SDKLIB_ImageData(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    TRACE_IMAGE("ImageData(0x%x, 0x%x, 0x%x, 0x%x)", a, b, c, d);
    return 0;
}

uint32_t SDKLIB_CreateCompatibleImage(uint32_t a, uint32_t b, uint32_t c,
                                      uint32_t d) {
    TRACE_IMAGE("CreateCompatibleImage(0x%x, 0x%x, 0x%x, 0x%x)", a, b, c, d);
    return 0;
}

uint32_t SDKLIB_SetPixel(uint32_t a, uint32_t b, uint32_t c) {
    TRACE_IMAGE("SetPixel(0x%x, 0x%x, 0x%x)", a, b, c);
    return 0;
}

uint32_t SDKLIB_GetPixel(uint32_t a, uint32_t b) {
    TRACE_IMAGE("GetPixel(0x%x, 0x%x)", a, b);
    return 0;
}
