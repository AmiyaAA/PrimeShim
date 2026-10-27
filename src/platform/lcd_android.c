/*
 * Android display + input backend.
 *
 * The firmware draws straight into a plain RAM buffer that we hand it through
 * the LCD_MAGIC structure (same contract as the PC/SDL backend).  A compose
 * thread scales that 320x240 ARGB buffer into the ANativeWindow, while the
 * Java side forwards touch and key events through JNI.
 *
 * Unlike the desktop profile there is no second process and no shared memory
 * file: Android denies PROT_EXEC mappings backed by app-writable files, so
 * keeping everything in one address space is both simpler and required.
 */
#include "platform.h"

#if PRIMESHIM_HOST_ANDROID

#include "host_ipc.h"
#include "input.h"
#include "log.h"
#include "peripheral.h"
#include "primeshim_android.h"

#include <android/native_window.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Must match the layout the firmware expects at the LCD register block. */
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

#define LCD_PIXELS (PRIMESHIM_LCD_WIDTH * PRIMESHIM_LCD_HEIGHT)

static struct LCD_MAGIC lcd_magic;
static uint32_t framebuffer[LCD_PIXELS];
static uint32_t shadow[LCD_PIXELS];

static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_cond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *window;
/* Emulation speed in percent; 100 matches the previous cadence. */
static volatile int speed_percent = 100;

void primeshim_set_speed_percent(int percent) {
    if (percent < 10) percent = 10;
    if (percent > 400) percent = 400;
    speed_percent = percent;
}

int primeshim_get_speed_percent(void) { return speed_percent; }
static volatile int redraw_requested = 1;

static pthread_t compose_thread;
static int compose_thread_started;
static volatile int compose_running;
static int first_frame_logged;

/* Cached nearest-neighbour index tables for the current surface size. */
static int *col_index;
static int *row_index;
static int table_width;
static int table_height;

static void initialize_lcd_magic(void) {
    lcd_magic.SomeVal = 0x5850;
    lcd_magic.x_res = PRIMESHIM_LCD_WIDTH;
    lcd_magic.y_res = PRIMESHIM_LCD_HEIGHT;
    lcd_magic.pixel_bits = 32;
    lcd_magic.bytes_per_line = PRIMESHIM_LCD_WIDTH * sizeof(uint32_t);
    lcd_magic.brightness_level = 2;
    lcd_magic.unk1_0 = 8;
    lcd_magic.window1_bufferstart = (uint32_t)(uintptr_t)framebuffer;
}

/* Map surface pixels to firmware pixels.  The image is scaled to the surface
 * width (keeping 4:3) instead of an integer factor: a 480-pixel-wide phone
 * otherwise shows the 320x240 screen at 1:1, where the firmware's 6-pixel UI
 * font is unreadable.  Margins are painted black. */
static int build_tables(int width, int height) {
    int dst_w, dst_h, off_x, off_y, x, y;
    int *cols, *rows;

    if (width <= 0 || height <= 0) return -1;
    if (width == table_width && height == table_height && col_index && row_index) return 0;

    dst_w = width;
    dst_h = (width * PRIMESHIM_LCD_HEIGHT) / PRIMESHIM_LCD_WIDTH;
    if (dst_h > height) {
        dst_h = height;
        dst_w = (height * PRIMESHIM_LCD_WIDTH) / PRIMESHIM_LCD_HEIGHT;
    }
    if (dst_w < 1 || dst_h < 1) return -1;
    off_x = (width - dst_w) / 2;
    off_y = (height - dst_h) / 2;

    cols = malloc(sizeof(int) * (size_t)width);
    rows = malloc(sizeof(int) * (size_t)height);
    if (!cols || !rows) {
        free(cols);
        free(rows);
        return -1;
    }

    for (x = 0; x < width; ++x) {
        cols[x] = (x < off_x || x >= off_x + dst_w)
                      ? -1
                      : (x - off_x) * PRIMESHIM_LCD_WIDTH / dst_w;
    }
    for (y = 0; y < height; ++y) {
        rows[y] = (y < off_y || y >= off_y + dst_h)
                      ? -1
                      : (y - off_y) * PRIMESHIM_LCD_HEIGHT / dst_h;
    }

    free(col_index);
    free(row_index);
    col_index = cols;
    row_index = rows;
    table_width = width;
    table_height = height;
    PS_LOG_INFO("display", "will draw 320x240 into %dx%d at +%d+%d",
                dst_w, dst_h, off_x, off_y);
    return 0;
}

