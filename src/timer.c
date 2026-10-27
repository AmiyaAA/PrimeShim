#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "log.h"
#include "runtime_patcher.h"

// --- Configuration ---
#define INITIAL_VALUE 0xA2          // 162
#define TICK_INTERVAL_NS 80000L     // 80us per tick
// Total Cycle = (162 + 1) * 80000 = 13,040,000 ns
#define CYCLE_INTERVAL_NS ( (INITIAL_VALUE + 1) * TICK_INTERVAL_NS )

// --- Global State ---
static struct timespec g_start_time;
static pthread_t g_timer_thread;
static timer_t g_posix_timer_id;
static bool g_timer_initialized = false;

// Interrupt state
static volatile bool g_irq_enabled = false;
static void (*g_callback_fun)(void) = NULL;

// --- Helper: Calculate Timer Value on the fly ---
// This assumes standard ABI: returns value in R0
uint8_t calculate_current_timer_value(void) {
    if (!g_timer_initialized) return INITIAL_VALUE;

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    // Calculate elapsed nanoseconds
    uint64_t elapsed_ns = (current_time.tv_sec - g_start_time.tv_sec) * 1000000000ULL +
                          (current_time.tv_nsec - g_start_time.tv_nsec);

    uint64_t total_ticks = elapsed_ns / TICK_INTERVAL_NS;

    // Modulo operation simulates the wrapping from 0 back to 0xA2
    // Value counts DOWN: Initial - (ticks % range)
    uint8_t val = INITIAL_VALUE - (total_ticks % (INITIAL_VALUE + 1));
    return val;
}

// --- Internal: OS Callback (Simulated ISR) ---
// This is called by the kernel (via a helper thread) every time the timer cycle wraps.
static void timer_expiration_handler(void) {
    if (g_irq_enabled && g_callback_fun != NULL) {
        g_callback_fun();
    }
}

static void timer_sigev_expiration_handler(union sigval value)
{
    (void)value;
    timer_expiration_handler();
}

static int start_sigev_thread_timer(void)
{
    struct sigevent event;
    struct itimerspec interval;

    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD;
    event.sigev_notify_function = timer_sigev_expiration_handler;
    if (timer_create(CLOCK_MONOTONIC, &event, &g_posix_timer_id) != 0)
        return -1;

    memset(&interval, 0, sizeof(interval));
    interval.it_value.tv_sec = CYCLE_INTERVAL_NS / 1000000000L;
    interval.it_value.tv_nsec = CYCLE_INTERVAL_NS % 1000000000L;
    interval.it_interval = interval.it_value;
    if (timer_settime(g_posix_timer_id, 0, &interval, NULL) != 0) {
        timer_delete(g_posix_timer_id);
        return -1;
    }
    return 0;
}

static void add_cycle_interval(struct timespec *when) {
    when->tv_nsec += CYCLE_INTERVAL_NS;
    if (when->tv_nsec >= 1000000000L) {
        when->tv_nsec -= 1000000000L;
        ++when->tv_sec;
    }
}

static void *timer_worker(void *opaque) {
    struct timespec next = g_start_time;

    (void)opaque;
    add_cycle_interval(&next);

    for (;;) {
        int error;

        do {
            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                    &next, NULL);
        } while (error == EINTR);
        if (error != 0) {
            errno = error;
            PS_LOG_ERRNO_ERROR("timer", errno, "timer worker sleep failed");
            return NULL;
        }

        timer_expiration_handler();
        add_cycle_interval(&next);
    }
}

// --- Exported API: Initialization ---

// 1. Initialize the hardware timer (starts counting immediately)
void inittimer4(void) {
    const char *legacy_timer;
    int error;

    if (g_timer_initialized) return;

    // 1. Set the "Zero" time for calculation
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    legacy_timer = getenv("PRIMESHIM_TEST_SIGEV_THREAD_TIMER");
    if (legacy_timer && *legacy_timer && strcmp(legacy_timer, "0") != 0) {
        g_timer_initialized = true;
        if (start_sigev_thread_timer() != 0) {
            g_timer_initialized = false;
            PS_LOG_ERRNO_ERROR("timer", errno, "SIGEV_THREAD timer failed");
            exit(1);
        }
        PS_LOG_INFO("timer", "using legacy SIGEV_THREAD test mode");
        return;
    }

    /* SIGEV_THREAD creates a new thread for every expiry.  At 76.7 Hz that
     * consumed several CPU seconds per calculation on the single-core ARM9.
     * One persistent worker preserves the absolute timer cadence without the
     * repeated pthread creation and teardown. */
    g_timer_initialized = true;
    error = pthread_create(&g_timer_thread, NULL, timer_worker, NULL);
    if (error != 0) {
        g_timer_initialized = false;
        errno = error;
        PS_LOG_ERRNO_ERROR("timer", errno, "timer worker creation failed");
        exit(1);
    }
    pthread_detach(g_timer_thread);

    PS_LOG_INFO("timer", "hardware timer initialized (period %ld ns)",
                CYCLE_INTERVAL_NS);
}

