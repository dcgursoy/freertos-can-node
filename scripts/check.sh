#!/usr/bin/env bash
# Everything that has to pass before a commit, in the order CI runs it.
#   scripts/check.sh
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
here="$FCN_ROOT/scripts"

step() {
    echo
    echo "################################################################ $*"
}

step "static analysis"
"$here/static-analysis.sh"

step "build: debug and release, warnings as errors"
"$here/build.sh" debug release

step "unit tests and platform self-tests under ASan + UBSan"
"$here/test-unit.sh" asan

step "coverage of lib/"
"$here/coverage.sh"

step "system tests"
"$here/test-system.sh"

echo
echo "check: all steps passed"
