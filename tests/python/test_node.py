# SPDX-License-Identifier: MIT
"""fcn_tools.node.NodeProcess, the supervision of the node process, against stand-in nodes:
small shell scripts that end the way a node can end. (tests/python/test_harness.py checks
that the fixtures of tests/conftest.py act on what NodeProcess reports.)

What depends on it: a system test of the form "the node must not send X" passes against a
node that died at once, unless a death the test did not look at fails the test."""
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

import pytest

from fcn_tools import node as node_module
from fcn_tools.node import NodeProcess

# A stand-in prints "ready" once its signal handling is set up; tests that signal it wait for
# that line first, so the signal never arrives before the trap exists.
STAND_INS = {
    # An assertion in the firmware: a message on stderr and status 20 (RESET_ASSERT).
    "dies": 'echo "FATAL assertion failed: app.c 42" >&2\nexit 20\n',
    # A run that ends cleanly but wrote to stderr, where only fatal messages and sanitizer
    # reports go.
    "noisy": 'echo "runtime error: load of misaligned address" >&2\nexit 0\n',
    "clean": "exit 0\n",
    # Runs until it is stopped, and stops cleanly on SIGTERM.
    "runs": "trap 'exit 0' TERM\necho ready\nwhile :; do sleep 0.02; done\n",
    # Reports a stack overflow (21) when asked to stop.
    "fails_to_stop": "trap 'exit 21' TERM\necho ready\nwhile :; do sleep 0.02; done\n",
    # Ignores SIGTERM: only SIGKILL ends it.
    "hangs": "trap '' TERM\necho ready\nexec sleep 30\n",
    # Writes what the harness passed it, then the statistics file named on its command line.
    "reports": ('echo "parent=$FCN_DIE_WITH_PARENT args=$*"\n'
                'while [ $# -gt 0 ]; do\n'
                '  if [ "$1" = --stats-json ]; then echo \'{"schema": "stand-in"}\' > "$2"; fi\n'
                '  shift\n'
                'done\n'),
}


@pytest.fixture
def stand_in(tmp_path):
    """Factory: stand_in(kind) returns the path of an executable stand-in node."""
    def _make(kind: str) -> Path:
        path = tmp_path / f"{kind}.sh"
        path.write_text("#!/bin/sh\n" + STAND_INS[kind])
        path.chmod(0o755)
        return path
    return _make


@pytest.fixture
def started(tmp_path):
    """Factory: started(binary, **kwargs) starts a NodeProcess; every one is killed at the end."""
    nodes: list[NodeProcess] = []

    def _start(binary: Path, **kwargs) -> NodeProcess:
        node = NodeProcess(workdir=tmp_path / "work", binary=binary, **kwargs).start()
        nodes.append(node)
        return node

    yield _start
    for node in nodes:
        node.kill()


def wait_until_ended(node: NodeProcess, timeout: float = 10.0) -> None:
    """Wait for the process to end without telling the node (unlike wait(), which counts as
    the caller looking at the exit status)."""
    deadline = time.monotonic() + timeout
    while node.running:
        assert time.monotonic() < deadline, "the stand-in did not end"
        time.sleep(0.01)


def wait_until_ready(node: NodeProcess, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while "ready" not in node.stdout_path.read_text():
        assert time.monotonic() < deadline, "the stand-in did not get ready"
        time.sleep(0.01)


# ------------------------------------------------------------------------- unnoticed ends


def test_a_death_nobody_looked_at_is_reported_with_its_status_and_stderr(stand_in, started):
    node = started(stand_in("dies"))
    wait_until_ended(node)
    problem = node.unnoticed_end()
    assert problem is not None
    assert "ended by itself with status 20 while the test was not looking" in problem
    assert "FATAL assertion failed: app.c 42" in problem


def test_a_death_the_caller_waited_for_is_its_business(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait(timeout=10) == 20
    assert node.unnoticed_end() is None


def test_a_clean_exit_with_output_on_stderr_is_reported_even_when_waited_for(stand_in, started):
    node = started(stand_in("noisy"))
    assert node.wait(timeout=10) == 0
    problem = node.unnoticed_end()
    assert problem is not None and "exited 0 but wrote to stderr" in problem
    assert "runtime error: load of misaligned address" in problem


def test_a_clean_exit_that_was_waited_for_is_not_a_problem(stand_in, started):
    node = started(stand_in("clean"))
    assert node.wait(timeout=10) == 0
    assert node.unnoticed_end() is None


def test_a_node_that_is_still_running_is_not_a_problem_yet(stand_in, started):
    node = started(stand_in("runs"))
    wait_until_ready(node)
    assert node.running and node.unnoticed_end() is None
    assert node in node_module.still_running()


def test_a_node_killed_on_purpose_is_not_a_problem(stand_in, started):
    node = started(stand_in("runs"))
    wait_until_ready(node)
    node.kill()
    assert not node.running and node.unnoticed_end() is None
    assert node not in node_module.still_running()


def test_a_new_start_forgets_that_the_last_end_was_noticed(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait(timeout=10) == 20
    node.start()
    wait_until_ended(node)
    assert "status 20 while the test was not looking" in (node.unnoticed_end() or "")


# ---------------------------------------------------------------------------------- stop()


def test_stop_returns_the_status_the_node_ended_with(stand_in, started):
    node = started(stand_in("runs"))
    wait_until_ready(node)
    assert node.stop(timeout=10) == 0

    node = started(stand_in("fails_to_stop"), name="second")
    wait_until_ready(node)
    assert node.stop(timeout=10) == 21
    assert node.unnoticed_end() is None      # the caller has the status


def test_stop_of_a_node_that_already_ended_returns_its_status(stand_in, started):
    node = started(stand_in("dies"))
    wait_until_ended(node)
    assert node.stop() == 20
    assert node.unnoticed_end() is None


def test_a_node_that_ignores_the_stop_request_is_killed_and_the_timeout_raised(stand_in, started):
    node = started(stand_in("hangs"))
    wait_until_ready(node)
    with pytest.raises(subprocess.TimeoutExpired):
        node.stop(timeout=0.3)
    assert not node.running
    assert node not in node_module.still_running()


# ----------------------------------------------------------------- start() and its files


def test_the_harness_passes_its_process_id_and_the_file_options(stand_in, started, tmp_path):
    node = started(stand_in("reports"), can="udp:21000", sim_port=21016, args=("--duration-s", "1"))
    assert node.wait(timeout=10) == 0
    out = node.stdout_path.read_text()
    assert f"parent={os.getpid()} " in out
    assert (f"--log-file {node.log_path} --stats-json {node.stats_path} --can udp:21000 "
            "--sim-port 21016 --duration-s 1") in out
    assert node.stats() == {"schema": "stand-in"}


def test_statistics_of_an_earlier_run_never_answer_for_this_one(stand_in, started, tmp_path):
    workdir = tmp_path / "work"
    workdir.mkdir()
    (workdir / "node.stats.json").write_text('{"schema": "an earlier run"}')
    (workdir / "node.stderr.txt").write_text("FATAL from an earlier run\n")
    node = started(stand_in("clean"))
    assert node.wait(timeout=10) == 0
    with pytest.raises(FileNotFoundError):
        node.stats()
    assert node.stderr() == "" and node.unnoticed_end() is None


def test_a_missing_binary_is_reported_before_anything_starts(tmp_path):
    node = NodeProcess(workdir=tmp_path / "work", binary=tmp_path / "no-such-node")
    with pytest.raises(FileNotFoundError, match="does not exist"):
        node.start()
    assert not node.running and node.unnoticed_end() is None
