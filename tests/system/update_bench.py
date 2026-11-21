# SPDX-License-Identifier: MIT
"""The bench of the update tests (test_boot.py, test_update.py, test_update_cuts.py): what a
test does to a flash file and how it reads one, kept in one place.

A test makes its flash with the flash tool (fcn_flash factory and stage, with --no-wait: the
tool's waits are not under test there), starts the node through the bootloader (NodeProcess
with a flash file, tests/conftest.py) and looks at it from outside: the bootloader's log, the
node's log and statistics, the frames on the bus (BMS_NodeInfo, BMS_BootInfo, BMS_Fault), and
the flash afterwards, read twice: by `fcn_flash info` (the C code: lib/boot's boot_describe(),
lib/image, lib/bootimg's id) and by fcn_tools.flash, a second implementation from the text of
docs/update.md. info() fails the test when the two disagree on any member, so every reading of
a flash in these tests is that cross-check as well.

expected_boot() is what the bootloader's select must decide for the metadata a test has just
read, written from the state machine in docs/update.md, and check_invariants() what every flash
must satisfy whatever happened to it, a power cut included (docs/update.md, "Decision 5's
invariants" in the power-cut campaign).

The images (the `images` fixture, tests/system/conftest.py) are packed from the release build of
the node, stripped, and from the test payloads of tests/tools: an image of the asan node does
not fit a slot.
"""
from __future__ import annotations

import json
import random
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import can
import pytest

from fcn_tools import bus as bus_module
from fcn_tools import flash as flash_decoder
from fcn_tools import paths
from fcn_tools.decode import DecodedFrame, Decoder
from fcn_tools.node import EXIT_OK, NodeProcess

#: UPDATE_ROLLBACK, fault 16 (app/fault.h), as a bit of BMS_Fault's masks.
ROLLBACK_BIT = 1 << 16

#: BOOT_MAX_ATTEMPTS (lib/boot/boot.h): the starts a trial gets before it is rolled back.
MAX_ATTEMPTS = 3

#: How long a test waits for a trial's confirm on the bus: the confirm comes after 5 s of good
#: health; the rest is room for a slow host. Not a timing tolerance: a node that does not
#: confirm in this time fails the test (the window itself is timed in test_update.py).
CONFIRM_WAIT_S = 30.0

#: How long a test waits for a run of the node that ends by itself (--duration-s, a reset). Not
#: a timing tolerance either: it only bounds the wait.
RUN_WAIT_S = 30.0

# The test's random choices (bits to flip, operations to cut, seeds) come from generators seeded
# with this, so that a run repeats exactly.
BENCH_SEED = 20261121


# ------------------------------------------------------------------------------ the flash tool


def flash_tool(*args: str | Path, expect: int = 0) -> dict:
    """Run fcn_flash; its JSON output. The exit status must be `expect`."""
    tool = paths.flash_tool_binary()
    if not tool.exists():
        pytest.fail(f"{tool} not found: build it with scripts/build.sh release", pytrace=False)
    run = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                         timeout=120, check=False)
    assert run.returncode == expect, f"fcn_flash {args}: {run.returncode}\n{run.stdout}{run.stderr}"
    return json.loads(run.stdout) if run.stdout.strip() else {}


def factory(flash: Path, image: Path) -> dict:
    """A new flash file with `image` in slot A, adopted as the CONFIRMED image."""
    return flash_tool("factory", "--flash-file", flash, "--no-wait", image)


def stage(flash: Path, image: Path, *options: str, expect: int = 0) -> dict:
    """An update with `image`, as phase 5's download will make it: committed as the TRIAL."""
    return flash_tool("stage", "--flash-file", flash, "--no-wait", *options, image, expect=expect)


def info(flash: Path) -> dict:
    """`fcn_flash info`, after checking that fcn_tools.flash reads the same from the file."""
    reported = flash_tool("info", "--flash-file", flash)
    decoded = flash_decoder.describe(flash.read_bytes())
    reported_without_counts = {key: value for key, value in reported.items() if key != "flash"}
    assert decoded == reported_without_counts, (
        "fcn_tools.flash and fcn_flash info disagree:\n"
        f"decoder {json.dumps(decoded, indent=1)}\ntool {json.dumps(reported_without_counts, indent=1)}")
    return reported


