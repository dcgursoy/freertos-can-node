/* SPDX-License-Identifier: MIT */
/*
 * The CAN controller "hardware" of the host build.
 *
 * One thread outside the scheduler plays the peripheral. It owns the transport
 * (can_backend: SocketCAN or UDP) and runs the controller model (lib/can/can_ctrl) against
 * CLOCK_MONOTONIC: it sleeps until the model's next deadline or until a socket has input,
 * moves frames between the model and the transport, and raises the simulated CAN interrupt.
 *
 * The driver side (plat_can.h) is called by FreeRTOS tasks. The two sides meet only in
 * lock-free rings and atomics, because a thread outside the scheduler must not call the
 * kernel and a task must not block on a host lock:
 *
 *     driver task  --tx ring-->   hardware thread  --transport-->  bus
 *     driver task  <--rx ring--   hardware thread  <--transport--  bus
 *     driver task  <--event ring-- hardware thread     (TX done, TX aborted, state change)
 *     driver task  --command word + eventfd--> hardware thread   (abort, restart, wake up)
 *
 * Each ring has exactly one producer and one consumer. On the driver side that is why
 * plat_can_tx(), plat_can_rx() and plat_can_event() may each be called from one task only.
 *
 * What can be lost when the driver falls behind, and what cannot:
 *
 *  - Received frames can: the receive FIFO overruns, as on hardware, and the overrun is
 *    counted (rx_fifo_overruns).
 *  - The outcome of a transmit request (TX_DONE, TX_ABORTED) cannot. A driver that never
 *    heard what became of a frame would keep its mailbox for ever. plat_can_tx() therefore
 *    accepts a request only while fewer than PLAT_CAN_TX_MAILBOXES outcomes are still to be
 *    taken, and state events never use the last PLAT_CAN_TX_MAILBOXES entries of the event
 *    FIFO: an outcome always finds room. event_fifo_overruns counts the violations of this
 *    argument; it stays 0.
 *  - The latest error state cannot. A state change that finds the event FIFO filled up to
 *    that mark is not queued, but the state the controller is in is sent as soon as there is
 *    room (tell_state). A slow driver sees fewer changes, never a stale last one: a bus off
 *    it was never told about would never be recovered from.
 *
 * Timing. A frame is written to the transport when the hardware thread wakes up at the
 * frame's modelled end of frame. How late that wake-up is (the host's scheduling latency) is
 * the pacing error; it is measured here and reported, never hidden in the model's own times.
 */
#include "posix_platform.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "can/can_ctrl.h"
#include "can_backend.h"
#include "plat_can.h"
#include "posix_sim.h"
#include "ring/spsc_ring.h"

#define CAN_IRQ_LINE 0U

#define TX_RING_ENTRIES    16U /* power of two, at least PLAT_CAN_TX_MAILBOXES */
#define RX_RING_ENTRIES    PLAT_CAN_RX_FIFO_DEPTH
#define EVENT_RING_ENTRIES PLAT_CAN_EVENT_FIFO_DEPTH

/* State events fill the event FIFO only up to here; the rest is kept for transmit outcomes. */
#define STATE_EVENT_LIMIT (EVENT_RING_ENTRIES - PLAT_CAN_TX_MAILBOXES)

/* Most frames taken from the transport in one pass of the hardware thread, so that a sender
 * flooding the transport cannot keep the thread reading: the model advances once per pass
 * whatever arrives. What is left stays in the socket for the next pass, which follows at once. */
#define TRANSPORT_READS_PER_PASS 256U

#define COMMAND_ABORT_TX 0x1U
#define COMMAND_RESTART  0x2U

/* The driver interface numbers the error states like the fault-confinement module and has as
 * many mailboxes as the model: the code below passes both through without translation. */
_Static_assert((int)PLAT_CAN_ERROR_ACTIVE == (int)CAN_FC_ERROR_ACTIVE, "state numbering");
_Static_assert((int)PLAT_CAN_ERROR_PASSIVE == (int)CAN_FC_ERROR_PASSIVE, "state numbering");
_Static_assert((int)PLAT_CAN_BUS_OFF == (int)CAN_FC_BUS_OFF, "state numbering");
_Static_assert(PLAT_CAN_TX_MAILBOXES == CAN_CTRL_TX_MAILBOXES, "mailbox count");
_Static_assert(TX_RING_ENTRIES >= CAN_CTRL_TX_MAILBOXES, "transmit ring too small");
_Static_assert(EVENT_RING_ENTRIES >= (2U * PLAT_CAN_TX_MAILBOXES), "event ring too small");

