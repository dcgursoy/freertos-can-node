/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The application's tasks: priorities, periods and stacks in one place, and the task table the
 * schedulability check reads (tools/fcn_tools/sched.py, schema fcn.task_table/1).
 *
 *   task     priority  released by                                 does
 *   can_rx   6         the CAN interrupt, and every 100 ms         dispatches received frames,
 *                      (receive cycle of TESTER_Command)           checks TESTER_Command
 *   sensor   5         every 10 ms (xTaskDelayUntil)               heartbeat; reads the front end,
 *                                                                  posts a sample
 *   process  4         a sample in the queue (or a timeout)        lib/bms, pack state, protected
 *                                                                  telemetry
 *   can_tx   3         a producer or the controller notifies it    loads the controller's mailboxes
 *   health   2         every 100 ms (xTaskDelayUntil)              task statistics, node info,
 *                                                                  ends the run
 *
 * Rate-monotonic order holds for the tasks with a period of their own (sensor above health)
 * and for the receive task, modelled by its demand per tick (task_table.c): a period of 1 ms,
 * no longer than that of any task below it. The transmit task is the
 * exception, on purpose: it is released by every frame it has to load, more often than the
 * tasks above it, and it runs below the tasks that produce the frames so that a producer is
 * never preempted by its own output. The response-time analysis, which holds for any fixed
 * priorities, is the verdict on it (task_table.c).
 */
#ifndef FCN_APP_TASKS_H
#define FCN_APP_TASKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_io.h"

/* Priorities. The two CAN tasks are defined by can_io.h. */
#define APP_SENSOR_PRIORITY  5U
#define APP_PROCESS_PRIORITY 4U
#define APP_HEALTH_PRIORITY  2U

/* Periods. The sensor task's period is the cycle of BMS_PackStatus and of the heartbeat; the
 * cells are read every APP_CELLS_EVERY-th period, the temperatures every APP_TEMPS_EVERY-th
 * (app.c checks them against the cycle times of the DBC). */
#define APP_SENSOR_PERIOD_MS 10U
#define APP_CELLS_EVERY      2U
#define APP_TEMPS_EVERY      10U
#define APP_HEALTH_PERIOD_MS 100U

/* The health task sends BMS_TaskStats every APP_TASK_STATS_EVERY-th period (200 ms), one task
 * per frame, and BMS_NodeInfo every APP_NODE_INFO_EVERY-th (1 s). */
#define APP_TASK_STATS_EVERY 2U
#define APP_NODE_INFO_EVERY  10U

/* Sensor supervision: the processing task waits this long for a sample, three periods of the
 * sensor task, before it counts a timeout. */
#define APP_SAMPLE_TIMEOUT_MS 30U

/* Samples the queue between the sensor and the processing task holds. */
#define APP_SAMPLE_QUEUE_LENGTH 4U

/* Stack of every application task, in words (StackType_t): the size meant for the target. */
#define APP_TASK_STACK_WORDS 512U

/** One entry of the task table, in the fields of fcn.task_table/1 (docs of
 *  tools/fcn_tools/sched.py). Times in microseconds. */
typedef struct {
    const char *name;
    const char *mutex;         /**< the mutex the task takes, or NULL */
    uint32_t priority;         /**< FreeRTOS priority; above configMAX_PRIORITIES: an interrupt */
    uint32_t period_us;        /**< period, or minimum inter-arrival time */
    uint32_t deadline_us;      /**< from the arrival; at most period_us */
    uint32_t wcet_us;          /**< execution-time budget of one job (a design figure) */
    uint32_t jitter_us;        /**< release jitter */
    uint32_t nonpreemptive_us; /**< longest stretch inside taskENTER_CRITICAL or with the
                                    scheduler suspended */
    uint32_t mutex_us;         /**< longest time it holds the mutex in one job */
    bool periodic;             /**< true: period_us is the period; false: the shortest time
                                    between two releases (sporadic) */
} app_task_budget_t;

/** The task table of this build: `*count` entries, highest priority first. The budgets are
 *  design figures, not measurements ("wcet_basis": "design"). Any context. */
const app_task_budget_t *app_task_table(size_t *count);

/** The table's "description": where its figures come from. Any context. */
const char *app_task_table_description(void);

#endif /* FCN_APP_TASKS_H */
