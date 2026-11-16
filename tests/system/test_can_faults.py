"""Bus faults, applied to the simulated hardware through simulation control and observed
from outside the node: on the bus, in the controller's status and in the node's statistics.

The step-by-step behaviour of the controller model under each fault is asserted in model time
by its unit tests (tests/unit/test_can_ctrl.c). These tests check that the whole chain holds
together in the running node: fault, controller, interrupt, transmit task, queue policy, bus.
"""
from __future__ import annotations

import time

import can
import pytest

from fcn_tools.bus import ID_PING, collect, is_pong, is_status, status_gaps, wait_for
from fcn_tools.node import EXIT_OK


def ping(payload: bytes) -> can.Message:
    return can.Message(arbitration_id=ID_PING, data=payload, is_extended_id=False)


def wait_status(sim, reached, timeout: float = 5.0) -> dict:
    """Poll the controller's status until `reached(status)` holds; fail with the last status."""
    deadline = time.monotonic() + timeout
    while True:
        status = sim.can_status()
        if reached(status):
            return status
        if time.monotonic() > deadline:
            pytest.fail(f"the controller did not get there within {timeout} s: {status}",
                        pytrace=False)
        time.sleep(0.005)


def assert_silent(bus, seconds: float = 0.3) -> None:
    """Nothing from the node reaches the bus. Frames that were already under way when the
    fault was applied are let through first."""
    collect(bus, 0.05)
    assert collect(bus, seconds, keep=lambda m: is_status(m) or is_pong(m)) == []


def assert_fresh_and_in_step(frames: list[can.Message]) -> None:
    """What comes out after a fault is the current value, the first frame included, not one
    that waited in a mailbox through the fault: such a value would be followed by a jump of
    the fault's whole length (tens of periods and more), far more than status_gaps() allows
    for a host that stalled the node."""
    assert len(frames) >= 10
    status_gaps(frames)


# ------------------------------------------------------------------------ no acknowledgement


