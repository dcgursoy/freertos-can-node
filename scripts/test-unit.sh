#!/usr/bin/env bash
# Build a preset and run the CTest suite: Unity unit tests and the platform self-tests.
#   scripts/test-unit.sh           # asan preset: AddressSanitizer + UndefinedBehaviorSanitizer
#   scripts/test-unit.sh debug
# A JUnit report is written to <build>/test-results/.
#
# Tests labelled "timing" depend on the host keeping time (for example the tick self-test
# asserts the tick period within 3 %). They may be repeated once before they count as failed;
# every other test must pass on the first run.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

preset="${1:-asan}"
dir="$FCN_BUILD_ROOT/$preset"
mkdir -p "$FCN_BUILD_ROOT"
fcn_build "$preset"
mkdir -p "$dir/test-results"

status=0
ctest --test-dir "$dir" -LE timing --output-on-failure \
    --output-junit "$dir/test-results/ctest.xml" || status=1
ctest --test-dir "$dir" -L timing --output-on-failure --repeat until-pass:2 \
    --output-junit "$dir/test-results/ctest-timing.xml" || status=1
exit "$status"
