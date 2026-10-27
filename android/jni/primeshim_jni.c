/*
 * JNI bridge for the Android front-end.
 *
 * The whole runtime (firmware loader, patcher, shim, VFS) lives in
 * libprimeshim.so.  Java owns the lifecycle: it extracts the firmware from
 * assets, calls nativeStart() with a working directory, and feeds input.
 * Firmware execution happens on its own pthread because primeshim_main()
 * does not return until the guest exits.
 */
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

#include "address_window.h"
#include "firmware_image.h"
#include "runtime_patcher.h"
#include "log_capture.h"
#include "log.h"
#include "primeshim_android.h"

/* src/main.c is compiled with -Dmain=primeshim_main so it can be called as a
 * library entry point instead of a process entry point. */
extern int primeshim_main(int argc, char **argv);

#define TAG "PrimeShim"

static pthread_t vm_thread;
static int vm_thread_started;
static int vm_exit_code;

/* ---- guest crash capture -------------------------------------------- */
/*
 * The firmware runs on vm_thread.  A fault in the guest would normally take
 * the whole app down, so arm a sigsetjmp() on that thread and let a signal
 * handler write a report to stderr (captured by log_capture) and unwind back
 * to vm_thread_main.  The app stays alive; MainActivity polls
 * nativeCrashSignal() and shows the report.
 *
 * Only the guest thread is armed.  Faults on the UI/Java threads keep the
 * default Android behaviour.
 */
static __thread sigjmp_buf *tls_crash_jmp;
static __thread volatile sig_atomic_t tls_crash_armed;
static volatile sig_atomic_t last_crash_signal;
static volatile uintptr_t last_crash_address;
static volatile size_t crash_text_length;
static char crash_text[8192];

static void crash_write(const char *text, size_t length) {
    while (length > 0) {
        ssize_t written = write(STDERR_FILENO, text, length);
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}

static void crash_puts(const char *text) {
    size_t length = strlen(text);
    size_t offset = crash_text_length;
    if (offset + length < sizeof(crash_text)) {
        memcpy(crash_text + offset, text, length);
        crash_text_length = offset + length;
        crash_text[crash_text_length] = '\0';
    }
    crash_write(text, length);
}

static void crash_format(const char *format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    crash_puts(buffer);
}

static const char *crash_signal_name(int number) {
    switch (number) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default:      return "signal";
    }
}

static _Unwind_Reason_Code crash_unwind(struct _Unwind_Context *context,
                                        void *opaque) {
    int *count = opaque;
    uintptr_t pc = (uintptr_t)_Unwind_GetIP(context);
    if (pc != 0) {
        crash_format("  #%02d pc=%p\n", *count, (void *)pc);
        ++*count;
    }
    return *count >= 24 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void crash_handler(int signal_number, siginfo_t *information,
                          void *opaque_context) {
    if (!tls_crash_armed || !tls_crash_jmp) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(signal_number, &action, NULL);
        raise(signal_number);
        return;
    }

    tls_crash_armed = 0;
    crash_text_length = 0;
    crash_text[0] = '\0';
    crash_puts("\n*** PRIMESHIM GUEST CRASH ***\n");
    crash_format("signal: %d (%s)\n", signal_number,
                 crash_signal_name(signal_number));
    crash_format("fault address: %p\n",
                 information ? (void *)information->si_addr : NULL);
    crash_format("thread: %lu\n", (unsigned long)pthread_self());
#if defined(__arm__)
    if (opaque_context) {
        const ucontext_t *context = opaque_context;
        crash_format("pc=%p lr=%p sp=%p cpsr=%08lx\n",
                     (void *)(uintptr_t)context->uc_mcontext.arm_pc,
                     (void *)(uintptr_t)context->uc_mcontext.arm_lr,
                     (void *)(uintptr_t)context->uc_mcontext.arm_sp,
                     (unsigned long)context->uc_mcontext.arm_cpsr);
    }
#endif
    crash_puts("backtrace:\n");
    {
        int count = 0;
        _Unwind_Backtrace(crash_unwind, &count);
    }
    crash_puts("*** end of crash report ***\n");

    last_crash_address = information ?
                         (uintptr_t)information->si_addr : 0;
    last_crash_signal = signal_number;
    siglongjmp(*tls_crash_jmp, 1);
}

