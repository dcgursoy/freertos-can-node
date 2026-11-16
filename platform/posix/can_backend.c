/* SPDX-License-Identifier: MIT */
/* Chooses the transport from a "kind:argument" specification. */
#include "can_backend.h"

#include <linux/sock_diag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void can_backend_refresh_counters(can_backend_t *backend)
{
    /* SO_MEMINFO returns the socket's memory counters; one of them is the number of packets
     * the kernel had to drop because the receive buffer was full. It is exact at the moment
     * of the call. (The other way to learn it, SO_RXQ_OVFL, reports it with the next packet
     * that is received, so the drops at the end of a burst stay unseen until more arrives.) */
    uint32_t counters[SK_MEMINFO_VARS];
    socklen_t len = (socklen_t)sizeof(counters);

    if ((backend->fd >= 0) &&
        (getsockopt(backend->fd, SOL_SOCKET, SO_MEMINFO, counters, &len) == 0) &&
        (len >= (socklen_t)((SK_MEMINFO_DROPS + 1U) * sizeof(counters[0])))) {
        backend->rx_overflow = counters[SK_MEMINFO_DROPS];
    }
}

bool can_backend_open(can_backend_t *backend, const char *spec, char *error, size_t error_len)
{
    static const char udp_prefix[] = "udp";
    static const char socketcan_prefix[] = "socketcan:";

    if (strncmp(spec, socketcan_prefix, sizeof(socketcan_prefix) - 1U) == 0) {
        return can_backend_socketcan_open(backend, &spec[sizeof(socketcan_prefix) - 1U], error,
                                          error_len);
    }

    if (strcmp(spec, udp_prefix) == 0) {
        return can_backend_udp_open(backend, (uint16_t)CAN_UDP_DEFAULT_PORT, error, error_len);
    }

    if ((strncmp(spec, udp_prefix, sizeof(udp_prefix) - 1U) == 0) &&
        (spec[sizeof(udp_prefix) - 1U] == ':')) {
        const char *digits = &spec[sizeof(udp_prefix)];
        char *end = NULL;
        const unsigned long port = strtoul(digits, &end, 10);

        if ((end != digits) && (*end == '\0') && (port > 0UL) && (port <= 65535UL)) {
            return can_backend_udp_open(backend, (uint16_t)port, error, error_len);
        }
    }

    (void)snprintf(error, error_len,
                   "unknown CAN backend '%s' (use udp, udp:<base port> or socketcan:<interface>)",
                   spec);
    return false;
}
