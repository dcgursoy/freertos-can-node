# SPDX-License-Identifier: MIT
"""fcn_tools.flasher and fcn_tools.diag without a node: the blocks of a download, the boot
information record, the request timeouts from the part's maximum times, and the programming
sequence against a fake UDS client that answers as the node's server does (lib/uds/uds.h) or as
the test makes it answer: the steps in their order, the report, a refusal, a trial that confirmed
itself before the routine, an answer of the old image while the node resets, and the exit
statuses of the command line. The node itself is the system tests' (tests/system/test_download.py)."""
from __future__ import annotations

import contextlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest
import udsoncan
from udsoncan.exceptions import NegativeResponseException, TimeoutException

from fcn_tools import diag, flasher
from fcn_tools import image as image_format


def boot_info_bytes(slot: int, mode: int, attempt: int, state: int, version: int,
                    handoff: bool = True) -> bytes:
    """DID 0xFD00 as lib/uds encodes it (layout 1)."""
    return bytes([1, 1 if handoff else 0, slot if handoff else 0xFF, mode if handoff else 0,
                  attempt, state, 0, 0xFF]) + (version if handoff else 0).to_bytes(4, "big")


def test_the_blocks_carry_max_length_minus_two_bytes_and_the_counter_wraps():
    blocks = flasher.plan_blocks(1024 * 300 + 5, 1026)
    assert len(blocks) == 301
    assert blocks[0] == (1, 0, 1024)
    assert blocks[254] == (255, 254 * 1024, 1024)
    assert blocks[255] == (0, 255 * 1024, 1024)       # 0xFF wraps to 0x00
    assert blocks[256] == (1, 256 * 1024, 1024)
    assert blocks[-1] == (301 & 0xFF, 300 * 1024, 5)
    assert flasher.plan_blocks(1024, 1026) == [(1, 0, 1024)]
    assert flasher.plan_blocks(1025, 1026) == [(1, 0, 1024), (2, 1024, 1)]
    with pytest.raises(ValueError):
        flasher.plan_blocks(10, 2)


def test_the_boot_information_record_is_decoded_as_lib_uds_writes_it():
    info = diag.decode_boot_info(boot_info_bytes(1, 2, 1, 1, 0x01020003))
    assert (info.handoff, info.slot, info.mode, info.attempt, info.confirm_state,
            info.image_version) == (True, "B", "trial", 1, "WAITING", "1.2.3")
    none = diag.decode_boot_info(boot_info_bytes(0, 0, 0, 0, 0, handoff=False))
    assert (none.handoff, none.slot, none.mode, none.image_version) == (False, None, None, None)
    assert none.confirm_state == "NOT_NEEDED"
    with pytest.raises(ValueError):
        diag.decode_boot_info(bytes(11))
    with pytest.raises(ValueError):
        diag.decode_boot_info(bytes([2]) + bytes(11))
    assert diag.version_text(0x01FF1234) == "1.255.4660"