/* Longest sleep of the hardware thread: bounds how long stopping it takes. */
#define MAX_SLEEP_NS 50000000ULL

typedef struct {
    can_frame_t frame;
    uint32_t handle;
} tx_request_t;

static can_backend_t s_backend;
static can_ctrl_t s_ctrl;    /* touched by the hardware thread only */
static uint64_t s_pass_ns;   /* hardware thread only: the time of the current pass of its loop */
static uint8_t s_told_state; /* hardware thread only: the error state the driver was last sent */
static uint32_t s_bitrate;

static uint8_t s_tx_storage[TX_RING_ENTRIES * sizeof(tx_request_t)];
static uint8_t s_rx_storage[RX_RING_ENTRIES * sizeof(plat_can_rx_t)];
static uint8_t s_event_storage[EVENT_RING_ENTRIES * sizeof(plat_can_event_t)];
static spsc_ring_t s_tx_ring;
static spsc_ring_t s_rx_ring;
static spsc_ring_t s_event_ring;

static int s_wake_fd = -1;
static pthread_t s_thread;
static bool s_thread_started;
static _Atomic bool s_thread_run;
static _Atomic uint32_t s_commands;
static _Atomic uint32_t s_irq_causes;
static plat_can_isr_t s_isr;
static void *s_isr_context;

/* Transmit requests the driver made and has not yet taken the outcome of: the mailboxes in
 * use as the driver has to see them. Counted up by plat_can_tx(), down by plat_can_event(). */
static _Atomic uint32_t s_tx_outstanding;

/* Published by the hardware thread for readers in other threads. */
static _Atomic uint16_t s_tec;
static _Atomic uint16_t s_rec;
static _Atomic uint8_t s_state;
static _Atomic uint32_t s_rx_fifo_overruns;
static _Atomic uint32_t s_event_fifo_overruns;
static _Atomic uint32_t s_state_events_merged;
static _Atomic uint64_t s_pacing_lag_max_ns;
static _Atomic uint64_t s_pacing_lag_sum_ns;
static _Atomic uint64_t s_pacing_samples;
static uint64_t s_started_ns;
static uint64_t s_stopped_ns;

/* ------------------------------------------------------------------ hardware thread side */

static void raise_irq(uint32_t causes)
{
    (void)atomic_fetch_or(&s_irq_causes, causes);
    posix_irq_raise(CAN_IRQ_LINE);
}

/* Queue an event and interrupt the driver. Returns false if the event FIFO was full. */
static bool push_event(const plat_can_event_t *event)
{
    const bool pushed = spsc_ring_push(&s_event_ring, event);

    if (pushed) {
        raise_irq(PLAT_CAN_IRQ_EVENT);
    }

    return pushed;
}

/* Queue the outcome of a transmit request (TX_DONE or TX_ABORTED). There is always room for
 * it, by the argument at the top of this file; the counter is there to show that the argument
 * holds, and the tests check that it stays 0. */
static void push_outcome(const plat_can_event_t *event)
{
    if (!push_event(event)) {
        (void)atomic_fetch_add(&s_event_fifo_overruns, 1U);
    }
}

/* Tell the driver that a frame left its mailbox without being transmitted. */
static void report_aborted(uint32_t handle, uint64_t time_ns)
{
    plat_can_event_t event;

    (void)memset(&event, 0, sizeof(event));
    event.kind = PLAT_CAN_EVENT_TX_ABORTED;
    event.handle = handle;
    event.eof_ns = time_ns;
    push_outcome(&event);
}

/* Send the driver a state event unless `state` is what it was last sent. Returns true if the
 * driver has been sent `state` when the function returns, false if the event FIFO had no room
 * for a state event. The old state in the event is the one the driver was last sent, so the
 * events form an unbroken chain even when changes in between were never queued. */
