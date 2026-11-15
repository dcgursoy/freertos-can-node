/* SPDX-License-Identifier: MIT */
/*
 * Phase 0 skeleton of the application: two tasks and one queue, enough to exercise static
 * allocation, a periodic task, the log path, run-time statistics and an orderly shutdown.
 * The telemetry tasks replace `ticker` in the next phases; `health` stays.
 */
#include "app.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "log/log.h"
#include "platform.h"

#define TICKER_PERIOD_MS  10U
#define HEALTH_PERIOD_MS  100U
#define TICKER_PRIORITY   3U
#define HEALTH_PRIORITY   2U
#define TASK_STACK_WORDS  512U
#define BEAT_QUEUE_LENGTH 8U

static const char TAG[] = "app";

static StaticTask_t s_ticker_tcb;
static StackType_t s_ticker_stack[TASK_STACK_WORDS];
static StaticTask_t s_health_tcb;
static StackType_t s_health_stack[TASK_STACK_WORDS];

static StaticQueue_t s_beat_queue_state;
static uint8_t s_beat_queue_storage[BEAT_QUEUE_LENGTH * sizeof(uint32_t)];
static QueueHandle_t s_beat_queue;

static app_config_t s_config;
static node_stats_t s_final_stats;

/* Periodic producer: every tenth period it reports how many periods have run. */
static void ticker_task(void *arg)
{
    TickType_t last_wake;
    uint32_t periods = 0U;

    (void)arg;
    plat_task_enter("ticker");
    last_wake = xTaskGetTickCount();

    for (;;) {
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICKER_PERIOD_MS));
        periods++;

        if ((periods % 10U) == 0U) {
            /* No waiting: if the consumer is behind, this beat is skipped, not queued up. */
            (void)xQueueSend(s_beat_queue, &periods, 0U);
        }
    }
}

static void health_task(void *arg)
{
    TickType_t last_wake;
    uint32_t cycles = 0U;
    uint32_t latest_periods = 0U;

    (void)arg;
    plat_task_enter("health");
    last_wake = xTaskGetTickCount();

    for (;;) {
        uint32_t periods;

        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEALTH_PERIOD_MS));
        cycles++;

        while (xQueueReceive(s_beat_queue, &periods, 0U) == pdTRUE) {
            latest_periods = periods;
        }

        if ((cycles % 10U) == 0U) {
            const uint32_t seconds = cycles / 10U;

            LOG_INF(TAG, "alive: %lu s, ticker periods %lu", (unsigned long)seconds,
                    (unsigned long)latest_periods);
        }

        const bool time_is_up = (s_config.run_seconds != 0U) &&
                                (cycles >= (s_config.run_seconds * (1000U / HEALTH_PERIOD_MS)));

        if (time_is_up || plat_shutdown_requested()) {
            node_stats_capture(&s_final_stats);
            LOG_INF(TAG, "stopping after %lu ticks", (unsigned long)xTaskGetTickCount());
            plat_end_scheduler();
        }
    }
}

void app_start(const app_config_t *config)
{
    s_config = *config;

    s_beat_queue = xQueueCreateStatic(BEAT_QUEUE_LENGTH, sizeof(uint32_t), s_beat_queue_storage,
                                      &s_beat_queue_state);
    configASSERT(s_beat_queue != NULL);

    (void)xTaskCreateStatic(ticker_task, "ticker", TASK_STACK_WORDS, NULL, TICKER_PRIORITY,
                            s_ticker_stack, &s_ticker_tcb);
    (void)xTaskCreateStatic(health_task, "health", TASK_STACK_WORDS, NULL, HEALTH_PRIORITY,
                            s_health_stack, &s_health_tcb);
}

const node_stats_t *app_final_stats(void)
{
    return &s_final_stats;
}
