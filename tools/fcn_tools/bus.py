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
    """All frames received during `seconds`, optionally only those `keep` accepts."""
    deadline = time.monotonic() + seconds
    return [m for m in receive_until(bus, deadline) if keep is None or keep(m)]


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


# Most consecutive status values a busy host can make the node replace before they go out:
# a host that does not run the node for 50 ms. The status message is cyclic, so a value that
# could not leave in time is superseded by the next one (lib/can/can_txq.h), which leaves a
# gap in the counters. A value that waited in a mailbox through a bus fault instead shows up as
# a jump the length of the whole fault, hundreds of milliseconds.
STATUS_MAX_STEP = 5


def status_gaps(frames: list[can.Message], max_step: int = STATUS_MAX_STEP) -> int:
    """Check a sequence of status frames as received and return how many values are missing.

    Asserted: the counters strictly increase, no step is larger than `max_step`, and each
    value's tick count is consistent with its counter (10 ticks per period, give or take the
    2 ticks a delayed task may read late). The missing values are the ones the node replaced
    before they got out; a test compares that number with the node's own statistics.
    """
    fields = [status_fields(frame) for frame in frames]
    missing = 0
    for (c0, t0), (c1, t1) in zip(fields, fields[1:]):
        step = c1 - c0
        assert 1 <= step <= max_step, f"status counter {c0} followed by {c1}"
        assert abs((t1 - t0) - 10 * step) <= 2, f"counters {c0}->{c1} but ticks {t0}->{t1}"
        missing += step - 1
    return missing


def wait_for_id(bus: can.BusABC, arbitration_id: int, timeout: float) -> can.Message | None:
    """The first data frame with this base-format identifier."""
    return wait_for(
        bus,
        lambda m: m.arbitration_id == arbitration_id and not m.is_extended_id
        and not m.is_remote_frame,
        timeout,
    )
