/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Host-only part of the platform layer: start-up, the models' way into the scheduler
 * (simulated interrupts) and the statistics the measurement scripts read.
 *
 * Threading rules of the FreeRTOS POSIX port that this layer is built around:
 *
 *  - Every FreeRTOS task is a pthread, but only one of them runs at a time. The port switches
 *    tasks from a signal handler (SIGALRM, the tick) or from a yield, and a critical section
 *    is "all signals blocked in the running task's thread".
 *  - A thread that the port did not create is invisible to the scheduler. It must block all
 *    signals (posix_external_thread_setup) and must never call a FreeRTOS API, not even the
 *    FromISR variants: nothing would stop the kernel's data from being modified under it.
 *  - Functions of the C library that take a lock (stdio streams, malloc) must not be called
 *    by a task where it can be switched out: a task switched out while it holds the lock
 *    deadlocks the next task that wants it. Tasks here never write to a stream and never
 *    allocate from the C library; formatting into a stack buffer with vsnprintf() takes no
 *    lock, and the one call that does allocate (in plat_task_enter()) is made inside a
 *    critical section.
 *
 * The models of the peripherals therefore live in external threads and talk to tasks through
 * lock-free rings plus a simulated interrupt (posix_irq_raise).
 */
#ifndef FCN_POSIX_PLATFORM_H
#define FCN_POSIX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "log/log.h"
#include "platform.h"

/* ------------------------------------------------------------------------------ start-up */

typedef enum {
    POSIX_TICK_LOCKED = 0, /**< tick count locked to CLOCK_MONOTONIC (default) */
    POSIX_TICK_STOCK = 1   /**< the port's unmodified behaviour, kept for comparison */
} posix_tick_mode_t;

typedef enum {
    POSIX_IRQ_SIGNAL = 0, /**< deliver simulated interrupts with a signal (default) */
    POSIX_IRQ_POLL = 1    /**< only poll for them in the tick hook: served at the next tick */
} posix_irq_mode_t;

typedef struct {
    posix_tick_mode_t tick_mode;
    posix_irq_mode_t irq_mode;
    log_level_t log_level;
    const char *log_path; /**< NULL: log to stderr */
} posix_config_t;

/** Call once from main() before any FreeRTOS object is created. */
void posix_platform_init(const posix_config_t *config);

/** Start the scheduler. Returns after a task called plat_end_scheduler(). */
void posix_platform_run(void);

/** Stop the helper threads and flush the log. Call after posix_platform_run() returned. */
void posix_platform_shutdown(void);

/* --------------------------------------------------------- threads outside the scheduler */

/** First call in every thread that is not a FreeRTOS task: blocks all signals in it so the
 *  port's tick and the simulated interrupts are only ever handled by task threads. */
void posix_external_thread_setup(const char *name);

/* -------------------------------------------------------------------- simulated interrupts */

#define POSIX_IRQ_LINES 8U

/** An interrupt service routine. Runs in the port's interrupt context: only FromISR kernel
 *  calls are allowed, and it sets *woken like a real ISR to request a context switch. */
typedef void (*posix_isr_fn)(void *context, BaseType_t *woken);

/** Connect an ISR to a line. Call before the scheduler starts. */
void posix_irq_attach(unsigned int line, posix_isr_fn isr, void *context);

/** Latch the line as pending and interrupt the running task. This is the one function an
 *  external thread may call to reach the scheduler. Safe to call from several threads. */
void posix_irq_raise(unsigned int line);

/** Service pending lines from the tick hook: the delivery path of POSIX_IRQ_POLL and the
 *  safety net of POSIX_IRQ_SIGNAL. */
void posix_irq_poll_from_tick(void);

/** Allow or stop delivery; the platform enables it around the scheduler's lifetime. */
void posix_irq_set_enabled(bool enabled);

typedef struct {
    uint64_t raised;             /**< calls to posix_irq_raise() */
    uint64_t signals_sent;       /**< of those, how many had to send a signal */
    uint64_t serviced_by_signal; /**< dispatch passes that found work, from the signal handler */
    uint64_t serviced_by_tick;   /**< dispatch passes that found work, from the tick hook */
    uint32_t max_nesting;        /**< deepest nesting of the signal handler: 1, or 0 if unused */
} posix_irq_stats_t;

void posix_irq_get_stats(posix_irq_stats_t *out);

/* ------------------------------------------------------------------------------- the tick */

typedef struct {
    posix_tick_mode_t mode;
    bool tick_thread_seen;  /**< the wrapper recognised the port's tick thread */
    uint64_t signals_sent;  /**< tick signals the port's thread has raised */
    uint64_t catch_ups;     /**< extra signals sent to make up for a lost tick */
    uint64_t dropped_ticks; /**< ticks given up because the count had fallen too far behind */
} posix_tick_stats_t;

void posix_tick_get_stats(posix_tick_stats_t *out);

/* -------------------------------------------------------------------------------- logging */

/** Number of log lines discarded because the log ring was full. */
uint32_t posix_log_dropped(void);

/* Internal to platform/posix. */
void posix_clock_init(void);
uint64_t posix_monotonic_ns(void);
void posix_tick_configure(posix_tick_mode_t mode);
void posix_irq_init(posix_irq_mode_t mode);
void posix_log_start(const char *path);
void posix_log_stop(void);
void posix_log_drain(void);
void posix_log_sink(const char *line, size_t len);
void posix_stack_init(void);
void posix_stack_freeze(void);
void posix_tick_check_active(void);
void posix_shutdown_guard_arm(uint32_t timeout_ms);

/* Exit status when ending the scheduler hung and the guard had to end the process. */
#define POSIX_EXIT_SHUTDOWN_TIMEOUT 70
/* How long an orderly stop may take before the guard steps in. */
#define POSIX_SHUTDOWN_TIMEOUT_MS 5000U

#endif /* FCN_POSIX_PLATFORM_H */
