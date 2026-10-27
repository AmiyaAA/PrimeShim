package dev.primeshim.android;

/** Thin JNI facade: everything below is implemented in libprimeshim.so. */
public final class PrimeShim {
    static {
        System.loadLibrary("primeshim");
    }

    private PrimeShim() {
    }

    /** Log levels mirror include/log.h. */
    public static final int LOG_ERROR = 0;
    public static final int LOG_WARN = 1;
    public static final int LOG_INFO = 2;
    public static final int LOG_DEBUG = 3;
    public static final int LOG_TRACE = 4;
    public static final int LOG_OFF = 5;

    public static final int TOUCH_DOWN = 1;
    public static final int TOUCH_MOVE = 2;
    public static final int TOUCH_UP = 3;

    /**
     * Starts the guest firmware on a background thread.
     *
     * @param firmwarePath absolute path to armfir.elf
     * @param workDir      working directory; firmware drives are resolved
     *                     relative to it (see PRIMESHIM_DATA_DIR)
     * @param logLevel     one of the LOG_* constants
     * @return 0 on success
     */
    public static native int nativeStart(String firmwarePath, String workDir, int logLevel);

    /**
     * Turns a user-selected file into a loadable firmware image.
     *
     * Accepts either an already extracted armfir.elf or an HP Prime update
     * container (PRIME_APP.DAT / prime-app.dat): the native side finds the
     * embedded ARM ELF and writes a validated copy to {@code destinationPath}.
     *
     * @return null on success, otherwise a human readable reason
     */
    public static native String nativeImportFirmware(String sourcePath,
                                                     String destinationPath);

    /** Live 320x240 framebuffer as 0x00RRGGBB pixels, or null before start. */
    public static native int[] nativeSnapshot();

    /** Recent runtime output (log.c records plus any crash dump). */
    public static native String nativeLog();

    public static native void nativeClearLog();

    /** Log every patched syscall (name + r0-r3) until switched off. */
    public static native void nativeSetTraceCalls(boolean enabled);

    /** Force the next composition to post a full frame. */
    public static native void nativeRequestRedraw();

    /** Detaches the surface; the guest keeps running until the process dies. */
    public static native void nativeStop();

    public static native void nativeSetSurface(android.view.Surface surface);

    /** action is TOUCH_*, coordinates are already in firmware pixels. */
    public static native void nativeTouch(int action, int x, int y);

    /** keycode is an Android KeyEvent keycode, not a Prime keycode. */
    public static native void nativeKey(int androidKeycode, int pressed);

    /**
     * One Android MotionEvent as a single firmware TOUCH batch.
     * actions/slots/xs/ys are parallel arrays; slots are 0..7.
     */
    public static native void nativeTouchMulti(int[] actions, int[] slots,
                                               int[] xs, int[] ys);

    /** Same, but encoded as a top-level key event instead of a KEY_BATCH. */
    public static native void nativeKeyDirect(int primeKeycode, int pressed);

    /** Emulation speed in percent (100 = the cadence used so far). */
    public static native void nativeSetSpeed(int percent);

    /** KEY_BATCH envelope with the Prime key code sent untouched. */
    public static native void nativeKeyBatch(int primeKeycode, int pressed);

    public static native void nativeSetLogLevel(int level);

    public static native int nativeExitCode();

    /** Non-zero signal number if the guest thread faulted and was unwound. */
    public static native int nativeCrashSignal();

    /** Full crash report written by the guest signal handler, or null. */
    public static native String nativeCrashReport();
}
