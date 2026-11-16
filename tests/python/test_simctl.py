# SPDX-License-Identifier: MIT
"""fcn_tools.simctl, the client of the node's simulation control channel, against a small
server written here from the protocol in platform/posix/posix_sim.h. (The node's own side of
the protocol is tested in tests/platform/test_posix_sim.c, the two together in the system
tests.)"""
from __future__ import annotations

import socket
import threading

import pytest

from fcn_tools.simctl import SimControl, SimError

LOOPBACK = "127.0.0.1"


class FakeNode:
    """Answers each request with what `replies` maps its command to. A value is the text after
    the token; a callable gets the request's words and returns that text, or None to stay
    silent. Every request is recorded."""

    def __init__(self, replies):
        self.replies = replies
        self.requests: list[str] = []
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind((LOOPBACK, 0))
        self._socket.settimeout(0.05)
        self.port = self._socket.getsockname()[1]
        self._run = True
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        while self._run:
            try:
                data, sender = self._socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            token, _, command = data.decode().partition(" ")
            self.requests.append(command)
            reply = self.replies.get(command.split(" ")[0], "err unknown command")
            if callable(reply):
                reply = reply(token, command)
            if reply is not None:
                self._socket.sendto(f"{token} {reply}".encode(), sender)

    def close(self) -> None:
        self._run = False
        self._thread.join(timeout=2)
        self._socket.close()


@pytest.fixture
def fake_node():
    nodes: list[FakeNode] = []

    def _start(replies) -> FakeNode:
        node = FakeNode(replies)
        nodes.append(node)
        return node

    yield _start
    for node in nodes:
        node.close()


def test_a_reply_is_returned_as_its_key_value_pairs(fake_node):
    node = fake_node({"ping": "ok pid=4242"})
    with SimControl(node.port) as sim:
        assert sim.request("ping") == {"pid": "4242"}
        assert sim.ping() == 4242
    assert node.requests == ["ping", "ping"]


def test_can_status_converts_numbers_and_keeps_words(fake_node):
    node = fake_node({"can.status": "ok state=passive tec=128 rec=0 fault=noack tx=17 busy_ns=123456"})
    with SimControl(node.port) as sim:
        assert sim.can_status() == {"state": "passive", "tec": 128, "rec": 0, "fault": "noack",
                                    "tx": 17, "busy_ns": 123456}


def test_commands_carry_their_arguments(fake_node):
    node = fake_node({"can.fault": "ok fault=short", "can.inject": "ok tx_errors=3"})
    with SimControl(node.port) as sim:
        assert sim.can_fault("short") == {"fault": "short"}
        assert sim.can_inject("tx", 3) == {"tx_errors": "3"}
    assert node.requests == ["can.fault short", "can.inject tx 3"]


def test_an_error_reply_raises_with_the_nodes_message(fake_node):
    node = fake_node({"can.fault": "err unknown fault 'lightning' (none, short, dominant, noack)"})
    with SimControl(node.port) as sim:
        with pytest.raises(SimError, match="unknown fault 'lightning'"):
            sim.can_fault("lightning")
    assert len(node.requests) == 1          # an error is an answer: it is not asked again


def test_a_reply_with_another_token_is_not_taken_for_the_answer(fake_node):
    node = fake_node({"hello": "ok", "ping": lambda token, command: None})
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stale, \
            SimControl(node.port, timeout=0.2) as sim:
        assert sim.request("hello") == {}            # the client's socket now has its port
        client_port = sim._socket.getsockname()[1]
        # A reply with a token of an earlier request is waiting ...
        stale.sendto(b"t1 ok pid=1", (LOOPBACK, client_port))
        # ... and the node never answers this one: the old reply must not count for it.
        with pytest.raises(SimError, match="no reply"):
            sim.request("ping", retries=0)


def test_a_silent_node_is_asked_again_and_then_reported(fake_node):
    node = fake_node({"ping": lambda token, command: None})
    with SimControl(node.port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply to 'ping'"):
            sim.request("ping", retries=2)
    assert node.requests == ["ping", "ping", "ping"]


def test_a_node_that_answers_the_second_time_is_enough(fake_node):
    attempts = []

    def second_time(token, command):
        attempts.append(token)
        return "ok pid=7" if len(attempts) >= 2 else None

    node = fake_node({"ping": second_time})
    with SimControl(node.port, timeout=0.1) as sim:
        assert sim.ping() == 7
    assert len(attempts) == 2 and attempts[0] != attempts[1]     # a fresh token per attempt


def test_nothing_listening_is_reported_not_raised_as_a_socket_error():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((LOOPBACK, 0))
        free_port = probe.getsockname()[1]
    with SimControl(free_port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply"):
            sim.request("ping", retries=1)
