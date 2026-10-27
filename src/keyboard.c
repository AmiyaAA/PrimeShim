#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/syscall.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include "log.h"
#include "ui.h"
#include "platform.h"
#include "peripheral.h"

// ================= CONFIGURATION =================
// Define this to use the direct path for a matrix keypad that uses a 16-byte event struct.
// Comment this out to use the dynamic scanning method for standard USB/PS2 keyboards.
#if PRIMESHIM_HOST_DEVICE
#define USE_MATRIX_KEYPAD
#endif

// Direct path to the keypad device. Only used when USE_MATRIX_KEYPAD is defined.
#define KEYPAD_DEV_PATH PRIMESHIM_KEYBOARD_DEVICE

#define MAX_SCAN_CODES 64

// =================================================

// Forward declaration, assuming this is defined elsewhere.
void EnqueuePacket(ui_event_prime_s* packet);
bool EnqueuePacketBlocking(ui_event_prime_s* packet, const int *keep_running);

static pthread_t keyboard_thread;
static int keyboard_thread_running = 0;
static int keyboard_fd = -1;
static pthread_t scripted_keyboard_thread;
static int scripted_keyboard_thread_started = 0;

#ifdef USE_MATRIX_KEYPAD
// Explicit 16-byte structure for legacy drivers or 32-bit input events.
struct input_event_compat {
    struct {
        uint32_t tv_sec;
        uint32_t tv_usec;
    } time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#endif

// Parse /proc/bus/input/devices or open a direct device path.
static int open_keyboard_device(void)
{
#ifdef USE_MATRIX_KEYPAD
    // === DIRECT OPEN MODE (For Matrix Keypad) ===
    if (keyboard_fd != -1) return 1;

    keyboard_fd = open(KEYPAD_DEV_PATH, O_RDONLY | O_NONBLOCK);
    if (keyboard_fd != -1) {
        PS_LOG_INFO("keyboard", "opened matrix keypad %s", KEYPAD_DEV_PATH);
        return 1;
    } else {
        // Only print error occasionally to avoid log spam in the loop
        static int error_count = 0;
        if (error_count++ % 10 == 0) {
            PS_LOG_ERRNO_WARN("keyboard", errno,
                              "cannot open keypad " KEYPAD_DEV_PATH);
        }
        return 0;
    }
#else
    // === DYNAMIC SCANNING MODE (For standard keyboards) ===
    FILE *fp = fopen("/proc/bus/input/devices", "r");
    if (!fp) {
        PS_LOG_ERRNO_ERROR("keyboard", errno,
                           "cannot open /proc/bus/input/devices");
        return 0;
    }

    char line[256];
    char handlers[256] = {0};
    int has_ev_key = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\n') {
            if (has_ev_key && strstr(handlers, "kbd")) {
                char *event_handler = strstr(handlers, "event");
                if (event_handler) {
                    char event_path[256];
                    int event_num;
                    if (sscanf(event_handler, "event%d", &event_num) == 1) {
                        snprintf(event_path, sizeof(event_path), "/dev/input/event%d", event_num);
                        keyboard_fd = open(event_path, O_RDONLY | O_NONBLOCK);
                        if (keyboard_fd != -1) {
                            PS_LOG_INFO("keyboard", "opened %s", event_path);
                            fclose(fp);
                            return 1;
                        }
                    }
                }
            }
            handlers[0] = '\0';
            has_ev_key = 0;
        }
        else if (strncmp(line, "H: Handlers=", 12) == 0) {
            strncpy(handlers, line + 12, sizeof(handlers) - 1);
            handlers[sizeof(handlers) - 1] = '\0';
        }
        else if (strncmp(line, "B: EV=", 6) == 0) {
            if (strstr(line, "EV=120013") || strstr(line, "EV=100013")) {
                 has_ev_key = 1;
            }
        }
    }
    PS_LOG_WARN("keyboard", "no suitable keyboard device found");
    fclose(fp);
    return 0;
#endif
}


#ifdef USE_MATRIX_KEYPAD

#define SCANCODE(row, col) (row*8+col)
#define SHUTDOWN_ON_SCANCODE SCANCODE(0, 0)
#define SHUTDOWN_SYMB_SCANCODE SCANCODE(4, 2)

