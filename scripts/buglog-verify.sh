#!/usr/bin/env bash
# Prove that every entry of the bug log is real and still detected.
#   scripts/buglog-verify.sh
#
# Each directory docs/buglog/<id>/ holds
#   reintroduce.patch   puts the bug back into the current sources
#   test.sh             runs the test that caught it (exit status 0 = the test passes);
#                       it is started in the scratch source tree with BUILD set to the build
#                       directory of the debug preset
# For each entry this script applies the patch to a scratch copy of the sources, builds, runs
# the test and requires it to FAIL. An entry whose bug can no longer be re-created, or whose
# test no longer notices it, fails the script.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

work="$FCN_BUILD_ROOT/buglog-verify"
src="$work/src"
build="$work/build"
rm -rf "$work"
mkdir -p "$src"

# Scratch copy of the project's own files; third-party code is linked, not copied.
for item in CMakeLists.txt CMakePresets.json cmake config lib app platform hal dbc tests tools \
    bench scripts pyproject.toml requirements.txt; do
    [[ -e "$FCN_ROOT/$item" ]] && cp -a "$FCN_ROOT/$item" "$src/"
done
ln -s "$FCN_ROOT/third_party" "$src/third_party"

shopt -s nullglob
entries=("$FCN_ROOT"/docs/buglog/*/)
if [[ ${#entries[@]} -eq 0 ]]; then
    echo "bug log: no entries"
    exit 0
fi

status=0
for entry in "${entries[@]}"; do
    id="$(basename "$entry")"
    if [[ ! -f "$entry/reintroduce.patch" || ! -f "$entry/test.sh" ]]; then
        echo "$id: incomplete entry (needs reintroduce.patch and test.sh)"
        status=1
        continue
    fi

    if ! patch -p1 -s -d "$src" <"$entry/reintroduce.patch"; then
        echo "$id: reintroduce.patch no longer applies to the sources"
        status=1
        continue
    fi

    log="$work/$id.log"
    if ! { cmake --preset debug -S "$src" -B "$build" && cmake --build "$build"; } >"$log" 2>&1; then
        # A bug that only shows as a build failure is still caught.
        echo "$id: caught (the build fails with the bug re-created)"
    elif (cd "$src" && BUILD="$build" FCN_BUILD_ROOT="$work" bash "$entry/test.sh") >>"$log" 2>&1; then
        echo "$id: NOT caught: the test passes with the bug re-created (see $log)"
        status=1
    else
        echo "$id: caught (the test fails with the bug re-created)"
    fi

    patch -R -p1 -s -d "$src" <"$entry/reintroduce.patch"
done
exit "$status"
