"""The node's host-only trace as fcn_tools.trace reads it and takes it apart: the file, the tick's
grid, the running intervals, the jobs of a task and the frames, on constructed traces whose answers
are worked out by hand in the comments."""
from __future__ import annotations

import numpy as np
import pytest

from fcn_tools import trace
from fcn_tools.trace import (DELAY_UNTIL, FLAG_BLOCKED, READY, SENSOR, SWITCH, TICK_BASE, TX,
                             EVENT_DTYPE, HEADER, Trace)


def events(*rows: dict) -> np.ndarray:
    """Records from dicts of their fields (missing fields are 0)."""
    out = np.zeros(len(rows), dtype=EVENT_DTYPE)
    for i, row in enumerate(rows):
        for key, value in row.items():
            out[i][key] = value
    return out


def switch(t, into, out=0, blocked=False, tick=0):
    return {"kind": SWITCH, "time_ns": t, "task": into, "other": out,
            "flags": FLAG_BLOCKED if blocked else 0, "value": tick}


def ready(t, task, tick=0):
    return {"kind": READY, "time_ns": t, "task": task, "value": tick}


def delay(t, task, wake):
    return {"kind": DELAY_UNTIL, "time_ns": t, "task": task, "value": wake}


NAMES = {1: "idle", 2: "sensor", 3: "health"}


def make(*rows: dict) -> Trace:
    return Trace(events=events(*rows), names=dict(NAMES))


# ------------------------------------------------------------------------------------ the file


def test_read_takes_a_file_as_the_node_writes_it(tmp_path):
    records = events(switch(100, 1), ready(200, 2, tick=5))
    path = tmp_path / "trace.bin"
    path.write_bytes(HEADER.pack(b"FCNTRACE", 1, 32, 2, 0, 16, 20_500, 4096, 0)
                     + records.tobytes())
    stats = {"events": 2, "dropped": 0, "tasks": [{"number": 1, "name": "IDLE"}]}
    found = trace.read(path, stats)
    assert found.capacity == 16 and found.cost_ps == 20_500 and found.cost_runs == 4096
    assert found.names == {1: "IDLE"} and found.number("IDLE") == 1
    assert np.array_equal(found.events, records)


@pytest.mark.parametrize("header, body, stats, why", [
    ((b"FCNTRACX", 1, 32, 0, 0, 0, 0, 0, 0), b"", {"events": 0, "dropped": 0}, "magic"),
    ((b"FCNTRACE", 2, 32, 0, 0, 0, 0, 0, 0), b"", {"events": 0, "dropped": 0}, "version"),
    ((b"FCNTRACE", 1, 32, 2, 0, 0, 0, 0, 0), bytes(32), {"events": 2, "dropped": 0}, "holds"),
    ((b"FCNTRACE", 1, 32, 1, 3, 0, 0, 0, 0), bytes(32), {"events": 1, "dropped": 0}, "dropped"),
])
def test_read_refuses_a_file_that_is_not_the_trace_the_statistics_describe(tmp_path, header,
                                                                           body, stats, why):
    path = tmp_path / "trace.bin"
    path.write_bytes(HEADER.pack(*header) + body)
    with pytest.raises(trace.TraceError, match=why):
        trace.read(path, {**stats, "tasks": []})


def test_a_task_the_trace_never_saw_has_no_number():
    with pytest.raises(trace.TraceError):
        make().number("diag")


# ------------------------------------------------------------------------------------ the tick


def test_every_tick_is_due_on_the_base_in_force_for_it():
    # Base 1,000,000 ns for ticks from 1, period 1 ms: tick k at 1e6 + (k - 1) ms. From tick 50
    # on, after three ticks were given up, the base is 3 ms later.
    grid = trace.tick_grid(make(
        {"kind": TICK_BASE, "time_ns": 1_000_000, "value": 1, "arg": 1_000_000},
        {"kind": TICK_BASE, "time_ns": 4_000_000, "value": 50, "arg": 1_000_000}))
    assert grid.due_ns([1, 2, 49]).tolist() == [1_000_000, 2_000_000, 49_000_000]
    assert grid.due_ns([50, 51]).tolist() == [53_000_000, 54_000_000]
    with pytest.raises(trace.TraceError):
        grid.due_ns([0])


def test_a_trace_without_a_base_or_with_two_periods_has_no_grid():
    with pytest.raises(trace.TraceError, match="no TICK_BASE"):
        trace.tick_grid(make(switch(1, 1)))
    with pytest.raises(trace.TraceError, match="period"):
        trace.tick_grid(make({"kind": TICK_BASE, "time_ns": 0, "value": 1, "arg": 1000},
                             {"kind": TICK_BASE, "time_ns": 0, "value": 9, "arg": 2000}))


# --------------------------------------------------------------------------- running intervals


def intervals_trace() -> Trace:
    # idle 0-100, sensor 100-130, idle 130-500, sensor 500-520, health 520-600, idle 600-...
    return make(switch(0, 1), switch(100, 2, 1), switch(130, 1, 2, blocked=True),
                switch(500, 2, 1), switch(520, 3, 2, blocked=True),
                switch(600, 1, 3, blocked=True))


def test_each_switch_starts_an_interval_of_the_task_switched_in():
    found = trace.running_intervals(intervals_trace())
    assert found.task.tolist() == [1, 2, 1, 2, 3]
    assert found.start_ns.tolist() == [0, 100, 130, 500, 520]
    assert found.end_ns.tolist() == [100, 130, 500, 520, 600]