static void install_crash_handlers(void) {
    static int installed;
    struct sigaction action;

    if (installed) return;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGTRAP, &action, NULL);
    installed = 1;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm;
    (void)reserved;
    /* Claim 0x30600000 before ART grows into it; the loader asks for it back
     * immediately before mapping the firmware image. */
    primeshim_address_window_reserve(PRIMESHIM_FW_WINDOW_BASE,
                                     PRIMESHIM_FW_WINDOW_SIZE);
    return JNI_VERSION_1_6;
}

/* ---- Android keycode -> HP Prime keycode ----------------------------
 *
 * The values mirror the firmware's own keycodes (see the Linux evdev table in
 * src/keyboard.c).  Letters follow the QWERTY layout the calculator's
 * keyboard-style input expects. */
static int map_android_key(int code) {
    switch (code) {
    /* digits */
    case AKEYCODE_0: return 0x30;
    case AKEYCODE_1: return 0x59;
    case AKEYCODE_2: return 0x5A;
    case AKEYCODE_3: return 0x33;
    case AKEYCODE_4: return 0x55;
    case AKEYCODE_5: return 0x56;
    case AKEYCODE_6: return 0x57;
    case AKEYCODE_7: return 0x51;
    case AKEYCODE_8: return 0x52;
    case AKEYCODE_9: return 0x53;

    /* arithmetic + separators */
    case AKEYCODE_NUMPAD_ADD:
    case AKEYCODE_PLUS: return 0xB7;
    case AKEYCODE_MINUS:
    case AKEYCODE_NUMPAD_SUBTRACT: return 0xB9;
    case AKEYCODE_STAR:
    case AKEYCODE_NUMPAD_MULTIPLY: return 0x58;
    case AKEYCODE_SLASH:
    case AKEYCODE_NUMPAD_DIVIDE: return 0x54;
    case AKEYCODE_COMMA: return 0x4F;
    case AKEYCODE_PERIOD:
    case AKEYCODE_NUMPAD_DOT: return 0xB8;
    case AKEYCODE_EQUALS:
    case AKEYCODE_NUMPAD_EQUALS: return 0x0D;

    /* navigation / editing */
    case AKEYCODE_DPAD_LEFT: return 0x02;
    case AKEYCODE_DPAD_UP: return 0x03;
    case AKEYCODE_DPAD_RIGHT: return 0x04;
    case AKEYCODE_DPAD_DOWN: return 0x05;
    case AKEYCODE_DEL: return 0x0C;
    case AKEYCODE_ENTER:
    case AKEYCODE_NUMPAD_ENTER: return 0x0D;
    case AKEYCODE_SPACE: return 0x20;
    case AKEYCODE_ESCAPE: return 0x01;
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT: return 0x8B;

    /* letters double as the calculator's function keys */
    case AKEYCODE_A: return 0x41; /* var   */
    case AKEYCODE_B: return 0x42; /* toolbox */
    case AKEYCODE_C: return 0x43; /* template */
    case AKEYCODE_D: return 0x44; /* x     */
    case AKEYCODE_E: return 0x45; /* a b/c */
    case AKEYCODE_F: return 0x46; /* ^     */
    case AKEYCODE_G: return 0x47; /* sin   */
    case AKEYCODE_H: return 0x48; /* cos   */
    case AKEYCODE_I: return 0x49; /* tan   */
    case AKEYCODE_J: return 0x4A; /* ln    */
    case AKEYCODE_K: return 0x4B; /* log   */
    case AKEYCODE_L: return 0x4C; /* x^2   */
    case AKEYCODE_M: return 0x4D; /* +/-   */
    case AKEYCODE_N: return 0x4E; /* ()    */
    case AKEYCODE_O: return 0x4F; /* ,     */
    case AKEYCODE_P: return 0x50; /* 1e    */
    case AKEYCODE_Q: return 0x51; /* 7     */
    case AKEYCODE_R: return 0x52; /* 8     */
    case AKEYCODE_S: return 0x53; /* 9     */
    case AKEYCODE_T: return 0x54; /* /     */
    case AKEYCODE_U: return 0x55; /* 4     */
    case AKEYCODE_V: return 0x56; /* 5     */
    case AKEYCODE_W: return 0x57; /* 6     */
    case AKEYCODE_X: return 0x58; /* *     */
    case AKEYCODE_Y: return 0x59; /* 1     */
    case AKEYCODE_Z: return 0x5A; /* 2     */

    /* function row -> top-row calculator keys */
    case AKEYCODE_F1: return 0x91;  /* Symb  */
    case AKEYCODE_F2: return 0x93;  /* Msg   */
    case AKEYCODE_F3: return 0xB2;  /* Plot  */
    case AKEYCODE_F4: return 0xB3;  /* Num   */
    case AKEYCODE_F5: return 0xB4;  /* View  */
    case AKEYCODE_F6: return 0xB5;  /* CAS   */
    case AKEYCODE_F7: return 0xB6;  /* Alpha */
    case AKEYCODE_F8: return 0x95;  /* Help  */
    case AKEYCODE_F9:
    case AKEYCODE_MENU: return 0xB1; /* Apps  */
    case AKEYCODE_F10:
    case AKEYCODE_HOME: return 0xE047; /* Home/apps (G1 extended) */
    case AKEYCODE_CTRL_LEFT:
    case AKEYCODE_CTRL_RIGHT: return 0x83; /* ON */
    default: return -1;
    }
}

