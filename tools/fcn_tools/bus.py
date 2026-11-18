"""Open the project's CAN bus from one specification string, on either transport.

The node and every tool name a bus the same way:

    udp:29500           the virtual bus over UDP on the loopback interface, base port 29500
    udp                 the same on the default port
    socketcan:vcan0     a Linux SocketCAN interface

so a test or a script written against this module runs unchanged on both.
"""
from __future__ import annotations

import struct
import time
from pathlib import Path
from typing import Callable, Iterator

import can

DEFAULT_UDP_PORT = 29500

# Identifiers that exist before the DBC does; the DBC of phase 2 is the reference after that.
ID_STATUS = 0x030   # node -> bus, every 10 ms in phase 1
ID_PING = 0x6F0     # tester -> node
ID_PONG = 0x6F1     # node -> tester, same payload as the ping


def open_bus(spec: str, **kwargs) -> can.BusABC:
    """Open the bus named by `spec`. Extra keyword arguments go to python-can."""
    kind, _, argument = spec.partition(":")
    if kind == "udp":
        return can.Bus(interface="fcn_udp", channel=int(argument or DEFAULT_UDP_PORT), **kwargs)
    if kind == "socketcan":
        if not argument:
            raise ValueError("socketcan needs an interface, for example socketcan:vcan0")
        return can.Bus(interface="socketcan", channel=argument, **kwargs)
    raise ValueError(f"unknown bus '{spec}': use udp[:<base port>] or socketcan:<interface>")


def socketcan_available(interface: str = "vcan0") -> bool:
    """True if the SocketCAN interface exists and is up."""
    state = Path("/sys/class/net") / interface / "operstate"
    try:
        return state.read_text().strip() in ("up", "unknown")
    except OSError:
        return False


def receive_until(bus: can.BusABC, deadline: float) -> Iterator[can.Message]:
    """Yield every frame received until the monotonic time `deadline`."""
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        message = bus.recv(timeout=remaining)
        if message is not None:
            yield message


def collect(bus: can.BusABC, seconds: float,
            keep: Callable[[can.Message], bool] | None = None) -> list[can.Message]:
    """All frames received during `seconds`, optionally only those `keep` accepts.

    The result depends on how fast the host is: use it to let frames pass, or to show that
    nothing arrives. To get a number of frames, use collect_count()."""
    deadline = time.monotonic() + seconds
    return [m for m in receive_until(bus, deadline) if keep is None or keep(m)]


def collect_count(bus: can.BusABC, count: int, keep: Callable[[can.Message], bool] | None = None,
                  timeout: float = 5.0) -> list[can.Message]:
    """The next `count` frames that `keep` accepts. Fewer only if `timeout` runs out first, a
    bound for a node that has stopped sending, not a measurement: a test asserts the length."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    for message in receive_until(bus, deadline):
        if keep is None or keep(message):
            frames.append(message)
            if len(frames) == count:
                break
    return frames


def collect_while(bus: can.BusABC, running: Callable[[], bool],
                  keep: Callable[[can.Message], bool] | None = None,
                  timeout: float = 60.0) -> list[can.Message]:
    """Every frame received while `running()` holds (the node runs), then drain(): the frames
    that were still on their way when it stopped. However slowly the host ran the node, this
    listens for its whole run; `timeout` only bounds a node that never ends."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    while running() and time.monotonic() < deadline:
        message = bus.recv(timeout=0.05)
        if message is not None and (keep is None or keep(message)):
            frames.append(message)
    return frames + drain(bus, keep=keep)


