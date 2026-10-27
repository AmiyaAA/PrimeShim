#include "platform.h"
#include "log.h"
#include "peripheral.h"

#if PRIMESHIM_HOST_DEVICE

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/kd.h>
#include <linux/vt.h>

// DRM Headers
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/fb.h>

//==============================================================================
// Configuration & Structures
//==============================================================================

// The internal format the emulated application provides. This MUST be ARGB8888.
typedef uint32_t buffer_pixel_t;

// Define the LCD_MAGIC structure for the shim
struct LCD_MAGIC {
    uint16_t SomeVal;
    uint16_t x_res;
    uint16_t y_res;
    uint16_t pixel_bits;
    uint16_t bytes_per_line;
    uint16_t brightness_level;
    uint32_t unk1_0;
    uint32_t window1_bufferstart; // Virtual address of the framebuffer
};

// Enum to track the active display mode
typedef enum {
    DISPLAY_MODE_NONE,
    DISPLAY_MODE_DRM_ZERO_COPY,      // Optimal: Direct mapping
    DISPLAY_MODE_DRM_RENDER_THREAD,  // Fallback: DRM with threaded copy
    DISPLAY_MODE_FB_RENDER_THREAD    // Fallback: Framebuffer with threaded copy
} DisplayMode;

//==============================================================================
// Global State
//==============================================================================
static struct LCD_MAGIC LcdMagic;
static DisplayMode current_mode = DISPLAY_MODE_NONE;

// --- Threaded Rendering Resources (Used for fallbacks) ---
static buffer_pixel_t *shadow_buffer = NULL; // Software buffer
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t update_thread;
static volatile int update_running = 0;

// --- Unified Hardware Abstraction ---
static int hw_width = 0;
static int hw_height = 0;
static int hw_bpp = 0;
static int hw_stride = 0;
static void *hw_buffer_ptr = NULL; // Points to the mmap'd hardware VRAM

// --- DRM Specific Globals ---
static int drm_fd = -1;
static drmModeRes *drm_res = NULL;
static drmModeConnector *drm_conn = NULL;
static drmModeCrtc *drm_saved_crtc = NULL;
static uint32_t drm_fb_id = 0;
static uint32_t drm_dumb_handle = 0;
static uint64_t drm_map_size = 0;
static int drm_buffer_owned = 0;
static int drm_master_owned = 0;

// --- Virtual terminal ownership ---
static int vt_fd = -1;
static int vt_number = -1;
static int vt_saved_display_mode = KD_TEXT;
static int vt_saved_keyboard_mode = K_XLATE;
static int vt_display_mode_saved = 0;
static int vt_keyboard_mode_saved = 0;
static int vt_atexit_registered = 0;
static int vt_signal_handlers_installed = 0;
static volatile sig_atomic_t vt_signal_cleanup_running = 0;
static int vt_guardian_write_fd = -1;
static pid_t vt_guardian_pid = -1;

// --- Framebuffer Specific Globals ---
static int fb_fd = -1;
static long fb_screensize = 0;


//==============================================================================
// Forward Declarations
//==============================================================================
static int init_drm();
static int init_fb();
static void cleanup_resources();
static void* update_thread_func(void* arg);

static void release_vt(void)
{
    if (vt_fd < 0) return;

    if (vt_keyboard_mode_saved &&
        ioctl(vt_fd, KDSKBMODE, vt_saved_keyboard_mode) != 0) {
        PS_LOG_ERRNO_WARN("display", errno,
                          "cannot restore VT keyboard mode");
    }
    if (vt_display_mode_saved &&
        ioctl(vt_fd, KDSETMODE, vt_saved_display_mode) != 0) {
        PS_LOG_ERRNO_WARN("display", errno,
                          "cannot restore VT display mode");
    }

    close(vt_fd);
    PS_LOG_INFO("display", "released tty%d", vt_number);
    vt_fd = -1;

    if (vt_guardian_write_fd >= 0) {
        close(vt_guardian_write_fd);
        vt_guardian_write_fd = -1;
    }
    if (vt_guardian_pid > 0) {
        while (waitpid(vt_guardian_pid, NULL, 0) < 0 && errno == EINTR) {}
        vt_guardian_pid = -1;
    }

    vt_number = -1;
    vt_display_mode_saved = 0;
    vt_keyboard_mode_saved = 0;
}

static void release_vt_at_exit(void)
{
    release_vt();
}

