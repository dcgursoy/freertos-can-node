/* SPDX-License-Identifier: MIT */
/*
 * Keeps the tick of the FreeRTOS POSIX port locked to the wall clock.
 *
 * The port (third_party/FreeRTOS-Kernel/portable/ThirdParty/GCC/Posix/port.c) generates its
 * tick from a thread named "Scheduler timer" that loops
 *
 *     pthread_kill(<thread of the running task>, SIGALRM);
 *     usleep(portTICK_RATE_MICROSECONDS);
 *
 * Two properties of that loop make FreeRTOS time fall behind real time:
 *
 *   1. The sleep is relative, so every tick lasts the period plus the loop's own latency. On
 *      the development laptop a nominal 1 ms tick took 1.14 to 1.25 ms.
 *   2. SIGALRM is not queued. A tick raised while the previous one is still pending, or aimed
 *      at a thread that was switched out in the meantime, is lost.
 *
 * The kernel is a pinned, unmodified submodule, so the correction is applied at link time:
 * the executables are linked with -Wl,--wrap=usleep, which routes the port's call to
 * __wrap_usleep() below. For the tick thread it replaces the relative sleep with a sleep
 * until an absolute deadline that is derived from the number of ticks the kernel has actually
 * counted. A lost tick therefore shortens the next sleep and is made up, and the tick count
 * tracks CLOCK_MONOTONIC instead of drifting away from it.
 *
 * What cannot be fixed from here: if the host stops the whole process for longer than
 * MAX_CATCH_UP_TICKS, the missing ticks are given up rather than replayed as a burst (a burst
 * would make every periodic task run back to back). Those are counted as dropped_ticks and
 * reported, because they are host noise the measurements have to own up to.
 */
#include "posix_platform.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "task.h"

/* Name the port gives its tick thread (prvTimerTickHandler in port.c). */
#define PORT_TICK_THREAD_NAME "Scheduler timer"

/* Longest run of lost ticks that is made up one by one. */
#define MAX_CATCH_UP_TICKS 6ULL

/* Spacing of catch-up ticks: long enough for the previous signal to be handled. */
#define CATCH_UP_SPACING_NS 100000L

enum { ROLE_UNKNOWN = 0, ROLE_TICK_THREAD = 1, ROLE_OTHER = 2 };

static _Atomic int s_mode = POSIX_TICK_LOCKED;
static _Atomic bool s_tick_thread_seen;
static _Atomic uint64_t s_signals_sent;
static _Atomic uint64_t s_catch_ups;
static _Atomic uint64_t s_dropped_ticks;

/* The names are fixed by the linker's --wrap convention. */
int __real_usleep(useconds_t usec);
int __wrap_usleep(useconds_t usec);

void posix_tick_configure(posix_tick_mode_t mode)
{
    atomic_store(&s_mode, (int)mode);
}

void posix_tick_get_stats(posix_tick_stats_t *out)
{
    out->mode = (posix_tick_mode_t)atomic_load(&s_mode);
    out->tick_thread_seen = atomic_load(&s_tick_thread_seen);
    out->signals_sent = atomic_load(&s_signals_sent);
    out->catch_ups = atomic_load(&s_catch_ups);
    out->dropped_ticks = atomic_load(&s_dropped_ticks);
}

/* Called once, a little after start-up. If the wrapper below never saw the port's tick thread,
 * the tick is not under this file's control: the executable was linked without
 * -Wl,--wrap=usleep, or the port changed how it generates its tick. Every timing figure would
 * then silently be that of the stock port, so this is fatal rather than a warning. */
void posix_tick_check_active(void)
{
    if (!atomic_load(&s_tick_thread_seen)) {
        static const char message[] =
            "FATAL the tick wrapper is not active (link with -Wl,--wrap=usleep)\n";

        (void)!write(STDERR_FILENO, message, sizeof(message) - 1U);
        plat_reset(PLAT_RESET_ASSERT);
    }
}

static void sleep_until_ns(uint64_t deadline_ns)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(deadline_ns / 1000000000ULL);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ULL);

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        /* interrupted: sleep again until the same absolute deadline */
    }
}

static int identify_caller(void)
{
    char name[32] = {0};
    int role = ROLE_OTHER;

    if ((pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) &&
        (strcmp(name, PORT_TICK_THREAD_NAME) == 0)) {
        role = ROLE_TICK_THREAD;
    }

    return role;
}

int __wrap_usleep(useconds_t usec)
{
    static _Thread_local int role = ROLE_UNKNOWN;
    static _Thread_local uint64_t start_ns;

    /* The variable is thread-local and static: it keeps its value between calls. */
    /* cppcheck-suppress knownConditionTrueFalse */
    if (role == ROLE_UNKNOWN) {
        role = identify_caller();

        if (role == ROLE_TICK_THREAD) {
            /* The default timer slack of 50 us would be added to every wake-up. */
            (void)prctl(PR_SET_TIMERSLACK, 1UL);
            start_ns = posix_monotonic_ns();
            atomic_store(&s_tick_thread_seen, true);
        }
    }

    if (role != ROLE_TICK_THREAD) {
        return __real_usleep(usec);
    }

    (void)atomic_fetch_add(&s_signals_sent, 1U);

    if (atomic_load(&s_mode) == POSIX_TICK_STOCK) {
        return __real_usleep(usec);
    }

    /* The port has just raised a tick. `expected` is the number of ticks that should have
     * been counted by now (the first one was raised at start_ns), `counted` the number the
     * kernel has processed. The tick that was just raised may not have been processed yet,
     * so the count is only behind if it trails by more than that one. */
    const uint64_t period_ns = (uint64_t)usec * 1000ULL;
    const uint64_t now_ns = posix_monotonic_ns();
    uint64_t expected = ((now_ns - start_ns) / period_ns) + 1ULL;
    const uint64_t counted = (uint64_t)xTaskGetTickCount();

    if ((counted + 1ULL) < expected) {
        const uint64_t lag = expected - counted;

        if (lag > MAX_CATCH_UP_TICKS) {
            /* The host stalled the process. Shift the time base instead of bursting. */
            const uint64_t excess = lag - 1ULL;

            start_ns += excess * period_ns;
            expected -= excess;
            (void)atomic_fetch_add(&s_dropped_ticks, excess);
        } else {
            /* A tick was lost: return quickly so the port raises another one. */
            const struct timespec spacing = {0, CATCH_UP_SPACING_NS};

            (void)atomic_fetch_add(&s_catch_ups, 1U);
            (void)nanosleep(&spacing, NULL);
            return 0;
        }
    }

    /* Sleep to the boundary of the next tick. If a made-up tick turned out to be one too
     * many, `counted` is ahead and the sleep is one period longer, which gives it back. */
    const uint64_t next = (counted > expected) ? counted : expected;

    sleep_until_ns(start_ns + (next * period_ns));
    return 0;
}