def states(flash_info: dict) -> tuple[str, ...]:
    """The state of slot A and of slot B."""
    return tuple(slot["state"] for slot in flash_info["slots"])


def slot_index(name: str) -> int:
    return "AB".index(name)


def flip_bit(flash: Path, slot: str, where: str, seed: int) -> tuple[int, int]:
    """The bench flips one bit of the image in `slot`, as a bit that changed in the flash would:
    in its 256-byte header (`where` "header") or in its payload ("payload"), at a place drawn
    from `seed`. Returns the file offset and the bit."""
    offset, _ = flash_decoder.SLOTS[slot_index(slot)]
    data = bytearray(flash.read_bytes())
    header = data[offset:offset + 256]
    payload_size = int.from_bytes(header[16:20], "little")   # docs/update.md, the header
    draw = random.Random(seed)
    start, end = (0, 256) if where == "header" else (256, 256 + payload_size)
    at = offset + draw.randrange(start, end)
    bit = 1 << draw.randrange(8)
    data[at] ^= bit
    flash.write_bytes(bytes(data))
    return at, bit


# ----------------------------------------------------------------------- what a flash must be


@dataclass(frozen=True)
class Boot:
    """What the bootloader's select starts: a slot, as a trial (attempt 1 to 3) or as the
    confirmed image (attempt 0), and the state change it writes on the way."""
    slot: str
    mode: str          # "trial" or "confirmed", as in the node's statistics
    attempt: int
    event: str         # "NONE", "ROLLBACK", ... as in the node's statistics and BMS_BootInfo


def expected_boot(flash_info: dict) -> Boot:
    """What select must do with the flash `flash_info` describes, from the state machine of
    docs/update.md, for flashes whose images all validate as recorded (a trial with attempts
    left is started as its next attempt; one that has used them is rolled back and the
    confirmed image started; otherwise the confirmed image) or have been damaged by the bench
    (a trial that does not validate is rejected; a confirmed image that does not validate falls
    back to the previous one). Fails the test for a factory flash: every test adopts first."""
    md = flash_info["metadata"]
    assert md["have_record"], "a flash in the factory state"
    slots = {slot["name"]: slot for slot in flash_info["slots"]}
    active, trial = md["active_slot"], md["trial_slot"]
    if trial != "none":
        if slots[trial]["validation"] != "ok" or not slots[trial]["id_matches_record"]:
            return Boot(active, "confirmed", 0, "REJECT")
        if md["attempts_used"] < MAX_ATTEMPTS:
            return Boot(trial, "trial", md["attempts_used"] + 1, "NONE")
        return Boot(active, "confirmed", 0, "ROLLBACK")
    if slots[active]["validation"] == "ok" and slots[active]["id_matches_record"]:
        return Boot(active, "confirmed", 0, "NONE")
    other = "B" if active == "A" else "A"
    assert slots[other]["state"] == "previous" and slots[other]["id_matches_record"], (
        f"nothing bootable: {json.dumps(flash_info['slots'], indent=1)}")
    return Boot(other, "confirmed", 0, "FALLBACK")


def check_invariants(flash_info: dict) -> None:
    """What every flash of these tests satisfies that the bench has not damaged, whatever else
    happened to it, power cuts included (docs/update.md, boot control): the metadata have a
    newest record (the factory adoption is never undone); exactly one slot is CONFIRMED, and it
    holds the image the record names, intact; at most one slot is a TRIAL, with 0 to 3 attempts
    used; and every slot whose state names an image (TRIAL, CONFIRMED, PREVIOUS) holds that
    image, intact."""
    md = flash_info["metadata"]
    assert md["have_record"], "the metadata lost every record"
    by_state = {}
    for slot in flash_info["slots"]:
        by_state.setdefault(slot["state"], []).append(slot["name"])
    assert len(by_state.get("confirmed", [])) == 1, by_state
    assert len(by_state.get("trial", [])) <= 1, by_state
    active = flash_info["slots"][slot_index(md["active_slot"])]
    assert active["state"] == "confirmed"
    assert active["validation"] == "ok" and active["id_matches_record"], active
    assert 0 <= md["attempts_used"] <= MAX_ATTEMPTS
    for slot in flash_info["slots"]:
        if slot["state"] in ("trial", "confirmed", "previous"):
            assert slot["validation"] == "ok" and slot["id_matches_record"], slot