static void release_vt_on_signal(int signal_number)
{
    if (!vt_signal_cleanup_running) {
        vt_signal_cleanup_running = 1;
        if (vt_fd >= 0) {
            if (vt_keyboard_mode_saved)
                ioctl(vt_fd, KDSKBMODE, vt_saved_keyboard_mode);
            if (vt_display_mode_saved)
                ioctl(vt_fd, KDSETMODE, vt_saved_display_mode);
        }
    }

    signal(signal_number, SIG_DFL);
    raise(signal_number);
    _exit(128 + signal_number);
}

static int install_vt_signal_handlers(void)
{
    static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
    struct sigaction action;
    struct sigaction previous;
    size_t i;

    if (vt_signal_handlers_installed) return 0;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = release_vt_on_signal;

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], NULL, &previous) != 0) return -1;
        /* Do not override an application's custom or ignored disposition. */
        if (previous.sa_handler != SIG_DFL) continue;
        if (sigaction(signals[i], &action, NULL) != 0) return -1;
    }

    vt_signal_handlers_installed = 1;
    return 0;
}

static int spawn_vt_guardian(void)
{
    int pipe_fds[2];
    struct rlimit fd_limit;
    int max_fd = 4096;
    pid_t child;

    if (vt_guardian_pid > 0) return 0;
    if (getrlimit(RLIMIT_NOFILE, &fd_limit) == 0 &&
        fd_limit.rlim_cur < (rlim_t)max_fd)
        max_fd = (int)fd_limit.rlim_cur;
    if (pipe(pipe_fds) != 0) return -1;

    child = fork();
    if (child < 0) {
        int saved_errno = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        errno = saved_errno;
        return -1;
    }
    if (child == 0) {
        char pipe_text[32];
        char tty_text[32];
        char display_text[32];
        char keyboard_text[32];
        int fd;

        close(pipe_fds[1]);
        (void)fcntl(pipe_fds[0], F_SETFD, 0);
        (void)fcntl(vt_fd, F_SETFD, 0);

        for (fd = 3; fd < max_fd; ++fd) {
            if (fd != pipe_fds[0] && fd != vt_fd) close(fd);
        }

        snprintf(pipe_text, sizeof(pipe_text), "%d", pipe_fds[0]);
        snprintf(tty_text, sizeof(tty_text), "%d", vt_fd);
        snprintf(display_text, sizeof(display_text), "%d",
                 vt_saved_display_mode);
        snprintf(keyboard_text, sizeof(keyboard_text), "%d",
                 vt_saved_keyboard_mode);
        execl("/proc/self/exe", "primeshim-vt-guardian", "--vt-guardian",
              pipe_text, tty_text, display_text, keyboard_text, NULL);
        _exit(127);
    }

    close(pipe_fds[0]);
    vt_guardian_write_fd = pipe_fds[1];
    vt_guardian_pid = child;
    (void)fcntl(vt_guardian_write_fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static int acquire_active_vt(void)
{
    struct vt_stat state;
    char path[32];
    int control_fd;

    if (vt_fd >= 0) return 0;

    control_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (control_fd < 0) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot open /dev/tty0");
        return -1;
    }
    if (ioctl(control_fd, VT_GETSTATE, &state) != 0) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot query active VT");
        close(control_fd);
        return -1;
    }
    close(control_fd);

    snprintf(path, sizeof(path), "/dev/tty%u", state.v_active);
    vt_fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (vt_fd < 0) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot open %s", path);
        return -1;
    }
    vt_number = state.v_active;

    if (ioctl(vt_fd, KDGETMODE, &vt_saved_display_mode) != 0) {
        PS_LOG_ERRNO_ERROR("display", errno,
                           "cannot query tty%d display mode", vt_number);
        release_vt();
        return -1;
    }
    vt_display_mode_saved = 1;

    if (ioctl(vt_fd, KDGKBMODE, &vt_saved_keyboard_mode) == 0) {
        vt_keyboard_mode_saved = 1;
        /* A text VT left in K_RAW/K_OFF is generally residue from an
         * interrupted graphics owner and cannot operate as a console. */
        if (vt_saved_display_mode == KD_TEXT &&
            (vt_saved_keyboard_mode == K_RAW ||
             vt_saved_keyboard_mode == K_OFF)) {
            PS_LOG_WARN("display",
                        "tty%d has stale keyboard mode %d; restore K_XLATE",
                        vt_number, vt_saved_keyboard_mode);
            vt_saved_keyboard_mode = K_XLATE;
        }
    } else {
        PS_LOG_ERRNO_WARN("display", errno,
                          "cannot query tty%d keyboard mode", vt_number);
    }

    /* KD_GRAPHICS is the ownership boundary with fbcon: while it is set,
     * the console must not draw into the scanout buffer that we reuse. */
    if (ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) != 0) {
        PS_LOG_ERRNO_ERROR("display", errno,
                           "cannot put tty%d in graphics mode", vt_number);
        release_vt();
        return -1;
    }

    /* PrimeShim reads the keypad through evdev.  Keep those keys out of the
     * VT line discipline while the graphical application is active. */
    if (vt_keyboard_mode_saved && ioctl(vt_fd, KDSKBMODE, K_OFF) != 0) {
        PS_LOG_ERRNO_WARN("display", errno,
                          "cannot disable tty%d keyboard input", vt_number);
    }

    if (!vt_atexit_registered) {
        if (atexit(release_vt_at_exit) != 0) {
            PS_LOG_ERROR("display", "cannot register VT exit cleanup");
            release_vt();
            return -1;
        }
        vt_atexit_registered = 1;
    }
    if (install_vt_signal_handlers() != 0) {
        PS_LOG_ERRNO_ERROR("display", errno,
                           "cannot install VT signal cleanup");
        release_vt();
        return -1;
    }
    if (spawn_vt_guardian() != 0) {
        PS_LOG_ERRNO_ERROR("display", errno,
                           "cannot start VT restoration guardian");
        release_vt();
        return -1;
    }

    PS_LOG_INFO("display", "acquired tty%d in graphics mode", vt_number);
    return 0;
}

