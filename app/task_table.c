/* SPDX-License-Identifier: MIT */
/*
 * The task table of the node, for the schedulability check (python -m fcn_tools.sched; the
 * node writes the table into its statistics as "task_table").
 *
 * Priorities and periods are the ones the tasks are created with (app_tasks.h, can_io.h). The
 * execution-time budgets are DESIGN figures for a Cortex-M3 class target, not measurements:
 * on the POSIX port an execution time says nothing about a microcontroller, and phase 6
 * measures them on the target. The table says so ("wcet_basis": "design"). Each budget is the
 * sum of what its job does, as written next to it, rounded up; what the analysis then proves
 * is that the task set meets its deadlines as long as no job takes longer than its budget.
 *
 * HOW EACH TASK IS MODELLED (the modelling rules that come with tools/fcn_tools/sched.py: an
 * interrupt is a task above every task, a task released by another inherits its period and
 * gets its response time as release jitter, a task woken by several events gets the shortest
 * time between two of them):
 *
 *   can_irq   The CAN controller's interrupt: sporadic, above every task, at most once per
 *             frame on the bus, so the shortest frame at the highest bit rate the node accepts
 *             (44 bits and the intermission at PLAT_CAN_MAX_BITRATE: 47 us) is its minimum
 *             inter-arrival time. Every bit rate the node can be started with is covered.
 *   tick_irq  The kernel's tick: periodic, 1 ms, below the CAN interrupt (rate-monotonic among
 *             the interrupts).
 *   can_rx    Released by the CAN interrupt and by the receive cycle (100 ms). It takes at most
 *             CAN_IO_RX_BATCH frames per activation and, with a full batch, waits for the next
 *             tick (can_io.c); a full bus at 1 Mbit/s delivers at most 21.3 frames per tick.
 *             So it is modelled by its demand per tick: one job per tick of CAN_IO_RX_BATCH
 *             frames plus a receive cycle. In any window this is at least the work the frames
 *             of that window cause (4 us per frame against 140 us per 1000 us), so the
 *             interference it puts on the tasks below is an upper bound.
 *   sensor    Periodic. Its deadline is 2 ms, well inside its period: the release jitter of
 *             the processing task is then bounded by it.
 *   process   Released by the sensor task's sample, so periodic with the sensor's period and a
 *             release jitter of the sensor's worst-case response time, which the sensor's
 *             deadline bounds (2 ms). Its deadline counts from the sensor's arrival: the next
 *             sample, 10 ms later.
 *   can_tx    Released by every publish and by every transmit-done event, so like the receive
 *             task modelled by its demand per tick: at most 22 frames of the bus per tick at
 *             1 Mbit/s plus the 7 cyclic messages, which can all be published in one tick.
 *   health    Periodic.
 *
 * Blocking: every task that calls can_io (sensor, process, can_tx, health, and can_rx for the
 * pong) enters short critical sections around the transmit queue, at most 5 us; the health
 * task suspends the scheduler while uxTaskGetSystemState() reads the task list; the
 * processing and the health task share the pack mutex (priority inheritance).
 */
#include "app_tasks.h"

#include "FreeRTOS.h"
#include "can/can_bits.h"
#include "plat_can.h"

/* The shortest frame on the bus and the tick, in microseconds. */
#define SHORTEST_FRAME_BITS (44U + CAN_BITS_INTERMISSION)
#define SHORTEST_FRAME_US   ((SHORTEST_FRAME_BITS * 1000000U) / PLAT_CAN_MAX_BITRATE)
#define TICK_US             (1000000U / configTICK_RATE_HZ)

_Static_assert(SHORTEST_FRAME_US == 47U, "the shortest frame at 1 Mbit/s takes 47 us");
_Static_assert(TICK_US == 1000U, "the tick is 1 ms");
/* The per-tick model of the receive task relies on one batch covering the frames a full bus
 * delivers in a tick at PLAT_CAN_MAX_BITRATE. can_io.c checks that at compile time; it is not
 * checked a second time here, so that the bug log's entry 007, which takes that check out to
 * re-create the bug, still builds. */

/* Interrupt priorities: above every task priority (0 .. configMAX_PRIORITIES - 1). */
#define CAN_IRQ_PRIORITY  ((uint32_t)configMAX_PRIORITIES + 1U)
#define TICK_IRQ_PRIORITY ((uint32_t)configMAX_PRIORITIES)

/* Budgets of one job, in microseconds. */
#define CAN_IRQ_WCET_US  2U /* read the cause register, notify one or two tasks */
#define TICK_IRQ_WCET_US 3U /* xTaskIncrementTick() with a handful of delayed tasks */

/* can_rx: 4 us per frame (out of the FIFO, dispatch, a TESTER_Command check or a pong into the
 * queue), 12 us for a receive cycle (a check with no data and a state machine step). */
#define RX_FRAME_US 4U
#define RX_WCET_US  ((CAN_IO_RX_BATCH * RX_FRAME_US) + 12U)

