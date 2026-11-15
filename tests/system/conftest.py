"""Fixtures of the system tests.

A system test starts the real node binary and observes it from outside. Every test gets its
own work directory for the node's log, statistics and data files; the directory is kept so
that CI can upload it, and a node can never outlive the test that started it.
"""
from __future__ import annotations

import re
import shutil
from pathlib import Path

import pytest

from fcn_tools import node as node_module
from fcn_tools import paths
from fcn_tools.node import NodeProcess


def pytest_addoption(parser):
    parser.addoption(
        "--artifacts", default=None,
        help="directory that receives one sub-directory per test with the node's files "
             "(default: <build root>/system-test/logs)")


@pytest.fixture(scope="session")
def artifacts_root(request) -> Path:
    option = request.config.getoption("--artifacts")
    root = Path(option) if option else paths.build_root() / "system-test" / "logs"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    return root


@pytest.fixture(scope="session", autouse=True)
def no_node_survives_the_session():
    """A node left running after the last test is a harness bug: fail loudly, then clean up."""
    yield
    survivors = node_module.still_running()
    for node in survivors:
        node.kill()
    assert not survivors, f"{len(survivors)} node process(es) were still running at session end"


@pytest.fixture(scope="session")
def node_binary() -> Path:
    binary = paths.node_binary()
    if not binary.exists():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release", pytrace=False)
    return binary


@pytest.fixture
def workdir(artifacts_root, request) -> Path:
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.nodeid.split("::", 1)[-1])
    path = artifacts_root / name
    path.mkdir(parents=True, exist_ok=True)
    return path


@pytest.fixture
def start_node(workdir, node_binary):
    """Factory: start_node(*args, name="node") returns a running NodeProcess."""
    started: list[NodeProcess] = []

    def _start(*args: str, name: str = "node") -> NodeProcess:
        node = NodeProcess(workdir=workdir, args=args, binary=node_binary, name=name).start()
        started.append(node)
        return node

    yield _start

    for node in started:
        node.kill()