/* Firmware stores 0x00RRGGBB; the window wants byte order R,G,B,X. */
static inline uint32_t to_window_pixel(uint32_t pixel) {
    uint32_t r = (pixel >> 16) & 0xffu;
    uint32_t g = (pixel >> 8) & 0xffu;
    uint32_t b = pixel & 0xffu;
    return (0xffu << 24) | (b << 16) | (g << 8) | r;
}

/* The firmware paints the framebuffer in place; copying it row by row straight
 * into the window buffer produced half-updated frames, so take a snapshot. */
static uint32_t snapshot_pixels[PRIMESHIM_LCD_WIDTH * PRIMESHIM_LCD_HEIGHT];

static void compose(ANativeWindow *target) {
    ANativeWindow_Buffer buffer;
    int width, height, x, y;
    uint32_t *dst;

    width = ANativeWindow_getWidth(target);
    height = ANativeWindow_getHeight(target);
    if (build_tables(width, height) != 0) return;
    memcpy(snapshot_pixels, framebuffer, sizeof(snapshot_pixels));
    if (ANativeWindow_lock(target, &buffer, NULL) != 0) return;

    if (buffer.width != table_width || buffer.height != table_height) {
        /* Geometry changed between the tables and the lock; rebuild next tick. */
        ANativeWindow_unlockAndPost(target);
        table_width = 0;
        table_height = 0;
        return;
    }

    if (buffer.width <= 0 || buffer.height <= 0 || buffer.bits == NULL) {
        PS_LOG_WARN("display", "window buffer has no pixels (%dx%d)",
                    (int)buffer.width, (int)buffer.height);
        ANativeWindow_unlockAndPost(target);
        return;
    }

    dst = buffer.bits;
    for (y = 0; y < height; ++y) {
        uint32_t *line = (uint32_t *)((uint8_t *)dst + (size_t)y * (size_t)buffer.stride * 4u);
        int src_row = row_index[y];
        if (src_row < 0) {
            memset(line, 0, (size_t)width * 4u);
            continue;
        }
        for (x = 0; x < width; ++x) {
            int src_col = col_index[x];
            line[x] = src_col < 0
                          ? 0xff000000u
                          : to_window_pixel(snapshot_pixels[src_row * PRIMESHIM_LCD_WIDTH + src_col]);
        }
    }

    ANativeWindow_unlockAndPost(target);
}

static int frame_changed(void) {
    if (redraw_requested) return 1;
    return memcmp(shadow, framebuffer, sizeof(framebuffer)) != 0;
}

static void *compose_main(void *unused) {
    (void)unused;

    while (compose_running) {
        ANativeWindow *target;
        int changed;
        struct timespec deadline;

        pthread_mutex_lock(&window_lock);
        while (compose_running && !window) {
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&window_cond, &window_lock, &deadline);
        }
        if (!compose_running) {
            pthread_mutex_unlock(&window_lock);
            break;
        }
        target = window;
        ANativeWindow_acquire(target);
        changed = frame_changed();
        pthread_mutex_unlock(&window_lock);

        if (changed) {
            if (!first_frame_logged) {
                first_frame_logged = 1;
                PS_LOG_INFO("display", "composing first frame into %dx%d surface",
                            (int)ANativeWindow_getWidth(target),
                            (int)ANativeWindow_getHeight(target));
            }
            {
                struct timespec settle = {0, 12 * 1000 * 1000};
                nanosleep(&settle, NULL);   /* let the firmware finish the frame */
            }
            compose(target);
            memcpy(shadow, framebuffer, sizeof(shadow));
            redraw_requested = 0;
        }
        ANativeWindow_release(target);

        /* ~30 Hz cadence; wakes early when a surface change or stop arrives. */
        /* No artificial frame delay: the loop blocks until a redraw is requested
         * and only uses a short timeout as a safety net. */
        int period_ms = 2 * 100 / (speed_percent > 0 ? speed_percent : 100);
        if (period_ms < 2) period_ms = 2;
        if (period_ms > 200) period_ms = 200;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += (long) period_ms * 1000 * 1000;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&window_lock);
        while (compose_running) {
            if (pthread_cond_timedwait(&window_cond, &window_lock, &deadline) == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&window_lock);
    }
    return NULL;
}

