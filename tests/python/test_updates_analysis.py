"""The analysis of bench/updates.py on constructed inputs with known answers: the versions and
slots of a campaign, the big image's payload, the node's own account of a download in its log,
the step counts of the erase and the check, when a campaign can go on, and the figures and the
summary of constructed records."""
from __future__ import annotations

import importlib.util
import sys

import pytest

from fcn_tools import paths, results


def _load(name: str):
    path = paths.REPO_ROOT / "bench" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"bench_{name}", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


bench = _load("updates")

DOWNLOAD_LOG = """\
[ 81953.210755] I main: fcn_node 0.1.0 starting (FreeRTOS V11.3.3)
[ 81953.285683] I app: download: slot at 0x102000 erased for 244368 bytes, 61 steps (2752679 us)
[ 81953.621710] I app: download: the image validated and is committed as the trial, 3824 steps (2353 us)
[ 81953.700000] W app: ECUReset: resetting (requested by the tester)
"""

TRIAL_LOG = ("[ 3.1] I app: confirmed by the tester's confirm routine: slot B, image 2.1.0, is "
             "the active image (2086 us)\n")


def test_each_campaign_has_its_versions_and_the_updates_alternate_the_slots():
    assert bench.update_version("socketcan", 1) == "2.1.0"
    assert bench.update_version("udp", 50) == "3.50.0"
    for number in (0, 256):
        with pytest.raises(ValueError):
            bench.update_version("udp", number)
    assert [bench.target_slot(n) for n in (1, 2, 3, 4)] == ["B", "A", "B", "A"]


def test_the_big_payload_is_the_node_padded_or_seeded_data_of_exactly_its_size():
    node = b"\x7fELF" + bytes(range(200))
    padded, kind = bench.big_payload(node, 1000)
    assert kind == "node, padded" and len(padded) == 1000
    assert padded[:len(node)] == node and padded[len(node):] == bytes(1000 - len(node))
    exact, kind = bench.big_payload(node, len(node))
    assert (exact, kind) == (node, "node, padded")
    data, kind = bench.big_payload(node, 100, seed=7)
    assert kind == "data" and len(data) == 100
    assert data == bench.big_payload(node, 100, seed=7)[0] != bench.big_payload(node, 100, 8)[0]
    with pytest.raises(ValueError):
        bench.big_payload(node, 0)


def test_the_download_runs_log_gives_the_nodes_account_of_the_download():
    logged = bench.parse_download_log(DOWNLOAD_LOG)
    assert logged["erase"] == {"address": 0x102000, "bytes": 244368, "steps": 61,
                               "us": 2752679}
    assert logged["check"] == {"steps": 3824, "us": 2353}
    assert logged["reset_for_the_tester"] is True
    assert bench.download_log_problems(logged, 244368) == []
    empty = bench.parse_download_log("")
    assert empty == {"erase": None, "check": None, "reset_for_the_tester": False}
    assert len(bench.download_log_problems(empty, 244368)) == 3
    # 64 bytes more: the erase is of another size, and the check one validator step longer.
    assert len(bench.download_log_problems(logged, 244368 + 64)) == 2


def test_the_trial_runs_log_gives_the_confirm_by_the_routine():
    assert bench.parse_trial_log(TRIAL_LOG) == {"slot": "B", "version": "2.1.0", "us": 2086}
    assert bench.parse_trial_log("trial in slot B ran 5 s in good health") is None


@pytest.mark.parametrize("image_bytes, erase, check", [
    (244368, 61, 3824),        # the release node of docs/diagnostics.md's measurement
    (8448, 4, 137),            # a payload of 8192 bytes
    (4096, 2, 69),             # 3840 bytes of payload: 60 validator steps
    (262144 + 256, 66, 4105),  # the 256 KiB payload
])
def test_the_erase_and_the_check_take_the_steps_the_slot_writer_documents(image_bytes, erase,
                                                                          check):
    assert bench.erase_steps(image_bytes) == erase
    assert bench.check_steps(image_bytes) == check


def test_a_campaign_goes_on_only_from_a_confirmed_image_without_a_trial():
    assert bench.can_continue(["previous", "confirmed"])
    assert bench.can_continue(["confirmed", "writing"])
    assert not bench.can_continue(["confirmed", "trial"])
    assert not bench.can_continue(["empty", "empty"])


def record(number: int, success: bool = True, total: float = 24.0, transport: str = "udp") -> dict:
    steps = {step: 0.001 for step in bench.UPDATE_STEPS}
    steps.update(erase=2.75, transfer=19.7 + number / 100, wait_for_trial=1.2)
    return {"number": number, "transport": transport, "success": success,
            "image_bytes": 244368, "steps": steps if success else {"boot_info": 0.002},
            "total_s": total + number / 100, "flasher_wall_s": total + 0.5,
            "start_to_ready_s": 0.014, "confirmed_by": "routine" if number % 5 else "node",
            "node_download": {"erase": {"us": 2_750_000}},
            "report": {"transfer": {"bytes_per_s": 12_400.0},
                       "block_ms": {"p50": 82.5}}}


def test_the_counts_of_a_campaign():
    records = [record(1), record(2, success=False), record(3)]
    assert bench.campaign_counts(records, 5) == {"requested": 5, "attempted": 3, "succeeded": 2,
                                                 "failed": 1, "not_attempted": 2}
    durations = bench.step_durations(records, ("erase", "transfer"))
    assert durations["erase"] == [2.75, 2.75]
    assert durations["transfer"] == pytest.approx([19.71, 19.73])


def test_the_figures_of_a_campaign_count_only_its_successful_updates():
    records = [record(n) for n in range(1, 13)] + [record(13, success=False)]
    figures = bench.campaign_figures({"records": records})
    assert figures["total_s"]["n"] == 12 and figures["total_s"]["p50"] is not None
    assert figures["steps_s"]["transfer"]["n"] == 12
    assert figures["steps_s"]["erase"]["mean"] == pytest.approx(2.75)
    assert figures["confirmed_by"] == {"routine": 10, "node": 2}
    assert figures["raw_total_s"]["n"] == 12


def test_the_summary_gives_every_figure_with_its_unit():
    records = [record(n) for n in range(1, 4)]
    part = {"counts": bench.campaign_counts(records, 3), "records": records}
    part["figures"] = bench.campaign_figures(part)
    big_record = {"success": True, "steps": {step: 0.01 for step in bench.DOWNLOAD_STEPS},
                  "total_s": 25.0, "report": {"transfer": {"bytes_per_s": 12_000.0}},
                  "node": {"download": {"received": 262_400, "transfer_us": 21_000_000},
                           "jobs": {"write": {"run": 257, "total_ns": 257 * 4_000_000}}}}
    big = {"payload_bytes": 262_144, "image_bytes": 262_400, "transport": "socketcan",
           "counts": bench.campaign_counts([big_record], 1), "records": [big_record]}
    big["figures"] = bench.big_figures(big)
    assert big["figures"]["write_job_mean_ms"]["mean"] == pytest.approx(4.0)
    assert big["figures"]["node_transfer_bytes_per_s"]["mean"] == pytest.approx(262_400 / 21)
    summary = bench.summarise({"campaigns": {"udp": part}, "big_image": big})
    results.check_summary(summary)
    assert summary["updates_udp"]["succeeded"] == {"value": 3, "unit": "updates"}
    assert summary["big_image"]["payload_bytes"]["value"] == 262_144
