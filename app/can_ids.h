/* SPDX-License-Identifier: MIT */
/**
 * @file
 * CAN identifiers the application uses before the DBC of phase 2 exists. From phase 2 on the
 * DBC is the reference and this file is generated from it or removed.
 *
 * A lower identifier wins arbitration, so identifiers are handed out by urgency, not by
 * topic: what must get through on a busy bus has the smallest number.
 */
#ifndef FCN_CAN_IDS_H
#define FCN_CAN_IDS_H

#define CAN_ID_STATUS 0x030U /**< node -> bus, cyclic; phase 1: a counter and the tick count */
#define CAN_ID_PING   0x6F0U /**< tester -> node: echo request, any payload */
#define CAN_ID_PONG   0x6F1U /**< node -> tester: the payload of the ping */

/** Cyclic slots of the transmit queue, one per periodic message. */
#define CAN_SLOT_STATUS 0U

#endif /* FCN_CAN_IDS_H */
