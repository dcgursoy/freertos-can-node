/* SPDX-License-Identifier: MIT */
/* The hooks the kernel calls back into, and the handler behind configASSERT(). */
#include "posix_platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "task.h"

/* How long the idle task sleeps per pass. A pending tick or simulated interrupt ends the
 * sleep at once (the signal interrupts it), so this costs no wake-up latency; it only keeps
 * the idle task from burning a host core. */
#define IDLE_SLEEP_NS 100000L

void vApplicationIdleHook(void)
{
    static bool registered = false;
    const struct timespec nap = {0, IDLE_SLEEP_NS};

    if (!registered) {
        /* The kernel creates the idle task, so this is the first chance to register it. Its
         * stack matters: it is the one most "interrupts" (signal frames) land on. */
        registered = true;
        plat_task_enter("IDLE");
    }

    (void)nanosleep(&nap, NULL);
}

/* Ticks after which the tick source is checked: late enough for the port's tick thread to
 * have gone through its sleep many times. */
#define TICK_SOURCE_CHECK_AT 100U

void vApplicationTickHook(void)
{
    static uint32_t ticks = 0U;

    if (ticks < TICK_SOURCE_CHECK_AT) {
        ticks++;

        if (ticks == TICK_SOURCE_CHECK_AT) {
            posix_tick_check_active();
        }
    }

    posix_irq_poll_from_tick();
}

/* Report a fatal condition without going through the log ring: the caller may be anywhere,
 * including inside a critical section, and write() is safe in all of those places. */
static void fatal_message(const char *what, const char *detail, int number)
{
    char text[200];
    const int len = snprintf(text, sizeof(text), "FATAL %s: %s %d\n", what, detail, number);

    if (len > 0) {
        const size_t count = ((size_t)len < sizeof(text)) ? (size_t)len : (sizeof(text) - 1U);

        (void)!write(STDERR_FILENO, text, count);
    }
}

void plat_assert_failed(const char *file, int line)
{
    fatal_message("assertion failed", file, line);
    plat_reset(PLAT_RESET_ASSERT);
}

void vApplicationMallocFailedHook(void)
{
    fatal_message("pvPortMalloc failed", "free heap bytes", (int)xPortGetFreeHeapSize());
    plat_reset(PLAT_RESET_MALLOC_FAILED);
}

/* The signature is the kernel's; `name` cannot be made const here. */
/* cppcheck-suppress constParameterPointer */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    fatal_message("stack overflow in task", name, 0);
    plat_reset(PLAT_RESET_STACK_OVERFLOW);
}
