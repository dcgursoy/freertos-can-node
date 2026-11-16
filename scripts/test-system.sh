#!/usr/bin/env bash
# Python tests: unit tests of the tools (tests/python) and system tests (tests/system), which
# start the node and observe it from outside.
#   scripts/test-system.sh                 # all tests
#   scripts/test-system.sh -k smoke -x     # extra arguments go to pytest
# Output in $FCN_BUILD_ROOT/system-test/: JUnit reports and logs-*/<test>/ with the node's log,
# statistics and data files of every test.
#
# Three runs:
#  1. everything not marked "timing", against the release build of the node;
#  2. the same again against the asan build (AddressSanitizer and UndefinedBehaviorSanitizer),
#     so that the application and node_main.c run under the sanitizers too; a sanitizer report
#     ends the node with a non-zero status or writes to stderr, and either fails the test;
#  3. the tests marked "timing", which assert on the host's wall clock. If they fail, they are
#     repeated once, but only when every failure was a missed timing tolerance
#     (fcn_tools.timing); any other failure, a crash or a wrong value, fails at once.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

python="$(fcn_python)"
if ! "$python" -c 'import pytest, fcn_tools' >/dev/null 2>&1; then
    echo "pytest or fcn_tools missing in $FCN_VENV (run scripts/setup-python.sh)" >&2
    exit 1
fi

mkdir -p "$FCN_BUILD_ROOT"
fcn_build release
fcn_build asan
out="$FCN_BUILD_ROOT/system-test"
mkdir -p "$out"
cd "$FCN_ROOT"

# run_pytest <tag> <pytest arguments...>; "no tests selected" (exit status 5) is not a failure.
run_pytest() {
    local tag="$1"
    shift
    local rc=0
    "$python" -m pytest tests -o cache_dir="$out/.pytest_cache" \
        --junitxml="$out/pytest-$tag.xml" --artifacts "$out/logs-$tag" "$@" || rc=$?
    [[ "$rc" -eq 5 ]] && rc=0
    return "$rc"
}

status=0
run_pytest functional -m "not timing" "$@" || status=1

FCN_NODE="$FCN_BUILD_ROOT/asan/bin/fcn_node" \
    ASAN_OPTIONS="detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    run_pytest functional-asan -m "not timing" "$@" || status=1

report="$out/timing-failures.txt"
rm -f "$report"
if ! run_pytest timing -m timing --timing-report "$report" "$@"; then
    if [[ -s "$report" ]] && ! grep -qv '^miss ' "$report"; then
        echo "timing tolerances missed, repeating those tests once:"
        cat "$report"
        run_pytest timing-retry -m timing --last-failed "$@" || status=1
    else
        status=1
    fi
fi
exit "$status"
