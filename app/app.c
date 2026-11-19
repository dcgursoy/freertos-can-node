/* SPDX-License-Identifier: MIT */
/*
 * The application of phase 2: one battery module measured, judged and published on CAN, and
 * commanded from the bus. The tasks, their priorities and their periods are listed in
 * app_tasks.h; this file is what they do and how they talk to each other.
 *
 *   front end --> sensor --(sample queue)--> process --> protected telemetry --> can_io --> bus
 *                   |                           |                              ^
 *                   +--> heartbeat ------------ | ---------------------------->|
 *                                               v                              |
 *                                    pack state (mutex) <-- health --> task stats, node info
 *
 *   bus --> can_rx --> TESTER_Command check (command.c) --> telemetry switch --> process
 *
 * IPC
 *   - sensor -> process: a queue of APP_SAMPLE_QUEUE_LENGTH samples (bms_sample_t, copied).
 *     The sensor task never waits for room: a sample the queue cannot take is counted and
 *     dropped, and the processing task sees the gap in the sequence numbers (lib/bms counts it
 *     as lost). The processing task waits for a sample with a timeout of
 *     APP_SAMPLE_TIMEOUT_MS: a timeout means the sensor task has stopped delivering
 *     (supervision), and is counted and logged.
 *   - the pack state (lib/bms) is behind a mutex with priority inheritance: written by the
 *     processing task, read by the health task for its log line and the final statistics.
 *     Both hold it only to run lib/bms on one sample or to copy the state; frames are built
 *     from the copy, outside the lock.
 *   - the telemetry switch is written by the receive task (TESTER_Command) and read by the
 *     processing task, in short critical sections.
 *   - every frame goes out through can_io (a slot per cyclic message, app_message_slot()).
 *
 * WHO SENDS WHAT
 *   - BMS_Heartbeat: the sensor task, first thing in each of its periods. It is the status
 *     frame of phase 1, and its two numbers describe exactly that task: the number of its
 *     period and the tick it was released in. The sensor task is the highest-priority periodic
 *     task and is released by the tick alone, so the heartbeat keeps the timing of phase 1's
 *     ticker, and it says that the node's time base runs whatever the processing chain does.
 *     A processing task that stops shows on the bus by itself: its protected messages stop,
 *     and their receivers' E2E checks time out.
 *   - BMS_PackStatus, BMS_CellVoltages1/2, BMS_Temperatures: the processing task, for every
 *     sample that carries the measurements of the message (10, 20, 100 ms), protected end to
 *     end (telemetry.c), and only while the telemetry switch is on.
 *   - BMS_TaskStats, BMS_NodeInfo: the health task.
 *   - BMS_Pong: the receive task, as in phase 1.
 */
#include "app.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_tasks.h"
#include "can_db.h"
#include "can_io.h"
#include "command.h"
#include "log/log.h"
#include "plat_sensor.h"
#include "platform.h"
#include "telemetry.h"

#ifndef FCN_VERSION_MAJOR
#define FCN_VERSION_MAJOR 0
#endif
#ifndef FCN_VERSION_MINOR
#define FCN_VERSION_MINOR 0
#endif
#ifndef FCN_VERSION_PATCH
#define FCN_VERSION_PATCH 0
#endif

_Static_assert((FCN_VERSION_MAJOR >= 0) && (FCN_VERSION_MAJOR <= 255), "SwMajor: 8 bits");
_Static_assert((FCN_VERSION_MINOR >= 0) && (FCN_VERSION_MINOR <= 255), "SwMinor: 8 bits");
_Static_assert((FCN_VERSION_PATCH >= 0) && (FCN_VERSION_PATCH <= 255), "SwPatch: 8 bits");

/* Transmit-queue slots, one per cyclic message. The heartbeat keeps slot 0, the slot of the
 * status frame of phase 1. */
#define SLOT_HEARTBEAT      0U
#define SLOT_PACK_STATUS    1U
#define SLOT_CELL_VOLTAGES1 2U
#define SLOT_CELL_VOLTAGES2 3U
#define SLOT_TEMPERATURES   4U
#define SLOT_TASK_STATS     5U
#define SLOT_NODE_INFO      6U
#define SLOTS               7U

_Static_assert(SLOTS == NODE_STATS_MESSAGES, "one statistics entry per cyclic message");
_Static_assert(SLOTS <= CAN_TXQ_CYCLIC_SLOTS, "the transmit queue has a slot for each");

