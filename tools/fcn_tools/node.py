"""Start, supervise and stop the node process.

The node is a Linux process that stands in for a microcontroller, so this module plays the
bench around it: it applies power (starts the process), observes how the node went down (the
exit status is the reset reason, see platform/include/platform.h) and can pull the plug.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from . import paths

# Every NodeProcess that was started and may still be running; see still_running().
_LIVE: set["NodeProcess"] = set()


def still_running() -> list["NodeProcess"]:
    """Nodes that were started and never ended: a test harness checks this at the end."""
    return [node for node in _LIVE if node.running]

# Exit statuses of the node, mirroring plat_reset_reason_t.
EXIT_OK = 0
RESET_REQUESTED = 10
RESET_WATCHDOG = 11
RESET_POWER_LOSS = 12
RESET_IMAGE_FAULT = 13
RESET_ASSERT = 20
RESET_STACK_OVERFLOW = 21
RESET_MALLOC_FAILED = 22


@dataclass(eq=False)
class NodeProcess:
    """One run of the node. Use as a context manager so the process never outlives a test."""

    workdir: Path
    args: Sequence[str] = ()
    binary: Path = field(default_factory=paths.node_binary)
    name: str = "node"
    _proc: subprocess.Popen | None = field(default=None, init=False, repr=False)

    @property
    def log_path(self) -> Path:
        return self.workdir / f"{self.name}.log"

    @property
    def stats_path(self) -> Path:
        return self.workdir / f"{self.name}.stats.json"

    def start(self) -> "NodeProcess":
        if not self.binary.exists():
            raise FileNotFoundError(
                f"{self.binary} does not exist: build it with scripts/build.sh release"
            )
        self.workdir.mkdir(parents=True, exist_ok=True)
        command = [
            str(self.binary),
            "--log-file", str(self.log_path),
            "--stats-json", str(self.stats_path),
            *self.args,
        ]
        # A session of its own: a Ctrl-C aimed at the test runner must not reach the node
        # behind the test's back. FCN_DIE_WITH_PARENT carries this process's id: the node
        # asks the kernel to kill it when its parent exits, so a crashed test run leaves no
        # nodes behind. (The kernel ties that to the thread that started the node: start
        # nodes from a thread that outlives them, as the main thread does.)
        # stdout and stderr go to files, not pipes: nobody reads a pipe while the node runs,
        # and a full pipe would block the node.
        with open(self.workdir / f"{self.name}.stdout.txt", "wb") as out, \
                open(self.workdir / f"{self.name}.stderr.txt", "wb") as err:
            self._proc = subprocess.Popen(
                command, cwd=self.workdir, stdin=subprocess.DEVNULL,
                stdout=out, stderr=err, start_new_session=True,
                env=dict(os.environ, FCN_DIE_WITH_PARENT=str(os.getpid())),
            )
        _LIVE.add(self)
        return self

    @property
    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def wait(self, timeout: float) -> int:
        """Wait for the node to end by itself and return its exit status."""
        assert self._proc is not None
        return self._proc.wait(timeout=timeout)

    def stop(self, timeout: float = 5.0) -> int:
        """Ask for an orderly stop (SIGTERM) and return the exit status.

        If the node does not stop in time it is killed and the timeout is re-raised, because a
        node that ignores SIGTERM is a failure the caller has to see."""
        assert self._proc is not None
        if self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.kill()
                raise
        return self._proc.returncode

    def send_signal(self, signum: int) -> None:
        assert self._proc is not None
        self._proc.send_signal(signum)

    def kill(self) -> None:
        """Pull the plug: SIGKILL, no chance to clean up."""
        if self._proc is not None and self._proc.poll() is None:
            self._proc.kill()
            self._proc.wait(timeout=5.0)

    def stderr(self) -> str:
        """What the node wrote to stderr so far (fatal messages go there, not to the log)."""
        path = self.workdir / f"{self.name}.stderr.txt"
        return path.read_text(errors="replace") if path.exists() else ""

    def log(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def stats(self) -> dict:
        return json.loads(self.stats_path.read_text())

    def __enter__(self) -> "NodeProcess":
        return self.start()

    def __exit__(self, *exc_info) -> None:
        self.kill()
