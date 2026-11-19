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
participant at once. Both are selected with one string, by the node (`--can`) and by every
tool (`fcn_tools.bus.open_bus`). The base port of `udp:<base port>` is written in decimal
digits, 1 to 65520. The node refuses anything else at start, and also a `--sim-port` that is
one of the bus's 16 ports. What happens when the transport itself fails is described in
[When the transport fails](#when-the-transport-fails).

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
another port of the same bus on 127.0.0.1. Every frame therefore has one encoding, and a stray datagram
cannot become a frame. The C implementation ([can_backend_udp.c](../platform/posix/can_backend_udp.c))
and the Python one ([udpbus.py](../tools/fcn_tools/udpbus.py)) apply the same checks.
The SocketCAN transport drops and counts the same frames when the kernel hands them over: an
error frame, a length above 8, or a base-format identifier above 0x7FF. The kernel passes
such an identifier on unchanged (`cansend vcan0 EF0#55` sends one), and keeping only its 11
bits would turn it into another, valid identifier: 0xEF0 would become the ping 0x6F0.

```bash
python -m can.logger -i fcn_udp -c 29500
```

```bash
candump vcan0
```

## When the transport fails

The model decides what happens on the modelled bus; the transport only carries the result to
the other processes. A frame the transport refuses has still been transmitted as far as the
model and the driver are concerned: the driver gets its transmit-done event, and no bus fault
is derived from the failure (bus faults are applied through simulation control). The failure
is counted with its errno (`transport_counters.tx_failed` and `rx_failed` in the statistics,
`transport_tx_failed` and `transport_rx_failed` in `can.status`) and logged: the first one
at once, then at most one line a second with the number of failures since the last line.

- An interface that is **down** (ENETDOWN) is a failure like any other. A SocketCAN socket
  stays bound to it, and sending and receiving work again once it is up; so does the node.
- An interface that is **gone** (ENODEV, or ENXIO on a send: a deleted SocketCAN interface,
  from which the kernel has unbound the socket) also stops the node in an orderly way, since
  nothing will be sent or received again. The node writes its statistics with the reason
  (`transport_error`), prints `fcn_node: the CAN transport <spec> went away: <reason>`, and
  exits with status 74.

## The controller model

A socket delivers a frame instantly and intact. A 500 kbit/s bus carries one frame at a
time, a frame takes about a quarter of a millisecond, a disturbed bus destroys frames, and a
controller that keeps failing takes itself off the bus. The model
([can_ctrl.h](../lib/can/can_ctrl.h)) sits between the driver and the transport and adds
those properties. It is pure C with time passed in, so the unit tests run it in model time;
on the host a thread outside the scheduler runs it against `CLOCK_MONOTONIC`.

**One timeline for the bus.** Every frame, sent or received, occupies the bus for its exact
length in bit times, computed from its content with bit stuffing and the real CRC-15
([can_bits.h](../lib/can/can_bits.h)), and is followed by the 3-bit intermission. The bus
load can therefore never exceed 100 %.

- *Transmit.* The driver loads frames into mailboxes. Whenever the bus becomes free, the
  frames waiting for it compete: the node's best mailbox and the oldest frame another node is
  waiting to send. The one with the highest priority (the lowest arbitration field) starts, as
  in arbitration on a real bus; with equal fields the other node's frame goes first. The
  node's frame is written to the transport at its modelled end of frame, which is when a real
  receiver would have it.
- *Receive.* The transport delivers a frame once another node has sent it, so the model
  learns of it after the fact. If the bus has been free since before then and no other
  received frame waits, the frame passed while the bus was free: it ends at its arrival, or
  one frame length after the bus became free. Otherwise its sender is still waiting for the
  bus: the frame waits in a queue of 64 frames and starts when it wins arbitration. A tool
  that sends faster than the bit rate allows cannot push more frames into the node than the
  bus could carry. Its frames are delivered at bus speed, beyond the queue they are dropped
  and counted, and they never keep a frame of the node with a higher priority off the bus
  for more than the one frame on the bus at the time. The exception is an error-passive
  node that sent the last frame: while it waits out its suspend (8 bit times), a waiting
  frame of another node starts without competing.
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
sampling, acknowledgement by this node, or the detection of errors: it is told that an error
happened. Arbitration is decided between whole frames, not bit by bit. A frame from the
transport carries no sender, so frames from other participants keep the order the transport
delivers them in, and only the oldest of them competes with the node: a high-priority frame
of another node that waits behind a low-priority one waits for it. CAN FD is not supported.

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

The token is a word of 1 to 31 characters chosen by the client, and it comes back unchanged.
A request whose token is longer, or that has no command, cannot be matched to a reply and is
answered with `? err malformed request`.

| Command | Effect |
|---|---|
| `ping` | replies with the node's process id |
| `can.status` | state, error counters, fault, the counters of the model and of the transport (`transport_tx_failed` and `transport_rx_failed` among them), and how late the host carried out the model's times (`pacing_lag_*`, `wakeup_lag_*`, `rx_lag_*`; see [What the host adds to the timing](#what-the-host-adds-to-the-timing)) |
| `can.fault none\|short\|dominant\|noack` | applies or removes a bus fault |
| `can.inject tx\|rx <count>` | destroys the next `count` transmission attempts or received frames |
| `sensor.status` | the sensor model's seed, whether its front end is off line, its read counters and every channel's mode (see [sensor-model.md](sensor-model.md)) |
| `sensor.set <channel> <value>` | holds a channel of the sensor front end (`current`, `cell1`..`cell8`, `temp1`..`temp4`) at a value, which it reports as a valid reading |
| `sensor.open <channel>` | lets the front end declare the channel invalid, as with an open sense wire |
| `sensor.offline on\|off` | takes the whole front end off line (it answers no read) or back |
| `sensor.clear [<channel>]` | ends the fault of one channel, or with no channel every sensor fault, off line included |

`fcn_tools.simctl.SimControl` is the client the tests use. The node carries out every request
datagram it receives and does not remember tokens, so a request repeated after a timeout can
be carried out twice. The client therefore repeats a request only when that cannot change the
outcome. After a timeout it resends `ping`, `can.status`, `can.fault` and every `sensor.*`
command (each with a new token): they read a state or set one to a value, so a second delivery
changes nothing. It resends any command when the kernel reports that nothing was listening
yet, which happens while the node is still starting. `can.inject` adds to the errors still to
come, so it is sent once and waited for.

## The driver's side

[plat_can.h](../platform/include/plat_can.h) is the hardware interface the driver is written
against: transmit mailboxes, a receive FIFO, an event FIFO (transmit done, transmit aborted,
state change) and one interrupt with cause flags. On the host the interrupt is a simulated
one ([posix_irq.c](../platform/posix/posix_irq.c)).

- The **receive task** has the highest priority, is woken by the interrupt through a task
  notification and handles at most 32 frames per activation (`CAN_IO_RX_BATCH`), so its
  execution time per activation has a bound. When a batch is full, it lets the other tasks
  run for one tick. One batch per tick keeps up with a full bus at the highest bit rate: at
  1 Mbit/s the shortest frames, 47 bits with the intermission, come at up to 21.3 per
  millisecond. A compile-time check in [can_io.c](../app/can_io.c) ties the batch to
  `PLAT_CAN_MAX_BITRATE` and the tick rate.
- The **transmit task** is woken by producers and by the controller's events. What waits for
  a mailbox waits in the transmit queue ([can_txq.h](../lib/can/can_txq.h)): one slot per
  cyclic message, where a newer value replaces one that has not gone out, and a bounded FIFO
  with a timeout for frames that must not be replaced or reordered. The queue's accounting
  is exact at every moment: produced = sent + superseded + dropped + expired + pending. It
  also keeps the counters of every cyclic slot (`can_txq_slot_stats()`: produced = sent +
  superseded + pending per slot), which the node's statistics report per message under
  `telemetry.messages`; the bus tests account for the status frame's values with those of its
  slot (`fcn_tools.bus.status_queue_counts`).
- **Abort.** `plat_can_abort_tx()` takes back the frames that were loaded before the call, as
  a mailbox abort request does on a real controller, and none that are loaded after it,
  however soon. On the host, a request travels to its mailbox through a ring. Each request
  carries its submission order, and the hardware thread aborts only those made before the
  abort ([posix_can.c](../platform/posix/posix_can.c)).
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

The model's times are exact; when the host carries them out is not. The hardware thread
measures how late it is in three places and reports the figures in the node's statistics
(`can.pacing`) and in `can.status`. Measuring does not change the model's times.

- *Pacing*: a transmitted frame is written to the transport when the thread wakes up at the
  frame's modelled end of frame. The lag is the time from that end of frame to the write.
- *Wake-up*: a pass of the thread that serves a deadline it slept until is late by the time
  from the deadline to the start of the pass. Every host stall while the model had something
  due shows here, whatever is on the bus.
- *Receive*: a received frame reaches the receive FIFO late by the time from its modelled end
  of frame to the delivery.

The pacing lag alone does not show a stall during which a frame of the node waited for the
bus, for example behind received frames. When the thread wakes up late, the model moves the
start of a frame that waits for the bus to the time of the wake-up (`late_resyncs` counts
these moves). That frame's end of frame is then on time again, and the stall becomes part of
the time the frame waited for the bus. The wake-up lag shows such a stall, and so does the
receive lag of a received frame that was on the bus during it
(`test_a_host_stall_shows_in_the_wake_up_and_receive_lag` in
[test_node_errors.py](../tests/system/test_node_errors.py)).