def test_without_acknowledgement_the_node_goes_error_passive_and_never_bus_off(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("noack")

        # Sixteen failed attempts take the transmit counter to 128; from there the exception
        # for a node that is alone on the bus applies and the counter stays where it is.
        status = wait_status(sim, lambda s: s["state"] == "passive")
        assert status["tec"] == 128
        assert_silent(link.bus)
        later = sim.can_status()
        assert later["state"] == "passive" and later["tec"] == 128 and later["bus_off"] == 0
        assert later["tx_failed"] > status["tx_failed"] + 100      # it keeps trying

        sim.can_fault("none")
        frames = collect(link.bus, 0.4, keep=is_status)
        assert_fresh_and_in_step(frames)
        # One acknowledged frame takes the counter below 128: error active again.
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["tec"] < 128 and status["bus_off"] == 0

    assert link.node.stop() == EXIT_OK
    queue = link.node.stats()["can"]["queue"]
    assert queue["bus_off_count"] == 0
    assert queue["superseded"] > 0          # values that were replaced while nothing got out
    # Without bus off, only calling back the stale value from its mailbox kept it off the bus.
    assert queue["recall_requests"] > 0
    assert queue["produced"] == (queue["sent"] + queue["superseded"] + queue["dropped"]
                                 + queue["expired"] + queue["pending"])


# ----------------------------------------------------------------------------- shorted bus


def test_a_shorted_bus_takes_the_node_bus_off_and_it_returns_by_itself(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("short")

        # Bus off, recovery after the hold-off, bus off again because the fault is still
        # there, with the hold-off doubling each time (100, 200, 400 ms ...).
        status = wait_status(sim, lambda s: s["bus_off"] >= 3)
        assert status["rx"] == 0
        assert_silent(link.bus)

        sim.can_fault("none")
        # The node is somewhere in a hold-off of at most 1.6 s; after it, it is back.
        first = wait_for(link.bus, is_status, timeout=4.0)
        assert first is not None, "the node did not return to the bus"
        frames = [first] + collect(link.bus, 0.4, keep=is_status)
        assert_fresh_and_in_step(frames)
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["tec"] < 128
        bus_off_events = status["bus_off"]

        # And it still answers.
        link.bus.send(ping(b"\x42"))
        pong = wait_for(link.bus, is_pong, timeout=1.0)
        assert pong is not None and bytes(pong.data) == b"\x42"

    assert link.node.stop() == EXIT_OK
    stats = link.node.stats()["can"]
    queue = stats["queue"]
    assert stats["controller"]["bus_off_events"] == bus_off_events == queue["bus_off_count"]
    assert queue["restart_requests"] == queue["bus_off_count"]
    assert queue["superseded"] > 0
    assert queue["produced"] == (queue["sent"] + queue["superseded"] + queue["dropped"]
                                 + queue["expired"] + queue["pending"])
    assert "bus off: requesting recovery" in link.node.log()


# ------------------------------------------------------------------------ bus held dominant


def test_a_bus_held_dominant_stops_all_traffic_without_bus_off(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("dominant")

        # The node sees dominant bits where an idle bus should be recessive: its receive
        # counter climbs until it is error passive. It cannot send, so it never goes bus off.
        status = wait_status(sim, lambda s: s["state"] == "passive")
        assert status["rec"] >= 128 and status["tec"] == 0 and status["bus_off"] == 0
        sent_before = status["tx"]
        assert_silent(link.bus)

        # A frame from the tester cannot reach the node either.
        link.bus.send(ping(b"\x01"))
        assert wait_for(link.bus, is_pong, timeout=0.2) is None
        status = sim.can_status()
        assert status["tx"] == sent_before and status["rx_lost_fault"] >= 1
        assert status["bus_off"] == 0

        sim.can_fault("none")
        frames = collect(link.bus, 0.4, keep=is_status)
        assert_fresh_and_in_step(frames)

        # Transmitting does not lower the receive counter; the first frame the node receives
        # correctly does, and with it the node is error active again.
        assert sim.can_status()["state"] == "passive"
        link.bus.send(ping(b"\x02"))
        pong = wait_for(link.bus, is_pong, timeout=1.0)
        assert pong is not None and bytes(pong.data) == b"\x02"
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["rec"] <= 127


# ------------------------------------------------------------------------- single bit errors


def test_destroyed_transmissions_are_repeated_and_nothing_is_lost(open_link):
    link = open_link()
    with link.node.sim() as sim:
        before = sim.can_status()
        sim.can_inject("tx", 5)
        status = wait_status(sim, lambda s: s["tx_failed"] == before["tx_failed"] + 5)
        # Five errors add 40; every frame that got through since takes 1 off.
        assert 0 < status["tec"] <= 40 and status["state"] == "active"

    status_gaps(collect(link.bus, 0.4, keep=is_status))

    assert link.node.stop() == EXIT_OK
    stats = link.node.stats()["can"]
    # Each destroyed attempt was repeated: nothing was given up, nothing went bus off.
    assert stats["controller"]["tx_attempts_failed"] == 5
    assert stats["controller"]["tx_aborted"] == 0 and stats["queue"]["bus_off_count"] == 0
    assert stats["queue"]["dropped"] == 0 and stats["queue"]["expired"] == 0


def test_destroyed_receptions_do_not_reach_the_application(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_inject("rx", 3)
        for n in range(3):
            link.bus.send(ping(bytes([n])))
        # On a real bus the sender would repeat a destroyed frame; this tester does not, so
        # the three frames are simply gone. The fourth one arrives.
        link.bus.send(ping(b"\x99"))
        pongs = collect(link.bus, 0.5, keep=is_pong)
        assert [bytes(pong.data) for pong in pongs] == [b"\x99"]
        status = sim.can_status()
        assert status["rx_errors"] == 3 and status["rx"] == 1
        assert status["rec"] == 2          # three errors up, one good frame down