def test_running_time_counts_only_the_time_a_task_ran():
    found = trace.running_intervals(intervals_trace())
    # sensor ran 100-130 and 500-520.
    assert trace.running_time(found, 2, [0, 110, 130, 510, 1000]).tolist() == [0, 10, 30, 40, 50]
    assert trace.running_between(found, 2, [0, 120], [1000, 510]).tolist() == [50, 20]
    # Windows of 250 ns: sensor 30 in the first, 0, 20, 0.
    assert trace.demand_per_window(found, 2, [0, 250, 500, 750, 1000]).tolist() == [30, 0, 20, 0]
    assert trace.running_time(found, 9, [100]).tolist() == [0]


# ------------------------------------------------------------------------------------ the jobs


def jobs_trace() -> Trace:
    """sensor (2): its first stretch from its creation to its first block is no job. Then it
    waits for tick 10; the tick readies it at 1000 while idle runs; it starts at 1010, health
    (3) preempts it at 1020 (sensor stays ready), it runs again from 1050 and blocks at 1070,
    waiting for tick 20. The next job: readied at 2000, started at 2005, a READY of the running
    sensor at 2008 (no release), blocked at 2030. A third job is released at 3000 and never
    ends: not complete."""
    return make(
        ready(5, 2), switch(10, 2), delay(15, 2, 10), switch(20, 1, 2, blocked=True),
        ready(1000, 2, tick=10), switch(1010, 2, 1), switch(1020, 3, 2), switch(1050, 2, 3),
        delay(1060, 2, 20), switch(1070, 1, 2, blocked=True),
        ready(2000, 2, tick=20), switch(2005, 2, 1), ready(2008, 2, tick=20),
        delay(2020, 2, 30), switch(2030, 1, 2, blocked=True),
        ready(3000, 2, tick=30), switch(3001, 2, 1))


def test_a_job_runs_from_its_release_to_its_block_and_preemption_does_not_end_it():
    jobs = trace.task_jobs(jobs_trace(), 2)
    assert len(jobs) == 2
    assert jobs.release_ns.tolist() == [1000, 2000]
    assert jobs.release_tick.tolist() == [10, 20]
    assert jobs.wake_tick.tolist() == [10, 20]
    assert jobs.start_ns.tolist() == [1010, 2005]
    assert jobs.end_ns.tolist() == [1070, 2030]
    # 1010-1020 and 1050-1070; 2005-2030.
    assert jobs.exec_ns.tolist() == [30, 25]
    assert jobs.readied_by.tolist() == [1, 1]


def test_a_job_that_blocks_on_a_mutex_is_joined_to_its_first_part():
    # health (3) waits for tick 100, is released at 100, blocks at 120 (a mutex), is readied by
    # sensor (2) at 150, runs 160-170 and blocks in xTaskDelayUntil for tick 200.
    found = make(switch(0, 3), delay(1, 3, 100), switch(2, 1, 3, blocked=True),
                 ready(100, 3, tick=100), switch(110, 3, 1), switch(120, 2, 3, blocked=True),
                 ready(150, 3), switch(160, 3, 2, blocked=True), delay(165, 3, 200),
                 switch(170, 1, 3, blocked=True))
    parts = trace.task_jobs(found, 3)
    assert len(parts) == 2 and parts.wake_tick.tolist() == [100, -1]
    assert parts.readied_by.tolist() == [1, 2]
    jobs, joined = trace.periodic_jobs(found, 3)
    assert joined == 1 and len(jobs) == 1
    assert jobs.release_ns.tolist() == [100] and jobs.start_ns.tolist() == [110]
    assert jobs.end_ns.tolist() == [170] and jobs.exec_ns.tolist() == [20]
    assert jobs.parts.tolist() == [2]


def test_merge_continuations_drops_a_continuation_with_no_job_before_it():
    jobs = trace.task_jobs(jobs_trace(), 2)
    merged, joined = trace.merge_continuations(jobs, [True, False])
    assert joined == 0 and len(merged) == 1 and merged.release_ns.tolist() == [2000]
    with pytest.raises(ValueError):
        trace.merge_continuations(jobs, [True])


# ----------------------------------------------------------------------- frames and samples


def test_the_frames_come_in_the_order_of_their_end_of_frame():
    found = make(
        {"kind": TX, "time_ns": 900, "value": 800, "arg": 0x30, "task": 8, "flags": 0,
         "data": [1, 2, 3, 4, 5, 6, 7, 8]},
        {"kind": TX, "time_ns": 700, "value": 600, "arg": 0x20, "task": 2, "flags": 0,
         "data": [9, 9, 0, 0, 0, 0, 0, 0]})
    frames = trace.sent_frames(found)
    assert frames.can_id.tolist() == [0x20, 0x30] and frames.eof_ns.tolist() == [600, 800]
    assert frames.write_ns.tolist() == [700, 900] and frames.dlc.tolist() == [2, 8]
    assert frames.of(0x30).data.tolist() == [[1, 2, 3, 4, 5, 6, 7, 8]]
    assert len(frames.of(0x99)) == 0


def test_the_sensor_reads_are_its_samples_in_order():
    numbers, times = trace.sensor_reads(make({"kind": SENSOR, "time_ns": 50, "value": 0},
                                             {"kind": SENSOR, "time_ns": 60, "value": 1}))
    assert numbers.tolist() == [0, 1] and times.tolist() == [50, 60]


def test_the_window_is_the_first_to_the_last_switch():
    assert trace.window(intervals_trace()) == (0, 600)
    with pytest.raises(trace.TraceError):
        trace.window(make(switch(0, 1)))
