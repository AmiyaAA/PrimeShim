#include "address_window.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include "log.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint32_t held_base;
static size_t held_size;

int primeshim_address_window_reserve(uint32_t base, size_t size) {
    void *mapped;

    if (held_size != 0) return 0;

    mapped = mmap((void *)(uintptr_t)base, size, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED) {
        PS_LOG_WARN("runtime",
                    "cannot reserve 0x%08x-0x%08x: %s (the firmware window is "
                    "already taken; the loader will report what is there)",
                    base, base + (uint32_t)size, strerror(errno));
        return -1;
    }
    if ((uintptr_t)mapped != (uintptr_t)base) {
        PS_LOG_WARN("runtime", "reservation landed at %p instead of 0x%08x",
                    mapped, base);
        munmap(mapped, size);
        return -1;
    }

    held_base = base;
    held_size = size;
    PS_LOG_INFO("runtime", "reserved firmware window 0x%08x-0x%08x",
                base, base + (uint32_t)size);
    return 0;
}

int primeshim_address_window_release(void) {
    if (held_size == 0) return -1;
    if (munmap((void *)(uintptr_t)held_base, held_size) != 0) return -1;
    PS_LOG_INFO("runtime", "gave the firmware window back to the loader");
    held_size = 0;
    return 0;
}

/* Strong definition of the core's weak hook. */
int primeshim_release_address_window(uint32_t address, size_t size) {
    if (held_size == 0) return -1;
    if (address < held_base) return -1;
    if ((uint64_t)address + size > (uint64_t)held_base + held_size) return -1;
    return primeshim_address_window_release();
}