static int matrix_key_is_pressed(const uint8_t *key_state_bitmap,
                                 int scan_code)
{
    return !!(key_state_bitmap[scan_code / 8] &
              (1u << (scan_code % 8)));
}

static int map_key_to_device(int scancode)
{
    switch(scancode)
    {
        case SCANCODE(0,0): return 131; // on
        case SCANCODE(1,0): return 80;  // eex
        case SCANCODE(1,1): return 48;  // 0
        case SCANCODE(1,2): return 84;  // divide
        case SCANCODE(1,3): return 88;  // mul
        case SCANCODE(1,4): return 183; // minus
        case SCANCODE(1,5): return 185; // add
        case SCANCODE(1,6): return 32;  // space
        case SCANCODE(1,7): return 2;   // left
        case SCANCODE(2,0): return 12;  // backspace
        case SCANCODE(2,1): return 68;  // define
        case SCANCODE(2,2): return 67;  // units
        case SCANCODE(2,3): return 182; // alpha
        case SCANCODE(2,4): return 69;  // ab/c
        case SCANCODE(2,5): return 65;  // vars
        case SCANCODE(2,6): return 71;  // sin
        case SCANCODE(2,7): return 66;  // toolbox
        case SCANCODE(3,0): return 77;  // +/-
        case SCANCODE(3,1): return 76;  // square
        case SCANCODE(3,2): return 75;  // log
        case SCANCODE(3,3): return 74;  // ln
        case SCANCODE(3,4): return 73;  // tan
        case SCANCODE(3,5): return 72;  // cos
        case SCANCODE(3,6): return 139; // shift
        case SCANCODE(3,7): return 70;  // pow
        case SCANCODE(4,0): return 179; // num
        case SCANCODE(4,1): return 178; // plot
        case SCANCODE(4,2): return 145; // symb
        case SCANCODE(4,3): return 57415; // home
        case SCANCODE(4,4): return 177; // apps
        case SCANCODE(4,5): return 5;   // down
        case SCANCODE(4,6): return 1;   // esc
        case SCANCODE(4,7): return 78;  // Bracket
        case SCANCODE(5,0): return 83;  // 9
        case SCANCODE(5,1): return 181; // cas
        case SCANCODE(5,2): return 147; // menu
        case SCANCODE(5,3): return 180; // view
        case SCANCODE(5,4): return 3;   // up
        case SCANCODE(5,5): return 89;  // 1
        case SCANCODE(5,6): return 184; // period
        case SCANCODE(5,7): return 149; // help
        case SCANCODE(6,0): return 79;  // comma
        case SCANCODE(6,1): return 82;  // 8
        case SCANCODE(6,2): return 81;  // 7
        case SCANCODE(6,3): return 87;  // 6
        case SCANCODE(6,4): return 86;  // 5
        case SCANCODE(6,5): return 85;  // 4
        case SCANCODE(6,6): return 51;  // 3
        case SCANCODE(6,7): return 90;  // 2
        case SCANCODE(7,0): return 13;  // enter
        case SCANCODE(7,1): return 4;   // right
        default: return -1;
    }
}
#else
// Function to map Linux keycodes to device keycodes
static int map_key_to_device(int linux_keycode)
{
    switch(linux_keycode)
    {
        // === 标准功能键 ===
        case KEY_ESC:        return 0x01; // Esc
        case KEY_LEFT:       return 0x02; // 左
        case KEY_UP:         return 0x03; // 上
        case KEY_RIGHT:      return 0x04; // 右
        case KEY_DOWN:       return 0x05; // 下
        case KEY_BACKSPACE:  return 0x0C; // 退格
        case KEY_ENTER:      return 0x0D; // 回车
        case KEY_SPACE:      return 0x20; // 空格
        case KEY_LEFTSHIFT:  return 0x8B; // Shift
        case KEY_RIGHTSHIFT: return 0x8B; // Shift

        // === 数字与符号键 ===
        case KEY_0:          return 0x30; // 0
        case KEY_Y:
        case KEY_1:          return 0x59; // 1
        case KEY_Z:
        case KEY_2:          return 0x5A; // 2
        case KEY_3:          return 0x33; // 3
        case KEY_U:
        case KEY_4:          return 0x55; // 4
        case KEY_V:
        case KEY_5:          return 0x56; // 5
        case KEY_W:
        case KEY_6:          return 0x57; // 6
        case KEY_Q:
        case KEY_7:          return 0x51; // 7
        case KEY_R:
        case KEY_8:          return 0x52; // 8
        case KEY_S:
        case KEY_9:          return 0x53; // 9
        case KEY_D:          return 0x44; // X
        case KEY_O:
        case KEY_COMMA:      return 0x4F; // ,
        case KEY_T:
        case KEY_SLASH:      return 0x54; // /
        case KEY_X:
        case KEY_KPASTERISK: return 0x58; // *
        case KEY_DOT:        return 0xB8; // .
        case KEY_MINUS:      return 0xB7; // +
        case KEY_EQUAL:      return 0xB9; // - (Note: This mapping might need adjustment)

        // === 特殊功能键 (映射自 QWERTY 布局) ===
        case KEY_A:          return 0x41; // 变量
        case KEY_B:          return 0x42; // 工具箱
        case KEY_C:          return 0x43; // 数学模板
        case KEY_E:          return 0x45; // a b/c
        case KEY_F:          return 0x46; // ^
        case KEY_G:          return 0x47; // SIN
        case KEY_H:          return 0x48; // COS
        case KEY_I:          return 0x49; // TAN
        case KEY_J:          return 0x4A; // LN
        case KEY_K:          return 0x4B; // LOG
        case KEY_L:          return 0x4C; // ^2
        case KEY_M:          return 0x4D; // +/-
        case KEY_N:          return 0x4E; // ()
        case KEY_P:          return 0x50; // 1e
        case KEY_LEFTCTRL:   return 0x83; // ON
        case KEY_F1:         return 0x91; // 符号视图
        case KEY_F2:         return 0x93; // 消息
        case KEY_F3:         return 0xB2; // 绘图
        case KEY_F4:         return 0xB3; // 数值
        case KEY_F5:         return 0xB4; // 视图
        case KEY_F6:         return 0xB5; // CAS
        case KEY_F7:         return 0xB6; // Alpha
        case KEY_F8:         return 0x95; // 帮助
        case KEY_F9:         return 0xB1; // 应用
        case KEY_F10:        return 0xE047; // 应用

        default: return -1; // 未映射的键
    }
}
#endif
// This function builds the required UI packet and enqueues it.
/* Alternative encoding: the key event itself at the top level instead of a
 * KEY_BATCH container.  Some firmware revisions dispatch only this form. */