def drain(bus: can.BusABC, keep: Callable[[can.Message], bool] | None = None,
          quiet: float = 0.2, timeout: float = 10.0) -> list[can.Message]:
    """Every frame the bus still holds, read until none has arrived for `quiet` seconds. Meant
    for after the node has ended: what it sent is waiting in the socket by then, and a reader
    that the host delays still gets all of it. `timeout` only bounds a sender that never
    stops."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    while time.monotonic() < deadline:
        message = bus.recv(timeout=quiet)
        if message is None:
            break
        if keep is None or keep(message):
            frames.append(message)
    return frames


def wait_for(bus: can.BusABC, wanted: Callable[[can.Message], bool],
             timeout: float) -> can.Message | None:
    """The first frame that `wanted` accepts, or None if none arrives within `timeout`."""
    deadline = time.monotonic() + timeout
    for message in receive_until(bus, deadline):
        if wanted(message):
            return message
    return None


def is_status(message: can.Message) -> bool:
    return (message.arbitration_id == ID_STATUS and not message.is_extended_id
            and not message.is_remote_frame)


def is_pong(message: can.Message) -> bool:
    return (message.arbitration_id == ID_PONG and not message.is_extended_id
            and not message.is_remote_frame)


def status_fields(message: can.Message) -> tuple[int, int]:
    """(period counter, tick count) of a status frame: the number of the task's period, from
    1, and the node's tick count when the value was published."""
    if message.dlc != 8:
        raise ValueError(f"a status frame has 8 data bytes, this one {message.dlc}")
    return struct.unpack("<II", bytes(message.data))


# Ticks per period of the status frame (TICKER_PERIOD_MS in app/app.c at 1 kHz).
STATUS_PERIOD_TICKS = 10

# Most consecutive status values a busy host can make the node replace before they go out:
# a host that does not run the node for 50 ms. The status message is cyclic, so a value that
# could not leave in time is superseded by the next one (lib/can/can_txq.h), which leaves a
# gap in the counters. A value that waited in a mailbox through a bus fault instead shows up as
# a jump the length of the whole fault, hundreds of milliseconds.
STATUS_MAX_STEP = 5


def status_gaps(frames: list[can.Message], max_step: int | None = STATUS_MAX_STEP) -> int:
    """Check a sequence of status frames as received and return how many values are missing.

    Asserted:
      - the counters strictly increase and no step is larger than `max_step`. The limit
        depends on the host (a stall longer than STATUS_MAX_STEP periods exceeds it), so a
        test that has no bus fault to tell apart from a stall passes None: no limit;
      - the tick counts strictly increase, and no value carries a tick count below
        STATUS_PERIOD_TICKS times its counter: period n of the ticker ends at tick 10 n
        after its start (at tick 0 or later), and the tick is read after that.
    How late after the end of its period a value was published depends on the host; that is
    checked on the wall clock, in a test marked `timing`. The missing values are the ones the
    node replaced before they got out; a test compares that number with the node's own
    statistics.
    """
    fields = [status_fields(frame) for frame in frames]
    for counter, tick in fields:
        assert tick >= STATUS_PERIOD_TICKS * counter, f"status counter {counter} at tick {tick}"
    missing = 0
    for (c0, t0), (c1, t1) in zip(fields, fields[1:]):
        step = c1 - c0
        assert step >= 1 and (max_step is None or step <= max_step), (
            f"status counter {c0} followed by {c1}")
        assert t1 > t0, f"counters {c0}->{c1} but ticks {t0}->{t1}"
        missing += step - 1
    return missing


def status_missing_since_start(frames: list[can.Message],
                               max_step: int | None = STATUS_MAX_STEP) -> int:
    """status_gaps() of everything a tester heard from the node's start on, including the
    values before the first one heard (the counter starts at 1): the number to compare with
    the queue's count of superseded values."""
    if not frames:
        return 0
    return status_fields(frames[0])[0] - 1 + status_gaps(frames, max_step)


def status_publish_lateness(frames: list[can.Message]) -> list[int]:
    """How many ticks after the end of its period each value was published, relative to the
    earliest one in `frames` (the ticker's start tick is not on the bus). 0 for every value on
    a host that runs the node in time."""
    offsets = [tick - STATUS_PERIOD_TICKS * counter
               for counter, tick in (status_fields(frame) for frame in frames)]
    earliest = min(offsets)
    return [offset - earliest for offset in offsets]


def wait_for_id(bus: can.BusABC, arbitration_id: int, timeout: float) -> can.Message | None:
    """The first data frame with this base-format identifier."""
    return wait_for(
        bus,
        lambda m: m.arbitration_id == arbitration_id and not m.is_extended_id
        and not m.is_remote_frame,
        timeout,
    )
