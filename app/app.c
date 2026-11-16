/* SPDX-License-Identifier: MIT */
/*
 * The application as of phase 1: the CAN path end to end, with stand-in content.
 *
 *   task     priority  runs when                              does
 *   can_rx   6         the CAN interrupt notifies it          dispatches received frames
 *   ticker   4         every 10 ms (xTaskDelayUntil)          publishes the status frame
 *   can_tx   3         a producer or the controller notifies  loads the controller's mailboxes
 *   health   2         every 100 ms (xTaskDelayUntil)         logs, ends the run
 *
 * `ticker` stands in for the telemetry tasks of phase 2: it publishes a status frame that
 * carries a period counter and the tick count, which is enough for a test on the bus to see
 * every frame arrive, in order and on time. A ping frame is answered with a pong carrying the
 * same payload, which gives tests and measurements a stimulus-to-response path through the
 * receive task, the transmit queue and the controller.
 */
#include "app.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "can_ids.h"
#include "can_io.h"
#include "log/log.h"
#include "platform.h"

#define TICKER_PERIOD_MS  10U
#define HEALTH_PERIOD_MS  100U
#define TICKER_PRIORITY   4U
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

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8U) & 0xFFU);
    out[2] = (uint8_t)((value >> 16U) & 0xFFU);
    out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* Periodic producer: publishes the status frame every period and, every tenth period,
 * tells the health task how far it has got. */
static void ticker_task(void *arg)
{
    TickType_t last_wake;
    uint32_t periods = 0U;

    (void)arg;
    plat_task_enter("ticker");
    last_wake = xTaskGetTickCount();

    for (;;) {
        can_frame_t frame = {0};

        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICKER_PERIOD_MS));
        periods++;

        frame.id = CAN_ID_STATUS;
        frame.dlc = 8U;
        put_u32_le(&frame.data[0], periods);
        put_u32_le(&frame.data[4], (uint32_t)xTaskGetTickCount());
        (void)can_io_publish(CAN_SLOT_STATUS, &frame, plat_time_ns());

        if ((periods % 10U) == 0U) {
            /* No waiting: if the consumer is behind, this beat is skipped, not queued up. */
            (void)xQueueSend(s_beat_queue, &periods, 0U);
        }
    }
}

/* Receive task context: answer a ping with the same payload. */
static void on_ping(const can_frame_t *frame, uint64_t eof_ns)
{
    can_frame_t pong = *frame;

    (void)eof_ns;
    pong.id = CAN_ID_PONG;
    (void)can_io_send_event(&pong);
}

/*
 * End the run: take the final statistics and end the scheduler. Only the host build ever gets
 * here; firmware on a microcontroller does not stop.
 *
 * The task table is read first, while this task still has its own priority. Then the task
 * raises itself above every other, so that nothing runs between reading the counters and the
 * end of the scheduler: what the report says a task did is then all it did, and the counters
 * of the layers below can be checked against it frame for frame.
 */
static void stop_node(void)
{
    node_stats_capture_tasks(&s_final_stats);
    vTaskPrioritySet(NULL, (UBaseType_t)configMAX_PRIORITIES - 1U);
    node_stats_capture_can(&s_final_stats);
    LOG_INF(TAG, "stopping after %lu ticks", (unsigned long)xTaskGetTickCount());
    plat_end_scheduler();
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
            stop_node();
        }
    }
}

void app_start(const app_config_t *config)
{
    s_config = *config;

    s_beat_queue = xQueueCreateStatic(BEAT_QUEUE_LENGTH, sizeof(uint32_t), s_beat_queue_storage,
                                      &s_beat_queue_state);
    configASSERT(s_beat_queue != NULL);

    can_io_start();
    (void)can_io_on_frame(CAN_ID_PING, false, on_ping);

    (void)xTaskCreateStatic(ticker_task, "ticker", TASK_STACK_WORDS, NULL, TICKER_PRIORITY,
                            s_ticker_stack, &s_ticker_tcb);
    (void)xTaskCreateStatic(health_task, "health", TASK_STACK_WORDS, NULL, HEALTH_PRIORITY,
                            s_health_stack, &s_health_tcb);
}

const node_stats_t *app_final_stats(void)
{
    return &s_final_stats;
}