static bool tell_state(uint8_t state, uint16_t tec, uint16_t rec, uint64_t time_ns)
{
    bool told = (state == s_told_state);

    if (!told && (spsc_ring_count(&s_event_ring) < STATE_EVENT_LIMIT)) {
        plat_can_event_t event;

        (void)memset(&event, 0, sizeof(event));
        event.kind = PLAT_CAN_EVENT_STATE;
        event.eof_ns = time_ns;
        event.state = state;
        event.old_state = s_told_state;
        event.tec = tec;
        event.rec = rec;
        told = push_event(&event);

        if (told) {
            s_told_state = state;
        }
    }

    return told;
}

static void publish_status(void)
{
    atomic_store(&s_state, (uint8_t)can_ctrl_state(&s_ctrl));
    atomic_store(&s_tec, can_ctrl_tec(&s_ctrl));
    atomic_store(&s_rec, can_ctrl_rec(&s_ctrl));
}

static void record_pacing_lag(uint64_t model_eof_ns)
{
    const uint64_t now_ns = posix_monotonic_ns();
    const uint64_t lag_ns = (now_ns > model_eof_ns) ? (now_ns - model_eof_ns) : 0U;

    if (lag_ns > atomic_load(&s_pacing_lag_max_ns)) {
        atomic_store(&s_pacing_lag_max_ns, lag_ns);
    }

    (void)atomic_fetch_add(&s_pacing_lag_sum_ns, lag_ns);
    (void)atomic_fetch_add(&s_pacing_samples, 1U);
}

/* Take every action the model has due and carry it out. */
static void run_model(uint64_t now_ns)
{
    can_ctrl_action_t action;

    while (can_ctrl_poll(&s_ctrl, now_ns, &action)) {
        plat_can_event_t event;

        (void)memset(&event, 0, sizeof(event));

        switch (action.kind) {
        case CAN_CTRL_ACTION_TX_COMPLETE:
            /* The frame has reached its end of frame on the modelled bus: now, and only
             * now, the other participants get it. */
            (void)s_backend.send(&s_backend, &action.frame);
            record_pacing_lag(action.time_ns);
            event.kind = PLAT_CAN_EVENT_TX_DONE;
            event.handle = action.handle;
            event.submit_ns = action.submit_ns;
            event.start_ns = action.start_ns;
            event.eof_ns = action.time_ns;
            push_outcome(&event);
            break;

        case CAN_CTRL_ACTION_RX_DELIVER: {
            plat_can_rx_t received;

            received.frame = action.frame;
            received.eof_ns = action.time_ns;

            if (spsc_ring_push(&s_rx_ring, &received)) {
                raise_irq(PLAT_CAN_IRQ_RX);
            } else {
                /* The driver did not drain its FIFO: a receive overrun, as on hardware. */
                (void)atomic_fetch_add(&s_rx_fifo_overruns, 1U);
            }
            break;
        }

        case CAN_CTRL_ACTION_STATE_CHANGED:
            /* With the counters and the time of the change itself, if the event can be
             * queued now. If not, the end of this function catches up. */
            if (!tell_state((uint8_t)action.state, action.tec, action.rec, action.time_ns)) {
                (void)atomic_fetch_add(&s_state_events_merged, 1U);
            }
            break;

        case CAN_CTRL_ACTION_TX_ABORTED:
            /* The frame that was on the wire when the driver aborted, removed now that its
             * attempt has failed. */
            report_aborted(action.handle, action.time_ns);
            break;

        case CAN_CTRL_ACTION_NONE:
        default:
            break;
        }
    }

    /* A state change that found no room earlier, in this pass or in one before: send the
     * state the controller is in now. This runs at least once per MAX_SLEEP_NS. */
    (void)tell_state((uint8_t)can_ctrl_state(&s_ctrl), can_ctrl_tec(&s_ctrl), can_ctrl_rec(&s_ctrl),
                     now_ns);
    publish_status();
}