/* The periods of the tasks are the cycle times of the messages they send. */
_Static_assert(APP_SENSOR_PERIOD_MS == CAN_DB_BMS_HEARTBEAT_CYCLE_MS, "heartbeat: every period");
_Static_assert(APP_SENSOR_PERIOD_MS == CAN_DB_BMS_PACK_STATUS_CYCLE_MS, "PackStatus: every sample");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_CELLS_EVERY) == CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS,
               "CellVoltages1: every sample with cells");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_CELLS_EVERY) == CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS,
               "CellVoltages2: every sample with cells");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_TEMPS_EVERY) == CAN_DB_BMS_TEMPERATURES_CYCLE_MS,
               "Temperatures: every sample with temperatures");
_Static_assert((APP_HEALTH_PERIOD_MS * APP_TASK_STATS_EVERY) == CAN_DB_BMS_TASK_STATS_CYCLE_MS,
               "TaskStats: every second health period");
_Static_assert((APP_HEALTH_PERIOD_MS * APP_NODE_INFO_EVERY) == CAN_DB_BMS_NODE_INFO_CYCLE_MS,
               "NodeInfo: every tenth health period");
_Static_assert((1000U % APP_HEALTH_PERIOD_MS) == 0U, "whole health periods per second");

/* The tasks BMS_TaskStats goes round, in this order (TaskIndex). */
#define TASK_STATS_TASKS 6U
static const char *const s_stats_tasks[TASK_STATS_TASKS] = {"can_rx", "sensor", "process",
                                                            "can_tx", "health", "IDLE"};
_Static_assert(TASK_STATS_TASKS <= (CAN_DB_BMS_TASK_STATS_TASK_INDEX_RAW_MAX + 1U),
               "TaskIndex holds every index");
_Static_assert((uint32_t)configMAX_PRIORITIES <= (CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_RAW_MAX + 1U),
               "TaskPriority holds every priority");

/* The filters of lib/bms, counted in samples of each group: the current is sampled every
 * 10 ms (2^2 samples: about 40 ms), the cells every 20 ms (about 80 ms), the temperatures,
 * which change slowly anyway, every 100 ms (2^1 samples: about 200 ms). */
static const bms_config_t s_bms_config = {.cell_shift = 2U, .temp_shift = 1U, .current_shift = 2U};

/* The cyclic messages: what the statistics say about each. Indexed by slot. */
typedef struct {
    const char *name;
    uint32_t id;
    uint32_t cycle_ms;
    uint16_t data_id; /* meaningful if protected_e2e */
    bool protected_e2e;
    bool gated;
} message_info_t;

static const message_info_t s_messages[SLOTS] = {
    [SLOT_HEARTBEAT] = {"BMS_Heartbeat", CAN_DB_BMS_HEARTBEAT_ID, CAN_DB_BMS_HEARTBEAT_CYCLE_MS, 0U,
                        false, false},
    [SLOT_PACK_STATUS] = {"BMS_PackStatus", CAN_DB_BMS_PACK_STATUS_ID,
                          CAN_DB_BMS_PACK_STATUS_CYCLE_MS, CAN_DB_BMS_PACK_STATUS_E2E_DATA_ID, true,
                          true},
    [SLOT_CELL_VOLTAGES1] = {"BMS_CellVoltages1", CAN_DB_BMS_CELL_VOLTAGES1_ID,
                             CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS,
                             CAN_DB_BMS_CELL_VOLTAGES1_E2E_DATA_ID, true, true},
    [SLOT_CELL_VOLTAGES2] = {"BMS_CellVoltages2", CAN_DB_BMS_CELL_VOLTAGES2_ID,
                             CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS,
                             CAN_DB_BMS_CELL_VOLTAGES2_E2E_DATA_ID, true, true},
    [SLOT_TEMPERATURES] = {"BMS_Temperatures", CAN_DB_BMS_TEMPERATURES_ID,
                           CAN_DB_BMS_TEMPERATURES_CYCLE_MS, CAN_DB_BMS_TEMPERATURES_E2E_DATA_ID,
                           true, true},
    [SLOT_TASK_STATS] = {"BMS_TaskStats", CAN_DB_BMS_TASK_STATS_ID, CAN_DB_BMS_TASK_STATS_CYCLE_MS,
                         0U, false, false},
    [SLOT_NODE_INFO] = {"BMS_NodeInfo", CAN_DB_BMS_NODE_INFO_ID, CAN_DB_BMS_NODE_INFO_CYCLE_MS, 0U,
                        false, false}};

