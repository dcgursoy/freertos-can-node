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
 *   can_rx    Released by the CAN interrupt and by the receive cycle (100 ms), far more often
 *             than once per tick, so modelled by its demand per tick: one job per tick that
 *             does what any 1000 us can ask of the task, counted below at RX_WCET_US.
 *   sensor    Periodic. Its deadline is 2 ms, well inside its period: the release jitter of
 *             the processing task is then bounded by it.
 *   process   Released by the sensor task's sample, so periodic with the sensor's period and a
 *             release jitter of the sensor's worst-case response time, which the sensor's
 *             deadline bounds (2 ms). Its deadline counts from the sensor's arrival: the next
 *             sample, 10 ms later.
 *   can_tx    Released by every notification (a transmit-done interrupt, a publish, a pong put
 *             into the queue), so like the receive task modelled by its demand per tick,
 *             counted below at TX_WCET_US.
 *   health    Periodic.
 *
 * Why a budget per tick bounds what the two CAN tasks take from the tasks below them: every
 * wake-up of either is one pass of its loop (can_io.c), and RX_WCET_US and TX_WCET_US count the
 * most passes, frames and events that any 1000 us at PLAT_CAN_MAX_BITRATE can bring, each at
 * its cost. A busy period of a lower task begins with no work of theirs left over, so all they
 * do in it is caused within it: at most one budget per 1000 us, which is what the analysis
 * charges. The model is that of a bus without faults, the bus of can_irq's model of one
 * interrupt per frame: error frames, the controller's state changes and the aborts of frames
 * in the mailboxes are outside it. (The transmit queue asks for aborts only after bus
 * off, or when a frame has waited in its mailbox for a whole period of its message or for the
 * event timeout: can_txq.h, STALE FRAMES IN A MAILBOX.)
 *
 * Blocking: every task that calls can_io (sensor, process, can_tx, health, and can_rx for the
 * pong) enters short critical sections around the transmit queue, at most 5 us; the health
 * task suspends the scheduler while uxTaskGetSystemState() reads the task list; the
 * processing and the health task share the pack mutex (priority inheritance). The words that
 * pass between tasks in critical sections (the telemetry switch, the clear requests, the
 * summary of the DTCs) take less than the transmit queue's sections.
 *
 * Phase 3 added to two budgets: the processing task judges every sample for the fault
 * monitor and posts the verdict (10 us), and the health task runs the supervision cycle,
 * kicks the hardware watchdog, works the verdicts into the DTCs, judges the CAN controller and
 * sends BMS_Fault. The health task's frames are spread over its periods (app_tasks.h) so that
 * one period publishes at most two, as in phase 2: can_tx's count of passes for it stays at two.
 * The DTC work went to the health task, below can_tx, because the processing task is above
 * it: every microsecond added to the processing task's budget adds one to can_tx's response
 * time, whose margin is the smallest of the set.
 */
#include "app_tasks.h"

#include "FreeRTOS.h"
#include "can/can_bits.h"
#include "fault.h"
#include "plat_can.h"

/* The shortest frame on the bus and the tick, in microseconds, and the most frames that can
 * end on the bus in any 1000 us: one every 47 us at most, so 1000 / 47 = 21.3 rounded up. */
#define SHORTEST_FRAME_BITS (44U + CAN_BITS_INTERMISSION)
#define SHORTEST_FRAME_US   ((SHORTEST_FRAME_BITS * 1000000U) / PLAT_CAN_MAX_BITRATE)
#define TICK_US             (1000000U / configTICK_RATE_HZ)
#define BUS_FRAMES_PER_TICK ((TICK_US + SHORTEST_FRAME_US - 1U) / SHORTEST_FRAME_US)

_Static_assert(SHORTEST_FRAME_US == 47U, "the shortest frame at 1 Mbit/s takes 47 us");
_Static_assert(TICK_US == 1000U, "the tick is 1 ms");
_Static_assert(BUS_FRAMES_PER_TICK == 22U, "at most 22 frames end on the bus in any 1000 us");
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

