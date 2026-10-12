#!/usr/bin/env bash
# scripts/verify.sh — the one verify script. "The build passes" means this script exits 0 (AGENTS.md,
# "Verification"). The IDE run configuration calls it with no arguments and gets every leg; CI calls it once
# per job with that job's leg names, so what CI runs and what a developer runs is one definition.
#
#   bash scripts/verify.sh                      every leg, in order
#   bash scripts/verify.sh guards swift         only the named legs
#
# Every leg prints PASS <leg> or FAIL <leg>; the first FAIL ends the run with a non-zero status. A leg whose tool
# is missing is a FAIL, never a silent skip, except clang-tidy, which is report-only this stage and announces its
# absence on standard error.
set -euo pipefail

ALL_LEGS="build-debug build-sanitized header-four-ways guards consumer swift clang-tidy"

pass() {
    printf 'PASS %s\n' "$1"
}

fail() {
    printf 'FAIL %s: %s\n' "$1" "$2"
    exit 1
}

# One preset end to end: configure, build, run the suite. The three are chained with && because a function
# called on the left of || runs without set -e, and would otherwise answer with its last command alone.
build_and_test_preset() {
    local preset=$1
    cmake --preset "$preset" && cmake --build --preset "$preset" && ctest --preset "$preset"
}

leg_build_debug() {
    build_and_test_preset debug || fail build-debug "the debug preset did not configure, build or pass its tests"
    pass build-debug
}

leg_build_sanitized() {
    build_and_test_preset sanitized || fail build-sanitized "the sanitized preset did not configure, build or pass its tests"
    pass build-sanitized
}

# The public header compiled four ways (strict C11, gnu11 as Swift's importer parses it, C++20, C++23) and
# nothing else: the four object-library targets the build preset names.
leg_header_four_ways() {
    { cmake --preset debug && cmake --build --preset header-four-ways; } \
        || fail header-four-ways "the public header does not compile in one of its four modes"
    pass header-four-ways
}

# Each guard proves first that it can fail, then scans the tree.
leg_guards() {
    python3 scripts/check_house_rules.py --selftest || fail guards "check_house_rules.py --selftest"
    python3 scripts/check_house_rules.py . || fail guards "check_house_rules.py found a violation or was blind"
    python3 scripts/check_build_parity.py --selftest || fail guards "check_build_parity.py --selftest"
    python3 scripts/check_build_parity.py . || fail guards "the three builds disagree on the C standard or the source list"
    python3 scripts/check_disabled_cases.py --selftest || fail guards "check_disabled_cases.py --selftest"
    python3 scripts/check_disabled_cases.py . || fail guards "the disabled-case count does not match its pin"
    pass guards
}

# The library built the way a consumer builds it: through FetchContent, from this checkout, with no URL yet.
leg_consumer() {
    local root=$1
    {
        cmake -S examples/consumer -B build/consumer -DFETCHCONTENT_SOURCE_DIR_BC_FILE_SYSTEM="$root" \
            && cmake --build build/consumer \
            && build/consumer/consumer
    } || fail consumer "the FetchContent consumer did not configure, build or run"
    pass consumer
}

leg_swift() {
    command -v swift > /dev/null || fail swift "swift is not on PATH; the Swift package was not tested"
    swift test || fail swift "swift test"
    pass swift
}

# Report-only this stage: the findings are printed and the leg passes whenever clang-tidy ran. Two sweeps, one
# per language directory, each under that directory's own .clang-tidy (clang-tidy reads the nearest one).
leg_clang_tidy() {
    if ! command -v clang-tidy > /dev/null; then
        printf 'SKIP clang-tidy: clang-tidy is not on PATH; the lint ran nothing\n' >&2
        return 0
    fi
    [ -f build/debug/compile_commands.json ] || fail clang-tidy "no build/debug/compile_commands.json; run the build-debug leg first"
    # A Homebrew clang-tidy on macOS does not know Apple's SDK, and the compile database written for Apple's
    # clang names no sysroot, so the system headers are supplied here; elsewhere nothing is added.
    local sysroot_arguments=""
    if [ "$(uname)" = Darwin ] && command -v xcrun > /dev/null; then
        sysroot_arguments="--extra-arg=-isysroot$(xcrun --show-sdk-path)"
    fi
    local c_units cpp_units
    c_units=$(find core -name '*.c' | sort)
    cpp_units=$(find cpp -name '*.cpp' | sort)
    # shellcheck disable=SC2086  # the unit lists are whitespace-separated paths with no spaces in them
    clang-tidy -p build/debug --quiet $sysroot_arguments $c_units || true
    # shellcheck disable=SC2086
    clang-tidy -p build/debug --quiet $sysroot_arguments $cpp_units || true
    pass clang-tidy
}

run_leg() {
    local leg=$1
    local root=$2
    case "$leg" in
        build-debug) leg_build_debug ;;
        build-sanitized) leg_build_sanitized ;;
        header-four-ways) leg_header_four_ways ;;
        guards) leg_guards ;;
        consumer) leg_consumer "$root" ;;
        swift) leg_swift ;;
        clang-tidy) leg_clang_tidy ;;
        *) fail "$leg" "unknown leg; the legs are: $ALL_LEGS" ;;
    esac
}

main() {
    local root
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    cd "$root"
    local legs="$ALL_LEGS"
    if [ "$#" -gt 0 ]; then
        legs="$*"
    fi
    local leg
    for leg in $legs; do
        run_leg "$leg" "$root"
    done
}

main "$@"