/* What the application did with each message. Each message has one sender, the only task
 * that writes its entry; the health task reads all of them at the end. */
typedef struct {
    uint32_t published;
    uint32_t suppressed;
    uint32_t failed;
} message_counts_t;

static const char TAG[] = "app";

static StaticTask_t s_sensor_tcb;
static StackType_t s_sensor_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_process_tcb;
static StackType_t s_process_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_health_tcb;
static StackType_t s_health_stack[APP_TASK_STACK_WORDS];

/* The queue's storage as an array of its items: the right size and alignment by construction.
 * The kernel takes it as bytes (a conversion MISRA rule 11.3 permits, to a character type). */
static StaticQueue_t s_sample_queue_state;
static bms_sample_t s_sample_queue_storage[APP_SAMPLE_QUEUE_LENGTH];
static QueueHandle_t s_sample_queue;

static StaticSemaphore_t s_pack_mutex_state;
static SemaphoreHandle_t s_pack_mutex;

static app_config_t s_config;
static node_stats_t s_final_stats;

/* Written by the sensor task. */
static node_sensor_stats_t s_sensor;

/* Written by the processing task; s_pack only while it holds s_pack_mutex. */
static node_process_stats_t s_process;
static bms_pack_t s_pack;
static telemetry_sender_t s_sender;

/* Written by the health task. */
static uint32_t s_health_overruns;

/* Owned by the receive task: every call on it is made there (on_command, on_command_cycle). */
static command_rx_t s_command;

/* The telemetry switch as the receive task last left it; read and written in critical
 * sections. */
static bool s_telemetry_on;

static message_counts_t s_counts[SLOTS];

/* ------------------------------------------------------------------------------- helpers */

/* Hand a frame that was built (`built`) to its slot, and count what happened to it. */
static void publish(uint8_t slot, bool built, const can_frame_t *frame, uint64_t stamp_ns)
{
    if (built && can_io_publish(slot, frame, stamp_ns)) {
        s_counts[slot].published++;
    } else {
        s_counts[slot].failed++;
    }
}

static bool telemetry_switch(void)
{
    bool on;

    taskENTER_CRITICAL();
    on = s_telemetry_on;
    taskEXIT_CRITICAL();

    return on;
}

/* ---------------------------------------------------------------------------- the sensor */

/* Sample number `index` (0, 1, 2, ...): the current in every one, the cells in every
 * APP_CELLS_EVERY-th, the temperatures in every APP_TEMPS_EVERY-th, the first one with all
 * three. A group the front end did not answer goes in with every channel flagged invalid, so
 * that lib/bms makes it invalid rather than keeping its last values (lib/bms/bms.h). */
static void acquire(bms_sample_t *sample, uint32_t index)
{
    (void)memset(sample, 0, sizeof(*sample));
    sample->stamp_ns = plat_time_ns();
    sample->sequence = index;
    sample->groups = (uint8_t)BMS_SAMPLE_CURRENT;
    s_sensor.current_reads++;

    if (!plat_sensor_read_current(&sample->current_ma, &sample->current_invalid)) {
        sample->current_invalid = true;
        s_sensor.current_unanswered++;
    }

    if ((index % APP_CELLS_EVERY) == 0U) {
        sample->groups = (uint8_t)(sample->groups | BMS_SAMPLE_CELLS);
        s_sensor.cell_reads++;

        if (!plat_sensor_read_cells(sample->cell_mv, &sample->cell_invalid)) {
            sample->cell_invalid = (uint8_t)BMS_ALL_CELLS;
            s_sensor.cell_unanswered++;
        }
    }

    if ((index % APP_TEMPS_EVERY) == 0U) {
        sample->groups = (uint8_t)(sample->groups | BMS_SAMPLE_TEMPS);
        s_sensor.temp_reads++;

        if (!plat_sensor_read_temps(sample->temp_dc, &sample->temp_invalid)) {
            sample->temp_invalid = (uint8_t)BMS_ALL_TEMPS;
            s_sensor.temp_unanswered++;
        }
    }
}

