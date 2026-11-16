/* SPDX-License-Identifier: MIT */
#include "node_stats.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* Static: TaskStatus_t is large and this runs on a task stack. Only the health task calls it. */
static TaskStatus_t s_status[NODE_STATS_MAX_TASKS];

void node_stats_capture_tasks(node_stats_t *out)
{
    configRUN_TIME_COUNTER_TYPE total = 0U;
    const UBaseType_t count = uxTaskGetSystemState(s_status, NODE_STATS_MAX_TASKS, &total);

    out->total_run_time_us = (uint64_t)total;
    out->tick_count = (uint64_t)xTaskGetTickCount();
    out->task_count = (uint32_t)count;

    for (UBaseType_t i = 0U; i < count; i++) {
        node_task_stats_t *task = &out->tasks[i];

        (void)memset(task, 0, sizeof(*task));
        (void)strncpy(task->name, s_status[i].pcTaskName, NODE_STATS_NAME_LEN - 1U);
        task->priority = (uint32_t)s_status[i].uxCurrentPriority;
        task->run_time_us = (uint64_t)s_status[i].ulRunTimeCounter;
    }

    /* uxTaskGetSystemState() returns 0 when the array is too small for all tasks. */
    out->valid = (count > 0U);
}

void node_stats_capture_can(node_stats_t *out)
{
    can_io_get_stats(&out->can_io, &out->can_queue);
}