void primeshim_android_set_window(ANativeWindow *new_window) {
    ANativeWindow *old;

    if (new_window) ANativeWindow_acquire(new_window);

    pthread_mutex_lock(&window_lock);
    old = window;
    window = new_window;
    table_width = 0;
    table_height = 0;
    redraw_requested = 1;
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);

    if (old) ANativeWindow_release(old);

    if (new_window) {
        int32_t w = ANativeWindow_getWidth(new_window);
        int32_t h = ANativeWindow_getHeight(new_window);
        if (w > 0 && h > 0) {
            ANativeWindow_setBuffersGeometry(new_window, w, h, WINDOW_FORMAT_RGBA_8888);
        }
        PS_LOG_INFO("display", "surface attached (%dx%d)", (int)w, (int)h);
    } else {
        PS_LOG_INFO("display", "surface detached");
    }
}

void primeshim_android_snapshot(uint32_t *destination) {
    size_t i;

    if (!destination) return;
    /* The firmware stores 0x00RRGGBB: the high byte is zero, and a bitmap made
     * from that would be fully transparent (an "all black" export). */
    for (i = 0; i < LCD_PIXELS; ++i) {
        destination[i] = framebuffer[i] | 0xff000000u;
    }
}

void primeshim_android_request_redraw(void) {
    redraw_requested = 1;
    pthread_mutex_lock(&window_lock);
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);
}

/* ---- input ---------------------------------------------------------- */

void primeshim_android_touch_update(int action, int slot, int x, int y) {
    if (slot < 0 || slot >= 8) return;
    if (x < 0 || x >= PRIMESHIM_LCD_WIDTH || y < 0 || y >= PRIMESHIM_LCD_HEIGHT) return;
    switch (action) {
    case PRIMESHIM_TOUCH_DOWN:
    case PRIMESHIM_TOUCH_MOVE:
        update_touch_slot(slot, x, y, true);
        break;
    case PRIMESHIM_TOUCH_UP:
        update_touch_slot(slot, x, y, false);
        break;
    default:
        break;
    }
}

void primeshim_android_touch_commit(void) {
    commit_touch_frame();
}

void primeshim_android_touch(int action, int x, int y) {
    primeshim_android_touch_update(action, 0, x, y);
    primeshim_android_touch_commit();
}

void primeshim_android_key(int key, int pressed) {
    if (key <= 0) return;
    enqueue_key_packet(key, pressed != 0);
}

/* ---- firmware LCD entry points -------------------------------------- */

void SDKLIB_LCDOn(void) {
    PS_LOG_DEBUG("display", "LCD on");
    primeshim_android_request_redraw();
}

void SDKLIB_LCDOff(void) {
    PS_LOG_DEBUG("display", "LCD off");
}

void SDKLIB_ClearScreen(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
}

struct LCD_MAGIC **SDKLIB_GetActiveLCD(void) {
    static struct LCD_MAGIC *active = &lcd_magic;
    initialize_lcd_magic();
    return &active;
}

void LCD_drawcursor(int x, int y) {
    (void)x;
    (void)y;
}

/* ---- peripheral driver ---------------------------------------------- */

static int android_display_probe(void) {
    /* The surface may arrive after start(); the compose thread idles until
     * Java hands us one. */
    return 1;
}

static int android_display_start(void) {
    if (compose_thread_started) return 0;
    initialize_lcd_magic();
    compose_running = 1;
    if (pthread_create(&compose_thread, NULL, compose_main, NULL) != 0) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot start compose thread");
        compose_running = 0;
        return -1;
    }
    compose_thread_started = 1;
    return 0;
}

static void android_display_stop(void) {
    compose_running = 0;
    pthread_mutex_lock(&window_lock);
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);
    if (compose_thread_started) {
        pthread_join(compose_thread, NULL);
        compose_thread_started = 0;
    }
}

static const primeshim_peripheral_driver_t android_display_driver = {
    .name = "android-surface",
    .classes = PRIMESHIM_PERIPHERAL_DISPLAY |
               PRIMESHIM_PERIPHERAL_KEYBOARD |
               PRIMESHIM_PERIPHERAL_POINTER,
    .required = true,
    .probe = android_display_probe,
    .start = android_display_start,
    .stop = android_display_stop
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(android_display_driver)

#endif /* PRIMESHIM_HOST_ANDROID */