static int reuse_active_drm_framebuffer(void)
{
    struct drm_mode_map_dumb map_request;
    drmModeFB *framebuffer;

    if (!drm_saved_crtc || !drm_saved_crtc->buffer_id) return -1;
    framebuffer = drmModeGetFB(drm_fd, drm_saved_crtc->buffer_id);
    if (!framebuffer) return -1;
    if (framebuffer->width != 320 || framebuffer->height != 240 ||
        framebuffer->bpp != 32 || framebuffer->pitch != 320 * 4 ||
        !framebuffer->handle) {
        drmModeFreeFB(framebuffer);
        errno = EINVAL;
        return -1;
    }

    memset(&map_request, 0, sizeof(map_request));
    map_request.handle = framebuffer->handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) != 0) {
        drmModeFreeFB(framebuffer);
        return -1;
    }

    drm_map_size = (uint64_t)framebuffer->pitch * framebuffer->height;
    hw_buffer_ptr = mmap(NULL, drm_map_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, drm_fd, map_request.offset);
    if (hw_buffer_ptr == MAP_FAILED) {
        hw_buffer_ptr = NULL;
        drmModeFreeFB(framebuffer);
        return -1;
    }

    drm_fb_id = framebuffer->fb_id;
    drm_dumb_handle = framebuffer->handle;
    drm_buffer_owned = 0;
    hw_width = framebuffer->width;
    hw_height = framebuffer->height;
    hw_bpp = framebuffer->bpp;
    hw_stride = framebuffer->pitch;
    drmModeFreeFB(framebuffer);
    return 0;
}


//==============================================================================
// Public API
//==============================================================================

