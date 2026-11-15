"""Write measurement results with enough provenance to trust and to reproduce them.

Every number quoted in the README comes from a file written here: results/<name>.json holds
the data together with the command that produced it, the commit it was produced from and the
machine it ran on.
"""
from __future__ import annotations

import json
import os
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from . import paths


def _git(*args: str) -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(paths.REPO_ROOT), *args],
            capture_output=True, text=True, timeout=60, check=True,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def provenance() -> dict:
    """Who produced a result: command, commit, uncommitted source changes, host."""
    status = _git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules")
    modified = [
        line[3:] for line in status.splitlines()
        if line[3:] and not line[3:].startswith("results/")
    ]
    # The script's path relative to the repository it lives in, found by walking up from the
    # script itself (the package may be imported through a different mount of the same tree).
    script = Path(sys.argv[0]).resolve()
    script_name = script.name
    for parent in script.parents:
        if (parent / "CMakePresets.json").exists():
            script_name = script.relative_to(parent).as_posix()
            break
    release = platform.release()
    return {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "command": " ".join(["python", script_name, *sys.argv[1:]]),
        "git_commit": _git("rev-parse", "HEAD") or "unknown",
        "git_modified_files": modified,
        "host": {
            "kernel": release,
            "is_wsl": "microsoft" in release.lower(),
            "machine": platform.machine(),
            "cpu": _cpu_model(),
            "logical_cpus": os.cpu_count(),
            "python": platform.python_version(),
        },
    }


def percentiles(samples, unit_divisor: float = 1.0) -> dict:
    """Summary of a latency-like sample: count, mean, min, percentiles and max.

    A percentile p is only reported when the sample holds at least five values beyond it,
    n * (1 - p) >= 5, so p99 needs 500 samples and p99.9 needs 5,000. Below that the entry is
    None: with fewer samples the "percentile" is just the largest value seen, and a number
    the data cannot support is worse than no number. The count is always reported.
    """
    values = np.asarray(samples, dtype=np.float64) / unit_divisor
    n = int(values.size)
    if n == 0:
        return {"n": 0}

    def pct(q: float):
        if n * (1.0 - q / 100.0) < 5.0:
            return None
        return round(float(np.percentile(values, q)), 3)

    return {
        "n": n,
        "mean": round(float(values.mean()), 3),
        "min": round(float(values.min()), 3),
        "p50": pct(50),
        "p90": pct(90),
        "p99": pct(99),
        "p99_9": pct(99.9),
        "max": round(float(values.max()), 3),
    }


def write_result(name: str, data: dict) -> Path:
    """Write results/<name>.json and return its path."""
    out_dir = paths.results_dir()
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{name}.json"
    payload = {"schema": f"fcn.result/{name}/1", "provenance": provenance(), **data}
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")
    return path
