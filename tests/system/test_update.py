# SPDX-License-Identifier: MIT
"""The life of an update, without power cuts (those are in test_update_cuts.py): an image staged
with the flash tool is started as a trial from slot B, confirms itself after five seconds in good
health and stays the active image; one that fails is started exactly three times and rolled
back; a slot with one bit flipped by the bench is never started; and no update can begin while a
trial is pending (docs/update.md; decisions 5, 6 and 8 of phase 4).

The functional tests decide from what the node itself reports (its statistics, its frames, its
log, the flash afterwards), never from the host's clock; the one test of the five-second window
on the wall clock is marked `timing`. tests/system/update_bench.py is the bench.
"""
from __future__ import annotations

import re
import subprocess
import time

import pytest

from fcn_tools import paths
from fcn_tools.node import (BOOT_NOTHING_BOOTABLE, EXIT_OK, RESET_IMAGE_FAULT, RESET_WATCHDOG)
from fcn_tools.timing import check_timing

from update_bench import (BENCH_SEED, MAX_ATTEMPTS, ROLLBACK_BIT, RUN_WAIT_S, Boot,
                          check_frames_of, check_handoff, check_invariants, decoded,
                          expected_boot, factory, flip_bit, info, listen, stage, states,
                          until_confirmed)

#: UPDATE_CONFIRM_AFTER_S (app/update.h) in health periods of 100 ms.
WINDOW_PERIODS = 50


