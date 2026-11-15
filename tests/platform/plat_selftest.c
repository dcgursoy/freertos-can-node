/* SPDX-License-Identifier: MIT */
/*
 * Self-tests of the POSIX platform layer. Each sub-command is a small FreeRTOS program that
 * exercises one mechanism the node depends on. CTest runs them and checks the exit status;
 * the measurement scripts read the JSON that `tick` and `irq` print on stdout.
 *
 *   tick <locked|stock> <seconds>                  effective tick period, periodic wake-ups
 *   suspend <milliseconds>                         the tick keeps time across a suspended
 *                                                  scheduler
 *   irq <signal|poll> <period_us> <seconds> <load> simulated interrupts: latency, every raise
 *                                                  served, handlers never nested; period 0 is
 *                                                  an interrupt storm
 *   stack                                          stack painting measures real depth
 *   log <path>                                     lines from tasks and an external thread
 *                                                  arrive whole and in order
 *   assert | malloc-fail | reset                   fatal paths exit with the documented status
 *   run                                            idle until SIGINT/SIGTERM, then stop cleanly
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "posix_platform.h"

#define TASK_STACK_WORDS configMINIMAL_STACK_SIZE

/* ------------------------------------------------------------------------------ helpers */

static uint32_t s_run_seconds = 1U;

static int compare_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/* If FCN_SELFTEST_DUMP names a file, write the raw samples to it, one per line in
 * nanoseconds and in the order they were taken. bench/platform_timing.py plots them. */
static void dump_samples(const uint64_t *v, uint32_t n)
{
    const char *path = getenv("FCN_SELFTEST_DUMP");

    if (path != NULL) {
        FILE *file = fopen(path, "w");

        if (file != NULL) {
            for (uint32_t i = 0U; i < n; i++) {
                (void)fprintf(file, "%llu\n", (unsigned long long)v[i]);
            }

            (void)fclose(file);
        }
    }
}

/* Prints {"n":..,"mean":..,"min":..,"p50":..,"p99":..,"max":..} in microseconds; sorts v. */
static void print_stats_us(const char *key, uint64_t *v, uint32_t n)
{
    double sum = 0.0;

    if (n == 0U) {
        (void)printf("\"%s\": {\"n\": 0}", key);
        return;
    }

    qsort(v, n, sizeof(v[0]), compare_u64);

    for (uint32_t i = 0U; i < n; i++) {
        sum += (double)v[i];
    }

    const uint32_t median = n / 2U;
    const uint32_t p99 = (uint32_t)(((uint64_t)(n - 1U) * 99U) / 100U);

    (void)printf("\"%s\": {\"n\": %u, \"mean\": %.2f, \"min\": %.2f, \"p50\": %.2f, "
                 "\"p99\": %.2f, \"max\": %.2f}",
                 key, n, sum / (double)n / 1e3, (double)v[0] / 1e3, (double)v[median] / 1e3,
                 (double)v[p99] / 1e3, (double)v[n - 1U] / 1e3);
}

static void stopper_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_run_seconds * 1000U));
    plat_end_scheduler();

    for (;;) {
    }
}

static void create_task(TaskFunction_t code, const char *name, UBaseType_t priority,
                        TaskHandle_t *handle)
{
    const BaseType_t created = xTaskCreate(code, name, TASK_STACK_WORDS, NULL, priority, handle);

    configASSERT(created == pdPASS);
}

static void init_platform(posix_tick_mode_t tick, posix_irq_mode_t irq, const char *log_path)
{
    const posix_config_t config = {
        .tick_mode = tick, .irq_mode = irq, .log_level = LOG_LEVEL_INFO, .log_path = log_path};

    posix_platform_init(&config);
}

/* --------------------------------------------------------------------------------- tick */

#define TICK_MAX_SAMPLES    12000U
#define TICK_TASK_PERIOD_MS 10U

static uint64_t s_wake_ns[TICK_MAX_SAMPLES];
static volatile uint32_t s_wake_count;

