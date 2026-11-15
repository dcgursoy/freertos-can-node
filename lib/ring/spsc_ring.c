/* SPDX-License-Identifier: MIT */
#include "ring/spsc_ring.h"

#include <string.h>

static uint8_t *slot(const spsc_ring_t *ring, uint32_t index)
{
    return &ring->storage[(size_t)(index & ring->mask) * (size_t)ring->elem_size];
}

bool spsc_ring_init(spsc_ring_t *ring, void *storage, uint32_t elem_size, uint32_t capacity)
{
    bool ok = false;

    if ((ring != NULL) && (storage != NULL) && (elem_size >= 1U) && (capacity >= 2U) &&
        ((capacity & (capacity - 1U)) == 0U)) {
        ring->storage = storage;
        ring->elem_size = elem_size;
        ring->mask = capacity - 1U;
        atomic_init(&ring->head, 0U);
        atomic_init(&ring->tail, 0U);
        ok = true;
    }

    return ok;
}

bool spsc_ring_push(spsc_ring_t *ring, const void *elem)
{
    /* The producer owns head, so a relaxed load is enough for its own index. */
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    bool pushed = false;

    if ((head - tail) <= ring->mask) {
        (void)memcpy(slot(ring, head), elem, ring->elem_size);
        atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
        pushed = true;
    }

    return pushed;
}

bool spsc_ring_pop(spsc_ring_t *ring, void *elem)
{
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    bool popped = false;

    if (head != tail) {
        (void)memcpy(elem, slot(ring, tail), ring->elem_size);
        atomic_store_explicit(&ring->tail, tail + 1U, memory_order_release);
        popped = true;
    }

    return popped;
}

uint32_t spsc_ring_count(const spsc_ring_t *ring)
{
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    return head - tail;
}

uint32_t spsc_ring_capacity(const spsc_ring_t *ring)
{
    return ring->mask + 1U;
}