static void serve_driver(uint64_t now_ns)
{
    uint64_t wake_count;
    uint32_t commands;

    /* Reset the wake-up first and look at what the driver left afterwards. A request made
     * after this read writes the eventfd again, so the poll at the end of the pass returns at
     * once: nothing can arrive unseen between looking and sleeping. Taking the commands
     * first would leave such a gap: a command set after the exchange and before the read
     * would have its wake-up consumed here and wait for the thread's next pass. The value
     * read is not needed. */
    (void)!read(s_wake_fd, &wake_count, sizeof(wake_count));
    commands = atomic_exchange(&s_commands, 0U);

    if ((commands & COMMAND_ABORT_TX) != 0U) {
        uint32_t handles[CAN_CTRL_TX_MAILBOXES];
        const uint32_t removed = can_ctrl_tx_abort_all(&s_ctrl, handles, CAN_CTRL_TX_MAILBOXES);
        tx_request_t request;

        for (uint32_t i = 0U; i < removed; i++) {
            report_aborted(handles[i], now_ns);
        }

        /* A request that is still on its way to a mailbox was made before the abort and is
         * aborted with the rest. Without this it would be loaded after the abort, sit in its
         * mailbox through the bus-off and go out stale after recovery. */
        while (spsc_ring_pop(&s_tx_ring, &request)) {
            report_aborted(request.handle, now_ns);
        }
    }

    if ((commands & COMMAND_RESTART) != 0U) {
        can_ctrl_request_restart(&s_ctrl, now_ns);
    }

    /* Move transmit requests into the controller's mailboxes. The driver never queues more
     * than there are mailboxes, so the loop cannot leave a request stranded for long. */
    while (can_ctrl_tx_pending(&s_ctrl) < CAN_CTRL_TX_MAILBOXES) {
        tx_request_t request;

        if (!spsc_ring_pop(&s_tx_ring, &request)) {
            break;
        }

        if (!can_ctrl_tx_submit(&s_ctrl, &request.frame, request.handle, now_ns)) {
            /* An invalid frame: give the mailbox back to the driver. */
            report_aborted(request.handle, now_ns);
        }
    }
}

static void read_transport(uint64_t now_ns)
{
    can_frame_t frame;
    uint32_t reads = 0U;

    while ((reads < TRANSPORT_READS_PER_PASS) && (s_backend.recv(&s_backend, &frame) == 1)) {
        (void)can_ctrl_wire_rx(&s_ctrl, &frame, now_ns);
        reads++;
    }
}

static void *hardware_main(void *arg)
{
    (void)arg;
    posix_external_thread_setup("fcn-can");
    (void)prctl(PR_SET_TIMERSLACK, 1UL); /* wake-ups as close to the deadline as the host gives */

    while (atomic_load(&s_thread_run)) {
        struct pollfd fds[3];
        nfds_t count = 2U;

        /* One time for everything done in this pass, simulation control commands included:
         * the model requires that the times it is given never go backwards. */
        s_pass_ns = posix_monotonic_ns();
        posix_sim_service();
        serve_driver(s_pass_ns);
        read_transport(s_pass_ns);
        run_model(s_pass_ns);

        const uint64_t wake_ns = can_ctrl_next_wakeup(&s_ctrl);
        const uint64_t now_ns = posix_monotonic_ns();
        uint64_t sleep_ns = MAX_SLEEP_NS;

        if (wake_ns != CAN_CTRL_NO_WAKEUP) {
            sleep_ns = (wake_ns > now_ns) ? (wake_ns - now_ns) : 0U;

            if (sleep_ns > MAX_SLEEP_NS) {
                sleep_ns = MAX_SLEEP_NS;
            }
        }

        const struct timespec timeout = {(time_t)(sleep_ns / 1000000000ULL),
                                         (long)(sleep_ns % 1000000000ULL)};

        fds[0].fd = s_backend.fd;
        fds[0].events = POLLIN;
        fds[1].fd = s_wake_fd;
        fds[1].events = POLLIN;

        if (posix_sim_enabled()) {
            fds[2].fd = posix_sim_fd();
            fds[2].events = POLLIN;
            count = 3U;
        }

        (void)ppoll(fds, count, &timeout, NULL);
    }

    return NULL;
}

/* -------------------------------------------------------------------- simulation control */

static const char *state_name(can_fc_state_t state)
{
    static const char *const names[] = {"active", "passive", "busoff"};

    return ((unsigned int)state < 3U) ? names[state] : "?";
}

