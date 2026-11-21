# SPDX-License-Identifier: MIT
"""fcn_tools.node.NodeProcess, the supervision of the node process, against stand-in nodes:
small shell scripts that end the way a node can end. (tests/python/test_harness.py checks
that the fixtures of tests/conftest.py act on what NodeProcess reports.)

What depends on it: a system test of the form "the node must not send X" passes against a
node that died at once, unless a death the test did not look at fails the test; and a node
whose kicks never reach the hardware watchdog passes every run shorter than the watchdog's
timeout, unless the statistics of the run are checked (lost_kicks())."""
from __future__ import annotations

import json
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
    # A start-up error (an unusable option): status 1, which is no reset.
    "refuses": 'echo "cannot write --retained-file /no/such/cell" >&2\nexit 1\n',
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
    # Writes $STAND_IN_STATS as its statistics and ends with status $STAND_IN_STATUS (0).
    "writes_stats": ('while [ $# -gt 0 ]; do\n'
                     '  if [ "$1" = --stats-json ]; then printf %s "$STAND_IN_STATS" > "$2"; fi\n'
                     '  shift\n'
                     'done\n'
                     'exit "${STAND_IN_STATUS:-0}"\n'),
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


def test_the_retained_cell_is_passed_and_a_restart_keeps_it_and_the_options(stand_in, started,
                                                                             tmp_path):
    cell = tmp_path / "cell.bin"
    node = started(stand_in("reports"), can="udp:21000", sim_port=21016, retained_file=cell,
                   args=("--duration-s", "1"))
    assert node.wait(timeout=10) == 0
    assert (f"--sim-port 21016 --retained-file {cell} --duration-s 1") in \
        node.stdout_path.read_text()

    again = node.restarted()
    assert (again.name, again.retained_file, again.can, again.sim_port, tuple(again.args)) == \
        ("node-again", cell, "udp:21000", 21016, ("--duration-s", "1"))
    assert not again.running
    again.start()
    assert again.wait(timeout=10) == 0
    out = again.stdout_path.read_text()
    assert f"--log-file {again.log_path} " in out and f"--retained-file {cell} " in out
    # The first run's files are its own and stay.
    assert node.stdout_path.exists() and node.stdout_path != again.stdout_path
    assert node.restarted("third").name == "third"


def test_a_node_that_runs_cannot_be_restarted(stand_in, started):
    node = started(stand_in("runs"))
    wait_until_ready(node)
    with pytest.raises(AssertionError, match="still running"):
        node.restarted()