/*
 * can_rx, per pass of the loop in rx_task: 3 us for the wake-up, the read that finds the FIFO
 * empty and the look at the receive cycle's time; 4 us per frame (out of the FIFO, dispatch, a
 * TESTER_Command check or a pong into the queue); 12 us for a receive cycle (a check with no
 * data and a state machine step).
 *
 * Passes in any 1000 us: every received frame ends with an interrupt whose notification wakes
 * the task, and a notification that comes during a pass gives one more pass, not one each, so
 * at most one pass per frame that ends on the bus, BUS_FRAMES_PER_TICK; and one for the end of
 * a receive cycle or of the idle wait. A batch is cut, which costs a pass more, only when
 * CAN_IO_RX_BATCH frames wait at once, at least as many as end on the bus in a tick (can_io.c
 * checks that); the task, above every other, takes them as they come, so on this bus that
 * does not happen. Frames: the BUS_FRAMES_PER_TICK.
 */
#define RX_PASS_US         3U
#define RX_FRAME_US        4U
#define RX_CYCLE_US        12U
#define RX_PASSES_PER_TICK (BUS_FRAMES_PER_TICK + 1U)
#define RX_WCET_US                                                                                 \
    ((RX_PASSES_PER_TICK * RX_PASS_US) + (BUS_FRAMES_PER_TICK * RX_FRAME_US) + RX_CYCLE_US)

/* sensor: three front-end reads 80 us, the heartbeat 10 us, the sample into the queue 10 us. */
#define SENSOR_WCET_US     100U
#define SENSOR_DEADLINE_US 2000U

/* process: the sample out of the queue 10 us, lib/bms under the mutex 40 us, up to four frames
 * built, protected and published 25 us each, the verdict on the faults (fault_judge_sample(),
 * a comparison per reading) 5 us and the verdict (8 bytes, tagged with the count of clear
 * requests read with the telemetry switch) into the health task's queue 5 us. The processing
 * job makes no log call: a line is 100 us (the health task's budget below), more than can_tx's
 * margin, so the lines about the processing task's wait for samples are written by the health
 * task from its counts (app.c, log_sample_wait()). A job after a timeout of the wait is shorter:
 * the verdict alone. */
#define PROCESS_WCET_US 160U
#define PACK_MUTEX_US   40U /* the processing task's longest hold of the pack mutex */
#define CRITICAL_US     5U  /* the longest critical section around the transmit queue */

/*
 * can_tx, per pass of the loop in tx_task: 13 us for the wake-up, the look for controller
 * events, the poll of the queue, the look for a frame to load that finds none, and the wait's
 * next deadline; 3 us per event, a frame loaded into a mailbox or a transmit-done handled.
 *
 * Passes in any 1000 us: one per notification, or per timeout of the wait (a notification that
 * comes during a pass gives one more pass, not one each):
 *   - at most one per frame that ends on the bus, BUS_FRAMES_PER_TICK: one of the node's own
 *     frames ends with its transmit-done interrupt; a ping makes can_rx put a pong into the
 *     queue, which notifies can_tx; any other frame wakes it not at all;
 *   - one for the sensor and the processing task: both are above can_tx, which runs only once
 *     both have blocked, so the heartbeat and the protected frames of one period wake it once;
 *   - two for the health task, which is below can_tx: can_tx runs a pass at each of its
 *     publishes, and a period publishes BMS_Fault and at most one of BMS_TaskStats and
 *     BMS_NodeInfo (app_tasks.h);
 *   - one timeout: tx_wait_ticks() never waits less than a tick.
 * Events in any 1000 us: a transmit-done for each of the node's frames that ends on the bus, at
 * most BUS_FRAMES_PER_TICK, and a load for each of them and for each of the
 * CAN_IO_TX_MAILBOXES mailboxes that may be loaded with a frame that ends later.
 */
