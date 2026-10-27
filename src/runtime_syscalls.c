#include <stddef.h>
#include <stdint.h>

#include "runtime_patcher.h"

/* Keep declarations in this translation unit so legacy strong definitions
 * with reverse-engineered prototypes remain source-compatible. */
#define PRIMESHIM_SYSCALL(number, symbol) extern uint32_t symbol(void);
#include "syscall_registry.inc"
#undef PRIMESHIM_SYSCALL

typedef struct builtin_syscall {
    uint32_t number;
    void (*handler)(void);
    const char *name;
} builtin_syscall_t;

static const builtin_syscall_t g_builtin_syscalls[] = {
#define PRIMESHIM_SYSCALL(number, symbol) \
    { (number), (void (*)(void))symbol, #symbol },
#include "syscall_registry.inc"
#undef PRIMESHIM_SYSCALL
};

int runtime_register_builtin_syscalls(runtime_patcher_t *patcher) {
    size_t i;

    for (i = 0; i < sizeof(g_builtin_syscalls) / sizeof(g_builtin_syscalls[0]); ++i) {
        if (runtime_patcher_register_syscall(
                patcher,
                g_builtin_syscalls[i].number,
                g_builtin_syscalls[i].handler,
                RUNTIME_SYSCALL_BESTA_FRAME,
                RUNTIME_BRANCH,
                g_builtin_syscalls[i].name) != 0) {
            return -1;
        }
    }
    return 0;
}
