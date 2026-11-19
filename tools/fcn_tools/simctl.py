"""Client of the node's simulation control channel.

The simulated hardware around the node (the CAN bus, the sensor front end, later the flash and
the power supply) is controlled through a UDP socket on the loopback interface, the way a test
bench controls relays and signal generators around a real device. The firmware itself never
sees these commands. The protocol is documented in platform/posix/posix_sim.h:

    request:  <token> <command> [arguments...]
    reply:    <token> ok [key=value ...]     or     <token> err <message>

The node carries out every request it receives, once per datagram, and does not remember
tokens. A request that got no answer in time may still be waiting in the node's socket (a
host that did not run the node), so sending it again can make the node carry it out twice.
The client therefore repeats only a command for which that does no harm; see REPEATABLE.
"""
from __future__ import annotations

import itertools
import re
import socket
import time

# Commands that may reach the node more than once without changing the outcome: they only
# read (ping, can.status, sensor.status) or set a state to a value (can.fault and the other
# sensor commands: a channel held, opened or cleared twice, or the front end taken off line
# twice, ends as it would after once). Any other command, can.inject above all (it adds to the
# errors still to come), is sent once and waited for.
REPEATABLE = frozenset({"ping", "can.status", "can.fault",
                        "sensor.status", "sensor.set", "sensor.open", "sensor.offline",
                        "sensor.clear"})

# The channels of the sensor front-end model (platform/posix/posix_sensor.c).
SENSOR_CHANNELS = ("current",
                   *(f"cell{n}" for n in range(1, 9)),
                   *(f"temp{n}" for n in range(1, 5)))

_INTEGER = re.compile(r"-?[0-9]+")

# Pause before sending again to a port where nothing was listening yet (a node that is still
# starting): the request was not delivered, so it can be sent again whatever it is.
REFUSED_PAUSE_S = 0.01


class SimError(RuntimeError):
    """The node answered a command with an error, or did not answer."""


class SimControl:
    """One client socket. Use as a context manager, or call close()."""

    def __init__(self, port: int, timeout: float = 1.0) -> None:
        self._address = ("127.0.0.1", port)
        self._timeout = timeout
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Connected, so that the kernel reports a datagram nobody received (nothing bound to
        # the port) as ConnectionRefusedError instead of letting it vanish; and so that only
        # the node's port can answer.
        self._socket.connect(self._address)
        self._tokens = (f"t{n}" for n in itertools.count(1))

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "SimControl":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def request(self, command: str, retries: int = 2) -> dict[str, str]:
        """Send one command and return the reply's key=value pairs.

        The client waits up to (retries + 1) x timeout in all. A command in REPEATABLE is sent
        again, with a new token, each time `timeout` passes without an answer, up to `retries`
        times; any other command is sent once and waited for the whole time. A request that
        the kernel reports as not received (the node has not opened its socket yet, right
        after it started) is sent again in any case.
        """
        repeatable = command.split(" ", 1)[0] in REPEATABLE
        deadline = time.monotonic() + self._timeout * (retries + 1)
        unanswered = 0           # requests that may have reached the node
        last_error: Exception | None = None
        while unanswered < (retries + 1 if repeatable else 1):
            now = time.monotonic()
            if now >= deadline:
                break
            token = next(self._tokens)
            until = min(deadline, now + self._timeout) if repeatable else deadline
            try:
                self._socket.send(f"{token} {command}".encode())
                words = self._reply(token, until)
            except ConnectionRefusedError as error:
                last_error = error
                time.sleep(REFUSED_PAUSE_S)
                continue
            except TimeoutError as error:
                last_error = error
                unanswered += 1
                continue
            if len(words) >= 2 and words[1] == "ok":
                return dict(word.split("=", 1) for word in words[2:] if "=" in word)
            raise SimError(f"'{command}' failed: {' '.join(words[2:])}")
        raise SimError(f"no reply to '{command}' from {self._address}: {last_error}")

    def _reply(self, token: str, until: float) -> list[str]:
        """The words of the reply to `token`, read until the monotonic time `until`. Replies
        with another token answer an earlier attempt and are skipped."""
        while True:
            remaining = until - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out")
            self._socket.settimeout(remaining)
            data = self._socket.recv(2048)
            words = data.decode(errors="replace").split()
            if words and words[0] == token:
                return words

    def ping(self) -> int:
        """The node's process id; proves the node is up and serving commands."""
        return int(self.request("ping")["pid"])

    # ------------------------------------------------------------------ CAN controller model

    def can_status(self) -> dict:
        """State, error counters and statistics of the CAN controller model.

        Numeric values are converted to int; `state` (active, passive, busoff) and `fault`
        stay strings.
        """
        reply = self.request("can.status")
        return {key: (int(value) if value.isdigit() else value) for key, value in reply.items()}

    def can_fault(self, fault: str) -> dict[str, str]:
        """Apply a bus fault: none, short, dominant or noack."""
        return self.request(f"can.fault {fault}")

    def can_inject(self, kind: str, count: int) -> dict[str, str]:
        """Destroy the next `count` transmission attempts ("tx") or received frames ("rx").
        Not repeated on a timeout: the count adds to what is left (see REPEATABLE)."""
        return self.request(f"can.inject {kind} {count}")

    # --------------------------------------------------------------- sensor front-end model
    #
    # Channels are named as in SENSOR_CHANNELS; values are in the units of plat_sensor.h: mA
    # (positive when discharging), mV, and tenths of a degree Celsius. docs/sensor-model.md
    # describes the model and what each fault does.

    def sensor_status(self) -> dict:
        """Seed, off-line state, read counters and every channel's mode.

        Whole numbers are converted to int, negative ones included (a held value); `offline`
        (on, off) and a channel's `model` or `open` stay strings."""
        reply = self.request("sensor.status")
        return {key: (int(value) if _INTEGER.fullmatch(value) else value)
                for key, value in reply.items()}

    def sensor_set(self, channel: str, value: int) -> dict[str, str]:
        """Hold `channel` at `value`: the front end reports exactly that, as a valid reading,
        until the channel is cleared. Any value of the channel's type is accepted, also one
        the real sensor could not deliver."""
        if isinstance(value, bool) or not isinstance(value, int):
            raise TypeError(f"a sensor value is a whole number, not {value!r}")
        return self.request(f"sensor.set {channel} {value}")

    def sensor_open(self, channel: str) -> dict[str, str]:
        """Let the front end declare `channel` invalid (an open sense wire)."""
        return self.request(f"sensor.open {channel}")

    def sensor_offline(self, offline: bool = True) -> dict[str, str]:
        """Take the whole front end off line (it answers no read), or bring it back."""
        return self.request(f"sensor.offline {'on' if offline else 'off'}")

    def sensor_clear(self, channel: str | None = None) -> dict[str, str]:
        """End the fault of `channel`, or with no channel every fault, off line included."""
        return self.request("sensor.clear" if channel is None else f"sensor.clear {channel}")