/* ---- lifecycle ------------------------------------------------------ */

struct start_args {
    char *firmware;
    char *workdir;
};

static void *vm_thread_main(void *opaque) {
    struct start_args *args = opaque;
    char *argv[4];
    static __thread unsigned char alternate_stack[64 * 1024];
    stack_t stack;
    sigjmp_buf environment;

    if (args->workdir && chdir(args->workdir) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot chdir to %s", args->workdir);
    }

    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    sigaltstack(&stack, NULL);

    argv[0] = (char *)"primeshim";
    argv[1] = (char *)"--firmware";
    argv[2] = args->firmware;
    argv[3] = NULL;

    tls_crash_jmp = &environment;
    tls_crash_armed = 1;
    if (sigsetjmp(environment, 1) == 0) {
        vm_exit_code = primeshim_main(3, argv);
        tls_crash_armed = 0;
    } else {
        /* The signal handler unwound the guest stack.  The firmware state is
         * no longer usable, but this process (and the Java UI) survives so
         * the crash report can be shown. */
        vm_exit_code = 128 + (int)last_crash_signal;
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "firmware crashed: signal %d at %p",
                            (int)last_crash_signal,
                            (void *)last_crash_address);
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "firmware exited with %d", vm_exit_code);
    return NULL;
}

static char *copy_string(JNIEnv *env, jstring text) {
    const char *chars;
    char *copy;

    if (!text) return NULL;
    chars = (*env)->GetStringUTFChars(env, text, NULL);
    if (!chars) return NULL;
    copy = strdup(chars);
    (*env)->ReleaseStringUTFChars(env, text, chars);
    return copy;
}