void enqueue_key_packet_direct(int device_keycode, int is_press) {
    ui_event_prime_s packet;
    memset(&packet, 0, sizeof(packet));

    packet.event_type = is_press ? UI_EVENT_TYPE_KEY : UI_EVENT_TYPE_KEY_UP;
    packet.key_code0 = (unsigned short)device_keycode;
    packet.available_multipress_events = 1;
    packet.multipress_events[0].type = is_press ? UI_EVENT_TYPE_KEY : UI_EVENT_TYPE_KEY_UP;
    packet.multipress_events[0].key_code0 = (unsigned short)device_keycode;
    EnqueuePacket(&packet);
}

void enqueue_key_packet(int device_keycode, int is_press) {
    ui_event_prime_s packet;
    memset(&packet, 0, sizeof(packet));

    packet.event_type = UI_EVENT_TYPE_KEY_BATCH; // Keyboard Type
    packet.available_multipress_events = 1;

    packet.multipress_events[0].type = is_press ? UI_EVENT_TYPE_KEY : UI_EVENT_TYPE_KEY_UP;
    packet.multipress_events[0].key_code0 = device_keycode;

    EnqueuePacket(&packet);
}

static bool enqueue_scripted_key_packet(int device_keycode, int is_press)
{
    ui_event_prime_s packet;

    memset(&packet, 0, sizeof(packet));
    packet.event_type = UI_EVENT_TYPE_KEY_BATCH;
    packet.available_multipress_events = 1;
    packet.multipress_events[0].type =
        is_press ? UI_EVENT_TYPE_KEY : UI_EVENT_TYPE_KEY_UP;
    packet.multipress_events[0].key_code0 = device_keycode;
    return EnqueuePacketBlocking(&packet, &keyboard_thread_running);
}