# -------------------------------------------------------------------------------- the frames


def decoded(frames, name: str) -> list[DecodedFrame]:
    """The frames of message `name` among `frames`, decoded."""
    decoder = Decoder()
    out = []
    for message in list(frames):
        frame = decoder.decode(message)
        if frame is not None and frame.name == name:
            out.append(frame)
    return out


def wait_until_confirmed(reader: bus_module.BusReader, node: NodeProcess) -> None:
    """Wait for a BMS_BootInfo that says CONFIRMED, while the node runs."""
    deadline = time.monotonic() + CONFIRM_WAIT_S
    while time.monotonic() < deadline and node.running:
        if any(frame.signals["ConfirmState"] == "CONFIRMED"
               for frame in decoded(reader.frames, "BMS_BootInfo")):
            return
        time.sleep(0.1)
    pytest.fail(f"no BMS_BootInfo with ConfirmState CONFIRMED within {CONFIRM_WAIT_S} s\n"
                f"--- log\n{node.log()}\n--- stderr\n{node.stderr()}", pytrace=False)


def listen(bus_spec: str, start: Callable[[], NodeProcess],
           run: Callable[[NodeProcess, bus_module.BusReader], object] | None = None
           ) -> tuple[NodeProcess, list[can.Message]]:
    """Open the bus, start a node with `start()` and return it with every frame it sent.
    `run(node, reader)` waits for its end, or does what the test needs meanwhile; without one
    the node must end by itself in order (status 0: a run of --duration-s)."""
    bus = bus_module.open_bus(bus_spec)
    try:
        with bus_module.BusReader(bus) as reader:
            node = start()
            if run is None:
                status = node.wait(timeout=RUN_WAIT_S)
                assert status == EXIT_OK, f"{node.name} ended with {status}\n{node.stderr()}"
            else:
                run(node, reader)
    finally:
        bus.shutdown()
    return node, reader.frames


def until_confirmed(node: NodeProcess, reader: bus_module.BusReader) -> None:
    """For listen(): wait until the trial says CONFIRMED on the bus, then stop it in order."""
    wait_until_confirmed(reader, node)
    assert node.stop() == EXIT_OK


def check_frames_of(boot: Boot, frames: list[can.Message], version: str,
                    reset_cause: str | None = None) -> None:
    """The frames of a run started as `boot`: BMS_NodeInfo names the slot (and the reset cause
    of the start, if given), BMS_BootInfo the mode, the attempt, the bootloader's event and the
    image's `version` (major.minor.patch)."""
    infos = decoded(frames, "BMS_NodeInfo")
    boots = decoded(frames, "BMS_BootInfo")
    assert infos and boots, f"{len(infos)} BMS_NodeInfo, {len(boots)} BMS_BootInfo"
    for frame in infos:
        assert frame.signals["ActiveSlot"] == boot.slot, frame
        if reset_cause is not None:
            assert frame.signals["ResetCause"] == reset_cause, frame
    major, minor, patch = (int(part) for part in version.split("."))
    for frame in boots:
        assert frame.signals["BootMode"] == boot.mode.upper(), frame
        assert frame.raw["BootAttempt"] == boot.attempt, frame
        assert frame.signals["BootEvent"] == boot.event, frame
        assert (frame.raw["ImageVersionMajor"], frame.raw["ImageVersionMinor"],
                frame.raw["ImageVersionPatch"]) == (major, minor, patch), frame


def check_handoff(stats: dict, boot: Boot) -> None:
    """The hand-off the node took, in its statistics, is `boot`, and no write of the select
    failed on the way (none fails in these tests: a cut ends the run instead, and a select whose
    write fails starts the confirmed image as well, so that only the hand-off tells)."""
    handoff = stats["handoff"]
    assert handoff["verdict"] == "taken", handoff
    assert (handoff["slot"], handoff["mode"], handoff["attempt"], handoff["event"]) == (
        boot.slot, boot.mode, boot.attempt, boot.event), handoff
    assert handoff["rolled_back"] == (boot.event in ("ROLLBACK", "REJECT", "FALLBACK"))
    assert handoff["select_write_error"] == 0, handoff
