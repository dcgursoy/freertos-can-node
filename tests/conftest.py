"""Fixtures of the Python tests.

A system test starts the real node binary and observes it from outside, over the CAN bus and
through the simulation control channel. Every test gets

  - its own work directory for the node's log, statistics and data files, kept so that CI can
    upload it;
  - its own UDP ports (a bus and a simulation control port), so tests cannot talk to each
    other's nodes or to anything else running on the machine;
  - a clean end: its nodes are killed when it finishes, the test fails if one of them ended
    by itself without the test looking at its exit status (or exited 0 but wrote to stderr),
    the session fails if a node is still running at the end, and the kernel kills a node
    whose test run died.

With --timing-report FILE, every failed test is listed in FILE as "miss <test>" if it failed
only on a timing tolerance (fcn_tools.timing) and as "fail <test>" otherwise; that is how
scripts/test-system.sh decides whether a repetition is allowed.

Tests that use the `can_spec` or `open_link` fixture run once per transport: on the UDP bus and
on SocketCAN. The SocketCAN run is skipped on a machine without the interface, unless
FCN_REQUIRE_SOCKETCAN is set (CI sets it), in which case a missing interface is a failure.
"""
from __future__ import annotations

import fcntl
import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

import can
import pytest

from fcn_tools import bus as bus_module
from fcn_tools import node as node_module
from fcn_tools import paths
from fcn_tools.node import NodeProcess
from fcn_tools.ports import PortAllocator, PortBlock
from fcn_tools.timing import TimingToleranceMissed

SOCKETCAN_INTERFACE = os.environ.get("FCN_TEST_CAN_INTERFACE", "vcan0")

# Written into an artifacts directory the harness created; only such a directory is ever
# emptied by it.
ARTIFACTS_MARKER = ".fcn-artifacts"


def pytest_addoption(parser):
    parser.addoption(
        "--artifacts", default=None,
        help="directory that receives one sub-directory per test with the node's files "
             "(default: <build root>/system-test/logs)")
    parser.addoption(
        "--timing-report", default=None,
        help="append 'miss <test>' or 'fail <test>' to this file for every failed test")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    path = item.config.getoption("--timing-report")
    if path and report.failed:
        timing_only = (report.when == "call" and call.excinfo is not None
                       and call.excinfo.errisinstance(TimingToleranceMissed))
        with open(path, "a", encoding="utf-8") as out:
            out.write(f"{'miss' if timing_only else 'fail'} {item.nodeid}\n")


@pytest.fixture(scope="session")
def artifacts_root(request) -> Path:
    option = request.config.getoption("--artifacts")
    root = Path(option) if option else paths.build_root() / "system-test" / "logs"
    if root.exists():
        # Only a directory this harness made is emptied: --artifacts pointing at anything
        # else (the work tree, a directory of notes) must never lose a file.
        if not (root / ARTIFACTS_MARKER).is_file():
            if any(root.iterdir()):
                pytest.exit(f"--artifacts {root} is not empty and was not created by the test "
                            f"harness (no {ARTIFACTS_MARKER} in it): refusing to delete it",
                            returncode=4)
        else:
            shutil.rmtree(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / ARTIFACTS_MARKER).write_text("created by tests/conftest.py; emptied on every run\n")
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


@pytest.fixture(scope="session")
def port_allocator() -> PortAllocator:
    return PortAllocator()


@pytest.fixture
def ports(port_allocator) -> PortBlock:
    """UDP ports that belong to this test alone: `ports.bus` and `ports.sim`."""
    block = port_allocator.take()
    yield block
    block.release()


@pytest.fixture
def start_node(workdir, node_binary, ports):
    """Factory: start_node(*args, name="node", can=None) returns a running NodeProcess.

    Without `can` the node gets the test's own UDP bus. The first node of a test serves
    simulation control on the test's port; further nodes of the same test do not.
    """
    started: list[NodeProcess] = []

    def _start(*args: str, name: str = "node", can: str | None = None) -> NodeProcess:
        node = NodeProcess(
            workdir=workdir, args=args, binary=node_binary, name=name,
            can=can or f"udp:{ports.bus}",
            sim_port=None if started else ports.sim,
        ).start()
        started.append(node)
        return node

    yield _start

    # Asked before the nodes are killed: a death the test did not see fails the test.
    problems = [problem for problem in (node.unnoticed_end() for node in started) if problem]
    for node in started:
        node.kill()
    if problems:
        pytest.fail("\n\n".join(problems), pytrace=False)


# ------------------------------------------------------------------------------- the bus


@pytest.fixture(params=["udp", pytest.param("socketcan", marks=pytest.mark.socketcan)])
def can_spec(request, ports) -> str:
    """The bus of this test, once per transport."""
    if request.param == "udp":
        yield f"udp:{ports.bus}"
        return

    if not bus_module.socketcan_available(SOCKETCAN_INTERFACE):
        message = (f"SocketCAN interface {SOCKETCAN_INTERFACE} is not available "
                   "(scripts/vcan-up.sh creates it; see docs/wsl-vcan.md)")
        if os.environ.get("FCN_REQUIRE_SOCKETCAN"):
            pytest.fail(message, pytrace=False)
        pytest.skip(message)

    # One interface is shared by everything on the machine. A lock file keeps two test runs
    # from using it at the same time; the tests still assume nobody else transmits on it.
    lock_path = Path(tempfile.gettempdir()) / f"fcn-test-{SOCKETCAN_INTERFACE}.lock"
    with open(lock_path, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield f"socketcan:{SOCKETCAN_INTERFACE}"
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


@dataclass
class Link:
    """A running node and the tester's end of its bus."""

    bus: can.BusABC
    node: NodeProcess
    spec: str
    first_status: can.Message   # the first frame the node sent


@pytest.fixture
def open_link(can_spec, start_node):
    """Factory: open_link(*node_args) starts a node on the test's bus and returns a Link.

    The tester's end of the bus is opened before the node starts, so the tester sees every
    frame from the first one, and the link is handed out once that first frame has arrived:
    tests never sleep to wait for the node.
    """
    buses: list[can.BusABC] = []

    def _open(*node_args: str) -> Link:
        bus = bus_module.open_bus(can_spec)
        buses.append(bus)
        node = start_node(*node_args, can=can_spec)
        first = bus_module.wait_for_id(bus, bus_module.ID_STATUS, timeout=5.0)
        if first is None:
            pytest.fail("the node sent no status frame within 5 s of its start\n"
                        f"--- stderr\n{node.stderr()}\n--- log\n{node.log()}", pytrace=False)
        return Link(bus=bus, node=node, spec=can_spec, first_status=first)

    yield _open

    for bus in buses:
        bus.shutdown()