static int scripted_sleep_ms(unsigned milliseconds)
{
    while (keyboard_thread_running && milliseconds != 0) {
        unsigned slice = milliseconds > 50u ? 50u : milliseconds;
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = (long)slice * 1000000L,
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        milliseconds -= slice;
    }
    return keyboard_thread_running;
}

static unsigned scripted_delay_from_environment(const char *name,
                                                 unsigned fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (!text || !*text) return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || value > 600000u) {
        PS_LOG_WARN("keyboard", "ignoring invalid %s=%s", name, text);
        return fallback;
    }
    return (unsigned)value;
}

static void *scripted_keyboard_input_thread(void *unused)
{
    const char *environment = getenv("PRIMESHIM_TEST_KEY_SEQUENCE");
    unsigned initial_delay = scripted_delay_from_environment(
        "PRIMESHIM_TEST_KEY_DELAY_MS", 8000u);
    unsigned key_delay = scripted_delay_from_environment(
        "PRIMESHIM_TEST_KEY_INTERVAL_MS", 300u);
    char *sequence;
    char *cursor;
    char *token;
    unsigned count = 0;

    (void)unused;
    if (!environment || !*environment) return NULL;
    sequence = strdup(environment);
    if (!sequence) {
        PS_LOG_ERRNO_ERROR("keyboard", errno,
                           "cannot copy scripted key sequence");
        return NULL;
    }

    PS_LOG_INFO("keyboard",
                "test key injector waiting %u ms before sequence", initial_delay);
    if (!scripted_sleep_ms(initial_delay)) goto out;

    cursor = sequence;
    while (keyboard_thread_running &&
           (token = strsep(&cursor, ", \t\r\n")) != NULL) {
        char *end = NULL;
        long keycode;
        if (!*token) continue;
        errno = 0;
        keycode = strtol(token, &end, 0);
        if (errno || !end || *end != '\0' || keycode < 0 ||
            keycode > UINT16_MAX) {
            PS_LOG_WARN("keyboard", "invalid scripted keycode %s", token);
            goto out;
        }
        if (!enqueue_scripted_key_packet((int)keycode, 1)) goto out;
        if (!scripted_sleep_ms(50u)) goto out;
        if (!enqueue_scripted_key_packet((int)keycode, 0)) goto out;
        ++count;
        PS_LOG_DEBUG("keyboard", "test key %u code=%ld", count, keycode);
        if (!scripted_sleep_ms(key_delay)) goto out;
    }
    PS_LOG_INFO("keyboard", "test key injector completed %u keys", count);

out:
    free(sequence);
    return NULL;
}