def test_wait_for_reset_takes_a_reset_and_refuses_another_end(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait_for_reset(timeout=10) == 20
    assert node.unnoticed_end() is None

    clean = started(stand_in("clean"), name="clean")
    with pytest.raises(AssertionError, match="status 0, not a reset"):
        clean.wait_for_reset(timeout=10)


# -------------------------------------------------------------------- unexpected resets


def test_a_reset_the_caller_did_not_wait_for_as_a_reset_is_unexpected(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait(timeout=10) == 20               # seen, but not waited for as a reset
    problem = node.unexpected_reset()
    assert problem is not None
    assert "was reset (status 20) in a test that does not expect a reset" in problem
    assert "FATAL assertion failed: app.c 42" in problem

    stopped = started(stand_in("fails_to_stop"), name="stopped")
    wait_until_ready(stopped)
    assert stopped.stop(timeout=10) == 21            # a reset on the way down counts too
    assert "status 21" in (stopped.unexpected_reset() or "")


def test_a_reset_waited_for_or_an_end_that_is_no_reset_is_not_unexpected(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait_for_reset(timeout=10) == 20
    assert node.unexpected_reset() is None

    clean = started(stand_in("clean"), name="clean")
    assert clean.wait(timeout=10) == 0
    assert clean.unexpected_reset() is None

    refused = started(stand_in("refuses"), name="refused")   # a start-up error, status 1
    assert refused.wait(timeout=10) == 1
    assert refused.unexpected_reset() is None

    running = started(stand_in("runs"), name="running")
    assert running.unexpected_reset() is None


def test_a_new_start_forgets_that_a_reset_was_waited_for(stand_in, started):
    node = started(stand_in("dies"))
    assert node.wait_for_reset(timeout=10) == 20
    node.start()
    assert node.wait(timeout=10) == 20
    assert "status 20" in (node.unexpected_reset() or "")


# -------------------------------------------------------------------------- lost kicks


def kick_stats(started=True, hw_kicks=20, kicks=20, withheld=0, cycles=20) -> str:
    """Statistics as the node writes them, with only what lost_kicks() reads: by default those
    of a 2 s run whose 20 supervision cycles each ended with a kick."""
    return json.dumps({"hw_watchdog": {"started": started, "kicks": hw_kicks},
                       "watchdog_manager": {"kicks": kicks, "kicks_withheld": withheld,
                                            "cycles": cycles}})


def ended_run(stand_in, started, monkeypatch, stats: str, status: int = 0,
              name: str = "node") -> NodeProcess:
    """A stand-in run that wrote `stats` and ended with `status`, waited for."""
    monkeypatch.setenv("STAND_IN_STATS", stats)
    monkeypatch.setenv("STAND_IN_STATUS", str(status))
    node = started(stand_in("writes_stats"), name=name)
    assert node.wait(timeout=10) == status
    return node


@pytest.mark.parametrize("stats", [
    kick_stats(),
    # A withheld kick (an expired supervision) is a decision too: 17 kicks, 3 withheld.
    kick_stats(hw_kicks=17, kicks=17, withheld=3),
], ids=["all_kicked", "kicks_withheld"])
def test_an_orderly_run_whose_kicks_all_reached_the_watchdog_lost_none(stand_in, started,
                                                                       monkeypatch, stats):
    assert ended_run(stand_in, started, monkeypatch, stats).lost_kicks() is None


@pytest.mark.parametrize("stats, shown", [
    # The health task kicked, the watchdog counted none: the kick went nowhere.
    (kick_stats(hw_kicks=0), "started True, 0 kicks counted by the watchdog, 20 decided on"),
    # One kick the watchdog counted that the health task did not decide on.
    (kick_stats(hw_kicks=21), "21 kicks counted by the watchdog, 20 decided on"),
    (kick_stats(started=False), "started False"),
    # A supervision cycle without a decision, or one decision too many.
    (kick_stats(cycles=21), "20 decided on and 0 withheld by the health task in 21 "
                            "supervision cycles"),
    (kick_stats(hw_kicks=19, kicks=19, withheld=2), "19 decided on and 2 withheld"),
], ids=["kick_lost", "kick_from_elsewhere", "not_started", "cycle_without_decision",
        "decision_too_many"])
def test_an_orderly_run_with_a_kick_missing_is_reported_with_its_counts(stand_in, started,
                                                                         monkeypatch, stats,
                                                                         shown):
    problem = ended_run(stand_in, started, monkeypatch, stats).lost_kicks()
    assert problem is not None and problem.startswith("node lost kicks of the hardware watchdog")
    assert shown in problem


def test_statistics_without_the_kick_counts_are_reported(stand_in, started, monkeypatch):
    problem = ended_run(stand_in, started, monkeypatch, '{"schema": "stand-in"}').lost_kicks()
    assert problem == "node wrote statistics without 'hw_watchdog': its kicks cannot be checked"


def test_only_a_run_that_ended_in_order_with_statistics_is_checked(stand_in, started,
                                                                    monkeypatch):
    # Counts that are a lost kick in an orderly run (above), here after a reset (20) and a
    # start-up error (1), which end no run in order.
    lost = kick_stats(hw_kicks=0)
    assert ended_run(stand_in, started, monkeypatch, lost, status=20).lost_kicks() is None
    assert ended_run(stand_in, started, monkeypatch, lost, status=1,
                     name="refused").lost_kicks() is None
    clean = started(stand_in("clean"), name="clean")       # status 0, no statistics
    assert clean.wait(timeout=10) == 0 and clean.lost_kicks() is None
    running = started(stand_in("runs"), name="running")
    wait_until_ready(running)
    assert running.lost_kicks() is None
    running.kill()
    assert running.lost_kicks() is None
    assert NodeProcess(workdir=Path("never-started")).lost_kicks() is None


def test_a_missing_binary_is_reported_before_anything_starts(tmp_path):
    node = NodeProcess(workdir=tmp_path / "work", binary=tmp_path / "no-such-node")
    with pytest.raises(FileNotFoundError, match="does not exist"):
        node.start()
    assert not node.running and node.unnoticed_end() is None
