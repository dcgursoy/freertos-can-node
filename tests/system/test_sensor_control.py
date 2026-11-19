"""The sensor front-end model in the running node: the --sensor-seed option, and the sensor.*
commands of simulation control served by the node.

What the model produces, that the same seed gives the same readings, and every reply of the
commands are tested without a node in tests/platform/test_posix_sensor.c; what the application
makes of the readings belongs to the tests of the application."""
from __future__ import annotations

import pytest

from fcn_tools.node import EXIT_OK
from fcn_tools.simctl import SENSOR_CHANNELS, SimError


def test_the_node_serves_the_sensor_commands_with_the_seed_it_was_given(start_node):
    node = start_node("--sensor-seed", "4294967295")
    with node.sim() as sim:
        status = sim.sensor_status()
        assert status["seed"] == 4294967295
        assert status["offline"] == "off"
        assert all(status[channel] == "model" for channel in SENSOR_CHANNELS)

        sim.sensor_set("cell3", 4300)
        sim.sensor_open("temp2")
        sim.sensor_set("current", -400000)
        sim.sensor_offline(True)
        status = sim.sensor_status()
        assert (status["cell3"], status["temp2"], status["current"], status["offline"]) == \
            (4300, "open", -400000, "on")

        with pytest.raises(SimError, match="unknown channel 'cell9'"):
            sim.sensor_open("cell9")
        with pytest.raises(SimError, match="a value for cell1 is a whole number from 0 to 65535"):
            sim.sensor_set("cell1", 70000)

        sim.sensor_clear()
        status = sim.sensor_status()
        assert status["offline"] == "off"
        assert all(status[channel] == "model" for channel in SENSOR_CHANNELS)
    assert node.stop() == EXIT_OK


def test_without_the_option_the_seed_is_1(start_node):
    node = start_node()
    with node.sim() as sim:
        assert sim.sensor_status()["seed"] == 1
    assert node.stop() == EXIT_OK