static void periodic_task(void *arg)
{
    TickType_t last_wake;

    (void)arg;
    plat_task_enter("periodic");
    last_wake = xTaskGetTickCount();

    for (;;) {
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_TASK_PERIOD_MS));

        if (s_wake_count < TICK_MAX_SAMPLES) {
            s_wake_ns[s_wake_count] = plat_time_ns();
            s_wake_count++;
        }
    }
}

static int cmd_tick(posix_tick_mode_t mode)
{
    static uint64_t interval_ns[TICK_MAX_SAMPLES];
    posix_tick_stats_t tick;
    int status = EXIT_SUCCESS;

    init_platform(mode, POSIX_IRQ_SIGNAL, NULL);
    create_task(periodic_task, "periodic", 3U, NULL);
    create_task(stopper_task, "stopper", 5U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    posix_tick_get_stats(&tick);

    const uint32_t n = s_wake_count;

    if (n < 3U) {
        (void)printf("{\"test\": \"tick\", \"error\": \"too few samples\"}\n");
        return EXIT_FAILURE;
    }

    for (uint32_t i = 1U; i < n; i++) {
        interval_ns[i - 1U] = s_wake_ns[i] - s_wake_ns[i - 1U];
    }

    /* Steady-state tick period: wall time between the first and the last wake-up of the
     * periodic task over the ticks that passed in between. */
    const double span_ns = (double)(s_wake_ns[n - 1U] - s_wake_ns[0]);
    const double span_ticks = (double)(n - 1U) * (double)pdMS_TO_TICKS(TICK_TASK_PERIOD_MS);
    const double period_us = span_ns / span_ticks / 1e3;
    const double nominal_us = 1e6 / (double)configTICK_RATE_HZ;
    const double drift_us = (span_ns / 1e3) - (span_ticks * nominal_us);

    /* What locked mode promises is an identity: every tick period of wall time is either
     * counted by the kernel or reported as dropped, so (counted + dropped) periods equal the
     * time that passed. On a host with a CPU to spare nothing is dropped and this is the plain
     * statement that the tick keeps time. On an overloaded host ticks are dropped, the run is
     * marked as such, and the identity still has to hold: time is never lost silently.
     * The stock mode is only measured (it reports no dropped ticks, so there the figure is
     * simply how far the tick count fell behind). */
    const double accounted_us = (span_ticks + (double)tick.dropped_ticks) * nominal_us;
    const double unaccounted = ((span_ns / 1e3) - accounted_us) / (span_ns / 1e3);

    (void)printf("{\"test\": \"tick\", \"mode\": \"%s\", \"seconds\": %u, "
                 "\"nominal_tick_period_us\": %.1f, \"effective_tick_period_us\": %.3f, "
                 "\"drift_us\": %.1f, \"tick_thread_seen\": %s, \"signals_sent\": %llu, "
                 "\"catch_ups\": %llu, \"dropped_ticks\": %llu, "
                 "\"unaccounted_time_percent\": %.3f, ",
                 (mode == POSIX_TICK_LOCKED) ? "locked" : "stock", (unsigned int)s_run_seconds,
                 nominal_us, period_us, drift_us, tick.tick_thread_seen ? "true" : "false",
                 (unsigned long long)tick.signals_sent, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks, unaccounted * 100.0);
    dump_samples(interval_ns, n - 1U);
    print_stats_us("wake_interval_us", interval_ns, n - 1U);
    (void)printf("}\n");

    if (!tick.tick_thread_seen) {
        (void)fprintf(stderr, "the usleep wrapper never saw the port's tick thread\n");
        status = EXIT_FAILURE;
    }

    if ((mode == POSIX_TICK_LOCKED) && ((unaccounted < -0.005) || (unaccounted > 0.005))) {
        (void)fprintf(stderr,
                      "tick count and wall clock disagree by %.2f %% (tick period %.1f us, "
                      "%llu ticks reported as dropped)\n",
                      unaccounted * 100.0, period_us, (unsigned long long)tick.dropped_ticks);
        status = EXIT_FAILURE;
    }

    if ((mode == POSIX_TICK_LOCKED) && (tick.dropped_ticks != 0U)) {
        (void)fprintf(stderr,
                      "note: the host did not run the node for %llu tick periods (%.1f %% of "
                      "the run); timing figures of this run are those of an overloaded host\n",
                      (unsigned long long)tick.dropped_ticks,
                      100.0 * (double)tick.dropped_ticks / span_ticks);
    }

    return status;
}

/* ------------------------------------------------------------------ suspended scheduler */

/*
 * A task keeps the scheduler suspended across several tick periods. The kernel takes its
 * ticks all the same, notes them and adds them to the tick count on resume. The locked tick
 * must not mistake those ticks for lost ones (docs/bug-log.md, entry 003): the tick count has
 * to advance by the time that passed, during the suspension and after it.
 */
#define SUSPEND_SETTLE_MS 50U
#define SUSPEND_AFTER_MS  100U
#define SUSPEND_TOLERANCE 3U /* ticks: boundary effects at both ends of an interval */

static uint32_t s_suspend_ms = 20U;
static uint32_t s_suspend_ticks_during;
static uint32_t s_suspend_ticks_after;
static uint64_t s_suspend_wall_during_ns;
static uint64_t s_suspend_wall_after_ns;

static void busy_for_ms(uint32_t milliseconds)
{
    static volatile uint32_t sink;
    const uint64_t until = plat_time_ns() + ((uint64_t)milliseconds * 1000000ULL);

    while (plat_time_ns() < until) {
        sink++;
    }
}

static void suspend_task(void *arg)
{
    (void)arg;
    plat_task_enter("suspend");
    vTaskDelay(pdMS_TO_TICKS(SUSPEND_SETTLE_MS));

    const TickType_t before = xTaskGetTickCount();
    const uint64_t t0 = plat_time_ns();

    vTaskSuspendAll();
    busy_for_ms(s_suspend_ms);
    (void)xTaskResumeAll();

    const TickType_t resumed = xTaskGetTickCount();
    const uint64_t t1 = plat_time_ns();

    busy_for_ms(SUSPEND_AFTER_MS);

    const TickType_t after = xTaskGetTickCount();
    const uint64_t t2 = plat_time_ns();

    s_suspend_ticks_during = (uint32_t)(resumed - before);
    s_suspend_ticks_after = (uint32_t)(after - resumed);
    s_suspend_wall_during_ns = t1 - t0;
    s_suspend_wall_after_ns = t2 - t1;
    plat_end_scheduler();

    for (;;) {
    }
}

/* True if `ticks` is the number of tick periods in `wall_ns`, give or take the tolerance and
 * the ticks the run reports as dropped (an overloaded host drops ticks, and says so). */
static bool ticks_match_wall(uint32_t ticks, uint64_t wall_ns, uint64_t dropped)
{
    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;
    const uint64_t expected = wall_ns / period_ns;

    return (((uint64_t)ticks + SUSPEND_TOLERANCE + dropped) >= expected) &&
           ((uint64_t)ticks <= (expected + SUSPEND_TOLERANCE));
}

static int cmd_suspend(void)
{
    posix_tick_stats_t tick;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(suspend_task, "suspend", 3U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    posix_tick_get_stats(&tick);

    (void)printf("{\"test\": \"suspend\", \"suspended_ms\": %u, "
                 "\"ticks_during\": %u, \"wall_during_us\": %llu, "
                 "\"ticks_after\": %u, \"wall_after_us\": %llu, "
                 "\"catch_ups\": %llu, \"dropped_ticks\": %llu}\n",
                 (unsigned int)s_suspend_ms, (unsigned int)s_suspend_ticks_during,
                 (unsigned long long)(s_suspend_wall_during_ns / 1000ULL),
                 (unsigned int)s_suspend_ticks_after,
                 (unsigned long long)(s_suspend_wall_after_ns / 1000ULL),
                 (unsigned long long)tick.catch_ups, (unsigned long long)tick.dropped_ticks);

    if (!ticks_match_wall(s_suspend_ticks_during, s_suspend_wall_during_ns, tick.dropped_ticks)) {
        (void)fprintf(stderr,
                      "the tick count advanced by %u ticks while the scheduler was suspended "
                      "for %llu us\n",
                      (unsigned int)s_suspend_ticks_during,
                      (unsigned long long)(s_suspend_wall_during_ns / 1000ULL));
        status = EXIT_FAILURE;
    }

    if (!ticks_match_wall(s_suspend_ticks_after, s_suspend_wall_after_ns, tick.dropped_ticks)) {
        (void)fprintf(stderr,
                      "the tick count advanced by %u ticks in the %llu us after the scheduler "
                      "was resumed\n",
                      (unsigned int)s_suspend_ticks_after,
                      (unsigned long long)(s_suspend_wall_after_ns / 1000ULL));
        status = EXIT_FAILURE;
    }

    return status;
}

/* ---------------------------------------------------------------------------------- irq */

#define IRQ_MAX_SAMPLES 1000000U
#define IRQ_TEST_LINE   0U
/* No task of this test has a reason to go deeper than this. More means interrupt handlers
 * were stacked on top of each other (docs/bug-log.md, entry 002). */
#define IRQ_STACK_LIMIT_BYTES (48U * 1024U)

static TaskHandle_t s_rx_task;
static _Atomic uint64_t s_irq_t0; /* time of the oldest raise not yet seen by the task */
static _Atomic uint32_t s_source_raised;
static _Atomic uint32_t s_task_seen;
static _Atomic bool s_source_run;
static uint32_t s_irq_period_us = 1000U; /* 0: raise as fast as the source thread can */
static uint64_t s_source_first_ns;
static uint64_t s_source_last_ns;
static uint64_t s_latency_ns[IRQ_MAX_SAMPLES];
static volatile uint32_t s_latency_count;
static volatile uint32_t s_latency_overflow;
static volatile uint32_t s_task_wakeups;

static void test_isr(void *context, BaseType_t *woken)
{
    (void)context;
    vTaskNotifyGiveFromISR(s_rx_task, woken);
}

static void rx_task(void *arg)
{
    (void)arg;
    plat_task_enter("rx");

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_task_wakeups++;

        const uint32_t raised = atomic_load(&s_source_raised);
        const uint64_t t0 = atomic_exchange(&s_irq_t0, 0U);

        if (t0 != 0U) {
            if (s_latency_count < IRQ_MAX_SAMPLES) {
                s_latency_ns[s_latency_count] = plat_time_ns() - t0;
                s_latency_count++;
            } else {
                s_latency_overflow++;
            }
        }

        atomic_store(&s_task_seen, raised);
    }
}

/* A mid-priority task that is busy 2 ms out of every 5 ms, so that interrupts also arrive
 * while a task other than idle is running and while it sits in the kernel. */
static void load_task(void *arg)
{
    static volatile uint32_t sink;

    (void)arg;
    plat_task_enter("load");

    for (;;) {
        const uint64_t until = plat_time_ns() + 2000000ULL;

        while (plat_time_ns() < until) {
            sink++;
        }

        vTaskDelay(pdMS_TO_TICKS(3U));
    }
}

static void irq_stopper_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_run_seconds * 1000U));
    atomic_store(&s_source_run, false);
    vTaskDelay(pdMS_TO_TICKS(50U)); /* let the last interrupt be served */
    plat_end_scheduler();

    for (;;) {
    }
}

