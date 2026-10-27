#include <pthread.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "ui.h"
#include "platform.h"
#include "input.h"
#include "peripheral.h"

void LCD_drawcursor(int x, int y) ;
void EnqueuePacket(ui_event_prime_s* packet);
// ================= CONFIGURATION =================
#define TOUCH_DEV_PATH PRIMESHIM_TOUCH_DEVICE
#define MAX_TOUCH_SLOTS 8       // Can be adjusted, up to protocol max
// =================================================

// Global variables for cursor position (Tracks Finger/Slot 0 for visual feedback)
static int cursor_x = 0;
static int cursor_y = 0;
static pthread_mutex_t cursor_mutex = PTHREAD_MUTEX_INITIALIZER;


// --- NEW PROTOCOL STATE MACHINE ---
typedef struct {
    bool active;        // Is finger currently on screen?
    bool just_pressed;  // Did it appear this frame?
    bool just_released; // Did it vanish this frame?
    int x;
    int y;
} TouchSlot;

static TouchSlot slots[MAX_TOUCH_SLOTS];
static pthread_mutex_t touch_state_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize slots
void init_touch_system() {
    pthread_mutex_lock(&touch_state_mutex);
    memset(slots, 0, sizeof(slots));
    pthread_mutex_unlock(&touch_state_mutex);
}

// API for Input Threads to update state
void update_touch_slot(int slot_id, int x, int y, bool is_down) {
    if (slot_id < 0 || slot_id >= MAX_TOUCH_SLOTS) return;

    pthread_mutex_lock(&touch_state_mutex);

    TouchSlot* s = &slots[slot_id];
    s->x = x;
    s->y = y;

    if (is_down) {
        if (!s->active) {
            // New touch detected
            s->active = true;
            s->just_pressed = true;
            s->just_released = false;
        }
    } else { // is_down is false
        if (s->active) {
            // Finger was lifted
            s->just_released = true;
            // The final coordinates are preserved for the lift event
        }
    }

    pthread_mutex_unlock(&touch_state_mutex);
}


// Processes the current state and sends a single packet to the UI
void commit_touch_frame() {
    pthread_mutex_lock(&touch_state_mutex);

    ui_event_prime_s packet;
    memset(&packet, 0, sizeof(packet));
    packet.event_type = UI_EVENT_TYPE_TOUCH;

    int event_idx = 0;
    bool has_updates = false;

    // Iterate over ALL slots to build the event packet
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        if (!slots[i].active && !slots[i].just_released) continue;

        // Stop if the packet is full
        if (event_idx >= (sizeof(packet.multipress_events) / sizeof(UIMultipressEvent))) break;

        UIMultipressEvent* evt = &packet.multipress_events[event_idx];
        evt->finger_id = (unsigned short)i;
        evt->touch_x = slots[i].x;
        evt->touch_y = slots[i].y;

        if (slots[i].just_pressed) {
            evt->type = UI_EVENT_TYPE_TOUCH_BEGIN;
        } else if (slots[i].just_released) {
            evt->type = UI_EVENT_TYPE_TOUCH_END;
        } else { // It's an active, ongoing touch
            evt->type = UI_EVENT_TYPE_TOUCH_MOVE;
        }
        has_updates = true;
        event_idx++;
    }

    packet.available_multipress_events = event_idx;

    // Clean up state for the *next* frame
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        slots[i].just_pressed = false;
        if (slots[i].just_released) {
            slots[i].just_released = false;
            slots[i].active = false; // The slot is now fully inactive
        }
    }

    pthread_mutex_unlock(&touch_state_mutex);

    // Only enqueue if there was at least one active or state-changing finger
    if (has_updates) {
        if (primeshim_log_enabled(PRIMESHIM_LOG_TRACE)) {
            UIMultipressEvent *event = &packet.multipress_events[0];
            PS_LOG_TRACE("input",
                         "enqueue touch batch type=%u finger=%u x=%u y=%u",
                         event->type, event->finger_id, event->touch_x,
                         event->touch_y);
        }
        EnqueuePacket(&packet);
    }
}

