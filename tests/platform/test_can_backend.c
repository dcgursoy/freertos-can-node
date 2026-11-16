/* SPDX-License-Identifier: MIT */
/*
 * Tests of the CAN transports (platform/posix/can_backend*): the UDP datagram format, the UDP
 * bus between several participants on the loopback interface, and SocketCAN when the machine
 * has a vcan0 interface (the test is reported as ignored when it does not).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "can_backend.h"
#include "test_bus_ports.h"
#include "unity.h"

#define RECEIVE_TIMEOUT_MS 500

static char s_error[160];

void setUp(void)
{
    s_error[0] = '\0';
}

void tearDown(void)
{
}

/* Wait until a frame arrives or the timeout passes. */
static int receive_within(can_backend_t *backend, can_frame_t *frame, int timeout_ms)
{
    struct pollfd fd = {.fd = backend->fd, .events = POLLIN, .revents = 0};
    int result = backend->recv(backend, frame);

    if ((result == 0) && (poll(&fd, 1U, timeout_ms) > 0)) {
        result = backend->recv(backend, frame);
    }

    return result;
}

static can_frame_t make_frame(uint32_t id, uint8_t flags, uint8_t dlc, uint8_t seed)
{
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = id;
    frame.flags = flags;
    frame.dlc = dlc;

    if ((flags & CAN_FLAG_RTR) == 0U) {
        for (uint8_t i = 0U; i < dlc; i++) {
            frame.data[i] = (uint8_t)(seed + i);
        }
    }

    return frame;
}

static void assert_frames_equal(const can_frame_t *expected, const can_frame_t *actual)
{
    TEST_ASSERT_EQUAL_HEX32(expected->id, actual->id);
    TEST_ASSERT_EQUAL_HEX8(expected->flags, actual->flags);
    TEST_ASSERT_EQUAL_UINT8(expected->dlc, actual->dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->data, actual->data, CAN_MAX_DLC);
}

/* ------------------------------------------------------------------ the datagram format */

static void test_udp_datagram_has_the_documented_layout(void)
{
    /* Base-format identifier 0x123, three data bytes: can_id 0x00000123 little-endian,
     * length 3, three zero bytes, then the data and zero fill. */
    const uint8_t expected_base[CAN_UDP_DATAGRAM_LEN] = {0x23, 0x01, 0x00, 0x00, 0x03, 0x00,
                                                         0x00, 0x00, 0x11, 0x22, 0x33, 0x00,
                                                         0x00, 0x00, 0x00, 0x00};
    /* Extended identifier 0x1ABCDEF0: bit 31 marks the format, so can_id is 0x9ABCDEF0. */
    const uint8_t expected_extended[CAN_UDP_DATAGRAM_LEN] = {0xF0, 0xDE, 0xBC, 0x9A, 0x01, 0x00,
                                                             0x00, 0x00, 0x7F, 0x00, 0x00, 0x00,
                                                             0x00, 0x00, 0x00, 0x00};
    /* Remote request for identifier 0x7FF with DLC 8: bit 30 set, no data on the wire. */
    const uint8_t expected_remote[CAN_UDP_DATAGRAM_LEN] = {0xFF, 0x07, 0x00, 0x40, 0x08, 0x00,
                                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                           0x00, 0x00, 0x00, 0x00};
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x123U;
    frame.dlc = 3U;
    frame.data[0] = 0x11U;
    frame.data[1] = 0x22U;
    frame.data[2] = 0x33U;
    frame.data[3] = 0xEEU; /* beyond the length: must not reach the wire */
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_base, datagram, CAN_UDP_DATAGRAM_LEN);

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x1ABCDEF0U;
    frame.flags = CAN_FLAG_EXT;
    frame.dlc = 1U;
    frame.data[0] = 0x7FU;
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_extended, datagram, CAN_UDP_DATAGRAM_LEN);

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x7FFU;
    frame.flags = CAN_FLAG_RTR;
    frame.dlc = 8U;
    frame.data[0] = 0xAAU; /* a remote frame carries no data */
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_remote, datagram, CAN_UDP_DATAGRAM_LEN);
}

