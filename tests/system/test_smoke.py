"""The node as a process: it starts, keeps time, reports its statistics and stops cleanly."""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import time

import pytest

from fcn_tools.node import EXIT_OK


def test_version_names_the_node_and_the_kernel(node_binary):
    out = subprocess.run([str(node_binary), "--version"], capture_output=True, text=True,
                         timeout=10)
    assert out.returncode == 0
    assert out.stdout.startswith("fcn_node ")
    assert "FreeRTOS V11.3.3" in out.stdout


def test_unknown_option_is_a_usage_error(node_binary):
    out = subprocess.run([str(node_binary), "--tick-mode", "nonsense"], capture_output=True,
                         text=True, timeout=10)
    assert out.returncode == 1
    assert "invalid --tick-mode" in out.stderr


def test_runs_for_the_requested_time_and_reports_statistics(start_node):
    node = start_node("--duration-s", "2")
    assert node.wait(timeout=20) == EXIT_OK

    stats = node.stats()
    assert stats["schema"] == "fcn.node_stats/1"
    assert stats["kernel"] == "V11.3.3"
    assert stats["tick"]["thread_seen"] is True
    # The node stops itself after 2 s of tick time: 2000 ticks plus the last task period.
    assert 2000 <= stats["tick"]["count"] <= 2150
    assert stats["log_lines_dropped"] == 0

    tasks = {task["name"]: task for task in stats["tasks"]}
    assert {"ticker", "health", "IDLE"} <= set(tasks)
    assert tasks["ticker"]["priority"] > tasks["health"]["priority"] > tasks["IDLE"]["priority"]
    assert sum(task["run_time_us"] for task in tasks.values()) <= stats["run_time_total_us"]
    # A node that only blinks is idle nearly all the time.
    assert tasks["IDLE"]["run_time_us"] > 0.5 * stats["run_time_total_us"]
    # Every task registered its real stack, and none of them is anywhere near its limit.
    for task in tasks.values():
        assert 0 < task["stack_peak_bytes"] < 256 * 1024

    log = node.log()
    assert "starting" in log
    assert "stopping after" in log
    assert "scheduler stopped" in log


@pytest.mark.parametrize("stop_signal", [signal.SIGTERM, signal.SIGINT])
def test_a_stop_signal_ends_the_node_through_its_orderly_path(start_node, stop_signal):
    node = start_node()
    time.sleep(0.5)
    assert node.running
    node.send_signal(stop_signal)
    assert node.wait(timeout=5) == EXIT_OK
    assert "scheduler stopped" in node.log()
    assert node.stats()["tick"]["count"] >= 400


@pytest.mark.timing
def test_the_tick_tracks_the_wall_clock(start_node):
    node = start_node("--duration-s", "3")
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()
    tick = stats["tick"]
    # Every tick period of wall time is either counted or reported as dropped (dropped: the
    # host did not run the node; zero on a machine with a CPU to spare). elapsed_ns includes
    # starting and stopping the scheduler, so this bound is loose; the platform self-test
    # checks the same identity precisely.
    period_us = stats["elapsed_ns"] / 1000 / (tick["count"] + tick["dropped"])
    assert 970 <= period_us <= 1030, (
        f"one accounted tick period took {period_us:.1f} us ({tick['dropped']} dropped)")


def test_the_stock_tick_mode_still_runs(start_node):
    node = start_node("--duration-s", "1", "--tick-mode", "stock")
    assert node.wait(timeout=20) == EXIT_OK
    assert node.stats()["tick"]["mode"] == "stock"


# A harness passes its process id in FCN_DIE_WITH_PARENT; see platform/posix/node_main.c.

_START_A_NODE_AND_DIE = """
import os, subprocess, sys, time
node = subprocess.Popen(sys.argv[1:], env=dict(os.environ, FCN_DIE_WITH_PARENT=str(os.getpid())))
print(node.pid, flush=True)
time.sleep(0.5)          # the node is up and running by now
os._exit(0)              # the harness dies without stopping its node
"""


def _process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_a_node_is_killed_when_the_harness_that_started_it_dies(node_binary):
    # The duration only bounds the damage if this test fails: the node then ends by itself.
    harness = subprocess.run([sys.executable, "-c", _START_A_NODE_AND_DIE, str(node_binary),
                              "--duration-s", "30"],
                             capture_output=True, text=True, timeout=20)
    node_pid = int(harness.stdout.strip())

    deadline = time.monotonic() + 5.0
    while _process_exists(node_pid) and time.monotonic() < deadline:
        time.sleep(0.05)
    try:
        assert not _process_exists(node_pid), "the node outlived its harness"
    finally:
        if _process_exists(node_pid):
            os.kill(node_pid, signal.SIGKILL)


def test_a_node_whose_harness_is_gone_before_it_looks_does_not_run(node_binary):
    # The process id of a parent that this node does not have: what the node sees when its
    # harness died before the node could ask the kernel to be killed with it.
    out = subprocess.run([str(node_binary), "--duration-s", "5"], capture_output=True, text=True,
                         timeout=10, env=dict(os.environ, FCN_DIE_WITH_PARENT="1"))
    assert out.returncode == 1
