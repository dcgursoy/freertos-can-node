"""The analysis of bench/node_timing.py that holds the node's tick against the grid of its trace,
on slices of real traces (traces/, read by trace_slices) and on constructed ones: a job released
more than a tick early, with the tick count two ahead, is counted and kept as the node made it;
a run whose tick count stays ahead of the grid for longer than LEAD_LIMIT_PERIODS, which a node
that gives its made-up ticks back cannot do, is refused; a condition adds up its runs' jobs
released early and pools the tick count's figures over its runs."""
from __future__ import annotations

import importlib.util
import sys

import numpy as np
import pytest

import trace_slices
from fcn_tools import paths, runner
from fcn_tools import trace as trace_module
from fcn_tools.trace import EVENT_DTYPE, READY, TICK_BASE, TickLead, Trace


def _load(name: str):
    path = paths.REPO_ROOT / "bench" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"bench_{name}", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


bench = _load("node_timing")

MS = 1_000_000

#: The periods of the node's task table (app/task_table.c) that the analysis reads.
STATS = {"task_table": {"tasks": [{"name": "sensor", "period_us": 10_000},
                                  {"name": "health", "period_us": 100_000}]}}


def run_of(found: Trace):
    return bench.Run(condition="flood", transport="socketcan", name="flood", stats=STATS,
                     trace=found, frames=np.zeros(0), sidecar={}, extra={})


def ahead_for(periods: int, every: int = 1) -> Trace:
    """Base 10 ms, period 1 ms (tick k due at 9 + k ms); counts 1, 1 + `every`, ... up to
    `periods` each first seen exactly a period before its tick was due (the others never seen,
    as in an idle run): one ahead, a stretch from count 1's sight (9 ms) to the due time of the
    last (9 + `periods` ms)."""
    counts = np.arange(1, periods + 1, every)
    assert counts[-1] == periods
    events = np.zeros(counts.size + 1, dtype=EVENT_DTYPE)
    events["kind"] = [TICK_BASE, *[READY] * counts.size]
    events["time_ns"] = [10 * MS, *((8 + counts) * MS)]
    events["value"] = [1, *counts]
    events["arg"][0] = MS
    events["task"][1:] = 4
    return Trace(events=events, names={4: "sensor"})


# ------------------------------------------------------------------------------ the releases


def test_a_job_released_with_the_count_two_ahead_is_counted_and_kept():
    # traces/two-ahead.txt: the sensor job that waited for tick 3202 (xTaskDelayUntil) was
    # released at count 3202, 3199.480 ms after the base, and tick 3202 was due at 3201 ms:
    # 1519.6 us early, more than the period (1 ms) and EARLY_MARGIN_NS (0.1 ms). It is a release
    # the node made: counted, and kept in the lateness figures.
    found = trace_slices.load("two-ahead.txt")
    grid = trace_module.tick_grid(found)
    entry, jobs, arrival = bench.periodic_release(run_of(found), "sensor", grid,
                                                  trace_module.running_intervals(found), None)
    assert jobs.wake_tick.tolist() == [3202] and jobs.release_tick.tolist() == [3202]
    assert (arrival - grid.bases_ns[0]).tolist() == [3201 * MS]
    assert entry["jobs"] == 1 and entry["early"] == 1 and entry["beyond_a_tick"] == 1
    assert entry["release_lateness"].tolist() == [-1_519_634]
    after = int(entry["start_after_release"][0])
    assert 0 < after and entry["start_lateness"].tolist() == [-1_519_634 + after]
    # Out of the window, it is not counted.
    entry, _, _ = bench.periodic_release(run_of(found), "sensor", grid,
                                         trace_module.running_intervals(found), (0, 1))
    assert (entry["jobs"], entry["early"], entry["beyond_a_tick"]) == (0, 0, 0)


def test_more_than_a_tick_early_is_more_than_the_period_and_the_margin():
    # traces/two-ahead.txt with its grid moved earlier: by 470 us, the job is released 1049.6 us
    # before its tick was due, more than the period (1 ms) but within EARLY_MARGIN_NS (0.1 ms)
    # of it, which is room for the clock reads: early, not more than a tick early. By 380 us,
    # 1139.6 us before: more than a tick early.
    found = trace_slices.load("two-ahead.txt")
    for moved, beyond in ((470_000, 0), (380_000, 1)):
        events = found.events.copy()
        events["time_ns"][events["kind"] == TICK_BASE] -= moved
        shifted = Trace(events=events, names=found.names)
        entry, _, _ = bench.periodic_release(run_of(shifted), "sensor",
                                             trace_module.tick_grid(shifted),
                                             trace_module.running_intervals(shifted), None)
        assert entry["release_lateness"].tolist() == [-1_519_634 + moved]
        assert (entry["early"], entry["beyond_a_tick"]) == (1, beyond)


