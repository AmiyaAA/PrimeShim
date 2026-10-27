#ifndef PRIMESHIM_PLATFORM_H
#define PRIMESHIM_PLATFORM_H

/* Set by the build system: exactly one host profile must be selected.
 *
 * PRIMESHIM_HOST_PC      - Linux desktop, ARM firmware under QEMU user mode,
 *                          display/input through the native SDL2 launcher.
 * PRIMESHIM_HOST_DEVICE  - native ARM Linux (calculator), DRM/fbdev + evdev + USB gadget.
 * PRIMESHIM_HOST_ANDROID - Android app, native ARM execution, ANativeWindow display
 *                          and input delivered over JNI.
 */
#ifndef PRIMESHIM_HOST_PC
#define PRIMESHIM_HOST_PC 0
#endif

#ifndef PRIMESHIM_HOST_DEVICE
#define PRIMESHIM_HOST_DEVICE 0
#endif

#ifndef PRIMESHIM_HOST_ANDROID
#define PRIMESHIM_HOST_ANDROID 0
#endif

#if (PRIMESHIM_HOST_PC + PRIMESHIM_HOST_DEVICE + PRIMESHIM_HOST_ANDROID) != 1
#error "Select exactly one PrimeShim host profile"
#endif

#ifndef PRIMESHIM_KEYBOARD_DEVICE
#define PRIMESHIM_KEYBOARD_DEVICE "/dev/input/event0"
#endif

#ifndef PRIMESHIM_TOUCH_DEVICE
#define PRIMESHIM_TOUCH_DEVICE "/dev/input/event1"
#endif

/* Patch every registered SVC site at load time instead of waiting for the
 * first fault.  Android defaults to on: host kernels may refuse Syscall User
 * Dispatch and app seccomp filters make the fallbacks unreliable, while the
 * firmware's SVC sites are static and known ahead of time. */
#ifndef PRIMESHIM_STATIC_SYSCALLS_DEFAULT
#if PRIMESHIM_HOST_ANDROID
#define PRIMESHIM_STATIC_SYSCALLS_DEFAULT 1
#else
#define PRIMESHIM_STATIC_SYSCALLS_DEFAULT 0
#endif
#endif

#ifndef PRIMESHIM_DATA_DIR
#define PRIMESHIM_DATA_DIR "runtime/prime_data"
#endif

#ifndef PRIMESHIM_FIRMWARE_PATH
#define PRIMESHIM_FIRMWARE_PATH "firmware/2025-15270/armfir.elf"
#endif

#endif