def confirmed_update(start_node, workdir, ports, images):
    """Factory image 1.0.0 in slot A, an update to 1.1.0 staged into slot B and started; returns
    the node once its trial has confirmed itself and stopped, and the frames it sent."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    staged = stage(flash, images["node-1.1.0"])
    assert (staged["slot"], staged["address"]) == ("B", 0x102000)
    before = info(flash)
    assert states(before) == ("confirmed", "trial")
    assert before["metadata"]["attempts_used"] == 0
    assert expected_boot(before) == Boot("B", "trial", 1, "NONE")
    return listen(f"udp:{ports.bus}", lambda: start_node(flash_file=flash, retained_file=cell),
                  until_confirmed)


def test_a_trial_confirms_itself_and_stays_the_active_image(start_node, restart_node, workdir,
                                                             ports, images):
    node, frames = confirmed_update(start_node, workdir, ports, images)
    flash = node.flash_file
    assert "select: slot B as a trial, attempt 1 of 3, image 1.1.0" in node.boot_log()
    assert "confirmed: slot B, image 1.1.0, is the active image" in node.log()
    stats = node.stats()
    check_handoff(stats, Boot("B", "trial", 1, "NONE"))
    confirm = stats["confirm"]
    assert (confirm["state"], confirm["status"]) == ("CONFIRMED", "ok")
    assert confirm["flash_task"]["confirms"] == 1
    assert confirm["flash_task"]["confirm_status"] == "ok"
    assert confirm["flash_task"]["trial_slot"] == "B"
    # The window, from the node's own counts: 50 health periods in good health in a row, the
    # last of them the period that asked for the confirm, after every period that cut an
    # earlier window short. (How long that took on the wall clock is the timing test below.)
    assert confirm["periods_needed"] == WINDOW_PERIODS
    assert confirm["healthy_periods"] == WINDOW_PERIODS
    assert confirm["requested_period"] >= WINDOW_PERIODS + confirm["window_restarts"]
    # The confirm is one record: a program of the record and one of its commit field.
    assert (stats["flash"]["programs"], stats["flash"]["erases"]) == (2, 0)

    seen = [(frame.signals["ConfirmState"], frame.raw["ConfirmSeconds"])
            for frame in decoded(frames, "BMS_BootInfo")]
    # Once a second: waiting with the healthy seconds so far, then (after one frame that may
    # still say CONFIRMING) confirmed with all 5 of them.
    first = next(i for i, (state, _) in enumerate(seen) if state != "WAITING")
    assert all(state == "WAITING" and seconds < 5 for state, seconds in seen[:first]), seen
    assert seen[first][0] in ("CONFIRMING", "CONFIRMED") and seen[first][1] == 5, seen
    assert all(entry == ("CONFIRMED", 5) for entry in seen[first + 1:]), seen
    assert seen[-1] == ("CONFIRMED", 5), seen
    check_frames_of(Boot("B", "trial", 1, "NONE"), frames, "1.1.0", reset_cause="POWER_ON")

    after = info(flash)
    check_invariants(after)
    assert states(after) == ("previous", "confirmed")
    assert (after["metadata"]["event"], after["metadata"]["seq"]) == ("confirm", 4)
    assert expected_boot(after) == Boot("B", "confirmed", 0, "NONE")

    # It stays the active image: the next start is of slot B as the confirmed image.
    again, frames = listen(f"udp:{ports.bus}",
                           lambda: restart_node(node, args=("--duration-s", "2")))
    assert "select: slot B, the confirmed image, image 1.1.0" in again.boot_log()
    check_handoff(again.stats(), Boot("B", "confirmed", 0, "NONE"))
    assert again.stats()["confirm"]["state"] == "NOT_NEEDED"
    check_frames_of(Boot("B", "confirmed", 0, "NONE"), frames, "1.1.0")
    assert info(flash)["metadata"]["seq"] == 4        # a start of the confirmed image writes nothing


#: A line of the node's log: its CLOCK_MONOTONIC stamp in seconds, and the text.
LOG_LINE = re.compile(r"\[\s*(\d+\.\d+)\] . (.*)")


def stamp_of(log: str, text: str) -> float:
    for line in log.splitlines():
        match = LOG_LINE.match(line)
        if match and text in match.group(2):
            return float(match.group(1))
    raise AssertionError(f"no log line with {text!r}:\n{log}")


#: How much later than 5 s after the start the confirm may be asked for on a busy host: two
#: health periods. A timing tolerance.
WINDOW_LATE_S = 0.2


@pytest.mark.timing
def test_the_window_of_good_health_is_five_seconds_on_the_wall_clock(start_node, workdir, ports,
                                                                    images):
    """The trial asks for its confirm 5 s after the node's start (its first log line), by the
    node's own CLOCK_MONOTONIC stamps: never earlier (the 50 health periods of the window follow
    the tick, which follows CLOCK_MONOTONIC and never runs ahead of it), and later only if the
    host held the node up, which is a timing tolerance, as is a window cut short by a stall."""
    node, _ = confirmed_update(start_node, workdir, ports, images)
    log = node.log()
    started = stamp_of(log, "fcn_node ")
    asked = stamp_of(log, "ran 5 s in good health: writing its confirm")
    window = asked - started
    assert window >= 4.9, f"the confirm was asked for {window:.3f} s after the start"
    confirm = node.stats()["confirm"]
    check_timing(confirm["window_restarts"] == 0 and confirm["requested_period"] == WINDOW_PERIODS,
                 f"the window of good health was cut short {confirm['window_restarts']} times "
                 f"(the confirm came in period {confirm['requested_period']}, not 50)")
    check_timing(window <= 5.0 + WINDOW_LATE_S,
                 f"the confirm was asked for {window:.3f} s after the start, not 5 s")


@pytest.mark.parametrize("payload, status", [("crash", RESET_IMAGE_FAULT),
                                             ("watchdog", RESET_WATCHDOG)])
def test_a_trial_that_fails_is_started_three_times_and_rolled_back_with_its_dtc(
        start_node, restart_node, workdir, ports, images, payload, status):
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images[payload])

    run = start_node(flash_file=flash, retained_file=cell)
    for attempt in range(1, MAX_ATTEMPTS + 1):
        assert run.wait_for_reset(timeout=RUN_WAIT_S) == status
        assert f"select: slot B as a trial, attempt {attempt} of 3" in run.boot_log()
        assert f"payload {payload}: hand-off slot B trial attempt {attempt}" in run.log()
        assert info(flash)["metadata"]["attempts_used"] == attempt
        if attempt < MAX_ATTEMPTS:
            run = restart_node(run, name=f"after-{attempt}")
    assert expected_boot(info(flash)) == Boot("A", "confirmed", 0, "ROLLBACK")

    # The fourth start, with the bus open from before it: its first BMS_Fault is on the bus.
    run, frames = listen(f"udp:{ports.bus}",
                         lambda: restart_node(run, name="rolled-back", args=("--duration-s", "2")))
    assert "rolled back: the trial in slot B used its 3 attempts" in run.boot_log()
    assert "the bootloader rolled back before this start (ROLLBACK of slot B)" in run.log()
    stats = run.stats()
    check_handoff(stats, Boot("A", "confirmed", 0, "ROLLBACK"))
    assert stats["handoff"]["event_slot"] == "B"
    dtc = next(d for d in stats["faults"]["dtcs"] if d["name"] == "UPDATE_ROLLBACK")
    assert (dtc["code"], dtc["status"] & 0x09) == ("P1B06-00", 0x09)  # testFailed, confirmedDTC
    assert stats["faults"]["most_recent"] == "UPDATE_ROLLBACK"
    # The reset cause of this start is the last failed attempt's.
    assert stats["reset"]["cause_code"] == status

    faults = decoded(frames, "BMS_Fault")
    assert faults, "no BMS_Fault on the bus"
    assert faults[0].raw["FaultActive"] & ROLLBACK_BIT and faults[0].raw["FaultConfirmed"] & ROLLBACK_BIT
    assert faults[0].signals["FaultMostRecent"] == "UPDATE_ROLLBACK"
    check_frames_of(Boot("A", "confirmed", 0, "ROLLBACK"), frames, "1.0.0",
                    reset_cause="IMAGE_FAULT" if status == RESET_IMAGE_FAULT else "WATCHDOG")
    assert decoded(frames, "BMS_BootInfo")[0].signals["BootEventSlot"] == "B"
    rolled = info(flash)
    check_invariants(rolled)
    assert states(rolled) == ("confirmed", "rejected")

    # The hand-off speaks to that start only: the next one has no rollback to report.
    last = restart_node(run, name="later", args=("--duration-s", "1"))
    assert last.wait(timeout=RUN_WAIT_S) == EXIT_OK
    assert last.stats()["handoff"]["event"] == "NONE"
    assert last.stats()["faults"]["active"] & ROLLBACK_BIT == 0


def flash_tool_run(*args) -> subprocess.CompletedProcess:
    return subprocess.run([str(paths.flash_tool_binary()), *map(str, args)], capture_output=True,
                          text=True, timeout=60, check=False)


def test_no_update_can_begin_while_a_trial_is_pending(start_node, workdir, ports, images):
    """While the trial runs, the node holds the flash file and the tool cannot open it; once it
    has stopped before its confirm, lib/boot refuses to begin an update of either slot (the
    confirmed one is the trial's fallback), and nothing of the flash changes."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])

    def stage_meanwhile(node, reader):
        deadline = time.monotonic() + RUN_WAIT_S
        while not decoded(reader.frames, "BMS_BootInfo"):      # the node has opened the flash
            assert node.running and time.monotonic() < deadline, node.stderr()
            time.sleep(0.01)
        busy = flash_tool_run("stage", "--flash-file", flash, "--no-wait", images["node-1.4.0"])
        assert busy.returncode == 1 and "in use by another process" in busy.stderr, busy
        assert node.wait(timeout=RUN_WAIT_S) == EXIT_OK

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash,
                                             retained_file=cell),
                          stage_meanwhile)
    check_frames_of(Boot("B", "trial", 1, "NONE"), frames, "1.1.0")
    assert all(frame.signals["ConfirmState"] == "WAITING"
               for frame in decoded(frames, "BMS_BootInfo"))
    pending = info(flash)
    assert (pending["metadata"]["trial_slot"], pending["metadata"]["attempts_used"]) == ("B", 1)
    before = flash.read_bytes()

    for options in ((), ("--address", "0x102000"), ("--address", "0x2000")):
        refused = stage(flash, images["node-1.4.0"], *options, expect=2)
        assert refused["status"] == "refused" and refused["detail"].startswith("state at address"), \
            refused
        assert flash.read_bytes() == before, f"stage {options} changed the flash"