static const char *fault_name(can_bus_fault_t fault)
{
    static const char *const names[] = {"none", "short", "dominant", "noack"};

    return ((unsigned int)fault < 4U) ? names[fault] : "?";
}

/* Commands under "can.": status | fault <none|short|dominant|noack> | inject <tx|rx> <count>.
 * Runs in the hardware thread, within a pass of its loop. */
static bool sim_can_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    const uint64_t now_ns = s_pass_ns;

    if (strcmp(verb, "status") == 0) {
        const can_ctrl_stats_t *stats = can_ctrl_stats(&s_ctrl);
        const uint64_t samples = atomic_load(&s_pacing_samples);

        can_backend_refresh_counters(&s_backend);

        (void)snprintf(
            reply, reply_len,
            "state=%s tec=%u rec=%u fault=%s tx=%llu tx_failed=%llu tx_aborted=%llu rx=%llu "
            "rx_errors=%llu rx_lost_fault=%llu wire_overruns=%llu bus_off=%llu late_resyncs=%llu "
            "busy_ns=%llu elapsed_ns=%llu rx_fifo_overruns=%u transport_tx=%llu transport_rx=%llu "
            "transport_malformed=%llu transport_overflow=%llu pacing_lag_max_ns=%llu "
            "pacing_lag_mean_ns=%llu",
            state_name(can_ctrl_state(&s_ctrl)), (unsigned int)can_ctrl_tec(&s_ctrl),
            (unsigned int)can_ctrl_rec(&s_ctrl), fault_name(can_ctrl_fault(&s_ctrl)),
            (unsigned long long)stats->tx_frames, (unsigned long long)stats->tx_attempts_failed,
            (unsigned long long)stats->tx_aborted, (unsigned long long)stats->rx_frames,
            (unsigned long long)stats->rx_errors, (unsigned long long)stats->rx_lost_fault,
            (unsigned long long)stats->wire_overruns, (unsigned long long)stats->bus_off_events,
            (unsigned long long)stats->late_resyncs, (unsigned long long)stats->busy_ns,
            (unsigned long long)(now_ns - s_started_ns),
            (unsigned int)atomic_load(&s_rx_fifo_overruns), (unsigned long long)s_backend.tx_frames,
            (unsigned long long)s_backend.rx_frames, (unsigned long long)s_backend.rx_malformed,
            (unsigned long long)s_backend.rx_overflow,
            (unsigned long long)atomic_load(&s_pacing_lag_max_ns),
            (unsigned long long)((samples > 0U) ? (atomic_load(&s_pacing_lag_sum_ns) / samples)
                                                : 0U));
        return true;
    }

    if (strcmp(verb, "fault") == 0) {
        static const struct {
            const char *name;
            can_bus_fault_t fault;
        } table[] = {{"none", CAN_BUS_FAULT_NONE},
                     {"short", CAN_BUS_FAULT_SHORT},
                     {"dominant", CAN_BUS_FAULT_STUCK_DOMINANT},
                     {"noack", CAN_BUS_FAULT_NO_ACK}};

        for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
            if (strcmp(args, table[i].name) == 0) {
                can_ctrl_set_fault(&s_ctrl, table[i].fault, now_ns);
                (void)snprintf(reply, reply_len, "fault=%s t_ns=%llu", table[i].name,
                               (unsigned long long)now_ns);
                return true;
            }
        }

        (void)snprintf(reply, reply_len, "unknown fault '%s' (none, short, dominant, noack)", args);
        return false;
    }

    if (strcmp(verb, "inject") == 0) {
        char what[8] = {0};
        unsigned int count = 0U;

        /* NOLINTNEXTLINE(cert-err34-c): a count that does not convert fails the match */
        if ((sscanf(args, "%7s %u", what, &count) == 2) && (count > 0U)) {
            if (strcmp(what, "tx") == 0) {
                can_ctrl_inject_tx_errors(&s_ctrl, count);
                (void)snprintf(reply, reply_len, "tx_errors=%u", count);
                return true;
            }

            if (strcmp(what, "rx") == 0) {
                can_ctrl_inject_rx_errors(&s_ctrl, count);
                (void)snprintf(reply, reply_len, "rx_errors=%u", count);
                return true;
            }
        }

        (void)snprintf(reply, reply_len, "usage: can.inject <tx|rx> <count>");
        return false;
    }

    (void)snprintf(reply, reply_len, "unknown command 'can.%s'", verb);
    return false;
}

