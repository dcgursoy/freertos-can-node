/* SPDX-License-Identifier: MIT */
/**
 * @file
 * CAN input and output of the application: the receive task, the transmit task and the
 * interrupt handler between them and the controller.
 *
 *                 can_io_publish()            +----------+   plat_can_tx()   +------------+
 *   producers ---------------------------->   | transmit |  --------------> | controller |
 *   (any task)    can_io_send_event()         |   task   |  <-------------- | (plat_can) |
 *                 -> transmit queue           +----------+   events, by     +------------+
 *                    (lib/can/can_txq)             ^         notification         |
 *                                                  |                             | interrupt
 *                                             +----------+                       v
 *   handlers  <-----------------------------  | receive  |  <--------------  can_io ISR
 *   (in the receive task's context)           |   task   |   plat_can_rx(), by notification
 *                                             +----------+
 *
 * Why two tasks. Reception is the most urgent thing the node does: a request has to be seen
 * and a flow-control frame answered within the transport protocol's timing, so the receive
 * task has the highest priority and does little per frame. Transmission is driven by what the
 * other tasks produce and by the controller's transmit-done events; it runs below the tasks
 * that produce the data, so a producer is never preempted by its own output.
 *
 * Why task notifications. Each task waits for "something for you" from an interrupt, with a
 * few distinct causes that may coincide. A notification value used as a set of bits does
 * exactly that without a kernel object per cause, and a bit set while the task is busy is not
 * lost: it is still there when the task next waits.
 */
#ifndef FCN_CAN_IO_H
#define FCN_CAN_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"
#include "can/can_txq.h"

/** Priorities of the two tasks (the application's priority table is in app.c). */
#define CAN_IO_RX_PRIORITY 6U
#define CAN_IO_TX_PRIORITY 3U

/** Most frames the receive task handles before it lets other work run for one tick. One batch
 *  per tick has to keep up with a full bus at PLAT_CAN_MAX_BITRATE (can_io.c checks this at
 *  compile time): 1 Mbit/s carries up to 21.3 of the shortest frames per millisecond, and
 *  the margin above that lets the task catch up after a late tick. */
#define CAN_IO_RX_BATCH 32U

/** Number of identifiers a handler can be registered for. */
#define CAN_IO_RX_HANDLERS 8U

/** Controller mailboxes the transmit task keeps loaded. */
#define CAN_IO_TX_MAILBOXES 3U

/**
 * Called in the receive task for every received frame whose identifier and format match.
 * `eof_ns` is when the frame ended on the bus. Keep it short: it runs at the highest task
 * priority.
 */
typedef void (*can_io_rx_handler_t)(const can_frame_t *frame, uint64_t eof_ns);

typedef struct {
    uint32_t rx_frames;      /**< frames taken from the controller */
    uint32_t rx_unhandled;   /**< of those, frames no handler was registered for */
    uint32_t rx_batches_cut; /**< times the receive task stopped at CAN_IO_RX_BATCH */
    uint32_t tx_load_failed; /**< times the controller refused a frame the queue handed out */
} can_io_stats_t;

/** Create the two tasks and connect the interrupt. Call once, before the scheduler starts. */
void can_io_start(void);

/** Register `handler` for data frames with this identifier. Call before the scheduler
 *  starts. Returns false if the table is full. */
bool can_io_on_frame(uint32_t id, bool extended, can_io_rx_handler_t handler);

/** Publish the current value of a cyclic message (see can_txq_publish). Any task. */
bool can_io_publish(uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns);

/** Queue an event frame (see can_txq_enqueue). Any task. Returns false if it was refused. */
bool can_io_send_event(const can_frame_t *frame);

/** Copies of the counters. Any task. */
void can_io_get_stats(can_io_stats_t *io, can_txq_stats_t *queue);

#endif /* FCN_CAN_IO_H */