void SDKLIB_LCDOn() {
    if (current_mode != DISPLAY_MODE_NONE) {
        PS_LOG_DEBUG("display", "LCD is already initialized");
        return;
    }

    // Stop fbcon before mapping or clearing its active scanout buffer.
    if (acquire_active_vt() != 0) return;

    // --- Stage 1: Attempt DRM Initialization ---
    if (init_drm() == 0) {
        // Check for optimal conditions for Zero-Copy
        if (hw_width == 320 && hw_height == 240 && hw_bpp == 32 && hw_stride == (320 * 4)) {
            PS_LOG_INFO("display", "using DRM zero-copy mode");
            current_mode = DISPLAY_MODE_DRM_ZERO_COPY;
            // Point the shim's buffer directly to the hardware VRAM
            LcdMagic.window1_bufferstart = (uint32_t)(uintptr_t)hw_buffer_ptr;
        } else {
            PS_LOG_INFO("display",
                        "DRM config %dx%d@%dbpp stride=%d; using render thread",
                        hw_width, hw_height, hw_bpp, hw_stride);
            current_mode = DISPLAY_MODE_DRM_RENDER_THREAD;
        }
    }
    // --- Stage 2: Fallback to Framebuffer if DRM fails ---
    else if (init_fb() == 0) {
        PS_LOG_WARN("display", "DRM unavailable; falling back to fbdev");
        current_mode = DISPLAY_MODE_FB_RENDER_THREAD;
    }
    // --- Stage 3: All initialization failed ---
    else {
        PS_LOG_ERROR("display", "all display initialization methods failed");
        release_vt();
        return;
    }

    // --- Configure LCD_MAGIC structure ---
    LcdMagic.SomeVal = 0x5850;
    LcdMagic.x_res = 320;
    LcdMagic.y_res = 240;
    LcdMagic.pixel_bits = sizeof(buffer_pixel_t) * 8; // Always 32-bit from app's perspective
    LcdMagic.brightness_level = 2;
    LcdMagic.unk1_0 = 8;

    // --- Start Render Thread if required (for non-zero-copy modes) ---
    if (current_mode == DISPLAY_MODE_DRM_RENDER_THREAD || current_mode == DISPLAY_MODE_FB_RENDER_THREAD) {
        shadow_buffer = (buffer_pixel_t*)malloc(320 * 240 * sizeof(buffer_pixel_t));
        if (!shadow_buffer) {
            PS_LOG_ERROR("display", "cannot allocate shadow buffer");
            cleanup_resources();
            return;
        }
        LcdMagic.window1_bufferstart = (uint32_t)(uintptr_t)shadow_buffer;
        LcdMagic.bytes_per_line = 320 * sizeof(buffer_pixel_t);

        update_running = 1;
        if (pthread_create(&update_thread, NULL, update_thread_func, NULL) != 0) {
            PS_LOG_ERROR("display", "cannot create render thread");
            cleanup_resources();
        }
    } else { // Zero-Copy mode
        LcdMagic.bytes_per_line = hw_stride; // Use the actual hardware stride
    }
}

void SDKLIB_LCDOff() {
    if (current_mode == DISPLAY_MODE_NONE) return;
    cleanup_resources();
}

void SDKLIB_ClearScreen() {
    if (current_mode == DISPLAY_MODE_NONE) return;

    if (current_mode == DISPLAY_MODE_DRM_ZERO_COPY) {
        // Write directly to hardware buffer
        memset(hw_buffer_ptr, 0, drm_map_size);
    } else if (shadow_buffer) {
        // Write to software buffer, thread will push it to screen
        pthread_mutex_lock(&buffer_mutex);
        memset(shadow_buffer, 0, 320 * 240 * sizeof(buffer_pixel_t));
        pthread_mutex_unlock(&buffer_mutex);
    }
}

struct LCD_MAGIC** SDKLIB_GetActiveLCD() {
    if (current_mode == DISPLAY_MODE_NONE) {
        SDKLIB_LCDOn();
    }
    static struct LCD_MAGIC* pLcdMagic = &LcdMagic;
    return &pLcdMagic;
}

//==============================================================================
// Initialization & Cleanup
//==============================================================================