/* ---------------------------------------------------------------------------- driver side */

/* The simulated CAN interrupt: runs on the task thread, in the port's interrupt context. */
static void can_irq(void *context, BaseType_t *woken)
{
    const uint32_t causes = atomic_exchange(&s_irq_causes, 0U);

    (void)context;

    if ((causes != 0U) && (s_isr != NULL)) {
        s_isr(s_isr_context, causes, woken);
    }
}

void plat_can_set_isr(plat_can_isr_t isr, void *context)
{
    s_isr = isr;
    s_isr_context = context;
}

/* Wake the hardware thread out of its poll. A write to an eventfd neither blocks nor takes a
 * lock in the C library, so a task may do it; it is repeated if a signal (the tick, a
 * simulated interrupt) cut it short, because a wake-up that is lost would leave a frame
 * waiting until the thread's next periodic pass. */
static void wake_hardware(void)
{
    const uint64_t one = 1U;
    ssize_t written;

    do {
        written = write(s_wake_fd, &one, sizeof(one));
    } while ((written < 0) && (errno == EINTR));
}

bool plat_can_tx(const can_frame_t *frame, uint32_t handle)
{
    bool accepted = false;

    /* This function is the only one that counts up and has a single caller, so the check and
     * the count need not be one atomic step. The count goes up before the request can be
     * seen by the hardware thread: its outcome may be taken, and counted down, by another
     * task before this function has returned. */
    if (atomic_load(&s_tx_outstanding) < PLAT_CAN_TX_MAILBOXES) {
        tx_request_t request;

        request.frame = *frame;
        request.handle = handle;
        (void)atomic_fetch_add(&s_tx_outstanding, 1U);

        if (spsc_ring_push(&s_tx_ring, &request)) {
            wake_hardware();
            accepted = true;
        } else {
            /* Cannot happen: the ring holds more requests than may be outstanding. */
            (void)atomic_fetch_sub(&s_tx_outstanding, 1U);
        }
    }

    return accepted;
}

bool plat_can_rx(plat_can_rx_t *out)
{
    return spsc_ring_pop(&s_rx_ring, out);
}

bool plat_can_event(plat_can_event_t *out)
{
    const bool taken = spsc_ring_pop(&s_event_ring, out);

    if (taken && (out->kind != PLAT_CAN_EVENT_STATE)) {
        /* The driver now knows what became of the frame: its mailbox is free again. */
        (void)atomic_fetch_sub(&s_tx_outstanding, 1U);
    }

    return taken;
}

static void send_command(uint32_t command)
{
    (void)atomic_fetch_or(&s_commands, command);
    wake_hardware();
}

void plat_can_abort_tx(void)
{
    send_command(COMMAND_ABORT_TX);
}

void plat_can_request_restart(void)
{
    send_command(COMMAND_RESTART);
}

void plat_can_get_status(plat_can_status_t *out)
{
    out->state = atomic_load(&s_state);
    out->tec = atomic_load(&s_tec);
    out->rec = atomic_load(&s_rec);
    out->rx_fifo_overruns = atomic_load(&s_rx_fifo_overruns);
}

/* ------------------------------------------------------------------------------ lifecycle */