static void sensor_task(void *arg)
{
    TickType_t last_wake;
    uint32_t periods = 0U;

    (void)arg;
    plat_task_enter("sensor");
    last_wake = xTaskGetTickCount();

    for (;;) {
        can_frame_t frame = {0};
        bms_sample_t sample;

        /* pdFALSE: the release time had passed already, the last period overran. */
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_SENSOR_PERIOD_MS)) == pdFALSE) {
            s_sensor.overruns++;
        }

        periods++;

        /* The heartbeat first, as phase 1's ticker sent its status frame: the tick it carries
         * is the one its period ended in, or a later one only when the task ran late. */
        const bool built = telemetry_heartbeat(periods, (uint32_t)xTaskGetTickCount(), &frame);

        publish(SLOT_HEARTBEAT, built, &frame, plat_time_ns());

        acquire(&sample, periods - 1U);

        /* No waiting: a processing task that is behind loses this sample, it does not hold up
         * the time base. */
        if (xQueueSend(s_sample_queue, &sample, 0U) == pdTRUE) {
            s_sensor.samples_posted++;
        } else {
            s_sensor.queue_full++;
        }

        s_sensor.periods = periods;
    }
}

/* ------------------------------------------------------------------------- the processing */

/* Build and publish one protected message, or count it as suppressed while the telemetry is
 * off. */
static void publish_protected(uint8_t slot, bool telemetry_on, const bms_pack_t *pack,
                              uint64_t stamp_ns)
{
    can_frame_t frame = {0};

    if (!telemetry_on) {
        s_counts[slot].suppressed++;
    } else {
        bool built;

        switch (slot) {
        case SLOT_PACK_STATUS:
            built = telemetry_pack_status(&s_sender, pack, &frame);
            break;
        case SLOT_CELL_VOLTAGES1:
            built = telemetry_cell_voltages1(&s_sender, pack, &frame);
            break;
        case SLOT_CELL_VOLTAGES2:
            built = telemetry_cell_voltages2(&s_sender, pack, &frame);
            break;
        default:
            built = telemetry_temperatures(&s_sender, pack, &frame);
            break;
        }

        publish(slot, built, &frame, stamp_ns);
    }
}

static void process_sample(const bms_sample_t *sample)
{
    bms_pack_t pack;
    bool telemetry_on;

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    bms_process(&s_pack, &s_bms_config, sample);
    pack = s_pack;
    (void)xSemaphoreGive(s_pack_mutex);

    s_process.samples++;
    telemetry_on = telemetry_switch();

    /* Each message goes out with the samples that carry its measurements, stamped with the
     * time of the sample, so that the transmit-done report gives the sample-to-bus latency. */
    publish_protected(SLOT_PACK_STATUS, telemetry_on, &pack, sample->stamp_ns);

    if ((sample->groups & BMS_SAMPLE_CELLS) != 0U) {
        publish_protected(SLOT_CELL_VOLTAGES1, telemetry_on, &pack, sample->stamp_ns);
        publish_protected(SLOT_CELL_VOLTAGES2, telemetry_on, &pack, sample->stamp_ns);
    }

    if ((sample->groups & BMS_SAMPLE_TEMPS) != 0U) {
        publish_protected(SLOT_TEMPERATURES, telemetry_on, &pack, sample->stamp_ns);
    }
}

static void process_task(void *arg)
{
    bool timed_out = false;

    (void)arg;
    plat_task_enter("process");

    for (;;) {
        bms_sample_t sample;

        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(APP_SAMPLE_TIMEOUT_MS)) ==
            pdTRUE) {
            const uint32_t behind = (uint32_t)uxQueueMessagesWaiting(s_sample_queue);

            /* The sample just taken and those behind it were in the queue at once. Any behind
             * it means this task did not finish before the next release: an overrun. */
            if ((behind + 1U) > s_process.high_water) {
                s_process.high_water = behind + 1U;
            }

            if (behind > 0U) {
                s_process.backlog++;
            }

            if (timed_out) {
                LOG_INF(TAG, "samples again after %lu timeouts", (unsigned long)s_process.timeouts);
                timed_out = false;
            }

            process_sample(&sample);
        } else {
            s_process.timeouts++;

            if (!timed_out) {
                LOG_WRN(TAG, "no sample from the sensor task for %u ms",
                        (unsigned int)APP_SAMPLE_TIMEOUT_MS);
                timed_out = true;
            }
        }
    }
}

/* -------------------------------------------------------------------- the command receiver */