def test_the_request_timeouts_allow_the_parts_maximum_times():
    # 240,272 bytes: 59 sectors of 4 KiB at 400 ms, the BEGIN record (a sector erase and three
    # page programs at their maximum), and the hosts' margin.
    assert diag.erase_timeout_s(240272) == pytest.approx(59 * 0.4 + 0.409 + 10.0)
    assert diag.check_timeout_s(240272) == pytest.approx((240272 // 64 + 8) * 0.0001 + 0.409
                                                         + 10.0)
    assert diag.write_timeout_s(1024) == pytest.approx(5 * 0.003 + 10.0)
    # P2*client allows the node's P2*server; responsePending comes every 2 s.
    assert diag.P2_STAR_CLIENT_S > diag.P2_STAR_SERVER_S > diag.PENDING_EVERY_S


def _negative(service, code: int) -> NegativeResponseException:
    return NegativeResponseException(udsoncan.Response(service=service, code=code))


class FakeClient:
    """Answers the flasher's requests as the node does; `answers` changes what one request
    answers: name -> a callable (raising or returning)."""

    def __init__(self, running: str = "A", version: int = 0x01010000) -> None:
        self.calls: list[tuple] = []
        self.config = {"request_timeout": 10.0}
        self.running = running
        self.version = version
        self.reset_done = False
        self.confirmed = False
        self.old_answers_after_reset = 0
        self.answers: dict = {}
        self.received = bytearray()

    def set_config(self, key, value):
        self.config[key] = value

    def _answer(self, name, default):
        self.calls.append((name, self.config["request_timeout"]))
        action = self.answers.get(name)
        return action() if action is not None else default()

    def change_session(self, session):
        return self._answer(f"session_{session}", lambda: SimpleNamespace())

    def read_data_by_identifier(self, did):
        def default():
            if did == diag.DID_BOOT_INFO:
                if self.reset_done and self.old_answers_after_reset > 0:
                    self.old_answers_after_reset -= 1
                    raw = boot_info_bytes(0, 1, 0, 0, 0x01000000)     # the old image
                elif self.reset_done:
                    raw = boot_info_bytes(1, 2, 1, 3 if self.confirmed else 1, self.version)
                else:
                    raw = boot_info_bytes("AB".index(self.running), 1, 0, 0, 0x01000000)
                return SimpleNamespace(service_data=SimpleNamespace(values={did: raw}))
            return SimpleNamespace(service_data=SimpleNamespace(
                values={did: diag.version_text(self.version)}))
        return self._answer(f"did_{did:04X}", default)

    def start_routine(self, rid, data=None):
        def default():
            if rid == diag.RID_CONFIRM_TRIAL:
                self.confirmed = True
            return SimpleNamespace(service_data=SimpleNamespace(routine_status_record=b"\x00\x00"))
        self.calls.append(("routine_data", data))
        return self._answer(f"routine_{rid:04X}", default)

    def request_download(self, location):
        self.calls.append(("location", location.address, location.memorysize))
        assert (location.address_format, location.memorysize_format) == (32, 32)  # ALFID 0x44
        return self._answer("download",
                            lambda: SimpleNamespace(service_data=SimpleNamespace(max_length=1026)))

    def transfer_data(self, counter, data):
        self.received += data
        return self._answer(f"transfer_{counter}", lambda: SimpleNamespace())

    def request_transfer_exit(self):
        return self._answer("exit", lambda: SimpleNamespace())

    def ecu_reset(self, reset_type):
        def default():
            self.reset_done = True
            return SimpleNamespace()
        return self._answer(f"reset_{reset_type}", default)


@pytest.fixture
def fake(monkeypatch):
    client = FakeClient()

    @contextlib.contextmanager
    def connect(spec, **kwargs):
        client.spec = spec
        yield client

    monkeypatch.setattr(diag, "connect", connect)
    monkeypatch.setattr(flasher, "TRIAL_POLL_S", 0.0)
    return client


@pytest.fixture
def image_file(tmp_path) -> Path:
    payload = bytes((i * 7) & 0xFF for i in range(3000))
    path = tmp_path / "node-1.1.0.img"
    path.write_bytes(image_format.pack(payload, image_format.parse_version("1.1.0"),
                                       image_format.TARGETS["host"]))
    return path


def test_the_sequence_writes_the_image_into_the_other_slot_and_confirms_it(fake, image_file):
    report = flasher.flash("udp:1", image_file).as_json()
    names = [step["name"] for step in report["steps"]]
    assert names == ["boot_info", "default_session", "extended_session", "programming_session",
                     "erase", "request_download", "transfer", "transfer_exit", "check",
                     "ecu_reset", "wait_for_trial", "read_version", "extended_session_trial",
                     "confirm", "boot_info_confirmed"]
    assert all(step["ok"] for step in report["steps"])
    assert report["result"] == "confirmed" and report["confirmed_by"] == "routine"
    size = image_file.stat().st_size
    assert (report["slot"], report["address"]) == ("B", 0x102000)
    assert ("location", 0x102000, size) in fake.calls
    assert ("routine_data", bytes([0x44]) + (0x102000).to_bytes(4, "big")
            + size.to_bytes(4, "big")) in fake.calls
    assert bytes(fake.received) == image_file.read_bytes()
    assert report["blocks"] == len(report["blocks_s"]) == -(-size // 1024)
    # The erase and the check wait as long as the part can take at its maximum times.
    timeouts = dict(fake.calls[i] for i in range(len(fake.calls)) if len(fake.calls[i]) == 2
                    and isinstance(fake.calls[i][1], float))
    assert timeouts["routine_FF00"] == pytest.approx(diag.erase_timeout_s(size))
    assert timeouts["routine_FF01"] == pytest.approx(diag.check_timeout_s(size))
    assert fake.config["request_timeout"] == 10.0          # put back after each
    assert report["schema"] == "fcn.flash_report/1"
    assert report["image"]["version"] == "1.1.0" and report["image"]["size"] == size
    json.dumps(report)


def test_a_slot_running_b_is_updated_in_a(fake, image_file):
    fake.running = "B"
    report = flasher.flash("udp:1", image_file, reset=False).as_json()
    assert (report["slot"], report["address"], report["result"]) == ("A", 0x2000, "committed")
    assert "ecu_reset" not in [step["name"] for step in report["steps"]]


def test_a_refused_erase_ends_the_sequence_with_its_nrc(fake, image_file):
    def refuse():
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_FF00"] = refuse
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "failed" and report["failed_step"] == "erase"
    erase = report["steps"][-1]
    assert (erase["name"], erase["ok"], erase["nrc"]) == ("erase", False, 0x22)
    assert erase["nrc_name"] == "ConditionsNotCorrect"


def test_a_timeout_is_reported_as_one(fake, image_file):
    def silent():
        raise TimeoutException("P2* timeout")
    fake.answers["exit"] = silent
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "transfer_exit"
    assert report["steps"][-1]["error"].startswith("timeout")


def test_a_trial_that_confirmed_itself_first_counts_as_confirmed_by_the_node(fake, image_file):
    def already():
        fake.confirmed = True
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_F000"] = already
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "confirmed" and report["confirmed_by"] == "node"
    confirm = [step for step in report["steps"] if step["name"] == "confirm"][0]
    assert confirm["ok"] and confirm["nrc"] == 0x22 and "confirmed itself" in confirm["outcome"]


def test_a_refused_confirm_of_a_trial_that_is_not_confirmed_fails(fake, image_file):
    def refuse():
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_F000"] = refuse
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "failed" and report["failed_step"] == "confirm_check"


def test_answers_of_the_old_image_after_the_reset_are_not_the_trial(fake, image_file):
    fake.old_answers_after_reset = 3
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "confirmed"
    asked = [call for call in fake.calls if call[0] == "did_FD00"]
    assert len(asked) == 1 + 4 + 1      # before, three old answers and the trial's, after


def test_no_trial_within_the_wait_fails_wait_for_trial(fake, image_file):
    fake.old_answers_after_reset = 10**6
    report = flasher.flash("udp:1", image_file, trial_wait_s=0.05).as_json()
    assert report["failed_step"] == "wait_for_trial"


def test_a_trial_of_another_version_fails(fake, image_file):
    fake.version = 0x01040000
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "version_check"


def test_a_node_without_a_hand_off_needs_an_address(fake, image_file):
    fake.answers["did_FD00"] = lambda: SimpleNamespace(service_data=SimpleNamespace(
        values={diag.DID_BOOT_INFO: boot_info_bytes(0, 0, 0, 0, 0, handoff=False)}))
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "target_slot"
    report = flasher.flash("udp:1", image_file, address=0x102000, reset=False).as_json()
    assert report["result"] == "committed" and report["slot"] == "B"


def test_the_command_line_writes_the_report_and_says_how_it_went(fake, image_file, tmp_path,
                                                                capsys):
    path = tmp_path / "report.json"
    assert flasher.main(["--can", "udp:1", "--report", str(path), str(image_file)]) == 0
    assert json.loads(path.read_text())["result"] == "confirmed"
    assert "result: confirmed" in capsys.readouterr().err

    def refuse():
        raise _negative(udsoncan.services.DiagnosticSessionControl, 0x7E)
    fake.answers["session_3"] = refuse
    assert flasher.main(["--can", "udp:1", str(image_file)]) == 2
    assert json.loads(capsys.readouterr().out)["failed_step"] == "extended_session"

    not_an_image = tmp_path / "junk.img"
    not_an_image.write_bytes(bytes(1000))
    assert flasher.main(["--can", "udp:1", str(not_an_image)]) == 1
    assert flasher.main(["--can", "udp:1", str(tmp_path / "missing.img")]) == 1
