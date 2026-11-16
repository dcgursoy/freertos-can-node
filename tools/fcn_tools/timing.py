"""Assertions about host timing, kept apart from all other assertions.

A test that checks how well the host kept time (a tick period, a frame period on the wall
clock) can fail because the machine was busy, without anything being wrong with the code.
Such a test is allowed one repetition (scripts/test-system.sh), but only when every failure of
the run was of that kind: a crash, a wrong exit status or a wrong value fails at once. To make
the distinction, tolerance checks raise TimingToleranceMissed and nothing else does.
"""
from __future__ import annotations


class TimingToleranceMissed(AssertionError):
    """A measured time was outside its tolerance; nothing else went wrong."""


def check_timing(ok: bool, message: str) -> None:
    """Assert a timing tolerance. Use plain assert for everything that is not one."""
    if not ok:
        raise TimingToleranceMissed(message)