static void test_udp_decoding_restores_what_was_encoded(void)
{
    const struct {
        uint32_t id;
        uint8_t flags;
    } kinds[] = {{0x000U, 0U},           {0x7FFU, 0U},
                 {0x000U, CAN_FLAG_EXT}, {0x1FFFFFFFU, CAN_FLAG_EXT},
                 {0x456U, CAN_FLAG_RTR}, {0x12345678U, (uint8_t)(CAN_FLAG_EXT | CAN_FLAG_RTR)}};

    for (size_t k = 0U; k < (sizeof(kinds) / sizeof(kinds[0])); k++) {
        for (uint8_t dlc = 0U; dlc <= CAN_MAX_DLC; dlc++) {
            const can_frame_t original =
                make_frame(kinds[k].id, kinds[k].flags, dlc, (uint8_t)(0x40U + dlc));
            uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
            can_frame_t decoded;

            can_udp_encode(&original, datagram);
            TEST_ASSERT_TRUE(can_udp_decode(datagram, sizeof(datagram), &decoded));
            assert_frames_equal(&original, &decoded);
        }
    }
}

static void test_udp_decoding_rejects_everything_that_is_not_a_frame(void)
{
    const can_frame_t valid = make_frame(0x123U, 0U, 4U, 0x10U);
    uint8_t good[CAN_UDP_DATAGRAM_LEN];
    uint8_t bad[CAN_UDP_DATAGRAM_LEN + 1U];
    can_frame_t decoded;

    can_udp_encode(&valid, good);
    TEST_ASSERT_TRUE(can_udp_decode(good, sizeof(good), &decoded));

    (void)memset(bad, 0, sizeof(bad));
    (void)memcpy(bad, good, sizeof(good));
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN - 1U, &decoded), "short");
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN + 1U, &decoded), "long");
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, 0U, &decoded), "empty");

    (void)memcpy(bad, good, sizeof(good));
    bad[3] |= 0x20U; /* bit 29 of can_id: an error frame */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded), "error flag");

    (void)memcpy(bad, good, sizeof(good));
    bad[4] = 9U;
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded), "length 9");

    (void)memcpy(bad, good, sizeof(good));
    bad[1] = 0x08U; /* identifier 0x823 without the extended flag */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "identifier too large for the base format");

    for (size_t pad = 5U; pad <= 7U; pad++) {
        (void)memcpy(bad, good, sizeof(good));
        bad[pad] = 1U;
        TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                                  "non-zero padding");
    }

    (void)memcpy(bad, good, sizeof(good));
    bad[8U + 4U] = 0x55U; /* a data byte beyond the length of 4 */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "data beyond the length");

    (void)memcpy(bad, good, sizeof(good));
    bad[3] |= 0x40U; /* remote request, but the data bytes are still there */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "remote frame with data");
}

/* ------------------------------------------------------------------------- the UDP bus */

static void test_three_udp_participants_hear_each_other_but_not_themselves(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t a;
    can_backend_t b;
    can_backend_t c;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&a, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&b, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&c, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_EQUAL_STRING("udp", a.kind);

    const can_frame_t from_a = make_frame(0x100U, 0U, 8U, 0xA0U);
    const can_frame_t from_c = make_frame(0x1ABCDE00U, CAN_FLAG_EXT, 2U, 0xC0U);

    TEST_ASSERT_TRUE(a.send(&a, &from_a));
    TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_a, &received);
    TEST_ASSERT_EQUAL_INT(1, receive_within(&c, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_a, &received);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, receive_within(&a, &received, 50), "a heard itself");

    TEST_ASSERT_TRUE(c.send(&c, &from_c));
    TEST_ASSERT_EQUAL_INT(1, receive_within(&a, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_c, &received);
    TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_c, &received);

    TEST_ASSERT_EQUAL_UINT64(1U, a.tx_frames);
    TEST_ASSERT_EQUAL_UINT64(1U, a.rx_frames);
    TEST_ASSERT_EQUAL_UINT64(2U, b.rx_frames);
    TEST_ASSERT_EQUAL_UINT64(0U, b.rx_malformed);

    a.close(&a);
    b.close(&b);
    c.close(&c);
    TEST_ASSERT_EQUAL_INT(-1, a.fd);
}