static int init_drm() {
    PS_LOG_DEBUG("display", "probing DRM device");
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        PS_LOG_ERRNO_DEBUG("display", errno, "cannot open /dev/dri/card0");
        return -1;
    }
    if (drmSetMaster(drm_fd) != 0) {
        PS_LOG_ERRNO_ERROR("display", errno,
                           "cannot acquire DRM master on /dev/dri/card0");
        close(drm_fd);
        drm_fd = -1;
        return -1;
    }
    drm_master_owned = 1;

    drm_res = drmModeGetResources(drm_fd);
    if (!drm_res) {
        PS_LOG_ERRNO_DEBUG("display", errno, "cannot get DRM resources");
        close(drm_fd); drm_fd = -1; drm_master_owned = 0; return -1;
    }

    for (int i = 0; i < drm_res->count_connectors; i++) {
        drm_conn = drmModeGetConnector(drm_fd, drm_res->connectors[i]);
        if (drm_conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(drm_conn); drm_conn = NULL;
    }

    if (!drm_conn || drm_conn->count_modes == 0) {
        PS_LOG_WARN("display", "no connected DRM connector with modes");
        if(drm_conn) drmModeFreeConnector(drm_conn);
        drmModeFreeResources(drm_res);
        close(drm_fd); drm_fd = -1; drm_master_owned = 0; return -1;
    }

    drmModeModeInfo mode = drm_conn->modes[0];
    uint32_t crtc_id = drm_res->crtcs[0];

    drm_saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);
    if (reuse_active_drm_framebuffer() == 0) {
        PS_LOG_INFO("display",
                    "reusing active DRM framebuffer at %dx%d@%dbpp",
                    hw_width, hw_height, hw_bpp);
        return 0;
    }
    PS_LOG_DEBUG("display",
                 "active framebuffer is not reusable; allocating scanout");

    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = mode.hdisplay;
    creq.height = mode.vdisplay;
    creq.bpp = 32; // Always request 32-bit for best compatibility with our app
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot create DRM dumb buffer");
        // Cleanup and fail
        drmModeFreeConnector(drm_conn);
        drmModeFreeResources(drm_res);
        close(drm_fd); drm_fd = -1; drm_master_owned = 0; return -1;
    }

    drm_dumb_handle = creq.handle;
    drm_buffer_owned = 1;

    if (drmModeAddFB(drm_fd, creq.width, creq.height, 24, 32, creq.pitch, creq.handle, &drm_fb_id)) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot add DRM framebuffer");
        // No need to destroy dumb buffer, it's tied to fd
        drmModeFreeConnector(drm_conn);
        drmModeFreeResources(drm_res);
        close(drm_fd); drm_fd = -1; drm_master_owned = 0; return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    drm_map_size = creq.size;
    hw_buffer_ptr = mmap(0, drm_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mreq.offset);
    if (hw_buffer_ptr == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot map DRM framebuffer");
        // ... full cleanup ...
        return -1;
    }

    drmModeSetCrtc(drm_fd, crtc_id, drm_fb_id, 0, 0, &drm_conn->connector_id, 1, &mode);

    // Populate hardware info
    hw_width = mode.hdisplay;
    hw_height = mode.vdisplay;
    hw_bpp = creq.bpp;
    hw_stride = creq.pitch;

    PS_LOG_INFO("display", "DRM initialized at %dx%d@%dbpp", hw_width,
                hw_height, hw_bpp);
    return 0;
}

