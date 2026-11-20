"""Start, supervise and stop the node process.

The node is a Linux process that stands in for a microcontroller, so this module plays the
bench around it: it applies power (starts the process), observes how the node went down (the
exit status is the reset reason, see platform/include/platform.h) and can pull the plug.

A reset ends the process, but on a board the microcontroller starts again at once, with its
retained RAM intact. The bench does the same with restarted(): a new run of the same node,
with the same options and the same retained-RAM cell (`retained_file`, the node's
`--retained-file`), so that the new run learns why the last one ended. Removing the cell's file
before the restart is a loss of power.
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
from .simctl import SimControl

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

#: The exit statuses that are resets of the node, after which a board starts again by itself.
RESETS = frozenset({RESET_REQUESTED, RESET_WATCHDOG, RESET_POWER_LOSS, RESET_IMAGE_FAULT,
                    RESET_ASSERT, RESET_STACK_OVERFLOW, RESET_MALLOC_FAILED})


@dataclass(eq=False)
class NodeProcess:
    """One run of the node. Use as a context manager so the process never outlives a test."""

    workdir: Path
    args: Sequence[str] = ()
    binary: Path = field(default_factory=paths.node_binary)
    name: str = "node"
    can: str | None = None        # bus to attach to ("udp:21000", "socketcan:vcan0")
    sim_port: int | None = None   # serve simulation control on this port; None: off
    # The retained-RAM cell (--retained-file); None: no cell, every start is a power-on.
    retained_file: Path | None = None
    _proc: subprocess.Popen | None = field(default=None, init=False, repr=False)
    # True once the caller has taken note of how the run ended: it waited for the node,
    # stopped it, or killed it on purpose. See unnoticed_end().
    _end_noticed: bool = field(default=False, init=False, repr=False)
    # True once the caller has waited for a reset with wait_for_reset(). See unexpected_reset().
    _reset_awaited: bool = field(default=False, init=False, repr=False)

    @property
    def log_path(self) -> Path:
        return self.workdir / f"{self.name}.log"

    @property
    def stats_path(self) -> Path:
        return self.workdir / f"{self.name}.stats.json"

    @property
    def stdout_path(self) -> Path:
        return self.workdir / f"{self.name}.stdout.txt"

    @property
    def stderr_path(self) -> Path:
        return self.workdir / f"{self.name}.stderr.txt"

    def start(self) -> "NodeProcess":
        if not self.binary.exists():
            raise FileNotFoundError(
                f"{self.binary} does not exist: build it with scripts/build.sh release"
            )
        self.workdir.mkdir(parents=True, exist_ok=True)
        # Files of an earlier run with the same name would otherwise answer for this one: a
        # node that dies before writing its statistics must leave no statistics file at all.
        for path in (self.log_path, self.stats_path, self.stdout_path, self.stderr_path):
            path.unlink(missing_ok=True)
        self._end_noticed = False
        self._reset_awaited = False
        command = [
            str(self.binary),
            "--log-file", str(self.log_path),
            "--stats-json", str(self.stats_path),
        ]
        if self.can is not None:
            command += ["--can", self.can]
        if self.sim_port is not None:
            command += ["--sim-port", str(self.sim_port)]
        if self.retained_file is not None:
            command += ["--retained-file", str(self.retained_file)]
        command += self.args
        # A session of its own: a Ctrl-C aimed at the test runner must not reach the node
        # behind the test's back. FCN_DIE_WITH_PARENT carries this process's id: the node
        # asks the kernel to kill it when its parent exits, so a crashed test run leaves no
        # nodes behind. (The kernel ties that to the thread that started the node: start
        # nodes from a thread that outlives them, as the main thread does.)
        # stdout and stderr go to files, not pipes: nobody reads a pipe while the node runs,
        # and a full pipe would block the node.
        with open(self.stdout_path, "wb") as out, open(self.stderr_path, "wb") as err:
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

    @property
    def pid(self) -> int:
        assert self._proc is not None
        return self._proc.pid

    def sim(self, timeout: float = 1.0) -> SimControl:
        """A client of this node's simulation control channel; the caller closes it."""
        if self.sim_port is None:
            raise RuntimeError("this node was started without a simulation control port")
        return SimControl(self.sim_port, timeout=timeout)

    def wait(self, timeout: float) -> int:
        """Wait for the node to end by itself and return its exit status."""
        assert self._proc is not None
        status = self._proc.wait(timeout=timeout)
        self._end_noticed = True
        return status

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
        self._end_noticed = True
        return self._proc.returncode

    def send_signal(self, signum: int) -> None:
        assert self._proc is not None
        self._proc.send_signal(signum)

    def wait_for_reset(self, timeout: float) -> int:
        """Wait for the node to end by itself, as a reset ends it, and return the exit status;
        raises AssertionError if it ended in any other way. TimeoutExpired if it does not end
        within `timeout` seconds (the caller decides whether that is a timing tolerance).
        This is how a test says that it expects a reset; see unexpected_reset()."""
        self._reset_awaited = True
        status = self.wait(timeout=timeout)
        assert status in RESETS, (f"{self.name} ended with status {status}, not a reset\n"
                                  f"--- stderr\n{self.stderr()}")
        return status

    def restarted(self, name: str | None = None) -> "NodeProcess":
        """A new run of this node, as a board starts again after a reset: the same binary,
        options, bus, simulation control port and retained-RAM cell, not yet started. It gets
        files of its own, under `name` (default: this run's name with "-again"), so that this
        run's log and statistics stay. This run must have ended: two runs never share a cell."""
        assert self._proc is not None and self._proc.poll() is not None, \
            f"{self.name} is still running"
        return NodeProcess(workdir=self.workdir, args=tuple(self.args), binary=self.binary,
                           name=name or f"{self.name}-again", can=self.can,
                           sim_port=self.sim_port, retained_file=self.retained_file)

    def kill(self) -> None:
        """Pull the plug: SIGKILL, no chance to clean up. A test that calls this has decided
        how the run ends, so the end counts as noticed."""
        self._end_noticed = True
        self._kill()

    def _kill(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            self._proc.kill()
            self._proc.wait(timeout=5.0)

    def unnoticed_end(self) -> str | None:
        """What went wrong with this run without the caller seeing it, or None.

        Two things count: the node ended by itself (an assertion, a crash, a sanitizer
        report) while the test still treated it as running and never looked at the exit
        status; or it ended with status 0 but wrote to stderr, where only fatal messages and
        sanitizer reports go. A harness asks this before it cleans up and fails the test if
        there is an answer: otherwise a test of the form "the node must not send X" passes
        against a node that died on its first frame.
        """
        if self._proc is None:
            return None
        status = self._proc.poll()
        if status is None:
            return None
        stderr = self.stderr().strip()
        tail = "\n".join(stderr.splitlines()[-20:])
        if not self._end_noticed:
            return (f"{self.name} ended by itself with status {status} while the test was not "
                    f"looking\n--- stderr (last lines)\n{tail}")
        if status == 0 and stderr:
            return f"{self.name} exited 0 but wrote to stderr\n--- stderr (last lines)\n{tail}"
        return None

    def unexpected_reset(self) -> str | None:
        """A reset of this run that the caller did not expect, or None.

        A run that ended with one of the statuses in RESETS (the watchdog's 11 among them) was
        reset, and a test expects a reset only by waiting for it with wait_for_reset(). Any
        other test whose node was reset fails, also one that looked at the status itself
        (wait(), stop()): so every system test that does not expect a reset also shows that
        its node's hardware watchdog was kicked for the whole run.
        """
        if self._proc is None:
            return None
        status = self._proc.poll()
        if status is None or status not in RESETS or self._reset_awaited:
            return None
        tail = "\n".join(self.stderr().strip().splitlines()[-20:])
        return (f"{self.name} was reset (status {status}) in a test that does not expect a "
                f"reset (it never called wait_for_reset())\n--- stderr (last lines)\n{tail}")

    def stderr(self) -> str:
        """What the node wrote to stderr so far (fatal messages go there, not to the log)."""
        path = self.stderr_path
        return path.read_text(errors="replace") if path.exists() else ""

    def log(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def stats(self) -> dict:
        """The statistics this run wrote when it stopped. Raises FileNotFoundError if it
        wrote none (killed, or died first); never an earlier run's file, which start()
        removed."""
        return json.loads(self.stats_path.read_text())

    def __enter__(self) -> "NodeProcess":
        return self.start()

    def __exit__(self, *exc_info) -> None:
        self._kill()
