#!/usr/bin/env bash
# System tests: pytest starts the release build of the node and observes it from outside.
#   scripts/test-system.sh                 # all tests
#   scripts/test-system.sh -k smoke -x     # extra arguments go to pytest
# Output in $FCN_BUILD_ROOT/system-test/: JUnit reports and logs*/<test>/ with the node's log,
# statistics and data files of every test.
#
# Tests marked "timing" assert on the host's wall clock. They run separately and the ones
# that fail are repeated once before they count as failed; every other test must pass on the
# first run.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

python="$(fcn_python)"
if ! "$python" -c 'import pytest, fcn_tools' >/dev/null 2>&1; then
    echo "pytest or fcn_tools missing in $FCN_VENV (run scripts/setup-python.sh)" >&2
    exit 1
fi

mkdir -p "$FCN_BUILD_ROOT"
fcn_build release
out="$FCN_BUILD_ROOT/system-test"
mkdir -p "$out"
cd "$FCN_ROOT"

# run_pytest <tag> <pytest arguments...>; "no tests selected" (exit status 5) is not a failure.
run_pytest() {
    local tag="$1"
    shift
    local rc=0
    # The test directory is named explicitly: pytest only knows the options that
    # tests/system/conftest.py adds when that directory is on the command line.
    "$python" -m pytest tests/system -o cache_dir="$out/.pytest_cache" \
        --junitxml="$out/pytest-$tag.xml" --artifacts "$out/logs-$tag" "$@" || rc=$?
    [[ "$rc" -eq 5 ]] && rc=0
    return "$rc"
}

status=0
run_pytest functional -m "not timing" "$@" || status=1
run_pytest timing -m timing "$@" ||
    run_pytest timing-retry -m timing --last-failed "$@" ||
    status=1
exit "$status"