/* Receive task context. The switch is copied out for the processing task after every call
 * that can change it. */
static void share_telemetry_switch(void)
{
    const bool on = command_telemetry_on(&s_command);

    taskENTER_CRITICAL();
    s_telemetry_on = on;
    taskEXIT_CRITICAL();
}

static void on_command(const can_frame_t *frame, uint64_t eof_ns)
{
    (void)eof_ns;
    command_on_frame(&s_command, frame);
    share_telemetry_switch();
}

static void on_command_cycle(void)
{
    command_on_cycle(&s_command);
    share_telemetry_switch();
}

/* Receive task context: answer a ping with the same payload. */
static void on_ping(const can_frame_t *frame, uint64_t eof_ns)
{
    can_frame_t pong = *frame;

    (void)eof_ns;
    pong.id = CAN_DB_BMS_PONG_ID;
    (void)can_io_send_event(&pong);
}

/* ------------------------------------------------------------------------------ health */

static uint32_t overruns_of(const char *name)
{
    uint32_t overruns = 0U;

    taskENTER_CRITICAL();

    if (strcmp(name, "sensor") == 0) {
        overruns = s_sensor.overruns;
    } else if (strcmp(name, "process") == 0) {
        overruns = s_process.backlog;
    } else if (strcmp(name, "health") == 0) {
        overruns = s_health_overruns;
    } else {
        /* The other tasks have no period of their own to overrun. */
    }

    taskEXIT_CRITICAL();

    return overruns;
}

/* The deepest stack use the platform has seen for this task, or 0 if it does not know it. */
static uint32_t stack_peak_of(const char *name)
{
    static plat_stack_info_t s_stacks[NODE_STATS_MAX_TASKS];
    const size_t count = plat_stack_report(s_stacks, NODE_STATS_MAX_TASKS);
    uint32_t peak = 0U;

    for (size_t i = 0U; i < count; i++) {
        if (strncmp(s_stacks[i].name, name, sizeof(s_stacks[i].name)) == 0) {
            peak = s_stacks[i].peak_bytes;
        }
    }

    return peak;
}

static void send_task_stats(uint32_t index)
{
    const char *const name = s_stats_tasks[index];
    node_task_stats_t task;
    uint64_t total_us = 0U;
    can_frame_t frame = {0};
    bool built = false;

    if (node_stats_task(name, &task, &total_us)) {
        const telemetry_task_t figures = {.index = index,
                                          .priority = task.priority,
                                          .run_time_us = task.run_time_us,
                                          .total_us = total_us,
                                          .stack_peak_bytes = stack_peak_of(name),
                                          .overruns = overruns_of(name)};

        built = telemetry_task_stats(&figures, &frame);
    }

    publish(SLOT_TASK_STATS, built, &frame, plat_time_ns());
}

static void send_node_info(void)
{
    const uint64_t seconds = (uint64_t)xTaskGetTickCount() / (uint64_t)configTICK_RATE_HZ;
    can_frame_t frame;
    const bool built = telemetry_node_info((uint8_t)FCN_VERSION_MAJOR, (uint8_t)FCN_VERSION_MINOR,
                                           (uint8_t)FCN_VERSION_PATCH, (uint32_t)seconds, &frame);

    publish(SLOT_NODE_INFO, built, &frame, plat_time_ns());
}

static const char *state_name(bms_state_t state)
{
    const char *name = "FAULT";

    if (state == BMS_STATE_STARTUP) {
        name = "STARTUP";
    } else if (state == BMS_STATE_NORMAL) {
        name = "NORMAL";
    } else {
        /* FAULT */
    }

    return name;
}

static void log_alive(uint32_t seconds)
{
    bms_state_t state;
    uint32_t pack_mv;
    int32_t current_ma;
    uint32_t periods;
    bool telemetry_on;

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    state = s_pack.state;
    pack_mv = s_pack.pack_mv;
    current_ma = s_pack.current_ma;
    (void)xSemaphoreGive(s_pack_mutex);

    taskENTER_CRITICAL();
    periods = s_sensor.periods;
    telemetry_on = s_telemetry_on;
    taskEXIT_CRITICAL();

    LOG_INF(TAG, "alive: %lu s, sensor periods %lu, pack %s %lu mV %ld mA, telemetry %s",
            (unsigned long)seconds, (unsigned long)periods, state_name(state),
            (unsigned long)pack_mv, (long)current_ma, telemetry_on ? "on" : "off");
}

