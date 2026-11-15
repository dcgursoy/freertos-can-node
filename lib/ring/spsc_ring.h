/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Lock-free single-producer / single-consumer ring of fixed-size elements.
 *
 * Exactly one context may push and exactly one other context may pop at the same time, with
 * no lock between them: the producer is the only writer of `head`, the consumer the only
 * writer of `tail`. Each side publishes its index with release ordering after it has finished
 * touching the element, and reads the other side's index with acquire ordering, so an element
 * is never visible to the consumer before it is completely written.
 *
 * This is the hand-over point between a context that must never block (an interrupt handler,
 * or on the host a thread that models a peripheral) and a task.
 *
 * The indices are free-running 32-bit counters; the element slot is `index & mask`. The
 * number of stored elements is `head - tail` in modular arithmetic, which stays correct when
 * the counters wrap around.
 */
#ifndef FCN_SPSC_RING_H
#define FCN_SPSC_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *storage;      /**< capacity * elem_size bytes, owned by the caller */
    uint32_t elem_size;    /**< size of one element in bytes */
    uint32_t mask;         /**< capacity - 1 */
    _Atomic uint32_t head; /**< count of elements ever pushed; written by the producer only */
    _Atomic uint32_t tail; /**< count of elements ever popped; written by the consumer only */
} spsc_ring_t;

/**
 * Prepare a ring over caller-provided storage.
 *
 * @param ring       ring to initialise
 * @param storage    at least capacity * elem_size bytes
 * @param elem_size  size of one element in bytes, at least 1
 * @param capacity   number of elements, a power of two and at least 2
 * @return false (ring untouched) if an argument is invalid
 */
bool spsc_ring_init(spsc_ring_t *ring, void *storage, uint32_t elem_size, uint32_t capacity);

/** Producer side: copy one element in. Returns false, storing nothing, when the ring is full. */
bool spsc_ring_push(spsc_ring_t *ring, const void *elem);

/** Consumer side: copy the oldest element out. Returns false when the ring is empty. */
bool spsc_ring_pop(spsc_ring_t *ring, void *elem);

/**
 * Number of stored elements, always between 0 and the capacity. The other side may be working
 * while the two indices are read, so the value is conservative for whoever asks:
 *
 *  - the consumer never gets more than are stored: it can pop at least that many;
 *  - the producer never gets fewer than are stored: at least capacity - count slots are free;
 *  - any other context gets an estimate, good for a fill-level display and nothing more.
 */
uint32_t spsc_ring_count(const spsc_ring_t *ring);

/** Number of elements the ring can hold. */
uint32_t spsc_ring_capacity(const spsc_ring_t *ring);

#endif /* FCN_SPSC_RING_H */