/* sensor: three front-end reads 80 us, the heartbeat 10 us, the sample into the queue 10 us. */
#define SENSOR_WCET_US     100U
#define SENSOR_DEADLINE_US 2000U

/* process: the sample out of the queue 10 us, lib/bms under the mutex 40 us, up to four frames
 * built, protected and published 25 us each. */
#define PROCESS_WCET_US 150U
#define PACK_MUTEX_US   40U /* the processing task's longest hold of the pack mutex */
#define CRITICAL_US     5U  /* the longest critical section around the transmit queue */

/* can_tx: 3 us per event (a mailbox loaded or a transmit-done handled) for 22 + 7 events, and
 * 13 us for the wake-up and the poll of the queue. */
#define TX_EVENTS_PER_TICK 29U
#define TX_WCET_US         ((TX_EVENTS_PER_TICK * 3U) + 13U)

/* health: the task list 20 us (with the scheduler suspended), the stack figure 100 us, two
 * frames 30 us, a log line 100 us, the rest 50 us. */
#define HEALTH_WCET_US       300U
#define HEALTH_SUSPENDED_US  20U
#define HEALTH_PACK_MUTEX_US 10U /* a copy of the pack state */

/* Microseconds in one millisecond, for the periods of app_tasks.h. */
#define US_PER_MS 1000U

_Static_assert(SENSOR_DEADLINE_US <= (APP_SENSOR_PERIOD_MS * US_PER_MS),
               "sensor: deadline within its period");
_Static_assert(RX_WCET_US <= TICK_US, "can_rx: one batch fits a tick");
_Static_assert(TX_WCET_US <= TICK_US, "can_tx: the events of a tick fit a tick");

static const app_task_budget_t s_table[] = {{.name = "can_irq",
                                             .priority = CAN_IRQ_PRIORITY,
                                             .periodic = false,
                                             .period_us = SHORTEST_FRAME_US,
                                             .deadline_us = SHORTEST_FRAME_US,
                                             .wcet_us = CAN_IRQ_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = 0U,
                                             .mutex = NULL,
                                             .mutex_us = 0U},
                                            {.name = "tick_irq",
                                             .priority = TICK_IRQ_PRIORITY,
                                             .periodic = true,
                                             .period_us = TICK_US,
                                             .deadline_us = TICK_US,
                                             .wcet_us = TICK_IRQ_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = 0U,
                                             .mutex = NULL,
                                             .mutex_us = 0U},
                                            {.name = "can_rx",
                                             .priority = CAN_IO_RX_PRIORITY,
                                             .periodic = false,
                                             .period_us = TICK_US,
                                             .deadline_us = TICK_US,
                                             .wcet_us = RX_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = CRITICAL_US,
                                             .mutex = NULL,
                                             .mutex_us = 0U},
                                            {.name = "sensor",
                                             .priority = APP_SENSOR_PRIORITY,
                                             .periodic = true,
                                             .period_us = APP_SENSOR_PERIOD_MS * US_PER_MS,
                                             .deadline_us = SENSOR_DEADLINE_US,
                                             .wcet_us = SENSOR_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = CRITICAL_US,
                                             .mutex = NULL,
                                             .mutex_us = 0U},
                                            {.name = "process",
                                             .priority = APP_PROCESS_PRIORITY,
                                             .periodic = true,
                                             .period_us = APP_SENSOR_PERIOD_MS * US_PER_MS,
                                             .deadline_us = APP_SENSOR_PERIOD_MS * US_PER_MS,
                                             .wcet_us = PROCESS_WCET_US,
                                             .jitter_us = SENSOR_DEADLINE_US,
                                             .nonpreemptive_us = CRITICAL_US,
                                             .mutex = "pack",
                                             .mutex_us = PACK_MUTEX_US},
                                            {.name = "can_tx",
                                             .priority = CAN_IO_TX_PRIORITY,
                                             .periodic = false,
                                             .period_us = TICK_US,
                                             .deadline_us = TICK_US,
                                             .wcet_us = TX_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = CRITICAL_US,
                                             .mutex = NULL,
                                             .mutex_us = 0U},
                                            {.name = "health",
                                             .priority = APP_HEALTH_PRIORITY,
                                             .periodic = true,
                                             .period_us = APP_HEALTH_PERIOD_MS * US_PER_MS,
                                             .deadline_us = APP_HEALTH_PERIOD_MS * US_PER_MS,
                                             .wcet_us = HEALTH_WCET_US,
                                             .jitter_us = 0U,
                                             .nonpreemptive_us = HEALTH_SUSPENDED_US,
                                             .mutex = "pack",
                                             .mutex_us = HEALTH_PACK_MUTEX_US}};

const app_task_budget_t *app_task_table(size_t *count)
{
    *count = sizeof(s_table) / sizeof(s_table[0]);
    return s_table;
}

const char *app_task_table_description(void)
{
    return "fcn_node: execution-time budgets of the design (app/task_table.c), not "
           "measurements; priorities and periods as the tasks are created";
}