// 2. Initialize the Interrupt logic (attach callback)
bool SDKLIB_InterruptInitialize(int irq, int b, void *fun) {
    (void)irq; (void)b; // Unused

    g_callback_fun = (void (*)(void))fun;

    // If the hardware hasn't been started yet, start it now.
    // (Depending on your binary logic, inittimer4 might be called before or after this)
    if (!g_timer_initialized) {
        inittimer4();
    }

    // Usually Init implies Enable, but we strictly follow API
    g_irq_enabled = true;
    return true;
}

void SDKLIB_InterruptEnable(uint32_t irq) {
    (void)irq;
    g_irq_enabled = true;
}

void SDKLIB_InterruptDisable(uint32_t irq) {
    (void)irq;
    g_irq_enabled = false;
}

void SDKLIB_InterruptDone(uint32_t irq) {
    (void)irq;
    // In a real generic interrupt controller, this clears the active bit.
    // In this simulation, we do nothing.
}

// --- Naked Functions (ARM Assembly) ---
// These call the C helper, but ensure the result ends up in the specific register
// and ALL other registers are preserved.

// Set R0 = TimerValue
void __attribute__((naked)) gettimer4r0(void) {
    asm volatile(
        "push {r1-r3, r12, lr}   \n\t" // Save caller-saved regs (R0 is target, no save)
        "bl calculate_current_timer_value \n\t" // R0 now holds the value
        "pop {r1-r3, r12, pc}    \n\t" // Restore and return
    );
}

// Set R1 = TimerValue
void __attribute__((naked)) gettimer4r1(void) {
    asm volatile(
        "push {r0-r3, r12, lr}   \n\t" // Save everything
        "bl calculate_current_timer_value \n\t" // R0 has result
        "str r0, [sp, #4]        \n\t" // Overwrite saved R1 (at sp+4) with result
        "pop {r0-r3, r12, pc}    \n\t" // Restore (R1 now contains result)
    );
}

// Set R2 = TimerValue
void __attribute__((naked)) gettimer4r2(void) {
    asm volatile(
        "push {r0-r3, r12, lr}   \n\t" // Save everything
        "bl calculate_current_timer_value \n\t" // R0 has result
        "str r0, [sp, #8]        \n\t" // Overwrite saved R2 (at sp+8) with result
        "pop {r0-r3, r12, pc}    \n\t" // Restore (R2 now contains result)
    );
}
void __attribute__((naked)) gettimer4r12(void) {
    asm volatile(
        "push {r0-r3, r12, lr}   \n\t"
        "bl calculate_current_timer_value \n\t"
        "str r0, [sp, #16]        \n\t"
        "pop {r0-r3, r12, pc}    \n\t"
    );
}
#define timerel1 *(uint32_t*)0x30d375b0
void FUN_30ae8b88(int param_1,unsigned int param_2)

{
  if (param_2 != 0) {
    param_2 = (timerel1 - (calculate_current_timer_value()>> 4)) + 1000;
  }
  *(unsigned int *)(param_1 + 0x18) = param_2;
  return;
}
bool FUN_30b476b4(int param_1)

{
  return timerel1 - (calculate_current_timer_value() >> 4) <= *(unsigned int *)(param_1 + 0x20);
}
int FUN_3078875c(void)

{
  return timerel1 - (calculate_current_timer_value() >> 4);
}

#define TIMER_REGISTER_BASE 0x51000000u
#define TIMER_REGISTER_SIZE 0x44u
#define TIMER4_COUNT_OFFSET  0x40u

static uint8_t timer_registers[TIMER_REGISTER_SIZE];

static uint32_t timer4_runtime_read(void *opaque, uint32_t address,
                                    unsigned width) {
    uint32_t offset = address - TIMER_REGISTER_BASE;
    uint32_t value = 0;

    (void)opaque;
    if (offset == TIMER4_COUNT_OFFSET && width == sizeof(uint32_t)) {
        return calculate_current_timer_value();
    }
    memcpy(&value, &timer_registers[offset], width);
    return value;
}

static void timer4_runtime_write(void *opaque, uint32_t address,
                                 unsigned width, uint32_t value) {
    uint32_t offset = address - TIMER_REGISTER_BASE;

    (void)opaque;
    memcpy(&timer_registers[offset], &value, width);
}

int timer_register_runtime_peripheral(runtime_patcher_t *patcher) {
    /* Keep the page unmapped so the first actual access identifies the exact
     * instruction.  Ordinary timer registers retain their written values;
     * TCNTO4 at +0x40 is synthesized from the monotonic clock. */
    return runtime_patcher_register_peripheral(
        patcher, TIMER_REGISTER_BASE, TIMER_REGISTER_SIZE,
        timer4_runtime_read, timer4_runtime_write, NULL,
        0, "s3c2416-timer4");
}
