# How the CAN bus is simulated

The node has no CAN hardware. This page says what stands in for it, what that stand-in models
and what it leaves out, so that a result about "the bus" can be read for what it is.

```
   application tasks                                   test / measurement tools
        |  can_io_publish(), can_io_send_event()            (python-can, can-utils)
        v                                                           |
   transmit queue (lib/can/can_txq)                                 |
        |                                                           |
   CAN tasks + ISR (app/can_io.c)          the driver               |
        |  plat_can.h: mailboxes, receive FIFO, events, one interrupt
        v                                                           |
   controller model (lib/can/can_ctrl, can_fc, can_bits)            |
        |  run in real time by the "hardware" thread (platform/posix/posix_can.c)
        v                                                           v
   transport (platform/posix/can_backend*.c):  SocketCAN (vcan0)  or  UDP bus on 127.0.0.1
```

Everything above the line `plat_can.h` is firmware and would run unchanged on a
microcontroller. Everything below it is the simulated hardware of the host build.

## The two transports

A transport only moves frames between processes: every frame reaches every other
participant at once and without error. Both are selected with one string, by the node
(`--can`) and by every tool (`fcn_tools.bus.open_bus`).

| | `socketcan:<interface>` | `udp[:<base port>]` |
|---|---|---|
| What it is | a raw CAN socket on a Linux SocketCAN interface, `vcan0` in tests and CI | UDP datagrams on the loopback interface |
| Needs | the `vcan` kernel module (not in the stock WSL2 kernel, see [wsl-vcan.md](wsl-vcan.md)) | nothing |
| Other tools | `candump`, `cansend`, python-can `socketcan` | python-can interface `fcn_udp` (installed with this project's Python package) |
| Default | | yes, base port 29500 |

**The UDP bus.** A bus is 16 UDP ports on 127.0.0.1, `B` to `B+15`; the base port names the
bus. A participant binds the first of them that is free (a 17th participant is refused) and
sends each frame as one datagram to each of the other 15 ports. There is no broker, and
nothing leaves the machine. A datagram is 16 bytes laid out like Linux's `struct can_frame`,
little-endian:

| Bytes | Field | Content |
|---|---|---|
| 0..3 | `can_id` | bits 0..28 identifier, bit 29 error frame (never sent), bit 30 remote request, bit 31 extended identifier |
| 4 | length | 0..8 |
| 5..7 | | zero |
| 8..15 | data | zero beyond the length; all zero in a remote request |

A receiver drops, and counts, every datagram that is not exactly that, or whose source is not
another port of the same bus. Every frame therefore has one encoding, and a stray datagram
cannot become a frame. The C implementation ([can_backend_udp.c](../platform/posix/can_backend_udp.c))
and the Python one ([udpbus.py](../tools/fcn_tools/udpbus.py)) apply the same checks.

```bash
python -m can.logger -i fcn_udp -c 29500
```

```bash
candump vcan0
```

## The controller model

A socket delivers a frame instantly and never fails. A 500 kbit/s bus carries one frame at a
time, a frame takes about a quarter of a millisecond, a disturbed bus destroys frames, and a
controller that keeps failing takes itself off the bus. The model
([can_ctrl.h](../lib/can/can_ctrl.h)) sits between the driver and the transport and adds
those properties. It is pure C with time passed in, so the unit tests run it in model time;
on the host a thread outside the scheduler runs it against `CLOCK_MONOTONIC`.

**One timeline for the bus.** Every frame, sent or received, occupies the bus for its exact
length in bit times, computed from its content with bit stuffing and the real CRC-15
([can_bits.h](../lib/can/can_bits.h)), and is followed by the 3-bit intermission. The bus
load can therefore never exceed 100 %.

- *Transmit.* The driver loads frames into mailboxes. When the bus is free, the pending frame
  with the highest priority (the lowest arbitration field) starts. It is written to the
  transport at its modelled end of frame, which is when a real receiver would have it.
- *Receive.* A frame read from the transport is taken to end on the bus at its arrival time
  or, if the bus was not free long enough before that, one frame length after the bus became
  free. A tool that sends faster than the bit rate allows cannot push more frames into the
  node than the bus could carry: they are delivered at bus speed, and beyond a queue of 64
  frames they are dropped and counted.
- `--bitrate 0` switches the pacing off, to measure the transport itself.

**Fault confinement.** The transmit and receive error counters and the error-active,
error-passive and bus-off states follow the twelve rules of section 8 of the Bosch CAN
Specification 2.0 Part B, which ISO 11898-1 adopted, including both exceptions to rule 3 and
the 128 occurrences of 11 recessive bits before a bus-off node may return
([can_fc.h](../lib/can/can_fc.h) lists each rule next to the event that applies it).
Recovery starts when the driver asks for it, as on controllers that do not recover by
themselves.

**Faults are physical, and injected from outside.** The test bench tells the model what is
wrong with the bus and the model derives what a controller would observe:

| Fault | What the node sees | Where it ends |
|---|---|---|
| `short` (CAN_H to CAN_L) | every transmission attempt fails at its first bit, and so does the node's own active error flag | +16 per attempt while error active, +8 once passive: error passive after 8 attempts, bus off after 24; recovery completes while the fault is still there, and the next attempts lead to bus off again |
| `dominant` (bus held dominant) | the bus is never idle: nothing can be sent; a stuff error after 6 bits (+1), a dominant bit after the error flag (+8), then +8 per 8 dominant bits from the 14th after an active flag (the 8th after a passive one) | error passive; never bus off; no recovery while the fault lasts |
| `noack` (no other node acknowledges) | every transmission ends in an acknowledgment error | the transmit counter stops at 128: error passive, never bus off |
| a number of destroyed transmissions or receptions | a bit error per frame | counters go up by 8 or 1 and come back down with good frames |

Each of these sequences is asserted step by step in the unit tests of the model.

**What is not modelled.** The model works on whole frames. It does not simulate bit timing or
sampling, bit-wise arbitration between nodes, acknowledgement by this node, or the detection
of errors: it is told that an error happened. Frames from other participants arrive in the
order the transport delivers them, not in priority order. CAN FD is not supported.

## Simulation control

On a real bench, faults are applied to the hardware around the device under test, not through
its firmware. The same holds here: the simulated hardware has a control channel of its own, a
UDP port on the loopback interface (`--sim-port`, off by default), and the application has no
test hooks and never sees these commands.

One request per datagram, one reply to the sender, ASCII:

```
request:  <token> <command> [arguments]
reply:    <token> ok [key=value ...]      or      <token> err <message>
```

| Command | Effect |
|---|---|
| `ping` | replies with the node's process id |
| `can.status` | state, error counters, fault, and the counters of the model and the transport |
| `can.fault none\|short\|dominant\|noack` | applies or removes a bus fault |
| `can.inject tx\|rx <count>` | destroys the next `count` transmission attempts or received frames |

`fcn_tools.simctl.SimControl` is the client the tests use.

## The driver's side

[plat_can.h](../platform/include/plat_can.h) is the hardware interface the driver is written
against: transmit mailboxes, a receive FIFO, an event FIFO (transmit done, transmit aborted,
state change) and one interrupt with cause flags. On the host the interrupt is a simulated
one ([posix_irq.c](../platform/posix/posix_irq.c)).

- The **receive task** has the highest priority, is woken by the interrupt through a task
  notification and handles at most 16 frames per activation, so its execution time per
  activation has a bound.
- The **transmit task** is woken by producers and by the controller's events. What waits for
  a mailbox waits in the transmit queue ([can_txq.h](../lib/can/can_txq.h)): one slot per
  cyclic message, where a newer value replaces one that has not gone out, and a bounded FIFO
  with a timeout for frames that must not be replaced or reordered. The queue's accounting
  is exact at every moment: produced = sent + superseded + dropped + expired + pending.
- **Bus off.** The queue has the mailboxes emptied, keeps accepting frames under the rules
  above, and asks for recovery after a hold-off that doubles with every further bus-off up to
  a limit and resets once the node has stayed on the bus for a while.
- **Faults without bus off.** A node that nobody acknowledges, or that sits on a bus held
  dominant, never goes bus off; the controller just keeps a frame in its mailbox. The queue
  therefore also calls frames back from the mailboxes when they have gone stale there: when
  a cyclic message publishes a new value while its previous value is still in a mailbox, and
  when an event frame has been in a mailbox for its timeout. The old value is then counted as
  superseded and the event frame as expired, and the first frame on the bus after the fault
  is the current value, not one from when the fault began (`recall_requests` in the node's
  statistics counts these call-backs).

## What the host adds to the timing

The model's times are exact; when the host carries them out is not. A frame is written to
the transport when the hardware thread wakes up at the frame's end of frame, and that
wake-up is late by the host's scheduling latency. The lateness is measured for every frame
and reported in the node's statistics (`can.pacing`), never folded into the model's times.