static void* keyboard_listener_thread(void* arg)
{
    (void)arg;
    PS_LOG_DEBUG("keyboard", "listener started");
    struct input_event ev;
    fd_set fds;

#ifdef USE_MATRIX_KEYPAD
    static uint8_t key_state_bitmap[(MAX_SCAN_CODES + 7) / 8] = {0};
    int shutdown_combo_armed = 0;
    struct input_event_compat ev_compat;
    const ssize_t target_size = sizeof(struct input_event_compat);
#else
    static ssize_t event_size_to_read = -1;
    char read_buf[24]; // Buffer large enough for the 64-bit event struct
#endif

    while(keyboard_thread_running)
    {
        if(keyboard_fd != -1)
        {
            FD_ZERO(&fds);
            FD_SET(keyboard_fd, &fds);

            struct timeval tv = {1, 0}; // Use a timeout to allow the thread to exit gracefully
            int ret = select(keyboard_fd + 1, &fds, NULL, NULL, &tv);

            if(ret > 0 && FD_ISSET(keyboard_fd, &fds))
            {
                ssize_t n = -1;
                int should_process_event = 0;

#ifdef USE_MATRIX_KEYPAD
                // === FIXED 16-BYTE READ PATH ===
                n = read(keyboard_fd, &ev_compat, target_size);
                if (n == target_size) {
                    // Convert compat struct to native struct input_event for processing
                    ev.type = ev_compat.type;
                    ev.code = ev_compat.code;
                    ev.value = ev_compat.value;
                    should_process_event = 1;
                }
#else
                // === DYNAMIC EVENT SIZE DETECTION PATH ===
                if (event_size_to_read == -1) {
                    ssize_t native_size = sizeof(struct input_event);
                    ssize_t other_size = (native_size == 16) ? 24 : 16;

                    n = read(keyboard_fd, read_buf, other_size);
                    if (n == other_size) {
                        event_size_to_read = other_size;
                        PS_LOG_DEBUG("keyboard",
                                     "kernel input_event is %zd bytes; app uses %zd",
                                     event_size_to_read, native_size);
                    } else if (n == -1 && errno == EINVAL) {
                        n = read(keyboard_fd, read_buf, native_size);
                        if (n == native_size) {
                           event_size_to_read = native_size;
                           PS_LOG_DEBUG("keyboard",
                                        "kernel input_event matches app size %zd",
                                        event_size_to_read);
                        }
                    }
                } else {
                    n = read(keyboard_fd, read_buf, event_size_to_read);
                }

                if (n == event_size_to_read) {
                    if (event_size_to_read != sizeof(struct input_event) && event_size_to_read == 24) {
                        // 32-bit app on 64-bit kernel: manually unpack the struct
                        memcpy(&ev.type,  read_buf + 16, sizeof(ev.type));
                        memcpy(&ev.code,  read_buf + 18, sizeof(ev.code));
                        memcpy(&ev.value, read_buf + 20, sizeof(ev.value));
                    } else {
                        // Sizes match, we can just copy the whole structure
                        memcpy(&ev, read_buf, sizeof(ev));
                    }
                    should_process_event = 1;
                }
#endif

                // --- COMMON ERROR HANDLING ---
                if (n == (ssize_t)-1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        PS_LOG_ERRNO_WARN("keyboard", errno, "read failed");
                        close(keyboard_fd);
                        keyboard_fd = -1;
#ifndef USE_MATRIX_KEYPAD
                        event_size_to_read = -1; // Reset detection on reconnect
#endif
                    }
                    continue;
                }

                if (n == 0) {
                     PS_LOG_WARN("keyboard", "device disconnected");
                     close(keyboard_fd);
                     keyboard_fd = -1;
#ifndef USE_MATRIX_KEYPAD
                     event_size_to_read = -1; // Reset detection on reconnect
#endif
                     continue;
                }

                // --- PROCESS THE EVENT ---
#ifndef USE_MATRIX_KEYPAD
                if (should_process_event && ev.type == EV_KEY)
                {
                    // Ignore key-repeat events, only process press (1) and release (0)
                    if (ev.value == 0 || ev.value == 1)
                    {
                        int device_keycode = map_key_to_device(ev.code);
                        if(device_keycode != -1)
                        {
                            enqueue_key_packet(device_keycode, ev.value);
                        }
                    }
                }
#else
                if (should_process_event && ev.type == EV_MSC && ev.code == MSC_SCAN)
                {
                    int raw_val = ev.value;
                    int scan_code = 0;
                    int is_press = 0;

                    // Decode the driver's custom logic:
                    // Press: code
                    // Release: (-code - 1)
                    if (raw_val >= 0) {
                        scan_code = raw_val;
                        is_press = 1;
                    } else {
                        scan_code = -(raw_val + 1);
                        is_press = 0;
                    }

                    // Safety check to prevent bitmap access out of bounds
                    if (scan_code >= 0 && scan_code < MAX_SCAN_CODES)
                    {
                        // === BITMAP STATE MACHINE ===
                        // Use a portable bitmap (array of bytes) for physical key states.
                        // key_state_bitmap is declared elsewhere, e.g.:
                        //   static uint8_t key_state_bitmap[(MAX_SCAN_CODES + 7) / 8];
                        int byte_index = scan_code / 8;
                        int bit_mask  = 1 << (scan_code % 8);

                        // Read the current state of this key (0 = released, 1 = pressed)
                        int current_state = (key_state_bitmap[byte_index] >> (scan_code % 8)) & 1;

                        // Only act if the state has changed
                        if (current_state != is_press)
                        {
                            // Update the bitmap: set or clear the corresponding bit
                            if (is_press)
                                key_state_bitmap[byte_index] |= bit_mask;   // set bit
                            else
                                key_state_bitmap[byte_index] &= ~bit_mask;  // clear bit

                            if (!shutdown_combo_armed && is_press &&
                                matrix_key_is_pressed(key_state_bitmap,
                                                      SHUTDOWN_ON_SCANCODE) &&
                                matrix_key_is_pressed(key_state_bitmap,
                                                      SHUTDOWN_SYMB_SCANCODE)) {
                                shutdown_combo_armed = 1;
                                PS_LOG_INFO("keyboard",
                                            "shutdown chord armed; release ON and SYMB");
                            }

                            // Map to UI keycode
                            int device_keycode = map_key_to_device(scan_code);

                            // Do not leak the shutdown chord into the firmware UI.
                            if (!shutdown_combo_armed && device_keycode != -1)
                            {
                                enqueue_key_packet(device_keycode, is_press);
                            }

                            /* Restore the VT only after the input core has
                             * delivered both release events.  Restoring it
                             * while the keys are held gives tty1 unmatched
                             * releases and can produce illegal keycodes. */
                            if (shutdown_combo_armed &&
                                !matrix_key_is_pressed(key_state_bitmap,
                                                       SHUTDOWN_ON_SCANCODE) &&
                                !matrix_key_is_pressed(key_state_bitmap,
                                                       SHUTDOWN_SYMB_SCANCODE)) {
                                PS_LOG_INFO("keyboard",
                                            "shutdown chord released; exiting");
                                peripheral_bus_stop();
                                exit(0);
                            }
                        }
                    }
                    else
                    {
                        PS_LOG_WARN("keyboard", "scancode %d is out of bounds",
                                    scan_code);
                    }
                }
#endif
            }
            else if(ret == -1)
            {
                PS_LOG_ERRNO_WARN("keyboard", errno, "select failed");
                close(keyboard_fd);
                keyboard_fd = -1;
#ifndef USE_MATRIX_KEYPAD
                event_size_to_read = -1; // Reset detection
#endif
            }
        }
        else
        {
            // Attempt to open the device if it's not open
            if(!open_keyboard_device()) {
                sleep(2); // Wait before retrying
            }
        }
    }

    if(keyboard_fd != -1) {
        close(keyboard_fd);
        keyboard_fd = -1;
    }

    return NULL;
}