def test_a_job_released_less_than_a_tick_early_is_early_but_not_beyond():
    # traces/old-give-back.txt (one ahead throughout): every sensor job of the 80 ms was released
    # before its tick was due, by less than a period.
    found = trace_slices.load("old-give-back.txt")
    entry, _, _ = bench.periodic_release(run_of(found), "sensor", trace_module.tick_grid(found),
                                         trace_module.running_intervals(found), None)
    assert entry["jobs"] > 0 and entry["early"] == entry["jobs"]
    assert entry["beyond_a_tick"] == 0
    assert all(-MS < late < 0 for late in entry["release_lateness"].tolist())


# --------------------------------------------------------------------- the count against the grid


def test_a_run_with_the_count_two_ahead_for_a_moment_is_not_refused():
    # Counts 3201 and 3202 first seen before their ticks were due, 1.63 ms in a row.
    found = trace_slices.load("two-ahead.txt")
    lead = bench.tick_lead_of(run_of(found), trace_module.tick_grid(found))
    assert lead.seen_beyond == 1 and lead.longest_counts == 2 and lead.longest_ns == 1_629_171


def test_a_run_whose_count_stays_ahead_as_with_the_old_give_back_is_refused():
    # traces/old-give-back.txt: 80 counts in a row seen ahead, 79.8 ms from 560.2 ms after the
    # first base, more than LEAD_LIMIT_PERIODS (50 ms at 1 ms a tick).
    found = trace_slices.load("old-give-back.txt")
    with pytest.raises(runner.BenchFailure,
                       match=r"flood: the tick count ran ahead of the trace's grid for 79\.8 ms "
                             r"\(80 counts in a row seen before their tick was due, from 560\.2 "
                             r"ms after the first tick\), more than 50 periods.*bug 011.*"
                             r"TICK_BASE"):
        bench.tick_lead_of(run_of(found), trace_module.tick_grid(found))


def test_the_limit_is_fifty_periods_ahead_in_a_row():
    assert bench.LEAD_LIMIT_PERIODS == 50
    at_limit = ahead_for(50)
    lead = bench.tick_lead_of(run_of(at_limit), trace_module.tick_grid(at_limit))
    assert (lead.longest_ns, lead.longest_counts, lead.seen_beyond) == (50 * MS, 50, 0)
    over = ahead_for(51)
    with pytest.raises(runner.BenchFailure, match=r"for 51\.0 ms \(51 counts"):
        bench.tick_lead_of(run_of(over), trace_module.tick_grid(over))
    # The limit is on the stretch's time, not on the counts the trace saw in it: one count in
    # five seen, 51 ms in 11 counts, is refused; 46 ms in 10 is not.
    sparse = ahead_for(46, every=5)
    lead = bench.tick_lead_of(run_of(sparse), trace_module.tick_grid(sparse))
    assert (lead.longest_ns, lead.longest_counts) == (46 * MS, 10)
    sparse = ahead_for(51, every=5)
    with pytest.raises(runner.BenchFailure, match=r"for 51\.0 ms \(11 counts"):
        bench.tick_lead_of(run_of(sparse), trace_module.tick_grid(sparse))


def test_a_condition_adds_up_its_runs_jobs_released_early_and_more_than_a_tick_early():
    # Two runs: the two-ahead slice (its one sensor job early, and more than a tick early) and
    # the old give-back's (every job early, none by more than a tick).
    parts = []
    for name in ("two-ahead.txt", "old-give-back.txt"):
        found = trace_slices.load(name)
        entry, _, _ = bench.periodic_release(run_of(found), "sensor",
                                             trace_module.tick_grid(found),
                                             trace_module.running_intervals(found), None)
        parts.append(entry)
    assert parts[1]["early"] > 1 and parts[1]["beyond_a_tick"] == 0
    assert bench.release_counts(parts) == {
        "released_at_a_later_tick": parts[0]["late_ticks"] + parts[1]["late_ticks"],
        "released_before_due": 1 + parts[1]["early"], "released_more_than_a_tick_early": 1}


def test_a_condition_adds_up_its_runs_counts_and_keeps_the_longest_stretch():
    data = bench.tick_lead_data([
        TickLead(counts_seen=100, seen_before_due=3, seen_beyond=1, most_before_due_ns=1_519_634,
                 longest_ns=1_629_171, longest_from_ns=5, longest_counts=2),
        TickLead(counts_seen=200, seen_before_due=4, seen_beyond=0, most_before_due_ns=900_000,
                 longest_ns=2_250_000, longest_from_ns=7, longest_counts=3)])
    assert (data["counts_seen"], data["seen_before_due"],
            data["seen_more_than_a_tick_before_due"]) == (300, 7, 1)
    assert data["most_before_due_us"] == 1519.634
    assert data["longest_stretch_ahead"] == {"value": 2.25, "unit": "ms"}
    assert data["stretch"] == bench.STRETCH_MEASURED