/* The application's counters into the final statistics. Runs in the health task after it has
 * raised itself above every other task, so that none of them is in the middle of an update. */
static void capture_app(node_stats_t *out)
{
    for (uint32_t i = 0U; i < out->task_count; i++) {
        out->tasks[i].overruns = overruns_of(out->tasks[i].name);
    }

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    out->pack = s_pack;
    (void)xSemaphoreGive(s_pack_mutex);

    taskENTER_CRITICAL();
    out->sensor = s_sensor;
    out->process = s_process;
    out->command = s_command;
    out->telemetry_on = s_telemetry_on;

    for (uint8_t slot = 0U; slot < SLOTS; slot++) {
        node_message_stats_t *message = &out->messages[slot];

        message->name = s_messages[slot].name;
        message->id = s_messages[slot].id;
        message->cycle_ms = s_messages[slot].cycle_ms;
        message->slot = slot;
        message->protected_e2e = s_messages[slot].protected_e2e;
        message->data_id = s_messages[slot].data_id;
        message->gated = s_messages[slot].gated;
        message->published = s_counts[slot].published;
        message->suppressed = s_counts[slot].suppressed;
        message->failed = s_counts[slot].failed;
        message->queue = out->can_slots[slot];
    }

    taskEXIT_CRITICAL();
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
    capture_app(&s_final_stats);
    LOG_INF(TAG, "stopping after %lu ticks", (unsigned long)xTaskGetTickCount());
    plat_end_scheduler();
}

static void health_task(void *arg)
{
    TickType_t last_wake;
    uint32_t cycles = 0U;
    uint32_t next_task = 0U;

    (void)arg;
    plat_task_enter("health");
    last_wake = xTaskGetTickCount();

    for (;;) {
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_HEALTH_PERIOD_MS)) == pdFALSE) {
            taskENTER_CRITICAL();
            s_health_overruns++;
            taskEXIT_CRITICAL();
        }

        cycles++;

        if ((cycles % APP_TASK_STATS_EVERY) == 0U) {
            send_task_stats(next_task);
            next_task = (next_task + 1U) % TASK_STATS_TASKS;
        }

        if ((cycles % APP_NODE_INFO_EVERY) == 0U) {
            send_node_info();
            log_alive(cycles / (1000U / APP_HEALTH_PERIOD_MS));
        }

        const bool time_is_up = (s_config.run_seconds != 0U) &&
                                (cycles >= (s_config.run_seconds * (1000U / APP_HEALTH_PERIOD_MS)));

        if (time_is_up || plat_shutdown_requested()) {
            stop_node();
        }
    }
}

/* ------------------------------------------------------------------------------ start-up */

void app_start(const app_config_t *config)
{
    s_config = *config;

    bms_init(&s_pack);
    telemetry_init(&s_sender);
    command_init(&s_command);
    s_telemetry_on = command_telemetry_on(&s_command);

    s_sample_queue = xQueueCreateStatic(APP_SAMPLE_QUEUE_LENGTH, sizeof(bms_sample_t),
                                        (uint8_t *)s_sample_queue_storage, &s_sample_queue_state);
    s_pack_mutex = xSemaphoreCreateMutexStatic(&s_pack_mutex_state);
    configASSERT((s_sample_queue != NULL) && (s_pack_mutex != NULL));

    can_io_start();
    (void)can_io_on_frame(CAN_DB_TESTER_PING_ID, false, on_ping);
    (void)can_io_on_frame(CAN_DB_TESTER_COMMAND_ID, false, on_command);
    can_io_on_rx_cycle(COMMAND_CYCLE_MS, on_command_cycle);

    (void)xTaskCreateStatic(sensor_task, "sensor", APP_TASK_STACK_WORDS, NULL, APP_SENSOR_PRIORITY,
                            s_sensor_stack, &s_sensor_tcb);
    (void)xTaskCreateStatic(process_task, "process", APP_TASK_STACK_WORDS, NULL,
                            APP_PROCESS_PRIORITY, s_process_stack, &s_process_tcb);
    (void)xTaskCreateStatic(health_task, "health", APP_TASK_STACK_WORDS, NULL, APP_HEALTH_PRIORITY,
                            s_health_stack, &s_health_tcb);
}

const node_stats_t *app_final_stats(void)
{
    return &s_final_stats;
}
