#!/usr/bin/env bash
# Static analysis of the project's own C code. Third-party code is never analysed.
#
#   scripts/static-analysis.sh               # everything below
#   scripts/static-analysis.sh --format-only # just the formatting check
#
#   1. clang-format   all project sources match .clang-format
#   2. cppcheck       warning, style, performance and portability checks on all project code
#   3. cppcheck MISRA the MISRA C:2012 addon on lib/ and app/, the code that would ship on a
#                     microcontroller; deviations are listed in config/misra-suppressions.txt
#                     and explained in docs/misra-deviations.md
#   4. clang-tidy     the checks in .clang-tidy, warnings as errors
#
# Any finding fails the script. Reports are written to $FCN_BUILD_ROOT/analysis/.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

out="$FCN_BUILD_ROOT/analysis"
mkdir -p "$out"
status=0

# ---------------------------------------------------------------- 1. formatting
echo "==> clang-format"
mapfile -t files < <(fcn_project_sources)
if clang-format --dry-run --Werror --style=file "${files[@]}" >"$out/clang-format.log" 2>&1; then
    echo "    ${#files[@]} files formatted correctly"
else
    grep -E 'error|warning' "$out/clang-format.log" | head -40
    echo "    formatting differs: run scripts/format.sh"
    status=1
fi
[[ "${1:-}" == "--format-only" ]] && exit "$status"

# The analysers read the compile commands of the debug build, reduced to project sources.
fcn_build debug >/dev/null
full_db="$FCN_BUILD_ROOT/debug/compile_commands.json"
filter="$FCN_ROOT/scripts/filter_compile_db.py"
db_tidy="$out/db-tidy"       # all project code, as compiled (for clang-tidy)
db_all="$out/db-all"         # all project code, third-party headers visible to cppcheck
db_misra="$out/db-misra"     # lib/ and app/ only
python3 "$filter" "$full_db" "$db_tidy" "$FCN_ROOT" "${FCN_CODE_DIRS[@]}"
python3 "$filter" --plain-includes "$full_db" "$db_all" "$FCN_ROOT" "${FCN_CODE_DIRS[@]}"
python3 "$filter" --plain-includes "$full_db" "$db_misra" "$FCN_ROOT" lib app

cppcheck_common=(
    --std=c11 --platform=unix64
    --inline-suppr
    --suppress=missingIncludeSystem
    "--suppress=*:*/third_party/*"
    --error-exitcode=1
    --quiet
)

# ---------------------------------------------------------------- 2. cppcheck
echo "==> cppcheck (all project code)"
mkdir -p "$out/cppcheck-build-all"
if cppcheck --project="$db_all/compile_commands.json" \
    --cppcheck-build-dir="$out/cppcheck-build-all" \
    --enable=warning,style,performance,portability \
    "${cppcheck_common[@]}" 2>"$out/cppcheck.log"; then
    echo "    no findings"
else
    cat "$out/cppcheck.log"
    status=1
fi

# ---------------------------------------------------------------- 3. cppcheck MISRA
echo "==> cppcheck MISRA C:2012 addon (lib/ and app/)"
mkdir -p "$out/cppcheck-build-misra"
# The addon reports through the 'style' severity, so that has to be enabled for it to speak.
if cppcheck --project="$db_misra/compile_commands.json" \
    --cppcheck-build-dir="$out/cppcheck-build-misra" \
    --addon=misra --enable=style \
    --suppressions-list="$FCN_ROOT/config/misra-suppressions.txt" \
    "${cppcheck_common[@]}" 2>"$out/cppcheck-misra.log"; then
    echo "    no findings outside the documented deviations"
else
    cat "$out/cppcheck-misra.log"
    echo "    $(grep -c 'misra-c2012' "$out/cppcheck-misra.log" || true) MISRA finding(s)"
    status=1
fi

# ---------------------------------------------------------------- 4. clang-tidy
echo "==> clang-tidy"
tidy_status=0
: >"$out/clang-tidy.log"
mapfile -t units < <(python3 -c '
import json, sys
for entry in json.load(open(sys.argv[1])):
    print(entry["file"])
' "$db_tidy/compile_commands.json")
for unit in "${units[@]}"; do
    clang-tidy -p "$db_tidy" --quiet --extra-arg=-Wno-unknown-warning-option "$unit" \
        >>"$out/clang-tidy.log" 2>&1 || tidy_status=1
done
if [[ "$tidy_status" -eq 0 ]]; then
    echo "    ${#units[@]} translation units clean"
else
    grep -E 'warning:|error:' "$out/clang-tidy.log" | sort -u | head -80
    status=1
fi

# ---------------------------------------------------------------- 5. the analysers' own test
# "No findings" only means something if the same tools, invoked the same way, do find a defect
# that is known to be there. scripts/selftest/planted_violations.c holds one for each of them.
echo "==> analyser self-test (planted violations)"
planted="$FCN_ROOT/scripts/selftest/planted_violations.c"
cppcheck --std=c11 --platform=unix64 --addon=misra --enable=warning,style --quiet \
    "$planted" 2>"$out/selftest-cppcheck.log" || true
clang-tidy --quiet "$planted" -- -std=c11 >"$out/selftest-clang-tidy.log" 2>&1 || true
clang-format --dry-run --Werror --style=file "$planted" >"$out/selftest-clang-format.log" 2>&1 &&
    format_caught=0 || format_caught=1

expect_finding() { # <log file> <finding id> <tool name>
    if grep -q "$2" "$1"; then
        echo "    $3 reports $2"
    else
        echo "    SELF-TEST FAILED: $3 did not report the planted $2"
        status=1
    fi
}
expect_finding "$out/selftest-cppcheck.log" "arrayIndexOutOfBounds" "cppcheck"
expect_finding "$out/selftest-cppcheck.log" "misra-c2012-15.6" "cppcheck MISRA addon"
expect_finding "$out/selftest-clang-tidy.log" "clang-analyzer-core.NullDereference" "clang-tidy"
if [[ "$format_caught" -eq 1 ]]; then
    echo "    clang-format reports the unbraced if"
else
    echo "    SELF-TEST FAILED: clang-format accepted the planted formatting defect"
    status=1
fi

if [[ "$status" -eq 0 ]]; then
    echo "static analysis: clean"
else
    echo "static analysis: FINDINGS (reports in $out)"
fi
exit "$status"