#define SCREEN_W 320
#define SCREEN_H 240
#if PRIMESHIM_HOST_DEVICE

// --- CALIBRATION DATA ---
#define TS_MIN_X 0
#define TS_MAX_X 447
#define TS_MIN_Y 0
#define TS_MAX_Y 319


static int map_val(int value, int min_in, int max_in, int max_out) {
    if (value < min_in) value = min_in;
    if (value > max_in) value = max_in;
    return (value - min_in) * max_out / (max_in - min_in);
}

// Structure to keep track of PHYSICAL hardware state
typedef struct {
    int x;
    int y;
    bool active; // Is the finger physically touching?
} HardwareSlot;

void* input_thread(void* arg) {
    PS_LOG_DEBUG("pointer", "touch listener started");
    int fd = open(TOUCH_DEV_PATH, O_RDONLY);
    if (fd == -1) {
        PS_LOG_ERRNO_ERROR("pointer", errno, "cannot open touchscreen %s",
                           TOUCH_DEV_PATH);
        return NULL;
    }

    // PERSISTENT state (Do not memset this inside the loop!)
    HardwareSlot hw_slots[MAX_TOUCH_SLOTS];
    memset(hw_slots, 0, sizeof(hw_slots));

    int current_slot = 0;

    // Ensure the UI system is clean
    init_touch_system();

    struct input_event ev;
    while (1) {
        if (read(fd, &ev, sizeof(struct input_event)) != sizeof(struct input_event)) {
            usleep(1000); // Prevent CPU hogging on error
            continue;
        }

        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_MT_SLOT:
                    if (ev.value >= 0 && ev.value < MAX_TOUCH_SLOTS) {
                        current_slot = ev.value;
                    }
                    break;

                case ABS_MT_TRACKING_ID:
                    // ID -1 means lifted, any other ID means pressed
                    if (ev.value == -1) {
                        hw_slots[current_slot].active = false;
                    } else {
                        hw_slots[current_slot].active = true;
                    }
                    break;

                case ABS_MT_POSITION_X:
                    hw_slots[current_slot].x = map_val(ev.value, TS_MIN_X, TS_MAX_X, SCREEN_W - 1);
                    break;

                case ABS_MT_POSITION_Y:
                    hw_slots[current_slot].y = map_val(ev.value, TS_MIN_Y, TS_MAX_Y, SCREEN_H - 1);
                    break;
            }
        }
        else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            // Hardware frame finished. Sync Hardware State -> UI State.

            for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
                // We pass the CURRENT hardware truth to the UI logic.
                // The UI logic (update_touch_slot) handles the "Just Pressed" vs "Move" logic.
                update_touch_slot(i, hw_slots[i].x, hw_slots[i].y, hw_slots[i].active);
            }

            // Send the packet
            commit_touch_frame();

            // --- Visual Feedback (Optional) ---
            if (hw_slots[0].active) {
                LCD_drawcursor(hw_slots[0].x, hw_slots[0].y);
            }
        }
    }
    close(fd);
    return NULL;
}
#endif

#if PRIMESHIM_HOST_DEVICE
static pthread_t mouse_thread;
static int mouse_thread_started;

void init_mouse() {
    if (!mouse_thread_started &&
        pthread_create(&mouse_thread, NULL, input_thread, NULL) == 0) {
        mouse_thread_started = 1;
    }
}

static int pointer_driver_start(void)
{
    init_mouse();
    return mouse_thread_started ? 0 : -1;
}

static void pointer_driver_stop(void)
{
    if (!mouse_thread_started) return;
    pthread_cancel(mouse_thread);
    pthread_join(mouse_thread, NULL);
    mouse_thread_started = 0;
}

static const primeshim_peripheral_driver_t touchscreen_driver = {
    .name = "multitouch-screen",
    .classes = PRIMESHIM_PERIPHERAL_POINTER,
    .required = true,
    .probe = NULL,
    .start = pointer_driver_start,
    .stop = pointer_driver_stop
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(touchscreen_driver)
#endif
