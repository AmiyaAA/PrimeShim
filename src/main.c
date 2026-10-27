#include <stdio.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include "platform.h"
#include "log.h"
#include "peripheral.h"
#include "firmware_image.h"
#include "runtime_patcher.h"
#define LCD_ADDRESS 0x4c800000
#define LCD_SIZE 0x100
#define RTC_ADDRESS 0x57000000
#define RTC_SIZE 0x94
#define DUMMY_ADDRESS 0x00000000
#define DUMMY_SIZE 4096
int timer_register_runtime_peripheral(runtime_patcher_t *patcher);
void fs_prepare_root(void);
void start_keyboard_listener(void);

#if PRIMESHIM_HOST_DEVICE
#include <linux/kd.h>
#include <sys/ioctl.h>

static int vt_guardian_main(int argc, char **argv)
{
    char *end;
    char token;
    long pipe_fd;
    long tty_fd;
    long display_mode;
    long keyboard_mode;
    int oom_fd;
    ssize_t result;

    if (argc != 6) return 2;

#define PARSE_GUARDIAN_ARG(index, destination)                              \
    do {                                                                    \
        errno = 0;                                                          \
        end = NULL;                                                         \
        destination = strtol(argv[index], &end, 10);                       \
        if (errno || !end || *end != '\0') return 2;                       \
    } while (0)

    PARSE_GUARDIAN_ARG(2, pipe_fd);
    PARSE_GUARDIAN_ARG(3, tty_fd);
    PARSE_GUARDIAN_ARG(4, display_mode);
    PARSE_GUARDIAN_ARG(5, keyboard_mode);
#undef PARSE_GUARDIAN_ARG

    /* The guardian is tiny and must survive an OOM kill of its parent. */
    oom_fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (oom_fd >= 0) {
        (void)write(oom_fd, "-1000", 5);
        close(oom_fd);
    }

    do {
        result = read((int)pipe_fd, &token, sizeof(token));
    } while (result < 0 && errno == EINTR);

    (void)ioctl((int)tty_fd, KDSKBMODE, (int)keyboard_mode);
    (void)ioctl((int)tty_fd, KDSETMODE, (int)display_mode);
    close((int)pipe_fd);
    close((int)tty_fd);
    return 0;
}
#endif

static uint8_t zero_page_ram[DUMMY_SIZE];

static uint32_t zero_page_read(void *opaque, uint32_t address, unsigned width) {
    uint32_t value = 0;

    memcpy(&value, (uint8_t *)opaque + address, width);
    return value;
}

static void zero_page_write(void *opaque, uint32_t address, unsigned width,
                            uint32_t value) {
    memcpy((uint8_t *)opaque + address, &value, width);
}

/* Optional LCD register tracing: the firmware programs the display controller
 * at 0x4c800000.  Mapping it as RAM hides which windows it sets up, so
 * PRIMESHIM_TRACE_LCD=1 registers it as a peripheral and logs every access. */
static uint8_t lcd_register_ram[LCD_SIZE];

static uint32_t lcd_register_read(void *opaque, uint32_t address, unsigned width) {
    uint32_t offset = address - LCD_ADDRESS;
    uint32_t value = 0;
    (void)opaque;
    if (offset + width <= LCD_SIZE) memcpy(&value, &lcd_register_ram[offset], width);
    PS_LOG_DEBUG("lcd", "read  [+0x%03x] w%u = 0x%08x", offset, width, value);
    return value;
}

static void lcd_register_write(void *opaque, uint32_t address, unsigned width,
                               uint32_t value) {
    uint32_t offset = address - LCD_ADDRESS;
    (void)opaque;
    PS_LOG_DEBUG("lcd", "write [+0x%03x] w%u = 0x%08x", offset, width, value);
    if (offset + width <= LCD_SIZE) memcpy(&lcd_register_ram[offset], &value, width);
}

static int register_lcd_tracing(runtime_patcher_t *patcher) {
    return runtime_patcher_register_peripheral(
        patcher, LCD_ADDRESS, LCD_SIZE, lcd_register_read, lcd_register_write,
        lcd_register_ram, 0, "lcd-registers");
}

static int register_zero_page_fallback(runtime_patcher_t *patcher) {
    return runtime_patcher_register_peripheral(
        patcher, DUMMY_ADDRESS, DUMMY_SIZE,
        zero_page_read, zero_page_write, zero_page_ram, 0, "zero-page-ram");
}

int memallocate(size_t desired_address,size_t size){
void *addr = (void *)desired_address;

    void *mem = mmap(addr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (mem == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("memory", errno, "cannot map %zu bytes at %p",
                           size, addr);
        return -1;
    }

    if (mem != addr) {
        PS_LOG_WARN("memory", "mapped at %p instead of requested %p", mem,
                    addr);
        return -2;
    } else {
        PS_LOG_DEBUG("memory", "mapped %zu bytes at %p", size, mem);
        return 0;
    }
}
static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--firmware armfir.elf] [--patch-only] "
            "[--continue-on-segfault] [--static-syscall-patch] "
            "[--no-static-syscall-patch] [--log-level LEVEL]\n",
            program);
}