bool posix_can_start(const char *backend_spec, uint32_t bitrate, char *error, size_t error_len)
{
    const can_ctrl_config_t config = {.bitrate = bitrate};

    if (!can_backend_open(&s_backend, backend_spec, error, error_len)) {
        return false;
    }

    s_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (s_wake_fd < 0) {
        (void)snprintf(error, error_len, "can: eventfd: %s", strerror(errno));
        s_backend.close(&s_backend);
        return false;
    }

    (void)spsc_ring_init(&s_tx_ring, s_tx_storage, (uint32_t)sizeof(tx_request_t), TX_RING_ENTRIES);
    (void)spsc_ring_init(&s_rx_ring, s_rx_storage, (uint32_t)sizeof(plat_can_rx_t),
                         RX_RING_ENTRIES);
    (void)spsc_ring_init(&s_event_ring, s_event_storage, (uint32_t)sizeof(plat_can_event_t),
                         EVENT_RING_ENTRIES);

    s_bitrate = bitrate;
    s_started_ns = posix_monotonic_ns();
    s_pass_ns = s_started_ns;
    can_ctrl_init(&s_ctrl, &config, s_started_ns);
    s_told_state = (uint8_t)can_ctrl_state(&s_ctrl);
    s_stopped_ns = 0U;
    atomic_store(&s_tx_outstanding, 0U);
    atomic_store(&s_commands, 0U);
    atomic_store(&s_irq_causes, 0U);
    atomic_store(&s_rx_fifo_overruns, 0U);
    atomic_store(&s_event_fifo_overruns, 0U);
    atomic_store(&s_state_events_merged, 0U);
    atomic_store(&s_pacing_lag_max_ns, 0U);
    atomic_store(&s_pacing_lag_sum_ns, 0U);
    atomic_store(&s_pacing_samples, 0U);
    publish_status();
    posix_irq_attach(CAN_IRQ_LINE, can_irq, NULL);
    posix_sim_register("can.", sim_can_handler);

    atomic_store(&s_thread_run, true);

    if (pthread_create(&s_thread, NULL, hardware_main, NULL) != 0) {
        (void)snprintf(error, error_len, "can: cannot start the hardware thread");
        s_backend.close(&s_backend);
        (void)close(s_wake_fd);
        s_wake_fd = -1;
        return false;
    }

    s_thread_started = true;
    return true;
}

void posix_can_stop(void)
{
    if (s_thread_started) {
        atomic_store(&s_thread_run, false);
        wake_hardware();
        (void)pthread_join(s_thread, NULL);
        s_thread_started = false;
        s_stopped_ns = posix_monotonic_ns();
        s_backend.close(&s_backend);
        (void)close(s_wake_fd);
        s_wake_fd = -1;
    }
}

void posix_can_get_report(posix_can_report_t *out)
{
    const can_ctrl_stats_t *stats = can_ctrl_stats(&s_ctrl);

    (void)memset(out, 0, sizeof(*out));
    out->transport = (s_backend.kind != NULL) ? s_backend.kind : "none";
    out->bitrate = s_bitrate;
    out->elapsed_ns = (s_stopped_ns > s_started_ns) ? (s_stopped_ns - s_started_ns) : 0U;
    out->state = (uint8_t)can_ctrl_state(&s_ctrl);
    out->tec = can_ctrl_tec(&s_ctrl);
    out->rec = can_ctrl_rec(&s_ctrl);
    out->tx_frames = stats->tx_frames;
    out->tx_attempts_failed = stats->tx_attempts_failed;
    out->tx_aborted = stats->tx_aborted;
    out->rx_frames = stats->rx_frames;
    out->rx_errors = stats->rx_errors;
    out->rx_lost_fault = stats->rx_lost_fault;
    out->wire_overruns = stats->wire_overruns;
    out->bus_off_events = stats->bus_off_events;
    out->late_resyncs = stats->late_resyncs;
    out->busy_ns = stats->busy_ns;
    out->transport_tx_frames = s_backend.tx_frames;
    out->transport_tx_failed = s_backend.tx_failed;
    out->transport_rx_frames = s_backend.rx_frames;
    out->transport_rx_malformed = s_backend.rx_malformed;
    out->transport_rx_overflow = s_backend.rx_overflow;
}

void posix_can_get_stats(posix_can_stats_t *out)
{
    const uint64_t samples = atomic_load(&s_pacing_samples);

    out->pacing_lag_max_ns = atomic_load(&s_pacing_lag_max_ns);
    out->pacing_lag_mean_ns = (samples > 0U) ? (atomic_load(&s_pacing_lag_sum_ns) / samples) : 0U;
    out->frames_paced = samples;
    out->rx_fifo_overruns = atomic_load(&s_rx_fifo_overruns);
    out->event_fifo_overruns = atomic_load(&s_event_fifo_overruns);
    out->state_events_merged = atomic_load(&s_state_events_merged);
}