static void test_a_burst_arrives_complete_and_in_order(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t sender;
    can_backend_t receiver;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&sender, base, s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&receiver, base, s_error, sizeof(s_error)),
                             s_error);

    /* 100 frames with nobody reading: they wait in the receiver's socket buffer. */
    for (uint32_t n = 0U; n < 100U; n++) {
        can_frame_t frame = make_frame(0x200U, 0U, 4U, 0U);

        frame.data[0] = (uint8_t)n;
        TEST_ASSERT_TRUE(sender.send(&sender, &frame));
    }

    for (uint32_t n = 0U; n < 100U; n++) {
        TEST_ASSERT_EQUAL_INT(1, receive_within(&receiver, &received, RECEIVE_TIMEOUT_MS));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)n, received.data[0]);
    }

    TEST_ASSERT_EQUAL_INT(0, receiver.recv(&receiver, &received));
    can_backend_refresh_counters(&receiver);
    TEST_ASSERT_EQUAL_UINT64(0U, receiver.rx_overflow);
    sender.close(&sender);
    receiver.close(&receiver);
}

static void test_frames_that_do_not_fit_the_socket_buffer_are_dropped_and_counted(void)
{
    /* UDP has no back-pressure: a participant that does not read loses what its socket
     * buffer cannot hold. What matters is that every loss is counted, so that a run which
     * dropped frames can be recognised and discarded. The kernel grants at most
     * net.core.rmem_max of the buffer this backend asks for; 20,000 frames exceed what even
     * a generous setting holds (a queued datagram costs the kernel close to a kilobyte). */
    enum { SENT = 20000 };
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t sender;
    can_backend_t receiver;
    can_frame_t received = {0};
    uint32_t arrived = 0U;
    uint32_t expected_next = 0U;
    bool in_order = true;

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&sender, base, s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&receiver, base, s_error, sizeof(s_error)),
                             s_error);

    for (uint32_t n = 0U; n < (uint32_t)SENT; n++) {
        can_frame_t frame = make_frame(0x300U, 0U, 4U, 0U);

        frame.data[0] = (uint8_t)(n & 0xFFU);
        frame.data[1] = (uint8_t)((n >> 8U) & 0xFFU);
        frame.data[2] = (uint8_t)((n >> 16U) & 0xFFU);
        frame.data[3] = 0U;
        TEST_ASSERT_TRUE(sender.send(&sender, &frame));
    }

    /* The kernel knows at once how many it dropped, before anything is read. */
    can_backend_refresh_counters(&receiver);
    TEST_ASSERT_TRUE_MESSAGE(receiver.rx_overflow > 0U, "20,000 unread frames fitted the buffer");

    /* What did arrive is the beginning of the sequence, unbroken: the buffer filled up and
     * everything after that point was dropped, since nobody was reading. */
    while (receiver.recv(&receiver, &received) == 1) {
        const uint32_t n = (uint32_t)received.data[0] | ((uint32_t)received.data[1] << 8U) |
                           ((uint32_t)received.data[2] << 16U);

        in_order = in_order && (n == expected_next);
        expected_next = n + 1U;
        arrived++;
    }

    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_TRUE(arrived > 0U);
    receiver.close(&receiver);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)SENT, (uint64_t)arrived + receiver.rx_overflow);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)arrived, receiver.rx_frames);
    sender.close(&sender);
}

static void test_a_seventeenth_udp_participant_is_refused_and_a_freed_slot_is_reused(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t slots[CAN_UDP_SLOTS];
    can_backend_t extra;

    for (uint32_t i = 0U; i < CAN_UDP_SLOTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&slots[i], base, s_error, sizeof(s_error)),
                                 s_error);
    }

    TEST_ASSERT_FALSE(can_backend_udp_open(&extra, base, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "slots"));

    slots[5].close(&slots[5]);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&extra, base, s_error, sizeof(s_error)), s_error);
    extra.close(&extra);

    for (uint32_t i = 0U; i < CAN_UDP_SLOTS; i++) {
        slots[i].close(&slots[i]);
    }
}

static void test_udp_drops_datagrams_that_do_not_come_from_the_bus(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    const can_frame_t valid = make_frame(0x321U, 0U, 2U, 0x77U);
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    struct sockaddr_in to;
    can_backend_t node;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&node, base, s_error, sizeof(s_error)), s_error);
    can_udp_encode(&valid, datagram);

    /* A perfectly formed datagram, but sent from a port that is not a slot of this bus. */
    const int outsider = socket(AF_INET, SOCK_DGRAM, 0);

    TEST_ASSERT_TRUE(outsider >= 0);
    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(base); /* the node took the first slot */
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)sizeof(datagram),
                          (int)sendto(outsider, datagram, sizeof(datagram), 0,
                                      (const struct sockaddr *)&to, sizeof(to)));
    (void)close(outsider);

    TEST_ASSERT_EQUAL_INT(0, receive_within(&node, &received, 100));
    TEST_ASSERT_EQUAL_UINT64(1U, node.rx_malformed);
    TEST_ASSERT_EQUAL_UINT64(0U, node.rx_frames);
    node.close(&node);
}

