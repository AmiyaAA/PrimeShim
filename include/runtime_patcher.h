#ifndef PRIMESHIM_RUNTIME_PATCHER_H
#define PRIMESHIM_RUNTIME_PATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct runtime_patcher runtime_patcher_t;

typedef enum runtime_branch_kind {
    RUNTIME_BRANCH = 0,
    RUNTIME_BRANCH_LINK = 1,
} runtime_branch_kind_t;

enum runtime_syscall_flags {
    /* BESTA enters an SWI after push {r0}; push {lr}. */
    RUNTIME_SYSCALL_BESTA_FRAME = 1u << 0,
};

enum runtime_peripheral_flags {
    /* Deprecated compatibility bit.  Dynamic range lookup is always used. */
    RUNTIME_PERIPHERAL_FAST_STATIC = 1u << 0,
};

typedef uint32_t (*runtime_peripheral_read_fn)(
    void *opaque, uint32_t address, unsigned width);
typedef void (*runtime_peripheral_write_fn)(
    void *opaque, uint32_t address, unsigned width, uint32_t value);

runtime_patcher_t *runtime_patcher_create(void);
void runtime_patcher_destroy(runtime_patcher_t *patcher);

int runtime_patcher_load_elf(runtime_patcher_t *patcher, const char *path);
void runtime_patcher_set_continue_on_segfault(runtime_patcher_t *patcher,
                                              bool enabled);

/* Registering the same number again replaces the earlier binding. */
int runtime_patcher_register_syscall(runtime_patcher_t *patcher,
                                     uint32_t number,
                                     void (*handler)(void),
                                     unsigned flags,
                                     runtime_branch_kind_t branch_kind,
                                     const char *name);

int runtime_patcher_register_peripheral(runtime_patcher_t *patcher,
                                        uint32_t base,
                                        uint32_t size,
                                        runtime_peripheral_read_fn read,
                                        runtime_peripheral_write_fn write,
                                        void *opaque,
                                        unsigned flags,
                                        const char *name);

int runtime_patcher_apply(runtime_patcher_t *patcher);

/* When non-zero every patched syscall logs its name and r0-r3 before the
 * handler runs.  Toggleable at runtime through JNI. */
extern volatile int primeshim_trace_calls;

/* Optional platform hook, called when the firmware cannot be mapped at its
 * fixed base.  Android implements it to give back the PROT_NONE reservation it
 * made in JNI_OnLoad (unmapping it frees the address for the real image).
 * Hosts that do not need it simply do not define it. */
__attribute__((weak))
int primeshim_release_address_window(uint32_t address, size_t size);

/* Rewrites every SVC instruction in the loaded image whose immediate is a
 * registered syscall, instead of waiting for the first fault to discover it.
 *
 * This is the portable path: it needs no kernel support for Syscall User
 * Dispatch, no seccomp filter and no SIGILL fallback, which makes it the only
 * reliable option inside an Android app.  Must run after
 * runtime_patcher_apply() (which allocates the trampoline pool).
 *
 * Returns the number of patched sites, or -1 on invalid state. */
int runtime_patcher_static_patch_syscalls(runtime_patcher_t *patcher);
uint32_t runtime_patcher_entry(const runtime_patcher_t *patcher);

/*
 * Syscall interception is entered per thread (SUD when available, otherwise
 * seccomp/SIGILL).  Call these immediately around every transition between
 * shim code and firmware code.
 */
int runtime_patcher_enter_firmware_thread(void);
void runtime_patcher_leave_firmware_thread(void);

/* Called by the register-preserving assembly bridge.  The bridge loads this
 * address from a literal pool (R_ARM_ABS32), which a shared object can only
 * resolve when the symbol is not preemptible, hence hidden. */
__attribute__((visibility("hidden")))
void runtime_peripheral_dispatch(uint32_t *frame, const void *metadata,
                                 uint32_t cpsr);

/* The generated BESTA syscall registry is one runtime patch module. */
int runtime_register_builtin_syscalls(runtime_patcher_t *patcher);

#endif