@pytest.mark.parametrize("where", ["header", "payload"])
def test_a_trial_with_one_bit_flipped_is_rejected_and_never_started(start_node, workdir, ports,
                                                                    images, where):
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    at, bit = flip_bit(flash, "B", where, BENCH_SEED)
    damaged = info(flash)
    verdict = damaged["slots"][1]["validation"]
    assert verdict != "ok", f"bit {bit:#04x} at {at:#x} flipped and slot B still validates"
    assert expected_boot(damaged) == Boot("A", "confirmed", 0, "REJECT")

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash,
                                             retained_file=cell))
    assert "select: rolled back: the trial in slot B does not validate" in node.boot_log()
    assert "as a trial" not in node.boot_log()
    assert "the bootloader rolled back before this start (REJECT of slot B)" in node.log()
    check_handoff(node.stats(), Boot("A", "confirmed", 0, "REJECT"))
    check_frames_of(Boot("A", "confirmed", 0, "REJECT"), frames, "1.0.0")
    faults = decoded(frames, "BMS_Fault")
    assert faults and faults[0].raw["FaultActive"] & ROLLBACK_BIT
    after = info(flash)
    assert states(after) == ("confirmed", "rejected")
    assert after["metadata"]["event"] == "reject"


def test_a_confirmed_image_with_one_bit_flipped_falls_back_to_the_one_before(
        start_node, restart_node, workdir, ports, images):
    node, _ = confirmed_update(start_node, workdir, ports, images)
    flash = node.flash_file
    assert states(info(flash)) == ("previous", "confirmed")
    flip_bit(flash, "B", "payload", BENCH_SEED)
    damaged = info(flash)
    assert damaged["slots"][1]["validation"] == "bad_digest"
    assert expected_boot(damaged) == Boot("A", "confirmed", 0, "FALLBACK")

    again, frames = listen(f"udp:{ports.bus}",
                           lambda: restart_node(node, args=("--duration-s", "2")))
    assert ("select: fell back: the confirmed image in slot B does not validate; the one before "
            "it, in slot A, is confirmed again") in again.boot_log()
    assert "the bootloader rolled back before this start (FALLBACK of slot A)" in again.log()
    check_handoff(again.stats(), Boot("A", "confirmed", 0, "FALLBACK"))
    check_frames_of(Boot("A", "confirmed", 0, "FALLBACK"), frames, "1.0.0")
    faults = decoded(frames, "BMS_Fault")
    assert faults and faults[0].raw["FaultActive"] & ROLLBACK_BIT
    after = info(flash)
    check_invariants(after)
    assert states(after) == ("confirmed", "rejected")
    # The next update goes to the slot that is not confirmed, the damaged one.
    assert stage(flash, images["node-1.4.0"])["slot"] == "B"
    assert states(info(flash)) == ("confirmed", "trial")


@pytest.mark.parametrize("where", ["header", "payload"])
def test_a_confirmed_image_with_one_bit_flipped_and_nothing_before_it_is_not_started(
        start_node, workdir, images, where):
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    flip_bit(flash, "A", where, BENCH_SEED)
    verdict = info(flash)["slots"][0]["validation"]
    assert verdict != "ok"
    node = start_node(flash_file=flash, retained_file=workdir / "cell.bin")
    assert node.wait(timeout=RUN_WAIT_S) == BOOT_NOTHING_BOOTABLE
    assert f"nothing bootable: slot A {verdict}, slot B bad_magic" in node.boot_log()
    assert not node.log_path.exists()       # no image ran
