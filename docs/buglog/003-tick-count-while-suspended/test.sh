#!/usr/bin/env bash
# A task keeps the scheduler suspended for 20 ms of busy work. The self-test fails if the tick
# count does not advance by the time that passed, during the suspension or in the 100 ms
# after it.
"$BUILD/tests/platform/plat_selftest" suspend 20