JNIEXPORT jint JNICALL
Java_dev_primeshim_android_PrimeShim_nativeStart(JNIEnv *env, jclass clazz,
                                                 jstring firmware_path,
                                                 jstring work_dir, jint log_level) {
    struct start_args *args;
    (void)clazz;

    if (vm_thread_started) return 0;

    /* Forget any crash from a previous import attempt before arming the
     * guest watcher. */
    last_crash_signal = 0;
    last_crash_address = 0;
    crash_text_length = 0;
    crash_text[0] = '\0';

    args = calloc(1, sizeof(*args));
    if (!args) return -1;
    args->firmware = copy_string(env, firmware_path);
    args->workdir = copy_string(env, work_dir);
    if (!args->firmware) {
        free(args->workdir);
        free(args);
        return -1;
    }

    /* Arm the guest-thread crash handler before the runtime installs its own
     * SIGSEGV/SIGILL/SIGSYS fault handlers; runtime_patcher saves ours as the
     * previous action, so unhandled guest faults unwind here instead of
     * killing the process. */
    install_crash_handlers();

    /* Everything the runtime writes to stderr (including the crash reporter)
     * becomes visible in the on-screen log panel. */
    primeshim_logcapture_start();

    /* primeshim_main() calls primeshim_log_init(), which re-reads the
     * environment and would silently drop the level set here - so publish it
     * as PRIMESHIM_LOG_LEVEL before the guest thread starts. */
    if (log_level >= 0 && log_level <= PRIMESHIM_LOG_OFF) {
        static const char *const names[] = {"error", "warn", "info", "debug",
                                            "trace", "off"};
        setenv("PRIMESHIM_LOG_LEVEL", names[log_level], 1);
        primeshim_log_set_level((primeshim_log_level_t)log_level);
    }

    if (pthread_create(&vm_thread, NULL, vm_thread_main, args) != 0) {
        free(args->firmware);
        free(args->workdir);
        free(args);
        return -1;
    }
    vm_thread_started = 1;
    return 0;
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeStop(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    /* The firmware has no cooperative shutdown path: the guest never returns
     * from its main loop, so Android kills the process when the activity is
     * destroyed.  We still detach the surface so the compose thread stops
     * touching it. */
    primeshim_android_set_window(NULL);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeSetSurface(JNIEnv *env, jclass clazz,
                                                      jobject surface) {
    ANativeWindow *window = NULL;
    (void)clazz;

    if (surface) window = ANativeWindow_fromSurface(env, surface);
    primeshim_android_set_window(window); /* takes its own reference */
    if (window) ANativeWindow_release(window);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeKeyDirect(JNIEnv *env, jclass clazz,
                                                     jint key, jint pressed) {
    (void)env;
    (void)clazz;
    extern void enqueue_key_packet_direct(int, int);
    enqueue_key_packet_direct((int)key, (int)pressed);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeSetSpeed(JNIEnv *env, jclass clazz,
                                                    jint percent) {
    (void)env;
    (void)clazz;
    extern void primeshim_set_speed_percent(int);
    primeshim_set_speed_percent((int)percent);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeKeyBatch(JNIEnv *env, jclass clazz,
                                                    jint prime_key, jint pressed) {
    (void)env;
    (void)clazz;
    /* KEY_BATCH envelope (the only form this firmware dispatches) with the key
     * code passed through untouched -- unlike nativeKey, no Android remap. */
    primeshim_android_key((int)prime_key, (int)pressed);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeTouch(JNIEnv *env, jclass clazz,
                                                 jint action, jint x, jint y) {
    (void)env;
    (void)clazz;
    primeshim_android_touch((int)action, (int)x, (int)y);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeTouchMulti(JNIEnv *env, jclass clazz,
                                                      jintArray actions,
                                                      jintArray slots,
                                                      jintArray xs,
                                                      jintArray ys) {
    jsize count;
    jint *action_values;
    jint *slot_values;
    jint *x_values;
    jint *y_values;
    jsize i;
    (void)clazz;

    if (!actions || !slots || !xs || !ys) return;
    count = (*env)->GetArrayLength(env, actions);
    if (count <= 0 ||
        (*env)->GetArrayLength(env, slots) != count ||
        (*env)->GetArrayLength(env, xs) != count ||
        (*env)->GetArrayLength(env, ys) != count) {
        return;
    }
    action_values = (*env)->GetIntArrayElements(env, actions, NULL);
    slot_values = (*env)->GetIntArrayElements(env, slots, NULL);
    x_values = (*env)->GetIntArrayElements(env, xs, NULL);
    y_values = (*env)->GetIntArrayElements(env, ys, NULL);
    if (action_values && slot_values && x_values && y_values) {
        for (i = 0; i < count; ++i) {
            primeshim_android_touch_update((int)action_values[i],
                                           (int)slot_values[i],
                                           (int)x_values[i],
                                           (int)y_values[i]);
        }
        primeshim_android_touch_commit();
    }
    if (action_values) (*env)->ReleaseIntArrayElements(env, actions, action_values, JNI_ABORT);
    if (slot_values) (*env)->ReleaseIntArrayElements(env, slots, slot_values, JNI_ABORT);
    if (x_values) (*env)->ReleaseIntArrayElements(env, xs, x_values, JNI_ABORT);
    if (y_values) (*env)->ReleaseIntArrayElements(env, ys, y_values, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeKey(JNIEnv *env, jclass clazz,
                                               jint android_keycode, jint pressed) {
    int key;
    (void)env;
    (void)clazz;

    key = map_android_key((int)android_keycode);
    if (key < 0) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "unmapped keycode %d", (int)android_keycode);
        return;
    }
    primeshim_android_key(key, (int)pressed);
}

static jstring crash_string(JNIEnv *env, const char *prefix) {
    char buffer[8448];
    if (crash_text_length > 0) {
        snprintf(buffer, sizeof(buffer), "%s\n%s", prefix, crash_text);
    } else {
        snprintf(buffer, sizeof(buffer), "%s (signal %d)", prefix,
                 (int)last_crash_signal);
    }
    return (*env)->NewStringUTF(env, buffer);
}

JNIEXPORT jstring JNICALL
Java_dev_primeshim_android_PrimeShim_nativeImportFirmware(JNIEnv *env, jclass clazz,
                                                          jstring source,
                                                          jstring destination) {
    char message[256];
    char *source_path;
    char *destination_path;
    sigjmp_buf environment;
    unsigned char *alternate = NULL;
    stack_t stack;
    int rc;
    (void)clazz;

    source_path = copy_string(env, source);
    destination_path = copy_string(env, destination);
    if (!source_path || !destination_path) {
        free(source_path);
        free(destination_path);
        return (*env)->NewStringUTF(env, "missing path");
    }

    /* The extractor walks attacker-controlled container offsets; a malformed
     * update package must not take the whole app down.  Arm the same signal
     * unwind used for the guest, but on this JNI thread. */
    install_crash_handlers();
    alternate = malloc(64 * 1024);
    if (alternate) {
        memset(&stack, 0, sizeof(stack));
        stack.ss_sp = alternate;
        stack.ss_size = 64 * 1024;
        sigaltstack(&stack, NULL);
    }
    tls_crash_jmp = &environment;
    tls_crash_armed = 1;
    if (sigsetjmp(environment, 1) == 0) {
        rc = primeshim_firmware_import(source_path, destination_path, message,
                                       sizeof(message));
        tls_crash_armed = 0;
    } else {
        tls_crash_armed = 0;
        rc = -1000; /* caught a SIGSEGV/SIGABRT while parsing the container */
    }
    if (alternate) {
        stack.ss_flags = SS_DISABLE;
        sigaltstack(&stack, NULL);
        free(alternate);
    }
    free(source_path);
    free(destination_path);
    if (rc == PRIMESHIM_FW_OK) return NULL;
    if (rc == -1000) return crash_string(env, "固件导入时崩溃：");
    return (*env)->NewStringUTF(env, message);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeSetLogLevel(JNIEnv *env, jclass clazz, jint level) {
    (void)env;
    (void)clazz;
    if (level >= 0 && level <= PRIMESHIM_LOG_OFF)
        primeshim_log_set_level((primeshim_log_level_t)level);
}

JNIEXPORT jintArray JNICALL
Java_dev_primeshim_android_PrimeShim_nativeSnapshot(JNIEnv *env, jclass clazz) {
    jintArray pixels;
    uint32_t *buffer;
    const jsize count = 320 * 240;
    (void)clazz;

    pixels = (*env)->NewIntArray(env, count);
    if (!pixels) return NULL;
    buffer = malloc(sizeof(uint32_t) * (size_t)count);
    if (!buffer) return NULL;
    primeshim_android_snapshot(buffer);
    (*env)->SetIntArrayRegion(env, pixels, 0, count, (const jint *)buffer);
    free(buffer);
    return pixels;
}

JNIEXPORT jstring JNICALL
Java_dev_primeshim_android_PrimeShim_nativeLog(JNIEnv *env, jclass clazz) {
    static char buffer[64 * 1024];
    (void)clazz;

    primeshim_logcapture_snapshot(buffer, sizeof(buffer));
    return (*env)->NewStringUTF(env, buffer);
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeClearLog(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    primeshim_logcapture_clear();
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeRequestRedraw(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    primeshim_android_request_redraw();
}

JNIEXPORT void JNICALL
Java_dev_primeshim_android_PrimeShim_nativeSetTraceCalls(JNIEnv *env, jclass clazz,
                                                         jint enabled) {
    (void)env;
    (void)clazz;
    primeshim_trace_calls = enabled ? 1 : 0;
    /* Trace records are INFO level; make sure they are not filtered out. */
    if (enabled && primeshim_log_get_level() < PRIMESHIM_LOG_DEBUG) {
        primeshim_log_set_level(PRIMESHIM_LOG_DEBUG);
    }
}

JNIEXPORT jint JNICALL
Java_dev_primeshim_android_PrimeShim_nativeExitCode(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return vm_thread_started ? vm_exit_code : -1;
}

JNIEXPORT jint JNICALL
Java_dev_primeshim_android_PrimeShim_nativeCrashSignal(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jint)last_crash_signal;
}

JNIEXPORT jstring JNICALL
Java_dev_primeshim_android_PrimeShim_nativeCrashReport(JNIEnv *env, jclass clazz) {
    (void)clazz;
    if (crash_text_length == 0) return NULL;
    return (*env)->NewStringUTF(env, crash_text);
}
