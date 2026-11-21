# SPDX-License-Identifier: MIT
"""Fixtures of the system tests on top of tests/conftest.py: the images the update tests write
into a flash (tests/system/update_bench.py says how they use them)."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from fcn_tools import image as image_format
from fcn_tools import paths


@pytest.fixture(scope="session")
def images(tmp_path_factory) -> dict[str, Path]:
    """Image files (fcn_tools.image pack, target host): "node-1.0.0", "node-1.1.0" and
    "node-1.4.0", the release build of the node, stripped, under three versions; "crash" (1.2.0)
    and "watchdog" (1.3.0), the test payloads of tests/tools/payload.c, which stand in for an
    image that crashes at once (status 13) and one that hangs until the watchdog resets it
    (status 11). The asan build of the node does not fit a slot (docs/update.md), so the asan
    run of scripts/test-system.sh starts these images as well; its fcn_boot and fcn_flash are
    the asan ones."""
    release = paths.build_root() / "release" / "bin"
    node = release / "fcn_node"
    payloads = {"crash": release / "fcn_payload_crash", "watchdog": release / "fcn_payload_watchdog"}
    for binary in (node, *payloads.values()):
        if not binary.exists():
            pytest.fail(f"{binary} not found: build it with scripts/build.sh release",
                        pytrace=False)
    out = tmp_path_factory.mktemp("images")
    stripped = out / "fcn_node.stripped"
    strip = shutil.which("strip")
    if strip:
        subprocess.run([strip, "-o", str(stripped), str(node)], check=True)
    else:
        shutil.copyfile(node, stripped)
    made = {}
    for name, payload, version in (("node-1.0.0", stripped, "1.0.0"),
                                   ("node-1.1.0", stripped, "1.1.0"),
                                   ("node-1.4.0", stripped, "1.4.0"),
                                   ("crash", payloads["crash"], "1.2.0"),
                                   ("watchdog", payloads["watchdog"], "1.3.0")):
        path = out / f"{name}.img"
        path.write_bytes(image_format.pack(payload.read_bytes(),
                                           image_format.parse_version(version),
                                           image_format.TARGETS["host"]))
        made[name] = path
    return made
