/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the application expects from whatever it runs on.
 *
 * platform/posix implements this for the Linux host under the FreeRTOS POSIX port. A target
 * build provides its own implementation; nothing in app/ or lib/ may include a host header.
 */
#ifndef FCN_PLATFORM_H
#define FCN_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Why the system is being reset. On the host the value is the exit status of the node
 * process, which is how the supervising test harness learns what happened; on a target it
 * would be stored in a reset-cause register or a no-init RAM cell.
 */
typedef enum {
    PLAT_RESET_REQUESTED = 10,   /**< commanded: UDS ECUReset or a command frame */
    PLAT_RESET_WATCHDOG = 11,    /**< a task stopped checking in */
    PLAT_RESET_POWER_LOSS = 12,  /**< simulated loss of supply (host only) */
    PLAT_RESET_IMAGE_FAULT = 13, /**< the running image crashed (simulated on the host) */
    PLAT_RESET_ASSERT = 20,      /**< configASSERT() failed */
    PLAT_RESET_STACK_OVERFLOW = 21,
    PLAT_RESET_MALLOC_FAILED = 22
} plat_reset_reason_t;

/** Monotonic time in nanoseconds. On the host this is CLOCK_MONOTONIC, so timestamps taken
 *  inside the node can be compared with timestamps taken by another process. */
uint64_t plat_time_ns(void);

/** Free-running microsecond counter since platform initialisation; the time base of the
 *  FreeRTOS run-time statistics. */
uint64_t plat_run_time_counter_us(void);

/** Called by configASSERT(). Reports the location and resets with PLAT_RESET_ASSERT. */
_Noreturn void plat_assert_failed(const char *file, int line);

/** Reset the system immediately. Nothing of the running image executes after this call. */
_Noreturn void plat_reset(plat_reset_reason_t reason);

/** True once an orderly stop was requested from outside (SIGINT or SIGTERM on the host).
 *  A target has no such thing and returns false. */
bool plat_shutdown_requested(void);

/** Stop the scheduler so that the platform's start function returns. Call from a task. */
void plat_end_scheduler(void);

/**
 * Every task calls this first. It registers the task for stack measurement; on the host it
 * also paints the unused part of the task's real stack so the peak depth can be read later.
 */
void plat_task_enter(const char *name);

/** Peak stack depth of one registered task. */
typedef struct {
    char name[16];
    /** Deepest use seen since plat_task_enter(). The measurement has a floor: use below it
     *  is reported as the floor itself. On the host the floor is 512 bytes (4096 in a
     *  sanitizer build), so that value means "at most that much". */
    uint32_t peak_bytes;
    uint32_t size_bytes; /**< total stack available to the task */
} plat_stack_info_t;

/** Fill `out` with one entry per registered task; returns the number of entries written.
 *  After plat_end_scheduler() it reports the depths recorded at that moment. */
size_t plat_stack_report(plat_stack_info_t *out, size_t max_entries);

#endif /* FCN_PLATFORM_H */