void start_keyboard_listener(void)
{
#ifndef USE_MATRIX_KEYPAD
    PS_LOG_DEBUG("keyboard", "application input_event size is %zu",
                 sizeof(struct input_event));
#endif

    keyboard_thread_running = 1;
    pthread_create(&keyboard_thread, NULL, keyboard_listener_thread, NULL);
    if (getenv("PRIMESHIM_TEST_KEY_SEQUENCE")) {
        int result = pthread_create(&scripted_keyboard_thread, NULL,
                                    scripted_keyboard_input_thread, NULL);
        if (result == 0) {
            scripted_keyboard_thread_started = 1;
        } else {
            PS_LOG_ERRNO_WARN("keyboard", result,
                              "cannot start test key injector");
        }
    }
}

void stop_keyboard_listener(void)
{
    if(keyboard_thread_running) {
        keyboard_thread_running = 0;
        if (scripted_keyboard_thread_started &&
            !pthread_equal(pthread_self(), scripted_keyboard_thread)) {
            pthread_join(scripted_keyboard_thread, NULL);
            scripted_keyboard_thread_started = 0;
        }
        if (pthread_equal(pthread_self(), keyboard_thread)) {
            if (keyboard_fd != -1) {
                close(keyboard_fd);
                keyboard_fd = -1;
            }
            return;
        }
        pthread_join(keyboard_thread, NULL);
    }
}

#if PRIMESHIM_HOST_DEVICE
static int keyboard_driver_start(void)
{
    start_keyboard_listener();
    return 0;
}

static const primeshim_peripheral_driver_t matrix_keyboard_driver = {
    .name = "matrix-keypad",
    .classes = PRIMESHIM_PERIPHERAL_KEYBOARD,
    .required = true,
    .probe = NULL,
    .start = keyboard_driver_start,
    .stop = stop_keyboard_listener
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(matrix_keyboard_driver)
#endif
