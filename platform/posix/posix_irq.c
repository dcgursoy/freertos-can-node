/* SPDX-License-Identifier: MIT */
/*
 * Simulated interrupts for the models of the peripherals.
 *
 * On a microcontroller a CAN controller raises an interrupt and the ISR hands the event to a
 * task. On the host the "controller" is a thread outside the scheduler, and it may not call
 * any FreeRTOS function. This file gives it an interrupt line instead.
 *
 * posix_irq_raise() latches the line in an atomic word and sends SIGUSR2 to the process.
 * The port keeps signals blocked in every thread except the one that runs the current task,
 * and blocks them there too during a critical section, so the signal is taken by the running
 * task's thread and only while "interrupts are enabled": exactly the masking behaviour of a
 * real interrupt, and the same mechanism the port uses for its own tick (SIGALRM). The
 * handler runs the attached ISRs, which use the FromISR kernel API and request a context
 * switch the usual way.
 *
 * The tick hook polls the same latch. In POSIX_IRQ_POLL mode that is the only delivery path
 * (an interrupt is then served up to one tick late); in POSIX_IRQ_SIGNAL mode it is a safety
 * net, so that a line raised while delivery was disabled is still served within one tick.
 */
#include "posix_platform.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "task.h"

#define IRQ_SIGNAL SIGUSR2 /* SIGUSR1 is taken by the port, SIGALRM is its tick */

typedef struct {
    posix_isr_fn isr;
    void *context;
} irq_line_t;

static irq_line_t s_lines[POSIX_IRQ_LINES];
static posix_irq_mode_t s_mode = POSIX_IRQ_SIGNAL;
static _Atomic uint32_t s_pending;
static _Atomic bool s_enabled;
static _Atomic uint64_t s_raised;
static _Atomic uint64_t s_signals_sent;
static _Atomic uint64_t s_serviced_by_signal;
static _Atomic uint64_t s_serviced_by_tick;

/* Run the ISR of every pending line. Interrupt context. Returns true if any line was served. */
static bool dispatch(BaseType_t *woken)
{
    uint32_t pending = atomic_exchange(&s_pending, 0U);
    const bool any = (pending != 0U);

    for (unsigned int line = 0U; pending != 0U; line++) {
        if (((pending & 1U) != 0U) && (s_lines[line].isr != NULL)) {
            s_lines[line].isr(s_lines[line].context, woken);
        }

        pending >>= 1U;
    }

    return any;
}

static void irq_signal_handler(int signum)
{
    const int saved_errno = errno;

    (void)signum;

    if (atomic_load(&s_enabled)) {
        BaseType_t woken = pdFALSE;

        /* The handler already runs with every signal blocked. Entering a critical section
         * here raises the port's nesting count, so that a yield or a nested critical section
         * inside an ISR cannot unblock signals before the handler is done. */
        vPortEnterCritical();

        if (dispatch(&woken)) {
            (void)atomic_fetch_add(&s_serviced_by_signal, 1U);
        }

        portYIELD_FROM_ISR(woken);
        vPortExitCritical();
    }

    errno = saved_errno;
}

void posix_irq_init(posix_irq_mode_t mode)
{
    struct sigaction action;
    sigset_t block;

    s_mode = mode;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = irq_signal_handler;
    action.sa_flags = SA_RESTART;
    (void)sigfillset(&action.sa_mask);
    (void)sigaction(IRQ_SIGNAL, &action, NULL);

    /* Until the port has set up its own masks, keep the signal away from the calling (main)
     * thread and from every thread created from it. Task threads unblock it when they run. */
    (void)sigemptyset(&block);
    (void)sigaddset(&block, IRQ_SIGNAL);
    (void)pthread_sigmask(SIG_BLOCK, &block, NULL);
}

void posix_irq_attach(unsigned int line, posix_isr_fn isr, void *context)
{
    if (line < POSIX_IRQ_LINES) {
        s_lines[line].isr = isr;
        s_lines[line].context = context;
    } else {
        configASSERT(line < POSIX_IRQ_LINES);
    }
}

void posix_irq_set_enabled(bool enabled)
{
    atomic_store(&s_enabled, enabled);
}

void posix_irq_raise(unsigned int line)
{
    if (line >= POSIX_IRQ_LINES) {
        return; /* no such line; this function has no way to report it and must not assert */
    }

    const uint32_t bit = (uint32_t)1U << line;
    const uint32_t before = atomic_fetch_or(&s_pending, bit);

    (void)atomic_fetch_add(&s_raised, 1U);

    /* If something was already latched, the signal for it is still on its way and its handler
     * will serve this line as well. */
    if ((s_mode == POSIX_IRQ_SIGNAL) && (before == 0U) && atomic_load(&s_enabled)) {
        (void)atomic_fetch_add(&s_signals_sent, 1U);
        (void)kill(getpid(), IRQ_SIGNAL);
    }
}

void posix_irq_poll_from_tick(void)
{
    if (atomic_load(&s_enabled) && (atomic_load(&s_pending) != 0U)) {
        BaseType_t woken = pdFALSE;

        /* Called from the tick interrupt: a task made ready here is switched to when the
         * tick handler returns, so `woken` needs no further action. */
        if (dispatch(&woken)) {
            (void)atomic_fetch_add(&s_serviced_by_tick, 1U);
        }
    }
}

void posix_irq_get_stats(posix_irq_stats_t *out)
{
    out->raised = atomic_load(&s_raised);
    out->signals_sent = atomic_load(&s_signals_sent);
    out->serviced_by_signal = atomic_load(&s_serviced_by_signal);
    out->serviced_by_tick = atomic_load(&s_serviced_by_tick);
}