int main(int argc, char **argv){
    const char *firmware_path = PRIMESHIM_FIRMWARE_PATH;
    runtime_patcher_t *patcher;
    uint32_t entry;
    int argi;
    bool patch_only = false;
    bool continue_on_segfault = false;
    bool static_syscall_patch = PRIMESHIM_STATIC_SYSCALLS_DEFAULT;

#if PRIMESHIM_HOST_DEVICE
    if (argc >= 2 && strcmp(argv[1], "--vt-guardian") == 0)
        return vt_guardian_main(argc, argv);
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    primeshim_log_init();
    /* The virtual drives are resolved against the current directory, which is
     * final by the time main() runs. */
    fs_prepare_root();
#if PRIMESHIM_HOST_PC
    if (getenv("PRIMESHIM_TEST_KEY_SEQUENCE"))
        start_keyboard_listener();
#endif

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--import-firmware") == 0) {
            char message[256];
            const char *source = argi + 1 < argc ? argv[argi + 1] : NULL;
            const char *target = argi + 2 < argc ? argv[argi + 2] : NULL;
            int rc;
            if (!source || !target) {
                usage(argv[0]);
                return 2;
            }
            rc = primeshim_firmware_import(source, target, message, sizeof(message));
            fprintf(stderr, "%s: %s\n", rc == 0 ? "imported" : "error", message);
            return rc == 0 ? 0 : 1;
        }
    }

    {
        /* Environment override, useful for A/B runs on the calculator and in
         * adb shell on Android. */
        const char *static_env = getenv("PRIMESHIM_STATIC_SYSCALLS");
        if (static_env && *static_env)
            static_syscall_patch = !(static_env[0] == '0' || static_env[0] == 'n' ||
                                     static_env[0] == 'N' || static_env[0] == 'f' ||
                                     static_env[0] == 'F');
    }

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--firmware") == 0 && argi + 1 < argc) {
            firmware_path = argv[++argi];
        } else if (strcmp(argv[argi], "--patch-only") == 0) {
            patch_only = true;
        } else if (strcmp(argv[argi], "--continue-on-segfault") == 0) {
            continue_on_segfault = true;
        } else if (strcmp(argv[argi], "--static-syscall-patch") == 0) {
            static_syscall_patch = true;
        } else if (strcmp(argv[argi], "--no-static-syscall-patch") == 0) {
            static_syscall_patch = false;
        } else if (strcmp(argv[argi], "--log-level") == 0 && argi + 1 < argc) {
            primeshim_log_level_t level;
            if (!primeshim_log_level_parse(argv[++argi], &level)) {
                fprintf(stderr, "invalid log level: %s\n", argv[argi]);
                return 2;
            }
            primeshim_log_set_level(level);
        } else if (strcmp(argv[argi], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
PS_LOG_INFO("main", "emulation starting (log level %s)",
            primeshim_log_level_name(primeshim_log_get_level()));
patcher = runtime_patcher_create();
if (!patcher) {
    PS_LOG_ERRNO_ERROR("runtime", errno, "cannot create runtime patcher");
    return 1;
}
runtime_patcher_set_continue_on_segfault(patcher, continue_on_segfault);
if (runtime_patcher_load_elf(patcher, firmware_path) != 0 ||
    runtime_register_builtin_syscalls(patcher) != 0 ||
    timer_register_runtime_peripheral(patcher) != 0 ||
    runtime_patcher_apply(patcher) != 0) {
    PS_LOG_ERRNO_ERROR("runtime", errno,
                       "failed to prepare runtime firmware image");
    runtime_patcher_destroy(patcher);
    return 1;
}
if (static_syscall_patch) {
    /* Portable interception: no SUD, no seccomp filter, no SIGILL retry. */
    if (runtime_patcher_static_patch_syscalls(patcher) < 0) {
        PS_LOG_ERROR("runtime", "static syscall patching failed");
        runtime_patcher_destroy(patcher);
        return 1;
    }
}
if (patch_only) {
    runtime_patcher_destroy(patcher);
    return 0;
}
if (getenv("PRIMESHIM_TRACE_LCD")) {
    if (register_lcd_tracing(patcher) != 0) {
        PS_LOG_ERROR("lcd", "cannot register LCD tracing peripheral");
    }
} else {
    memallocate(LCD_ADDRESS,LCD_SIZE);
}
memallocate(RTC_ADDRESS,RTC_SIZE);
/* A real mapping is fastest.  Kernels which reject address zero fall back to
 * the registered zero-page RAM peripheral when an access actually faults. */
if (getenv("PRIMESHIM_NO_ZERO_MAP") ||
    memallocate(DUMMY_ADDRESS, DUMMY_SIZE) != 0) {
    /* Mapping page zero is refused on Android (mmap_min_addr); the zero-page
     * RAM peripheral below is the documented fallback, not a failure. */
    if (register_zero_page_fallback(patcher) != 0) {
        PS_LOG_ERRNO_ERROR("runtime", errno,
                           "failed to register zero-page RAM fallback");
        runtime_patcher_destroy(patcher);
        return 1;
    }
    PS_LOG_INFO("runtime", "using fault-time zero-page RAM fallback");
}
if (peripheral_bus_start() != 0) {
    PS_LOG_ERRNO_ERROR("peripheral", errno,
                       "failed to start host peripherals");
    runtime_patcher_destroy(patcher);
    return 1;
}
entry = runtime_patcher_entry(patcher);
if (runtime_patcher_enter_firmware_thread() != 0) {
    PS_LOG_ERRNO_ERROR("runtime", errno,
                       "failed to enable runtime syscall interception");
    peripheral_bus_stop();
    runtime_patcher_destroy(patcher);
    return 1;
}
((void (*)(void))(uintptr_t)entry)();
runtime_patcher_leave_firmware_thread();
peripheral_bus_stop();
runtime_patcher_destroy(patcher);
return 0;
}