/*
 * The "peripheral": a thread outside the scheduler that raises the line.
 *
 * With a period it raises at absolute deadlines whose spacing is jittered by +-17 %, so that
 * the mean rate is the nominal one and raises are never phase-locked to the tick. With period
 * 0 it raises in a tight loop, millions of times per second: an interrupt storm, which is
 * what shows whether handlers can pile up on a task's stack.
 */
static void *irq_source(void *arg)
{
    uint32_t lfsr = 0xACE1U;
    uint64_t deadline_ns;

    (void)arg;
    posix_external_thread_setup("irq-src");
    (void)prctl(PR_SET_TIMERSLACK, 1UL); /* wake at the deadline, not up to 50 us after it */
    deadline_ns = plat_time_ns();

    while (atomic_load(&s_source_run)) {
        uint64_t none = 0U;
        uint64_t now_ns;

        if (s_irq_period_us != 0U) {
            lfsr = (lfsr >> 1U) ^ ((0U - (lfsr & 1U)) & 0xB400U);

            /* period * (0.83 .. 1.17) */
            deadline_ns += ((uint64_t)s_irq_period_us * 833ULL) +
                           (((uint64_t)(lfsr % 1000U) * (uint64_t)s_irq_period_us) / 3ULL);

            const struct timespec deadline = {(time_t)(deadline_ns / 1000000000ULL),
                                              (long)(deadline_ns % 1000000000ULL)};

            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        }

        now_ns = plat_time_ns();

        if (atomic_fetch_add(&s_source_raised, 1U) == 0U) {
            s_source_first_ns = now_ns;
        }

        s_source_last_ns = now_ns;
        (void)atomic_compare_exchange_strong(&s_irq_t0, &none, now_ns);
        posix_irq_raise(IRQ_TEST_LINE);
    }

    return NULL;
}

