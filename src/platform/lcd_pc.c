#include "platform.h"

#if PRIMESHIM_HOST_PC

#include "host_ipc.h"
#include "input.h"
#include "log.h"
#include "peripheral.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct LCD_MAGIC {
    uint16_t SomeVal;
    uint16_t x_res;
    uint16_t y_res;
    uint16_t pixel_bits;
    uint16_t bytes_per_line;
    uint16_t brightness_level;
    uint32_t unk1_0;
    uint32_t window1_bufferstart;
};

static struct LCD_MAGIC lcd_magic;
static uint32_t *framebuffer;
static int framebuffer_fd = -1;
static int input_fd = -1;
static pthread_t input_thread;
static int input_thread_started;
static volatile int input_running;

static int fd_from_environment(const char *name)
{
    const char *text = getenv(name);
    char *end;
    long value;

    if (!text || !*text) return -1;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || *end || value < 0 || value > 0x7fffffffL) return -1;
    if (fcntl((int)value, F_GETFD) < 0) return -1;
    return (int)value;
}

static int read_event(primeshim_host_event_t *event)
{
    unsigned char *cursor = (unsigned char *)event;
    size_t remaining = sizeof(*event);

    while (remaining && input_running) {
        ssize_t count = read(input_fd, cursor, remaining);
        if (count > 0) {
            cursor += count;
            remaining -= (size_t)count;
        } else if (count == 0) {
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return remaining == 0 ? 1 : 0;
}

/* Diagnostic: make the framebuffer read-only after a delay so the first pixel
 * write after that point faults and the crash reporter prints the firmware PC
 * that performed it.  Used to locate the UI painter. */
static void *fb_trap_thread(void *unused) {
    const char *text = getenv("PRIMESHIM_TRAP_FB_MS");
    long page;
    (void)unused;

    if (!text || !*text) return NULL;
    pthread_detach(pthread_self());
    usleep((useconds_t)atol(text) * 1000);
    page = sysconf(_SC_PAGESIZE);
    if (framebuffer) {
        PS_LOG_INFO("display", "making framebuffer read-only at %p", (void *)framebuffer);
        if (mprotect(framebuffer, PRIMESHIM_FRAMEBUFFER_BYTES, PROT_READ) != 0) {
            PS_LOG_ERRNO_ERROR("display", errno, "mprotect failed");
        }
        (void)page;
    }
    return NULL;
}


static void *fb_dump_thread(void *unused) {
    const char *path = getenv("PRIMESHIM_DUMP_FB");
    int frame = 0;
    (void)unused;
    if (!path || !*path) return NULL;
    pthread_detach(pthread_self());
    while (frame < 30) {
        usleep(1000 * 1000);
        if (!framebuffer) continue;
        FILE *out = fopen(path, "wb");
        if (out) {
            fprintf(out, "P6\n%d %d\n255\n", PRIMESHIM_LCD_WIDTH, PRIMESHIM_LCD_HEIGHT);
            for (int y = 0; y < PRIMESHIM_LCD_HEIGHT; ++y) {
                for (int x = 0; x < PRIMESHIM_LCD_WIDTH; ++x) {
                    uint32_t pixel = framebuffer[y * PRIMESHIM_LCD_WIDTH + x];
                    unsigned char rgb[3] = {
                        (unsigned char)((pixel >> 16) & 0xFF),
                        (unsigned char)((pixel >> 8) & 0xFF),
                        (unsigned char)(pixel & 0xFF)
                    };
                    fwrite(rgb, 1, 3, out);
                }
            }
            fclose(out);
        }
        ++frame;
    }
    return NULL;
}

static void *host_input_thread(void *unused)
{
    primeshim_host_event_t event;
    (void)unused;
    init_touch_system();

    while (input_running && read_event(&event) > 0) {
        PS_LOG_TRACE("input", "IPC event type=%u values=%d,%d,%d",
                     event.type, event.value0, event.value1, event.value2);
        switch (event.type) {
        case PRIMESHIM_HOST_EVENT_KEY:
            enqueue_key_packet(event.value0, event.value1 != 0);
            break;
        case PRIMESHIM_HOST_EVENT_TOUCH_BEGIN:
        case PRIMESHIM_HOST_EVENT_TOUCH_MOVE:
            update_touch_slot(0, event.value0, event.value1, true);
            commit_touch_frame();
            break;
        case PRIMESHIM_HOST_EVENT_TOUCH_END:
            update_touch_slot(0, event.value0, event.value1, false);
            commit_touch_frame();
            break;
        default:
            PS_LOG_WARN("input", "ignored unknown IPC event %u", event.type);
            break;
        }
    }
    return NULL;
}

static void initialize_lcd_magic(void)
{
    lcd_magic.SomeVal = 0x5850;
    lcd_magic.x_res = PRIMESHIM_LCD_WIDTH;
    lcd_magic.y_res = PRIMESHIM_LCD_HEIGHT;
    lcd_magic.pixel_bits = 32;
    lcd_magic.bytes_per_line = PRIMESHIM_LCD_WIDTH * sizeof(uint32_t);
    lcd_magic.brightness_level = 2;
    lcd_magic.unk1_0 = 8;
    lcd_magic.window1_bufferstart = (uint32_t)(uintptr_t)framebuffer;
}

static int pc_ipc_probe(void)
{
    int available = fd_from_environment(PRIMESHIM_FRAMEBUFFER_FD_ENV) >= 0 &&
                    fd_from_environment(PRIMESHIM_INPUT_FD_ENV) >= 0;
    if (!available) {
        PS_LOG_ERROR("display",
                     "SDL IPC descriptors are absent; run `make run-pc` or "
                     "put `build/pc/primeshim-sdl` before qemu-arm");
    }
    return available;
}

static int pc_ipc_start(void)
{
    void *mapping;

    if (framebuffer) return 0;
    framebuffer_fd = fd_from_environment(PRIMESHIM_FRAMEBUFFER_FD_ENV);
    input_fd = fd_from_environment(PRIMESHIM_INPUT_FD_ENV);
    if (framebuffer_fd < 0 || input_fd < 0) return -1;

    mapping = mmap(NULL, PRIMESHIM_FRAMEBUFFER_BYTES,
                   PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer_fd, 0);
    if (mapping == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("display", errno, "framebuffer mmap failed");
        return -1;
    }
    framebuffer = mapping;
    if (getenv("PRIMESHIM_DUMP_FB")) {
        pthread_t dump_thread;
        pthread_create(&dump_thread, NULL, fb_dump_thread, NULL);
    }
    initialize_lcd_magic();

    {
        pthread_t trap_thread;
        if (pthread_create(&trap_thread, NULL, fb_trap_thread, NULL) == 0) {
            pthread_detach(trap_thread);
        }
    }

    input_running = 1;
    if (pthread_create(&input_thread, NULL, host_input_thread, NULL) != 0) {
        PS_LOG_ERRNO_ERROR("input", errno, "cannot start IPC input thread");
        input_running = 0;
        munmap(framebuffer, PRIMESHIM_FRAMEBUFFER_BYTES);
        framebuffer = NULL;
        return -1;
    }
    input_thread_started = 1;
    return 0;
}

static void pc_ipc_stop(void)
{
    input_running = 0;
    if (input_thread_started) {
        pthread_cancel(input_thread);
        pthread_join(input_thread, NULL);
        input_thread_started = 0;
    }
    if (input_fd >= 0) {
        close(input_fd);
        input_fd = -1;
    }
    if (framebuffer) {
        munmap(framebuffer, PRIMESHIM_FRAMEBUFFER_BYTES);
        framebuffer = NULL;
    }
    if (framebuffer_fd >= 0) {
        close(framebuffer_fd);
        framebuffer_fd = -1;
    }
    initialize_lcd_magic();
}

void SDKLIB_LCDOn(void)
{
    if (!framebuffer && pc_ipc_start() != 0) {
        PS_LOG_ERROR("display", "no SDL host connection; use `make run-pc`");
    }
}

void SDKLIB_LCDOff(void)
{
    /* The peripheral bus owns the shared mapping's lifetime. */
}

void SDKLIB_ClearScreen(void)
{
    if (framebuffer) memset(framebuffer, 0, PRIMESHIM_FRAMEBUFFER_BYTES);
}

struct LCD_MAGIC **SDKLIB_GetActiveLCD(void)
{
    static struct LCD_MAGIC *active = &lcd_magic;
    if (!framebuffer) SDKLIB_LCDOn();
    initialize_lcd_magic();
    return &active;
}

void LCD_drawcursor(int x, int y)
{
    (void)x;
    (void)y;
}

static const primeshim_peripheral_driver_t pc_ipc_driver = {
    .name = "pc-sdl-ipc",
    .classes = PRIMESHIM_PERIPHERAL_DISPLAY |
               PRIMESHIM_PERIPHERAL_KEYBOARD |
               PRIMESHIM_PERIPHERAL_POINTER,
    .required = true,
    .probe = pc_ipc_probe,
    .start = pc_ipc_start,
    .stop = pc_ipc_stop
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(pc_ipc_driver)

#endif
