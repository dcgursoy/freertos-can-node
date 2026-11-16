"""The summary statistics every result file uses (tools/fcn_tools/results.py)."""
from __future__ import annotations

import pytest

from fcn_tools.results import percentiles


@pytest.mark.parametrize("key, smallest_n", [
    ("p50", 10), ("p90", 50), ("p99", 500), ("p99_9", 5000),
])
def test_a_percentile_is_reported_from_exactly_the_documented_sample_size(key, smallest_n):
    # n * (1 - p) >= 5: at the boundary it is reported, one sample fewer it is not.
    assert percentiles(range(smallest_n))[key] is not None
    assert percentiles(range(smallest_n - 1))[key] is None


def test_the_summary_of_a_known_sample():
    summary = percentiles([4.0, 1.0, 3.0, 2.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0])
    assert summary["n"] == 10
    assert summary["min"] == 1.0 and summary["max"] == 10.0
    assert summary["mean"] == 5.5
    assert summary["p50"] == 5.5
    assert summary["p90"] is None and summary["p99"] is None


def test_values_are_divided_by_the_unit_divisor():
    summary = percentiles([1000.0] * 10, unit_divisor=1000.0)
    assert summary["p50"] == 1.0 and summary["mean"] == 1.0


def test_an_empty_sample_has_only_its_count():
    assert percentiles([]) == {"n": 0}
