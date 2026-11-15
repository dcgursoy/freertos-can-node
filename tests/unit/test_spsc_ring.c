/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <string.h>

#include "ring/spsc_ring.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_init_rejects_invalid_arguments(void)
{
    spsc_ring_t ring;
    uint8_t storage[16];

    TEST_ASSERT_FALSE(spsc_ring_init(NULL, storage, 1U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, NULL, 1U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 0U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 0U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 1U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 12U)); /* not a power of two */
    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, 1U, 16U));
    TEST_ASSERT_EQUAL_UINT32(16U, spsc_ring_capacity(&ring));
    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

static void test_elements_come_out_in_the_order_they_went_in(void)
{
    spsc_ring_t ring;
    uint32_t storage[8];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 8U));

    for (uint32_t i = 0U; i < 5U; i++) {
        const uint32_t value = 100U + i;

        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &value));
    }

    TEST_ASSERT_EQUAL_UINT32(5U, spsc_ring_count(&ring));

    for (uint32_t i = 0U; i < 5U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT32(100U + i, out);
    }

    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

static void test_full_ring_rejects_push_and_keeps_its_contents(void)
{
    spsc_ring_t ring;
    uint8_t storage[4];
    uint8_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, 1U, 4U));

    for (uint8_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));
    }

    const uint8_t extra = 0xEEU;

    TEST_ASSERT_FALSE(spsc_ring_push(&ring, &extra));
    TEST_ASSERT_EQUAL_UINT32(4U, spsc_ring_count(&ring));

    for (uint8_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT8(i, out);
    }
}

static void test_empty_ring_rejects_pop_and_leaves_the_output_alone(void)
{
    spsc_ring_t ring;
    uint16_t storage[2];
    uint16_t out = 0xBEEFU;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint16_t), 2U));
    TEST_ASSERT_FALSE(spsc_ring_pop(&ring, &out));
    TEST_ASSERT_EQUAL_HEX16(0xBEEFU, out);
}

static void test_slots_are_reused_across_many_wraps(void)
{
    spsc_ring_t ring;
    uint32_t storage[4];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 4U));

    /* Keep the ring partly filled so pushes and pops straddle the wrap point. */
    for (uint32_t i = 0U; i < 1000U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));

        if (i >= 2U) {
            TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
            TEST_ASSERT_EQUAL_UINT32(i - 2U, out);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(2U, spsc_ring_count(&ring));
}

static void test_free_running_indices_survive_32_bit_overflow(void)
{
    spsc_ring_t ring;
    uint32_t storage[8];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 8U));

    /* Start three elements before the counters overflow. */
    atomic_store(&ring.head, UINT32_MAX - 2U);
    atomic_store(&ring.tail, UINT32_MAX - 2U);

    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));
    }

    const uint32_t extra = 99U;

    TEST_ASSERT_FALSE(spsc_ring_push(&ring, &extra));
    TEST_ASSERT_EQUAL_UINT32(8U, spsc_ring_count(&ring));

    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT32(i, out);
    }

    TEST_ASSERT_FALSE(spsc_ring_pop(&ring, &out));
}

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} frame_like_t;

static void test_multi_byte_elements_are_copied_whole(void)
{
    spsc_ring_t ring;
    frame_like_t storage[4];
    frame_like_t in;
    frame_like_t out;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(frame_like_t), 4U));

    for (uint32_t n = 0U; n < 10U; n++) {
        (void)memset(&in, 0, sizeof(in));
        in.id = 0x100U + n;
        in.dlc = 8U;
        (void)memset(in.data, (int)(0x10U + n), sizeof(in.data));

        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &in));
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
    }
}

/* One producer thread and one consumer thread, no lock: every value must arrive exactly once
 * and in order. A small ring forces both the full and the empty path many times. */
#define STRESS_ITEMS 200000U

static void *stress_producer(void *arg)
{
    spsc_ring_t *ring = arg;

    for (uint32_t value = 0U; value < STRESS_ITEMS;) {
        if (spsc_ring_push(ring, &value)) {
            value++;
        }
    }

    return NULL;
}

static void test_two_threads_transfer_a_sequence_without_loss_or_reordering(void)
{
    spsc_ring_t ring;
    static uint32_t storage[64];
    pthread_t producer;
    uint32_t expected = 0U;
    uint32_t out = 0U;
    uint32_t mismatches = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 64U));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&producer, NULL, stress_producer, &ring));

    while (expected < STRESS_ITEMS) {
        if (spsc_ring_pop(&ring, &out)) {
            if (out != expected) {
                mismatches++;
            }

            expected++;
        }
    }

    TEST_ASSERT_EQUAL_INT(0, pthread_join(producer, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, mismatches);
    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_invalid_arguments);
    RUN_TEST(test_elements_come_out_in_the_order_they_went_in);
    RUN_TEST(test_full_ring_rejects_push_and_keeps_its_contents);
    RUN_TEST(test_empty_ring_rejects_pop_and_leaves_the_output_alone);
    RUN_TEST(test_slots_are_reused_across_many_wraps);
    RUN_TEST(test_free_running_indices_survive_32_bit_overflow);
    RUN_TEST(test_multi_byte_elements_are_copied_whole);
    RUN_TEST(test_two_threads_transfer_a_sequence_without_loss_or_reordering);
    return UNITY_END();
}
