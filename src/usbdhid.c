#include "usbdhid.h"
#include "log.h"
#include "peripheral.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <sys/sysmacros.h>
#include <syscall.h>

#define HP_PRIME_USB_VENDOR_ID  "0x03f0"
#define HP_PRIME_USB_PRODUCT_ID "0x1541"

static pthread_t worker_thread;
static volatile bool running = false;
static volatile bool is_ready_flag = false;
static volatile bool rx_enabled_flag = false;
static int hidg_fd = -1;
static char active_udc_name[256] = {0};


static char active_gadget_path[512] = {0};
static char active_func_path[768] = {0};
static char active_symlink_path[1280] = {0};

static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static usbdhid_callbacks_t cb;

// Custom 64-byte IN/OUT Report Descriptor
static const unsigned char report_desc[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x02,        //   Usage (0x02)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x40,        //   Report Count (64)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x09, 0x03,        //   Usage (0x03)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x40,        //   Report Count (64)
    0x91, 0x02,        //   Output (Data, Var, Abs)
    0xC0               // End Collection
};

// --- Helper Functions ---
static long long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_sysfs(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int ret = write(fd, val, strlen(val));
    close(fd);
    return ret;
}

static int read_sysfs(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int ret = read(fd, buf, size - 1);
    if (ret >= 0) buf[ret] = '\0';
    close(fd);
    return ret;
}

static int write_sysfs_bin(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int ret = write(fd, data, len);
    close(fd);
    return ret;
}

