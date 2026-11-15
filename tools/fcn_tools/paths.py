"""Where the repository, its build trees and its binaries are.

The build root follows the same rule as the shell scripts: FCN_BUILD_ROOT if set, otherwise
<repo>/build. A binary can be overridden explicitly with FCN_NODE / FCN_SELFTEST.
"""
from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def build_root() -> Path:
    return Path(os.environ.get("FCN_BUILD_ROOT", REPO_ROOT / "build"))


def node_binary(preset: str = "release") -> Path:
    override = os.environ.get("FCN_NODE")
    return Path(override) if override else build_root() / preset / "bin" / "fcn_node"


def selftest_binary(preset: str = "release") -> Path:
    override = os.environ.get("FCN_SELFTEST")
    if override:
        return Path(override)
    return build_root() / preset / "tests" / "platform" / "plat_selftest"


def results_dir() -> Path:
    return REPO_ROOT / "results"
