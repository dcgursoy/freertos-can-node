# SPDX-License-Identifier: MIT
"""The node started through its bootloader: fcn_boot selects the image in the flash, writes its
hand-off into the retained cell and starts the image in its own process; the node reports what
it was handed (docs/update.md, the bootloader and the hand-off).

These tests are about the start: a factory flash, a flash with nothing to start, a start
without a retained cell, and the reset cause, which the bootloader leaves in the cell for the
image. An update's life (a trial, its confirm, its rollback, a damaged slot) is in
test_update.py, power cuts in test_update_cuts.py; tests/system/update_bench.py is the bench they
share.
"""
from __future__ import annotations

import time

from fcn_tools.node import BOOT_NOTHING_BOOTABLE, EXIT_OK, RESET_WATCHDOG

from update_bench import (RUN_WAIT_S, Boot, check_frames_of, check_handoff, decoded, factory,
                          info, listen, states)


def test_a_factory_flash_starts_slot_a_as_the_confirmed_image(start_node, workdir, ports, images):
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    made = factory(flash, images["node-1.0.0"])
    assert (made["slot"], made["image_version"], made["event"]) == ("A", "1.0.0", "adopt")
    assert states(info(flash)) == ("confirmed", "empty")

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash,
                                             retained_file=cell))

    assert "select: slot A, the confirmed image, image 1.0.0" in node.boot_log()
    assert "jump: slot A at 0x002000" in node.boot_log()
    assert "started by the bootloader: slot A, the confirmed image, image 1.0.0" in node.log()
    stats = node.stats()
    assert stats["handoff"] == {
        "verdict": "taken", "present": True, "slot": "A", "mode": "confirmed", "attempt": 0,
        "image_version": "1.0.0", "image_version_code": 0x01000000, "event": "NONE",
        "event_slot": None, "select_write_error": 0, "rolled_back": False}
    assert stats["confirm"]["state"] == "NOT_NEEDED"
    assert stats["confirm"]["flash_task"]["confirms"] == 0
    assert stats["confirm"]["flash_task"]["active_slot"] == "A"
    # A start of the confirmed image writes nothing: the bootloader only read the flash, and so
    # did the node.
    assert stats["flash"]["programs"] == 0 and stats["flash"]["erases"] == 0
    assert stats["faults"]["active"] == 0

    check_frames_of(Boot("A", "confirmed", 0, "NONE"), frames, "1.0.0", reset_cause="POWER_ON")
    boots = decoded(frames, "BMS_BootInfo")
    assert len(boots) == 2       # in health periods 2 and 12 of a 2 s run (0.2 s and 1.2 s)
    assert all(frame.signals["ConfirmState"] == "NOT_NEEDED" for frame in boots)
    after = info(flash)
    assert states(after) == ("confirmed", "empty") and after["metadata"]["seq"] == 1


def test_a_flash_with_nothing_bootable_ends_the_bootloader_with_its_own_status(start_node,
                                                                              workdir):
    flash = workdir / "flash.bin"          # missing: fcn_boot creates it erased
    node = start_node(flash_file=flash, retained_file=workdir / "cell.bin")
    assert node.wait(timeout=RUN_WAIT_S) == BOOT_NOTHING_BOOTABLE
    assert ("nothing bootable: slot A bad_magic, slot B bad_magic, and the metadata allow neither"
            in node.boot_log())
    assert "fcn_boot: nothing bootable" in node.stderr()
    assert not node.log_path.exists()       # no image ran


def test_without_a_retained_cell_the_image_gets_no_hand_off(start_node, workdir, ports, images):
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash))
    assert "no retained cell: the image gets no hand-off" in node.boot_log()
    assert "started without the bootloader (no hand-off)" in node.log()
    assert node.stats()["handoff"]["present"] is False
    # As a node started without the bootloader: ActiveSlot SNA, BootMode NONE.
    infos, boots = decoded(frames, "BMS_NodeInfo"), decoded(frames, "BMS_BootInfo")
    assert infos and all(frame.signals["ActiveSlot"] == "SNA" for frame in infos)
    assert boots and all(frame.signals["BootMode"] == "NONE" for frame in boots)


#: How long a test waits for the watchdog's reset after the hang (the node's bound is 0.8 s,
#: app/app.c, SUPERVISION). Not a timing tolerance: it only bounds the wait.
RESET_WAIT_S = 10.0


def test_the_reset_cause_reaches_the_image_through_the_bootloader(start_node, restart_node,
                                                                  workdir, ports, images):
    """The bootloader reads the retained cell first and leaves the reset cause in it for the
    image: the node, reset by its watchdog (sensor.hang) and started again through fcn_boot,
    reports WATCHDOG as the cause in BMS_NodeInfo, in its statistics and as the DTC
    RESET_WATCHDOG, next to the hand-off that the bootloader wrote into the same cell."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])

    def hang(node, reader) -> None:
        # Once BMS_NodeInfo has gone out, simulation control is up as well.
        deadline = time.monotonic() + RESET_WAIT_S
        while not decoded(reader.frames, "BMS_NodeInfo"):
            assert node.running and time.monotonic() < deadline, node.stderr()
            time.sleep(0.01)
        with node.sim() as sim:
            sim.sensor_hang("on")
        assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG

    first, before = listen(f"udp:{ports.bus}",
                           lambda: start_node(flash_file=flash, retained_file=cell), hang)
    check_frames_of(Boot("A", "confirmed", 0, "NONE"), before, "1.0.0", reset_cause="POWER_ON")

    again, after = listen(f"udp:{ports.bus}",
                          lambda: restart_node(first, args=("--duration-s", "2")))
    assert ("reset cause WATCHDOG (retained cell: reset, 1 resets since power-on)"
            in again.boot_log())
    check_frames_of(Boot("A", "confirmed", 0, "NONE"), after, "1.0.0", reset_cause="WATCHDOG")
    stats = again.stats()
    check_handoff(stats, Boot("A", "confirmed", 0, "NONE"))
    assert (stats["reset"]["cause"], stats["reset"]["verdict"]) == ("WATCHDOG", "reset")
    faults = decoded(after, "BMS_Fault")
    assert faults and faults[0].signals["FaultMostRecent"] == "RESET_WATCHDOG"
    assert again.wait(timeout=1) == EXIT_OK