static void test_backend_specification_strings(void)
{
    can_backend_t backend;

    TEST_ASSERT_FALSE(can_backend_open(&backend, "carrier-pigeon", s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "unknown CAN backend"));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:0", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:70000", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:12x", s_error, sizeof(s_error)));
    /* A bus is 16 consecutive ports: 65520 is the last base port that has them. */
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:65521", s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "out of range"));
    TEST_ASSERT_TRUE_MESSAGE(can_backend_open(&backend, "udp:65520", s_error, sizeof(s_error)),
                             s_error);
    backend.close(&backend);
    TEST_ASSERT_FALSE(can_backend_open(&backend, "socketcan:", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(
        can_backend_open(&backend, "socketcan:no-such-interface0", s_error, sizeof(s_error)));

    char spec[32];

    (void)snprintf(spec, sizeof(spec), "udp:%u", (unsigned int)test_bus_fresh_base_port());
    TEST_ASSERT_TRUE_MESSAGE(can_backend_open(&backend, spec, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_EQUAL_STRING("udp", backend.kind);
    backend.close(&backend);
}

/* ---------------------------------------------------------------------------- SocketCAN */

static void test_socketcan_participants_exchange_frames_on_vcan0(void)
{
    can_backend_t a;
    can_backend_t b;
    can_frame_t received = {0};

    if (!can_backend_socketcan_open(&a, "vcan0", s_error, sizeof(s_error))) {
        /* CI sets FCN_REQUIRE_SOCKETCAN: there the interface exists and must be tested. */
        if (getenv("FCN_REQUIRE_SOCKETCAN") != NULL) {
            TEST_FAIL_MESSAGE(s_error);
        }

        TEST_IGNORE_MESSAGE("vcan0 is not available on this machine (see docs/wsl-vcan.md)");
    }

    TEST_ASSERT_TRUE_MESSAGE(can_backend_socketcan_open(&b, "vcan0", s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_EQUAL_STRING("socketcan", a.kind);

    /* Other programs may be using vcan0; pick identifiers of our own and skip the rest. */
    const can_frame_t frames[] = {
        make_frame(0x5A1U, 0U, 8U, 0x01U),
        make_frame(0x5A2U, 0U, 0U, 0x00U),
        make_frame(0x15A5A5A5U, CAN_FLAG_EXT, 5U, 0x30U),
        make_frame(0x5A3U, CAN_FLAG_RTR, 4U, 0x00U),
    };

    for (size_t i = 0U; i < (sizeof(frames) / sizeof(frames[0])); i++) {
        bool found = false;

        TEST_ASSERT_TRUE(a.send(&a, &frames[i]));

        for (int attempt = 0; (attempt < 50) && !found; attempt++) {
            TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
            found = (received.id == frames[i].id) && (received.flags == frames[i].flags);
        }

        TEST_ASSERT_TRUE_MESSAGE(found, "the frame did not arrive on the second socket");
        assert_frames_equal(&frames[i], &received);
    }

    /* The sender does not receive its own frames. */
    TEST_ASSERT_EQUAL_INT(0, receive_within(&a, &received, 50));
    a.close(&a);
    b.close(&b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_udp_datagram_has_the_documented_layout);
    RUN_TEST(test_udp_decoding_restores_what_was_encoded);
    RUN_TEST(test_udp_decoding_rejects_everything_that_is_not_a_frame);
    RUN_TEST(test_three_udp_participants_hear_each_other_but_not_themselves);
    RUN_TEST(test_a_burst_arrives_complete_and_in_order);
    RUN_TEST(test_frames_that_do_not_fit_the_socket_buffer_are_dropped_and_counted);
    RUN_TEST(test_a_seventeenth_udp_participant_is_refused_and_a_freed_slot_is_reused);
    RUN_TEST(test_udp_drops_datagrams_that_do_not_come_from_the_bus);
    RUN_TEST(test_backend_specification_strings);
    RUN_TEST(test_socketcan_participants_exchange_frames_on_vcan0);
    return UNITY_END();
}
