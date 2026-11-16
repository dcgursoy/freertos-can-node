"""Client of the node's simulation control channel.

The simulated hardware around the node (the CAN bus, later the sensors, the flash and the
power supply) is controlled through a UDP socket on the loopback interface, the way a test
bench controls relays and signal generators around a real device. The firmware itself never
sees these commands. The protocol is documented in platform/posix/posix_sim.h:

    request:  <token> <command> [arguments...]
    reply:    <token> ok [key=value ...]     or     <token> err <message>
"""
from __future__ import annotations

import itertools
import socket


class SimError(RuntimeError):
    """The node answered a command with an error, or did not answer."""


class SimControl:
    """One client socket. Use as a context manager, or call close()."""

    def __init__(self, port: int, timeout: float = 1.0) -> None:
        self._address = ("127.0.0.1", port)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.settimeout(timeout)
        self._tokens = (f"t{n}" for n in itertools.count(1))

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "SimControl":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def request(self, command: str, retries: int = 2) -> dict[str, str]:
        """Send one command and return the reply's key=value pairs.

        A datagram on the loopback interface is not lost, but the node may not have opened its
        socket yet right after start, so the request is repeated a few times before giving up.
        """
        last_error: Exception | None = None
        for _ in range(retries + 1):
            token = next(self._tokens)
            self._socket.sendto(f"{token} {command}".encode(), self._address)
            try:
                while True:
                    data, _ = self._socket.recvfrom(2048)
                    words = data.decode(errors="replace").split()
                    if not words or words[0] != token:
                        continue  # a late reply to an earlier attempt
                    if len(words) >= 2 and words[1] == "ok":
                        return dict(word.split("=", 1) for word in words[2:] if "=" in word)
                    raise SimError(f"'{command}' failed: {' '.join(words[2:])}")
            except (socket.timeout, ConnectionRefusedError) as error:
                last_error = error
        raise SimError(f"no reply to '{command}' from {self._address}: {last_error}")

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
        """Destroy the next `count` transmission attempts ("tx") or received frames ("rx")."""
        return self.request(f"can.inject {kind} {count}")
