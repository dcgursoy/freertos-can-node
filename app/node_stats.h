/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Snapshot of the kernel's run-time statistics, taken while the scheduler is still running.
 */
#ifndef FCN_NODE_STATS_H
#define FCN_NODE_STATS_H

#include <stdbool.h>
#include <stdint.h>

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
} node_stats_t;

/** Fill `out` from uxTaskGetSystemState(). Call from a task. */
void node_stats_capture(node_stats_t *out);

#endif /* FCN_NODE_STATS_H */