#define TX_PASS_US         13U
#define TX_EVENT_US        3U
#define TX_PASSES_PER_TICK (BUS_FRAMES_PER_TICK + 1U + 2U + 1U)
#define TX_EVENTS_PER_TICK ((2U * BUS_FRAMES_PER_TICK) + CAN_IO_TX_MAILBOXES)
#define TX_WCET_US         ((TX_PASSES_PER_TICK * TX_PASS_US) + (TX_EVENTS_PER_TICK * TX_EVENT_US))

_Static_assert(TX_PASSES_PER_TICK == 26U, "can_tx: 22 + 1 + 2 + 1 passes in any 1000 us");
_Static_assert(TX_EVENTS_PER_TICK == 47U, "can_tx: 2 x 22 + 3 events in any 1000 us");

/* health, phase 2: the task list 20 us (with the scheduler suspended), the stack figure 100 us,
 * two frames 30 us, a log line 100 us, the rest 50 us: 300 us. Phase 3 adds:
 *   - the supervision cycle of four entities 15 us, the kick 2 us;
 *   - the verdicts: at most APP_FAULT_VERDICT_QUEUE_LENGTH per period (app.c takes no more in
 *     one job), each taken out of the queue and its tag of clear requests compared (3 us) and
 *     worked into up to nine DTCs (2 us each: the report, and the read of the DTC's testFailed
 *     after it for the summary of the period): 21 us per verdict;
 *   - the clears: at most one before each verdict and one after the last (a tester that changes
 *     CmdClearFaultsSeq with every command), 15 us each: every DTC cleared and the testFailed
 *     bits it ends noted;
 *   - the CAN controller's counters and state 10 us, its monitors 10 us, the summary of 16
 *     DTCs 20 us, BMS_Fault built, protected and published 25 us;
 *   - log lines at 100 us each: two for each fault, as one that rose and fell (or fell and
 *     rose) in the period has both, one for the clears, one for the expiry of a supervised task
 *     (once per run), and the two about the processing task's wait for samples (a stall that
 *     began, one that ended), which it does not write itself (PROCESS_WCET_US above).
 * The log lines are the largest term, and a period in which every fault changes twice does not
 * happen; the health task is the lowest-priority task, so its budget delays no other task. */
#define HEALTH_LOG_LINES  ((2U * (uint32_t)FAULT_EVENTS) + 1U + 1U + 2U)
#define HEALTH_VERDICT_US (3U + (9U * 2U))
#define HEALTH_CLEARS     (APP_FAULT_VERDICT_QUEUE_LENGTH + 1U)
#define HEALTH_FAULT_WORK_US                                                                       \
    (15U + 2U + (APP_FAULT_VERDICT_QUEUE_LENGTH * HEALTH_VERDICT_US) + (HEALTH_CLEARS * 15U) +     \
     10U + 10U + 20U + 25U + (HEALTH_LOG_LINES * 100U))
#define HEALTH_WCET_US (300U + HEALTH_FAULT_WORK_US)

_Static_assert(HEALTH_LOG_LINES == 36U, "health: 2 x 16 fault lines, the clears, the expiry, "
                                        "and the 2 lines of the processing task's wait");
_Static_assert(HEALTH_WCET_US == 5149U, "health: 300 us + the fault work of phase 3");
#define HEALTH_SUSPENDED_US  20U
#define HEALTH_PACK_MUTEX_US 10U /* a copy of the pack state */

/* Microseconds in one millisecond, for the periods of app_tasks.h. */
#define US_PER_MS 1000U

_Static_assert(SENSOR_DEADLINE_US <= (APP_SENSOR_PERIOD_MS * US_PER_MS),
               "sensor: deadline within its period");
_Static_assert(RX_WCET_US <= TICK_US, "can_rx: the passes and frames of a tick fit a tick");
_Static_assert(TX_WCET_US <= TICK_US, "can_tx: the passes and events of a tick fit a tick");

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