static int get_first_udc(char *udc_name, size_t max_len) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '.') {
            if (strlen(dir->d_name) >= max_len) continue;
            strcpy(udc_name, dir->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static bool sysfs_text_equals(const char *path, const char *expected) {
    char value[64];

    if (read_sysfs(path, value, sizeof(value)) <= 0) return false;
    value[strcspn(value, "\r\n")] = '\0';
    return strcasecmp(value, expected) == 0;
}

// --- Background Worker Thread ---
static void* worker_loop(void* arg) {
    (void)arg;
    PS_LOG_DEBUG("usb", "listener started");
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "/sys/class/udc/%s/state", active_udc_name);

    char udc_path[768];
    snprintf(udc_path, sizeof(udc_path), "%s/UDC", active_gadget_path);

    char state_str[64];
    bool last_ready = false;
    long long last_state_check = 0; // <-- NEW: Timer for state checks

    while (running) {
        long long now = get_time_ms();

        // --- NEW: Only read SysFS every 500ms. SysFS reads are slow and will
        // severely bottleneck high-speed packet transfers if done every loop! ---
        if (now - last_state_check > 500) {
            last_state_check = now;

            // Auto-rebind if someone else unbound our gadget
            char bound_udc[256] = {0};
            if (read_sysfs(udc_path, bound_udc, sizeof(bound_udc)) >= 0) {
                if (bound_udc[0] == '\n' || bound_udc[0] == '\0') {
                    write_sysfs(udc_path, active_udc_name);
                }
            }

            // Read connection state
            if (read_sysfs(state_path, state_str, sizeof(state_str)) > 0) {
                bool current_ready = (strstr(state_str, "configured") != NULL);
                if (current_ready != last_ready) {
                    last_ready = current_ready;
                    is_ready_flag = current_ready;
                    if (current_ready && cb.on_ready) cb.on_ready();
                    if (!current_ready && cb.on_plug_out) cb.on_plug_out();
                }
            }
        }

        // Fast-path: Handle Receiving packets
        if (is_ready_flag && rx_enabled_flag && hidg_fd >= 0) {
            struct pollfd pfd = { .fd = hidg_fd, .events = POLLIN };
            // Poll waits UP TO 100ms, but returns INSTANTLY if a packet arrives.
            int p = poll(&pfd, 1, 100);
            if (p > 0 && (pfd.revents & POLLIN)) {
                uint8_t buf[64] = {0};
                int n = read(hidg_fd, buf, sizeof(buf));
                PS_LOG_TRACE("usb", "received %d bytes", n);
                if (n > 0 && cb.on_out_packet) {
                    cb.on_out_packet(buf, n);
                }
            }
        } else {
            usleep(100000); // Only sleeps when disconnected or RX is paused
        }
    }
    return NULL;
}

// --- Public Interface ---

int usbdhid_init(const usbdhid_callbacks_t* callbacks) {
    if (callbacks) cb = *callbacks;

    if (get_first_udc(active_udc_name, sizeof(active_udc_name)) != 0) return -1;

    char bound_gadget_path[512] = {0};
    char target_config[1024] = {0};
    bool need_rebind = false;
    bool reuse_existing = false;
    bool identity_matches;
    int setup_errno = 0;

    // 1. Scan for an existing gadget bound to our UDC
    DIR *d = opendir("/sys/kernel/config/usb_gadget");
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_name[0] == '.') continue;
            char udc_path[768], tmp_udc[256] = {0};
            snprintf(udc_path, sizeof(udc_path), "/sys/kernel/config/usb_gadget/%s/UDC", dir->d_name);
            if (read_sysfs(udc_path, tmp_udc, sizeof(tmp_udc)) > 0) {
                tmp_udc[strcspn(tmp_udc, "\r\n")] = 0;
                if (strcmp(tmp_udc, active_udc_name) == 0) {
                    snprintf(bound_gadget_path, sizeof(bound_gadget_path), "/sys/kernel/config/usb_gadget/%s", dir->d_name);
                    break;
                }
            }
        }
        closedir(d);
    }

    if (bound_gadget_path[0] == '\0') {
        errno = ENODEV;
        PS_LOG_WARN("usb", "no bound ConfigFS gadget is available for HID");
        return -1;
    }
    strcpy(active_gadget_path, bound_gadget_path);

    {
        char configs_path[768];
        DIR *configs;
        struct dirent *entry;

        snprintf(configs_path, sizeof(configs_path), "%s/configs",
                 active_gadget_path);
        configs = opendir(configs_path);
        if (configs) {
            while ((entry = readdir(configs)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                snprintf(target_config, sizeof(target_config), "%s/%s",
                         configs_path, entry->d_name);
                break;
            }
            closedir(configs);
        }
    }
    if (target_config[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    snprintf(active_func_path, sizeof(active_func_path),
             "%s/functions/hid.usbdhid", active_gadget_path);
    snprintf(active_symlink_path, sizeof(active_symlink_path),
             "%s/hid.usbdhid", target_config);
    reuse_existing = access(active_func_path, F_OK) == 0 &&
                     access(active_symlink_path, F_OK) == 0;
    {
        char vendor_path[768];
        char product_path[768];

        snprintf(vendor_path, sizeof(vendor_path), "%s/idVendor",
                 active_gadget_path);
        snprintf(product_path, sizeof(product_path), "%s/idProduct",
                 active_gadget_path);
        identity_matches =
            sysfs_text_equals(vendor_path, HP_PRIME_USB_VENDOR_ID) &&
            sysfs_text_equals(product_path, HP_PRIME_USB_PRODUCT_ID);
    }
    if (reuse_existing && identity_matches) {
        PS_LOG_INFO("usb", "reusing existing composite HID function");
    } else {
        char udc_path[768];
        char vendor_path[768];
        char product_path[768];

        snprintf(udc_path, sizeof(udc_path), "%s/UDC",
                 active_gadget_path);
        if (write_sysfs(udc_path, "\n") < 0) return -1;
        need_rebind = true;
        snprintf(vendor_path, sizeof(vendor_path), "%s/idVendor",
                 active_gadget_path);
        snprintf(product_path, sizeof(product_path), "%s/idProduct",
                 active_gadget_path);
        if (write_sysfs(vendor_path, HP_PRIME_USB_VENDOR_ID) < 0 ||
            write_sysfs(product_path, HP_PRIME_USB_PRODUCT_ID) < 0) {
            setup_errno = errno ? errno : EIO;
            goto rebind;
        }
    }

    if (!reuse_existing) {
        char tmp_path[1024];

        if (mkdir(active_func_path, 0755) != 0 && errno != EEXIST) {
            setup_errno = errno;
            goto rebind;
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s/protocol", active_func_path);
        if (write_sysfs(tmp_path, "1") < 0) {
            setup_errno = errno;
            goto rebind;
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s/subclass", active_func_path);
        if (write_sysfs(tmp_path, "0") < 0) {
            setup_errno = errno;
            goto rebind;
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s/report_length",
                 active_func_path);
        if (write_sysfs(tmp_path, "64") < 0) {
            setup_errno = errno;
            goto rebind;
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s/report_desc",
                 active_func_path);
        if (write_sysfs_bin(tmp_path, report_desc, sizeof(report_desc)) < 0) {
            setup_errno = errno;
            goto rebind;
        }
        if (symlink(active_func_path, active_symlink_path) != 0 &&
            errno != EEXIST) {
            setup_errno = errno;
            goto rebind;
        }
    }

rebind:
    if (need_rebind) {
        char udc_path[768];
        snprintf(udc_path, sizeof(udc_path), "%s/UDC", active_gadget_path);
        if (write_sysfs(udc_path, active_udc_name) < 0) return -1;
    }
    if (setup_errno) {
        errno = setup_errno;
        return -1;
    }

    // 4. --- NEW: Wait for OUR specific /dev/hidgX to surface using exact Major:Minor map ---
    char dev_path[1024];
    snprintf(dev_path, sizeof(dev_path), "%s/dev", active_func_path);

    char target_hidg[32] = {0};
    for (int wait = 0; wait < 50; wait++) {
        char dev_str[32] = {0};
        // The /dev file in sysfs outputs "Major:Minor"
        if (read_sysfs(dev_path, dev_str, sizeof(dev_str)) > 0) {
            int maj = 0, min = 0;
            if (sscanf(dev_str, "%d:%d", &maj, &min) == 2) {
                dev_t target_rdev = makedev(maj, min);

                // Scan physical /dev/hidg* block devices to match the exact device node
                for(int i = 0; i < 16; i++) {
                    char dpath[32];
                    snprintf(dpath, sizeof(dpath), "/dev/hidg%d", i);
                    struct stat st;
                    if (stat(dpath, &st) == 0 && st.st_rdev == target_rdev) {
                        strcpy(target_hidg, dpath);
                        break;
                    }
                }
            }
        }
        if (strlen(target_hidg) > 0) break;
        usleep(100000);
    }
    if (strlen(target_hidg) == 0) strcpy(target_hidg, "/dev/hidg0"); // Default Fallback

    hidg_fd = open(target_hidg, O_RDWR | O_NONBLOCK);
    if (hidg_fd < 0) return -1;

    // 5. Start Manager Thread
    running = true;
    if (pthread_create(&worker_thread, NULL, worker_loop, NULL) != 0) {
        close(hidg_fd);
        return -1;
    }
    return 0; // Success
}

bool usbdhid_is_ready(void) {
    return is_ready_flag;
}

void usbdhid_set_rx_enabled(bool enabled) {
    rx_enabled_flag = enabled;
}

int usbdhid_send(const void* data, size_t len, int timeout_ms) {
    if (!is_ready_flag || hidg_fd < 0 || len == 0) return -1;

    pthread_mutex_lock(&tx_mutex);
    const uint8_t *ptr = (const uint8_t*)data;
    size_t sent = 0;

    // Auto Split Packet Loop
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 64) chunk = 64;

        uint8_t buf[64] = {0}; // Auto padded to 64 bytes for strict f_hid demands
        memcpy(buf, ptr + sent, chunk);

        long long start_ms = get_time_ms();
        bool chunk_sent = false;

        // Retry / Timeout Loop
        while (!chunk_sent) {
            long long now = get_time_ms();
            int remain_ms = timeout_ms - (int)(now - start_ms);
            if (remain_ms < 0) remain_ms = 0;

            struct pollfd pfd = { .fd = hidg_fd, .events = POLLOUT };
            int p = poll(&pfd, 1, remain_ms);

            if (p > 0 && (pfd.revents & POLLOUT)) {
                int n = write(hidg_fd, buf, 64); // f_hid always wants exactly report_length written
                if (n == 64) {
                    chunk_sent = true;
                    sent += chunk;
                } else if (n < 0 && (errno != EAGAIN && errno != EINTR)) {
                    // Fatal Error
                    pthread_mutex_unlock(&tx_mutex);
                    return -1;
                }
            } else if (p == 0) {
                // Poll timed out
                return -2;
                //break;
            }
        }
        if (!chunk_sent) return -2; // Timeout elapsed before we could send this chunk
    }

    pthread_mutex_unlock(&tx_mutex);
    return 0;
}

void usbdhid_cleanup(void) {
    if (running) {
        running = false;
        pthread_join(worker_thread, NULL);
    }
    if (hidg_fd >= 0) {
        close(hidg_fd);
        hidg_fd = -1;
    }
}


//-------------
#include "ui.h"
extern void EnqueuePacket(ui_event_prime_s* packet);
static void EmitUSBEvent(uint16_t key_code) {
    ui_event_prime_s ev ;
    memset(&ev,0,sizeof(ev));
    ev.event_type = (ui_event_type_e)0x100;
    ev.key_code0 = key_code;
    EnqueuePacket(&ev);
    memset(&ev,0,sizeof(ev));
    ev.event_type = (ui_event_type_e)0x4000;
    EnqueuePacket(&ev);
}
static void on_ready(void) {
    PS_LOG_INFO("usb", "host opened the connection");
    EmitUSBEvent(45);
    EmitUSBEvent(47);
}

// 2. Called if host cuts power, goes to sleep, or cable is removed
static void on_plug_out(void) {
    PS_LOG_WARN("usb", "device disconnected");
    EmitUSBEvent(36);
}

int usbd_deviceio_handler(int request,void* buffer,int len){
    if (request == 4) {
        PS_LOG_DEBUG("usb", "receive path enabled");
        usbdhid_set_rx_enabled(true);
        return 1;
    }
    else if (request == 263) {
        return !usbdhid_is_ready();
    }
    else if (request == 258) {
        if (buffer && len > 0) {
            PS_LOG_TRACE("usb", "sending %d bytes", len);
            primeshim_log_hexdump(PRIMESHIM_LOG_TRACE, "usb", buffer,
                                  len > 64 ? 64u : (size_t)len,
                                  "USB transmit prefix");
            int ret=usbdhid_send(buffer, len, 100);
            PS_LOG_TRACE("usb", "send returned %d", ret);
            return ret;
        }
        return -1;
    }
    else if (request == 259) {
        PS_LOG_DEBUG("usb", "receive callback set to %p", buffer);
        cb.on_out_packet=buffer;
        return 1;
    }
    return -1;

}

int usbd_init(void) {
    // Fill the callbacks struct
    usbdhid_callbacks_t cb = {
        .on_ready = on_ready,
        .on_plug_out = on_plug_out,
        .on_out_packet = 0
    };

    PS_LOG_INFO("usb", "initializing USB HID gadget");

    // Initiate the self-managing gadget
    if (usbdhid_init(&cb) != 0) {
        PS_LOG_ERRNO_WARN("usb", errno,
                          "failed to initialize USB HID gadget");
        return 1;
    }
    return 0;
}

static const primeshim_peripheral_driver_t usb_hid_driver = {
    .name = "configfs-usb-hid",
    .classes = PRIMESHIM_PERIPHERAL_USB,
    .required = false,
    .probe = NULL,
    .start = usbd_init,
    .stop = usbdhid_cleanup
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(usb_hid_driver)
