/* SPDX-License-Identifier: MIT */
/*
 * FreeRTOS configuration for the POSIX (Linux host) build.
 *
 * Three things differ from what the same file would say on a microcontroller, and they are
 * the reason for most of platform/posix:
 *
 *  - Stacks. The POSIX port runs every task on an ordinary pthread stack and uses the stack
 *    buffer FreeRTOS allocates only to store its per-thread record. The stack sizes below are
 *    therefore the sizes intended for the MCU build; on the host the overflow check and
 *    uxTaskGetStackHighWaterMark() cannot see real usage (platform/posix/posix_stack.c
 *    measures it instead).
 *
 *  - Tick. The port's tick comes from a thread, not a hardware timer
 *    (platform/posix/posix_tick.c keeps it locked to the wall clock).
 *
 *  - Run-time counter. The port's own counter is the process CPU time with 10 ms resolution;
 *    portALT_GET_RUN_TIME_COUNTER_VALUE below replaces it with a monotonic microsecond clock.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* plat_run_time_counter_us() and plat_assert_failed(), which the macros below expand to. The
 * kernel sources see them through this include. */
#include "platform.h"

/* ----------------------------------------------------------------------------- scheduler */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0 /* the POSIX port has no CLZ-based selection */
#define configUSE_TICKLESS_IDLE                 0
#define configTICK_RATE_HZ                      1000U
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256 /* words */
#define configMAX_TASK_NAME_LEN                 12
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_64_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configNUMBER_OF_CORES                   1
#define configSTACK_DEPTH_TYPE                  size_t
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_MINI_LIST_ITEM                1
#define configUSE_NEWLIB_REENTRANT              0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0

/* ----------------------------------------------------------------------- kernel features */
#define configUSE_TASK_NOTIFICATIONS          1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_MUTEXES                     1 /* with priority inheritance */
#define configUSE_RECURSIVE_MUTEXES           0
#define configUSE_COUNTING_SEMAPHORES         1
#define configUSE_QUEUE_SETS                  0
#define configQUEUE_REGISTRY_SIZE             0
#define configUSE_CO_ROUTINES                 0
#define configUSE_EVENT_GROUPS                0
#define configUSE_STREAM_BUFFERS              0
#define configMESSAGE_BUFFER_LENGTH_TYPE      size_t

/* No software timers: all periodic work lives in tasks with explicit periods, so everything
 * that consumes CPU time appears in the schedulability analysis. */
#define configUSE_TIMERS 0

/* ----------------------------------------------------------------------------- memory */
/* Every task and queue of the application is allocated statically. The heap stays enabled
 * (and small) so that the malloc-failed hook is a live code path rather than dead code. */
#define configSUPPORT_STATIC_ALLOCATION     1
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configTOTAL_HEAP_SIZE               (16U * 1024U)
#define configAPPLICATION_ALLOCATED_HEAP    0
#define configHEAP_CLEAR_MEMORY_ON_FREE     1

/* ------------------------------------------------------------------------------ hooks */
#define configUSE_IDLE_HOOK            1
#define configUSE_TICK_HOOK            1
#define configUSE_MALLOC_FAILED_HOOK   1
#define configCHECK_FOR_STACK_OVERFLOW 2

/* ---------------------------------------------------------- run-time stats and tracing */
#define configGENERATE_RUN_TIME_STATS               1
#define configUSE_TRACE_FACILITY                    1
#define configUSE_STATS_FORMATTING_FUNCTIONS        0
#define configRUN_TIME_COUNTER_TYPE                 uint64_t
#define portALT_GET_RUN_TIME_COUNTER_VALUE(counter) ((counter) = plat_run_time_counter_us())

/* ---------------------------------------------------------------------------- assert */
#define configASSERT(x)                                                                            \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            plat_assert_failed(__FILE__, __LINE__);                                                \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ optional functions */
#define INCLUDE_vTaskPrioritySet            0
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1 /* needed by vTaskEndScheduler() */
#define INCLUDE_vTaskSuspend                1 /* needed to block with portMAX_DELAY */
#define INCLUDE_xTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle      1
#define INCLUDE_eTaskGetState               1

#endif /* FREERTOS_CONFIG_H */
