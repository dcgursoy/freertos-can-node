/* SPDX-License-Identifier: MIT */
/*
 * Self-tests of the POSIX platform layer. Each sub-command is a small FreeRTOS program that
 * exercises one mechanism the node depends on. CTest runs them and checks the exit status;
 * the measurement scripts read the JSON that `tick` and `irq` print on stdout.
 *
 *   tick <locked|stock> <seconds>                  effective tick period, periodic wake-ups
 *   irq <signal|poll> <period_us> <seconds> <load> simulated-interrupt latency, lost-wake check
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

    (void)printf("{\"test\": \"tick\", \"mode\": \"%s\", \"seconds\": %u, "
                 "\"nominal_tick_period_us\": %.1f, \"effective_tick_period_us\": %.3f, "
                 "\"drift_us\": %.1f, \"tick_thread_seen\": %s, \"signals_sent\": %llu, "
                 "\"catch_ups\": %llu, \"dropped_ticks\": %llu, ",
                 (mode == POSIX_TICK_LOCKED) ? "locked" : "stock", (unsigned int)s_run_seconds,
                 nominal_us, period_us, drift_us, tick.tick_thread_seen ? "true" : "false",
                 (unsigned long long)tick.signals_sent, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks);
    dump_samples(interval_ns, n - 1U);
    print_stats_us("wake_interval_us", interval_ns, n - 1U);
    (void)printf("}\n");

    if (!tick.tick_thread_seen) {
        (void)fprintf(stderr, "the usleep wrapper never saw the port's tick thread\n");
        status = EXIT_FAILURE;
    }

    /* Locked mode has to hold the tick within 3 % of nominal; the stock mode is only measured. */
    if ((mode == POSIX_TICK_LOCKED) &&
        ((period_us < (nominal_us * 0.97)) || (period_us > (nominal_us * 1.03)))) {
        (void)fprintf(stderr, "tick period %.1f us is outside 3 %% of %.1f us\n", period_us,
                      nominal_us);
        status = EXIT_FAILURE;
    }

    return status;
}

/* ---------------------------------------------------------------------------------- irq */

#define IRQ_MAX_SAMPLES 200000U
#define IRQ_TEST_LINE   0U

static TaskHandle_t s_rx_task;
static _Atomic uint64_t s_irq_t0; /* time of the oldest raise not yet seen by the task */
static _Atomic uint32_t s_source_raised;
static _Atomic uint32_t s_task_seen;
static _Atomic bool s_source_run;
static uint32_t s_irq_period_us = 1000U;
static uint64_t s_latency_ns[IRQ_MAX_SAMPLES];
static volatile uint32_t s_latency_count;

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

        const uint32_t raised = atomic_load(&s_source_raised);
        const uint64_t t0 = atomic_exchange(&s_irq_t0, 0U);

        if ((t0 != 0U) && (s_latency_count < IRQ_MAX_SAMPLES)) {
            s_latency_ns[s_latency_count] = plat_time_ns() - t0;
            s_latency_count++;
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

/* The "peripheral": a thread outside the scheduler raising the line at a jittered rate, so
 * that raises are never phase-locked to the tick. */
static void *irq_source(void *arg)
{
    uint32_t lfsr = 0xACE1U;

    (void)arg;
    posix_external_thread_setup("irq-src");

    while (atomic_load(&s_source_run)) {
        lfsr = (lfsr >> 1U) ^ ((0U - (lfsr & 1U)) & 0xB400U);

        /* period * (0.83 .. 1.17) */
        const uint64_t period_ns = ((uint64_t)s_irq_period_us * 833ULL) +
                                   (((uint64_t)(lfsr % 1000U) * (uint64_t)s_irq_period_us) / 3ULL);
        const struct timespec pause = {(time_t)(period_ns / 1000000000ULL),
                                       (long)(period_ns % 1000000000ULL)};
        uint64_t none = 0U;

        (void)nanosleep(&pause, NULL);
        (void)atomic_fetch_add(&s_source_raised, 1U);
        (void)atomic_compare_exchange_strong(&s_irq_t0, &none, plat_time_ns());
        posix_irq_raise(IRQ_TEST_LINE);
    }

    return NULL;
}

static int cmd_irq(posix_irq_mode_t mode, bool with_load)
{
    posix_irq_stats_t irq;
    posix_tick_stats_t tick;
    pthread_t source;
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

    const uint32_t raised = atomic_load(&s_source_raised);
    const uint32_t seen = atomic_load(&s_task_seen);
    const uint32_t samples = s_latency_count;

    (void)printf("{\"test\": \"irq\", \"mode\": \"%s\", \"period_us\": %u, \"seconds\": %u, "
                 "\"load\": %s, \"raised\": %u, \"seen_by_task\": %u, \"signals_sent\": %llu, "
                 "\"serviced_by_signal\": %llu, \"serviced_by_tick\": %llu, "
                 "\"tick_catch_ups\": %llu, \"tick_dropped\": %llu, ",
                 (mode == POSIX_IRQ_SIGNAL) ? "signal" : "poll", (unsigned int)s_irq_period_us,
                 (unsigned int)s_run_seconds, with_load ? "true" : "false", (unsigned int)raised,
                 (unsigned int)seen, (unsigned long long)irq.signals_sent,
                 (unsigned long long)irq.serviced_by_signal,
                 (unsigned long long)irq.serviced_by_tick, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks);
    dump_samples(s_latency_ns, samples);
    print_stats_us("latency_us", s_latency_ns, samples);
    (void)printf("}\n");

    if (seen != raised) {
        (void)fprintf(stderr, "lost interrupt: raised %u, task saw %u\n", (unsigned int)raised,
                      (unsigned int)seen);
        status = EXIT_FAILURE;
    }

    if (samples == 0U) {
        (void)fprintf(stderr, "no interrupt reached the task\n");
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
     * task that used nothing. The slack covers signal frames and sanitizer overhead. */
    const uint32_t burned = BURN_LEVELS * BURN_BLOCK_BYTES;

    if ((deep < burned) || (deep > (burned + (48U * 1024U))) || (shallow == 0U) ||
        (deep < (shallow + (burned / 2U)))) {
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
    (void)fprintf(stderr,
                  "usage: plat_selftest tick <locked|stock> <seconds>\n"
                  "       plat_selftest irq <signal|poll> <period_us> <seconds> <load 0|1>\n"
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