static int cmd_irq(posix_irq_mode_t mode, bool with_load)
{
    plat_stack_info_t stacks[8];
    posix_irq_stats_t irq;
    posix_tick_stats_t tick;
    pthread_t source;
    uint32_t stack_peak = 0U;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, mode, NULL);
    posix_irq_attach(IRQ_TEST_LINE, test_isr, NULL);
    create_task(rx_task, "rx", 5U, &s_rx_task);
    create_task(irq_stopper_task, "stopper", 6U, NULL);

    if (with_load) {
        create_task(load_task, "load", 2U, NULL);
    }

    atomic_store(&s_source_run, true);

    if (pthread_create(&source, NULL, irq_source, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_platform_run();
    (void)pthread_join(source, NULL);
    posix_platform_shutdown();
    posix_irq_get_stats(&irq);
    posix_tick_get_stats(&tick);

    const size_t stack_count = plat_stack_report(stacks, sizeof(stacks) / sizeof(stacks[0]));

    for (size_t i = 0U; i < stack_count; i++) {
        if (stacks[i].peak_bytes > stack_peak) {
            stack_peak = stacks[i].peak_bytes;
        }
    }

    const uint32_t raised = atomic_load(&s_source_raised);
    const uint32_t seen = atomic_load(&s_task_seen);
    const uint32_t samples = s_latency_count;
    const double mean_period_us =
        (raised > 1U)
            ? ((double)(s_source_last_ns - s_source_first_ns) / (double)(raised - 1U) / 1e3)
            : 0.0;

    /* "raised" counts calls of the source; a line that is raised again before it was served
     * stays one pending interrupt, as with a real interrupt flag, so the task wakes up less
     * often than the source raises once the source is faster than the service.
     * "unserved_at_end" is what must be zero: the task ran after the last raise. */
    (void)printf("{\"test\": \"irq\", \"mode\": \"%s\", \"nominal_period_us\": %u, "
                 "\"mean_raise_period_us\": %.3f, \"seconds\": %u, \"load\": %s, "
                 "\"raised\": %u, \"task_wakeups\": %u, \"unserved_at_end\": %u, "
                 "\"signals_sent\": %llu, \"serviced_by_signal\": %llu, "
                 "\"serviced_by_tick\": %llu, \"max_handler_nesting\": %u, "
                 "\"stack_peak_max_bytes\": %u, \"tick_catch_ups\": %llu, "
                 "\"tick_dropped\": %llu, \"latency_samples_not_recorded\": %u, ",
                 (mode == POSIX_IRQ_SIGNAL) ? "signal" : "poll", (unsigned int)s_irq_period_us,
                 mean_period_us, (unsigned int)s_run_seconds, with_load ? "true" : "false",
                 (unsigned int)raised, (unsigned int)s_task_wakeups, (unsigned int)(raised - seen),
                 (unsigned long long)irq.signals_sent, (unsigned long long)irq.serviced_by_signal,
                 (unsigned long long)irq.serviced_by_tick, (unsigned int)irq.max_nesting,
                 (unsigned int)stack_peak, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks, (unsigned int)s_latency_overflow);
    dump_samples(s_latency_ns, samples);
    print_stats_us("latency_us", s_latency_ns, samples);
    (void)printf("}\n");

    if (seen != raised) {
        (void)fprintf(stderr, "an interrupt was never served: raised %u, task saw %u\n",
                      (unsigned int)raised, (unsigned int)seen);
        status = EXIT_FAILURE;
    }

    if (samples == 0U) {
        (void)fprintf(stderr, "no interrupt reached the task\n");
        status = EXIT_FAILURE;
    }

    if (irq.max_nesting > 1U) {
        (void)fprintf(stderr, "interrupt handlers nested %u deep\n", (unsigned int)irq.max_nesting);
        status = EXIT_FAILURE;
    }

    if (stack_peak > IRQ_STACK_LIMIT_BYTES) {
        (void)fprintf(stderr, "a task used %u bytes of stack, more than %u\n",
                      (unsigned int)stack_peak, (unsigned int)IRQ_STACK_LIMIT_BYTES);
        status = EXIT_FAILURE;
    }

    return status;
}

/* -------------------------------------------------------------------------------- stack */

#define BURN_LEVELS      24U
#define BURN_BLOCK_BYTES 1024U

static volatile uint32_t s_burn_result;

/* Uses about BURN_BLOCK_BYTES of stack per level. Not instrumented: AddressSanitizer would
 * move the array to its fake stack and the real stack would stay shallow. */
/* NOLINTNEXTLINE(misc-no-recursion): recursion is the point, it is what uses the stack */
__attribute__((noinline, no_sanitize("address"))) static uint32_t burn_stack(uint32_t levels)
{
    volatile uint8_t block[BURN_BLOCK_BYTES];
    uint32_t sum = 0U;

    for (uint32_t i = 0U; i < BURN_BLOCK_BYTES; i++) {
        block[i] = (uint8_t)(i + levels);
    }

    if (levels > 1U) {
        sum = burn_stack(levels - 1U);
    }

    return sum + block[levels % BURN_BLOCK_BYTES];
}

static void deep_task(void *arg)
{
    (void)arg;
    plat_task_enter("deep");
    s_burn_result = burn_stack(BURN_LEVELS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void shallow_task(void *arg)
{
    (void)arg;
    plat_task_enter("shallow");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static int cmd_stack(void)
{
    plat_stack_info_t info[8];
    uint32_t deep = 0U;
    uint32_t shallow = 0U;
    bool idle_seen = false;
    bool dead_stack_read = false;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(deep_task, "deep", 3U, NULL);
    create_task(shallow_task, "shallow", 3U, NULL);
    create_task(stopper_task, "stopper", 5U, NULL);
    posix_platform_run();
    posix_platform_shutdown();

    const size_t count = plat_stack_report(info, sizeof(info) / sizeof(info[0]));

    (void)printf("{\"test\": \"stack\", \"burned_bytes\": %u, \"tasks\": [",
                 BURN_LEVELS * BURN_BLOCK_BYTES);

    for (size_t i = 0U; i < count; i++) {
        (void)printf("%s{\"name\": \"%s\", \"peak_bytes\": %u, \"size_bytes\": %u}",
                     (i == 0U) ? "" : ", ", info[i].name, (unsigned int)info[i].peak_bytes,
                     (unsigned int)info[i].size_bytes);

        if (strcmp(info[i].name, "deep") == 0) {
            deep = info[i].peak_bytes;
        } else if (strcmp(info[i].name, "shallow") == 0) {
            shallow = info[i].peak_bytes;
        } else if (strcmp(info[i].name, "IDLE") == 0) {
            idle_seen = true;
        } else {
            /* any other task is reported but not judged by name */
        }

        /* No task here uses anything like half its stack. A larger figure means the scan read
         * memory that is no longer a live stack: the idle task is deleted when the scheduler
         * ends, and reading its stack after that once reported it as completely used. */
        if (info[i].peak_bytes > (info[i].size_bytes / 2U)) {
            dead_stack_read = true;
        }
    }

    (void)printf("], \"kernel_high_water_mark_is_meaningful\": false}\n");

    /* The deep task must show at least what it deliberately used, and clearly more than the
     * task that used nothing. The slack covers signal frames and sanitizer overhead. The
     * shallow task must have been measured at all: an unregistered task is not in the report
     * and leaves `shallow` at zero (a registered one reports at least the scan's floor). */
    const uint32_t burned = BURN_LEVELS * BURN_BLOCK_BYTES;

    if ((deep < burned) || (deep > (burned + (48U * 1024U))) || (shallow == 0U) ||
        (shallow > (16U * 1024U)) || (deep < (shallow + (burned / 2U)))) {
        (void)fprintf(stderr, "stack measurement implausible: deep %u, shallow %u, burned %u\n",
                      (unsigned int)deep, (unsigned int)shallow, (unsigned int)burned);
        status = EXIT_FAILURE;
    }

    if (!idle_seen || dead_stack_read) {
        (void)fprintf(stderr, "stack report after shutdown is wrong: idle task %s, %s\n",
                      idle_seen ? "reported" : "missing",
                      dead_stack_read ? "a task shows more than half its stack used"
                                      : "depths plausible");
        status = EXIT_FAILURE;
    }

    return status;
}

/* ---------------------------------------------------------------------------------- log */

#define LOG_LINES_PER_PRODUCER 400U

static _Atomic uint32_t s_log_producers_done;

static void log_task(void *arg)
{
    const char *tag = arg;

    plat_task_enter(tag);

    for (uint32_t seq = 0U; seq < LOG_LINES_PER_PRODUCER; seq++) {
        LOG_INF(tag, "seq %u payload abcdefghijklmnopqrstuvwxyz", (unsigned int)seq);

        if ((seq % 20U) == 19U) {
            vTaskDelay(1U);
        }
    }

    (void)atomic_fetch_add(&s_log_producers_done, 1U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void *log_external(void *arg)
{
    const struct timespec pause = {0, 200000L};

    (void)arg;
    posix_external_thread_setup("log-ext");

    for (uint32_t seq = 0U; seq < LOG_LINES_PER_PRODUCER; seq++) {
        LOG_INF("ext", "seq %u payload abcdefghijklmnopqrstuvwxyz", (unsigned int)seq);

        if ((seq % 20U) == 19U) {
            (void)nanosleep(&pause, NULL);
        }
    }

    (void)atomic_fetch_add(&s_log_producers_done, 1U);
    return NULL;
}

static void log_stopper_task(void *arg)
{
    (void)arg;

    while (atomic_load(&s_log_producers_done) < 3U) {
        vTaskDelay(pdMS_TO_TICKS(5U));
    }

    plat_end_scheduler();

    for (;;) {
    }
}

/* Check the log file: every line of a test producer is whole and its sequence numbers rise. */
static int verify_log_file(const char *path)
{
    static const char *const tags[3] = {"taskA", "taskB", "ext"};
    long next_seq[3] = {0, 0, 0};
    unsigned int lines[3] = {0U, 0U, 0U};
    unsigned int malformed = 0U;
    char line[512];
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        (void)fprintf(stderr, "cannot open %s\n", path);
        return EXIT_FAILURE;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char letter = '\0';
        unsigned long seconds = 0UL;
        unsigned long micros = 0UL;
        long seq = -1;
        char tag[16] = {0};
        char tail[64] = {0};

        /* NOLINTNEXTLINE(cert-err34-c): a field that does not convert makes the line not match */
        if (sscanf(line, "[%lu.%lu] %c %15[^:]: seq %ld payload %63s", &seconds, &micros, &letter,
                   tag, &seq, tail) != 6) {
            continue; /* a line of the platform itself, not of a test producer */
        }

        for (size_t i = 0U; i < 3U; i++) {
            if (strcmp(tag, tags[i]) == 0) {
                if ((seq != next_seq[i]) || (strcmp(tail, "abcdefghijklmnopqrstuvwxyz") != 0) ||
                    (line[strlen(line) - 1U] != '\n')) {
                    malformed++;
                }

                next_seq[i] = seq + 1;
                lines[i]++;
            }
        }
    }

    (void)fclose(file);

    const unsigned int dropped = (unsigned int)posix_log_dropped();
    const unsigned int total = lines[0] + lines[1] + lines[2];

    (void)printf("{\"test\": \"log\", \"lines\": [%u, %u, %u], \"dropped\": %u, "
                 "\"malformed\": %u}\n",
                 lines[0], lines[1], lines[2], dropped, malformed);

    if ((malformed != 0U) || (dropped != 0U) || (total != (3U * LOG_LINES_PER_PRODUCER))) {
        (void)fprintf(stderr, "log check failed\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int cmd_log(const char *path)
{
    static char tag_a[] = "taskA";
    static char tag_b[] = "taskB";
    pthread_t external;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, path);
    (void)xTaskCreate(log_task, "taskA", TASK_STACK_WORDS, tag_a, 2U, NULL);
    (void)xTaskCreate(log_task, "taskB", TASK_STACK_WORDS, tag_b, 3U, NULL);
    create_task(log_stopper_task, "stopper", 1U, NULL);

    if (pthread_create(&external, NULL, log_external, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_platform_run();
    (void)pthread_join(external, NULL);
    posix_platform_shutdown();
    return verify_log_file(path);
}

/* ---------------------------------------------------------------------------- fatal paths */

static void assert_task(void *arg)
{
    (void)arg;
    configASSERT(xTaskGetTickCount() == (TickType_t)0xFFFFFFFFU);

    for (;;) {
    }
}

static void malloc_fail_task(void *arg)
{
    void *block;

    (void)arg;
    block = pvPortMalloc((size_t)configTOTAL_HEAP_SIZE * 2U);
    vPortFree(block);

    for (;;) {
    }
}

static void reset_task(void *arg)
{
    (void)arg;
    plat_reset(PLAT_RESET_REQUESTED);
}

static void run_task(void *arg)
{
    (void)arg;
    plat_task_enter("run");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));

        if (plat_shutdown_requested()) {
            plat_end_scheduler();
        }
    }
}

static int cmd_single_task(TaskFunction_t code)
{
    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(code, "test", 3U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    return EXIT_SUCCESS;
}

/* --------------------------------------------------------------------------------- main */

static int usage(void)
{
    (void)fprintf(
        stderr,
        "usage: plat_selftest tick <locked|stock> <seconds>\n"
        "       plat_selftest suspend <milliseconds>\n"
        "       plat_selftest irq <signal|poll> <period_us, 0: storm> <seconds> <load 0|1>\n"
        "       plat_selftest stack | log <path> | assert | malloc-fail | reset | run\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }

    if ((strcmp(argv[1], "tick") == 0) && (argc == 4)) {
        s_run_seconds = (uint32_t)strtoul(argv[3], NULL, 10);
        return cmd_tick((strcmp(argv[2], "stock") == 0) ? POSIX_TICK_STOCK : POSIX_TICK_LOCKED);
    }

    if ((strcmp(argv[1], "suspend") == 0) && (argc == 3)) {
        s_suspend_ms = (uint32_t)strtoul(argv[2], NULL, 10);
        return cmd_suspend();
    }

    if ((strcmp(argv[1], "irq") == 0) && (argc == 6)) {
        s_irq_period_us = (uint32_t)strtoul(argv[3], NULL, 10);
        s_run_seconds = (uint32_t)strtoul(argv[4], NULL, 10);
        return cmd_irq((strcmp(argv[2], "poll") == 0) ? POSIX_IRQ_POLL : POSIX_IRQ_SIGNAL,
                       strcmp(argv[5], "1") == 0);
    }

    if (strcmp(argv[1], "stack") == 0) {
        return cmd_stack();
    }

    if ((strcmp(argv[1], "log") == 0) && (argc == 3)) {
        return cmd_log(argv[2]);
    }

    if (strcmp(argv[1], "assert") == 0) {
        return cmd_single_task(assert_task);
    }

    if (strcmp(argv[1], "malloc-fail") == 0) {
        return cmd_single_task(malloc_fail_task);
    }

    if (strcmp(argv[1], "reset") == 0) {
        return cmd_single_task(reset_task);
    }

    if (strcmp(argv[1], "run") == 0) {
        return cmd_single_task(run_task);
    }

    return usage();
}
