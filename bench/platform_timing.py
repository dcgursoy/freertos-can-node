#!/usr/bin/env python3
"""Measure the two host mechanisms that all of the node's timing rests on.

Everything measured here is the timing of a Linux host running the FreeRTOS POSIX port, not of
a microcontroller: the platform layer removes the port's systematic errors, the host's
scheduler still decides when a thread runs.

1. Tick accuracy. The FreeRTOS POSIX port generates its tick with a relative sleep and so
   runs slow against the wall clock; platform/posix/posix_tick.c locks the tick count to
   CLOCK_MONOTONIC. Both are measured with a 10 ms periodic task.
2. Simulated interrupt latency. Time from a peripheral model raising an interrupt line to the
   task it wakes, with signal delivery and with tick-hook polling
   (platform/posix/posix_irq.c), while a mid-priority task loads the scheduler.
3. Interrupt storm. The same line raised in a tight loop for a few seconds: every raise is
   served and interrupt handlers never stack up on a task's stack.

Runs the release build of tests/platform/plat_selftest and writes
results/platform_timing.json and results/platform_timing.png.

    python bench/platform_timing.py                 # 10 s per run, about a minute in total
    python bench/platform_timing.py --seconds 60    # longer runs, about 4.5 minutes
    python bench/platform_timing.py --replot        # redraw the figure from the last run
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from fcn_tools import paths, plotstyle, results

IRQ_PERIOD_US = 250  # about the frame rate of a saturated 500 kbit/s CAN bus
STORM_SECONDS_MAX = 5


def run_selftest(binary: Path, args: list[str], timeout: float) -> tuple[dict, np.ndarray]:
    """Run one self-test; return its JSON report and its raw samples in nanoseconds."""
    with tempfile.TemporaryDirectory() as tmp:
        dump = Path(tmp) / "samples.txt"
        env = dict(os.environ, FCN_SELFTEST_DUMP=str(dump))
        proc = subprocess.run([str(binary), *args], capture_output=True, text=True,
                              timeout=timeout, env=env)
        if proc.returncode != 0:
            raise RuntimeError(
                f"plat_selftest {' '.join(args)} exited {proc.returncode}: {proc.stderr.strip()}")
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        samples = np.loadtxt(dump, dtype=np.float64, ndmin=1)
    return report, samples


def measure(binary: Path, seconds: int) -> tuple[dict, dict]:
    data: dict = {"seconds_per_run": seconds, "tick": {}, "interrupt": {}, "interrupt_storm": {}}
    raw: dict = {}
    timeout = seconds * 3 + 30

    for mode in ("stock", "locked"):
        print(f"tick, {mode} mode, {seconds} s ...", flush=True)
        report, samples = run_selftest(binary, ["tick", mode, str(seconds)], timeout)
        raw[f"tick_{mode}"] = samples
        data["tick"][mode] = {
            "nominal_tick_period_us": report["nominal_tick_period_us"],
            "effective_tick_period_us": report["effective_tick_period_us"],
            "error_percent": round(
                (report["effective_tick_period_us"] / report["nominal_tick_period_us"] - 1) * 100,
                3),
            "drift_over_run_us": report["drift_us"],
            "tick_signals_sent": report["signals_sent"],
            "catch_up_ticks": report["catch_ups"],
            "dropped_ticks": report["dropped_ticks"],
            "unaccounted_time_percent": report["unaccounted_time_percent"],
            "wake_interval_of_10ms_task_us": results.percentiles(samples, 1e3),
        }

    for mode in ("signal", "poll"):
        print(f"interrupt latency, {mode} mode, {seconds} s ...", flush=True)
        report, samples = run_selftest(
            binary, ["irq", mode, str(IRQ_PERIOD_US), str(seconds), "1"], timeout)
        raw[f"irq_{mode}"] = samples
        data["interrupt"][mode] = {
            "nominal_raise_period_us": report["nominal_period_us"],
            "mean_raise_period_us": report["mean_raise_period_us"],
            "raised": report["raised"],
            "task_wakeups": report["task_wakeups"],
            "unserved_at_end": report["unserved_at_end"],
            "serviced_by_signal_handler": report["serviced_by_signal"],
            "serviced_by_tick_hook": report["serviced_by_tick"],
            "max_handler_nesting": report["max_handler_nesting"],
            "latency_samples_not_recorded": report["latency_samples_not_recorded"],
            "latency_us": results.percentiles(samples, 1e3),
        }

    # An interrupt storm: the source raises in a tight loop. What matters here is not a
    # latency but that the node survives it: every raise served, handlers never stacked.
    storm_seconds = min(seconds, STORM_SECONDS_MAX)
    for mode in ("signal", "poll"):
        print(f"interrupt storm, {mode} mode, {storm_seconds} s ...", flush=True)
        report, _ = run_selftest(binary, ["irq", mode, "0", str(storm_seconds), "1"], timeout)
        data["interrupt_storm"][mode] = {
            "seconds": storm_seconds,
            "raised": report["raised"],
            "raises_per_second": round(report["raised"] / storm_seconds),
            "task_wakeups": report["task_wakeups"],
            "unserved_at_end": report["unserved_at_end"],
            "max_handler_nesting": report["max_handler_nesting"],
            "stack_peak_max_bytes": report["stack_peak_max_bytes"],
        }

    return data, raw


def plot(raw: dict, data: dict, path: Path) -> None:
    import matplotlib.pyplot as plt

    plotstyle.apply()
    fig, (ax_tick, ax_irq) = plt.subplots(1, 2, figsize=(10.0, 4.3))
    fig.subplots_adjust(left=0.07, right=0.98, top=0.76, bottom=0.18, wspace=0.22)
    series = {"locked": ("locked tick (this project)", plotstyle.SERIES[0]),
              "stock": ("stock POSIX port", plotstyle.SERIES[1])}

    # Left: distribution of the wake-up interval of a 10 ms task.
    all_ms = np.concatenate([raw["tick_locked"], raw["tick_stock"]]) / 1e6
    low, high = np.percentile(all_ms, [0.2, 99.8])
    bins = np.linspace(min(low, 9.0), max(high, 11.0), 90)
    for mode in ("stock", "locked"):
        label, colour = series[mode]
        tick = data["tick"][mode]
        ax_tick.hist(raw[f"tick_{mode}"] / 1e6, bins=bins, histtype="stepfilled", alpha=0.22,
                     color=colour, linewidth=0, weights=np.full(raw[f"tick_{mode}"].size,
                                                                100.0 / raw[f"tick_{mode}"].size))
        ax_tick.hist(raw[f"tick_{mode}"] / 1e6, bins=bins, histtype="step", color=colour,
                     linewidth=1.6, label=f"{label}: tick = {tick['effective_tick_period_us']:.1f} µs",
                     weights=np.full(raw[f"tick_{mode}"].size, 100.0 / raw[f"tick_{mode}"].size))
    plotstyle.reference_line(ax_tick, 10.0, "nominal 10 ms")
    ax_tick.set_title("Wake-up interval of a 10 ms periodic task")
    ax_tick.set_xlabel("interval between consecutive wake-ups (ms)")
    ax_tick.set_ylabel("share of wake-ups (%)")
    plotstyle.legend_above(ax_tick)

    # Right: cumulative distribution of interrupt-to-task latency.
    irq_series = {"signal": ("signal delivery", plotstyle.SERIES[0]),
                  "poll": ("tick-hook polling", plotstyle.SERIES[1])}
    for mode in ("signal", "poll"):
        label, colour = irq_series[mode]
        us = np.sort(raw[f"irq_{mode}"]) / 1e3
        share = np.arange(1, us.size + 1) / us.size * 100.0
        p50 = data["interrupt"][mode]["latency_us"]["p50"]
        ax_irq.plot(us, share, color=colour, label=f"{label}: median {p50:.0f} µs")
    ax_irq.set_xscale("log")
    ax_irq.set_xlim(10, 5000)
    ax_irq.set_ylim(0, 100)
    plotstyle.reference_line(ax_irq, 1000.0, "1 tick")
    ax_irq.set_title("Simulated interrupt: raise to task running")
    ax_irq.set_xlabel("latency (µs, log scale)")
    ax_irq.set_ylabel("interrupts served within (%)")
    plotstyle.legend_above(ax_irq)

    host = "WSL2" if data["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/platform_timing.py, {data['seconds_per_run']} s per run, "
                            f"FreeRTOS POSIX port on a {host} host. Host timing, not MCU timing.")
    plotstyle.save(fig, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seconds", type=int, default=10, help="duration of each of the 4 runs")
    parser.add_argument("--replot", action="store_true",
                        help="only redraw the plot from the samples of the last run")
    args = parser.parse_args()

    json_path = paths.results_dir() / "platform_timing.json"
    png_path = json_path.with_suffix(".png")
    # The raw samples are kept next to the build, not in the repository; --replot redraws the
    # figure from them without measuring again.
    raw_path = paths.build_root() / "bench" / "platform_timing_raw.npz"

    if args.replot:
        if not (json_path.exists() and raw_path.exists()):
            print("nothing to replot: run without --replot first", file=sys.stderr)
            return 1
        plot(dict(np.load(raw_path)), json.loads(json_path.read_text()), png_path)
        print(f"redrew {png_path}")
        return 0

    binary = paths.selftest_binary("release")
    if not binary.exists():
        print(f"{binary} not found: run scripts/build.sh release", file=sys.stderr)
        return 1

    data, raw = measure(binary, args.seconds)
    results.write_result("platform_timing", data)
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(raw_path, **raw)
    plot(raw, json.loads(json_path.read_text()), png_path)

    for mode in ("stock", "locked"):
        tick = data["tick"][mode]
        print(f"tick {mode:6s}: {tick['effective_tick_period_us']:9.3f} us per tick "
              f"({tick['error_percent']:+.3f} %)")
    for mode in ("signal", "poll"):
        irq = data["interrupt"][mode]
        lat = irq["latency_us"]
        print(f"irq  {mode:6s}: one per {irq['mean_raise_period_us']:.1f} us, p50 {lat['p50']} us, "
              f"p99 {lat['p99']} us, max {lat['max']} us, unserved {irq['unserved_at_end']}")
    for mode in ("signal", "poll"):
        storm = data["interrupt_storm"][mode]
        print(f"storm {mode:6s}: {storm['raises_per_second']} raises/s, "
              f"{storm['task_wakeups']} wake-ups, handler nesting {storm['max_handler_nesting']}, "
              f"stack peak {storm['stack_peak_max_bytes']} bytes, "
              f"unserved {storm['unserved_at_end']}")
    print(f"wrote {json_path} and {png_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