static int init_fb() {
    PS_LOG_DEBUG("display", "probing /dev/fb0");
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        PS_LOG_ERRNO_DEBUG("display", errno, "cannot open /dev/fb0");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

    fb_screensize = finfo.line_length * vinfo.yres;
    hw_buffer_ptr = mmap(0, fb_screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (hw_buffer_ptr == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("display", errno, "cannot map fbdev framebuffer");
        close(fb_fd); fb_fd = -1;
        return -1;
    }

    hw_width = vinfo.xres;
    hw_height = vinfo.yres;
    hw_bpp = vinfo.bits_per_pixel;
    hw_stride = finfo.line_length;

    PS_LOG_INFO("display", "fbdev initialized at %dx%d@%dbpp", hw_width,
                hw_height, hw_bpp);
    return 0;
}

static void cleanup_resources() {
    // --- Stop render thread if it's running ---
    if (update_running) {
        update_running = 0;
        pthread_join(update_thread, NULL);
    }

    // --- Free software buffer if it was allocated ---
    if (shadow_buffer) {
        free(shadow_buffer);
        shadow_buffer = NULL;
    }

    // --- Cleanup based on the active mode ---
    switch(current_mode) {
        case DISPLAY_MODE_DRM_ZERO_COPY:
        case DISPLAY_MODE_DRM_RENDER_THREAD:
            if (drm_saved_crtc) {
                if (drm_buffer_owned) {
                    drmModeSetCrtc(drm_fd, drm_saved_crtc->crtc_id,
                                   drm_saved_crtc->buffer_id,
                                   drm_saved_crtc->x, drm_saved_crtc->y,
                                   &drm_conn->connector_id, 1,
                                   &drm_saved_crtc->mode);
                }
                drmModeFreeCrtc(drm_saved_crtc);
                drm_saved_crtc = NULL;
            }
            if (hw_buffer_ptr) munmap(hw_buffer_ptr, drm_map_size);

            if (drm_buffer_owned && drm_fb_id) {
                drmModeRmFB(drm_fd, drm_fb_id);
            }
            if (drm_buffer_owned && drm_dumb_handle) {
                struct drm_mode_destroy_dumb dreq = { .handle = drm_dumb_handle };
                ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
            }

            if (drm_conn) drmModeFreeConnector(drm_conn);
            if (drm_res) drmModeFreeResources(drm_res);
            if (drm_master_owned && drm_fd >= 0) drmDropMaster(drm_fd);
            if (drm_fd >= 0) close(drm_fd);
            break;

        case DISPLAY_MODE_FB_RENDER_THREAD:
            if (hw_buffer_ptr) munmap(hw_buffer_ptr, fb_screensize);
            if (fb_fd >= 0) close(fb_fd);
            break;

        case DISPLAY_MODE_NONE:
             // Nothing to do
            break;
    }

    // --- Reset all global state variables ---
    current_mode = DISPLAY_MODE_NONE;
    hw_buffer_ptr = NULL;
    drm_fd = -1;
    fb_fd = -1;
    drm_fb_id = 0;
    drm_dumb_handle = 0;
    drm_map_size = 0;
    drm_buffer_owned = 0;
    drm_master_owned = 0;
    drm_conn = NULL;
    drm_res = NULL;
    release_vt();
    PS_LOG_INFO("display", "cleanup complete");
}
//==============================================================================
// Render Thread (For Fallback Modes)
//==============================================================================

static void* update_thread_func(void* arg) {
    (void)arg;
    PS_LOG_DEBUG("display", "render thread started");
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 33000000}; // ~30 FPS

    // Allocate a temporary buffer for the locked copy
    buffer_pixel_t *local_display_buffer = malloc(320 * 240 * sizeof(buffer_pixel_t));
    if (!local_display_buffer) {
        PS_LOG_ERROR("display", "render thread cannot allocate local buffer");
        return NULL;
    }

    while (update_running) {
        // 1. Copy Shadow to a local buffer to minimize lock time
        pthread_mutex_lock(&buffer_mutex);
        memcpy(local_display_buffer, shadow_buffer, 320 * 240 * sizeof(buffer_pixel_t));
        pthread_mutex_unlock(&buffer_mutex);

        // 2. Render to hardware (pixel conversion if necessary)
        for (int y = 0; y < 240 && y < hw_height; y++) {
            buffer_pixel_t *src_row = local_display_buffer + (y * 320);
            uint8_t *dest_row = (uint8_t*)hw_buffer_ptr + (y * hw_stride);

            if (hw_bpp == 32) { // 32bpp -> 32bpp (XRGB8888)
                uint32_t *d = (uint32_t*)dest_row;
                for (int x = 0; x < 320 && x < hw_width; x++) {
                    d[x] = src_row[x]; // Direct copy
                }
            } else if (hw_bpp == 16) { // 32bpp -> 16bpp (RGB565)
                uint16_t *d = (uint16_t*)dest_row;
                for (int x = 0; x < 320 && x < hw_width; x++) {
                    uint32_t s = src_row[x];
                    uint8_t r = (s >> 16) & 0xFF;
                    uint8_t g = (s >> 8)  & 0xFF;
                    uint8_t b = (s)       & 0xFF;
                    d[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
            } else if (hw_bpp == 24) { // 32bpp -> 24bpp (RGB888)
                 for (int x = 0; x < 320 && x < hw_width; x++) {
                    uint32_t s = src_row[x];
                    dest_row[x*3 + 0] = s & 0xFF;         // B
                    dest_row[x*3 + 1] = (s >> 8) & 0xFF;  // G
                    dest_row[x*3 + 2] = (s >> 16) & 0xFF; // R
                 }
            }
        }

        // 3. For DRM, mark the buffer as dirty to force an update
        if (current_mode == DISPLAY_MODE_DRM_RENDER_THREAD) {
            drmModeDirtyFB(drm_fd, drm_fb_id, NULL, 0);
        }

        nanosleep(&ts, NULL);
    }

    free(local_display_buffer);
    PS_LOG_DEBUG("display", "render thread exiting");
    return NULL;
}

void LCD_drawcursor(){}

static int device_display_start(void)
{
    SDKLIB_LCDOn();
    return current_mode == DISPLAY_MODE_NONE ? -1 : 0;
}

static const primeshim_peripheral_driver_t device_display_driver = {
    .name = "device-drm-fbdev",
    .classes = PRIMESHIM_PERIPHERAL_DISPLAY,
    .required = true,
    .probe = NULL,
    .start = device_display_start,
    .stop = SDKLIB_LCDOff
};

PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(device_display_driver)

#endif
