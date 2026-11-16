/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Snapshot of the node's statistics, taken by a task while the scheduler is still running:
 * the kernel's run-time statistics and the counters of the CAN tasks.
 */
#ifndef FCN_NODE_STATS_H
#define FCN_NODE_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "can_io.h"

#define NODE_STATS_MAX_TASKS 12U
#define NODE_STATS_NAME_LEN  16U

typedef struct {
    char name[NODE_STATS_NAME_LEN];
    uint32_t priority;
    uint64_t run_time_us; /**< time spent in the Running state */
} node_task_stats_t;

typedef struct {
    bool valid;
    uint64_t total_run_time_us; /**< run-time counter when the snapshot was taken */
    uint64_t tick_count;
    uint32_t task_count;
    node_task_stats_t tasks[NODE_STATS_MAX_TASKS];
    can_io_stats_t can_io;
    can_txq_stats_t can_queue;
} node_stats_t;

/** Fill the task table of `out` from uxTaskGetSystemState(). Call from a task. */
void node_stats_capture_tasks(node_stats_t *out);

/** Fill the counters of the CAN tasks and of the transmit queue. Call from a task. */
void node_stats_capture_can(node_stats_t *out);

#endif /* FCN_NODE_STATS_H */
