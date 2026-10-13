#!/usr/bin/env bash
# scripts/android_legs.sh — the Android legs, run by hand on an emulator of this run's own (README, "Running the
# platform legs by hand"). CI has no Android emulator, so these legs are the evidence for Android until it does.
#
#   bash scripts/android_legs.sh <api> <port>        e.g. bash scripts/android_legs.sh 30 5580
#
# For one API level on arm64-v8a it builds the `android-arm64-api<api>` preset (the C tests and the GoogleTest suite
# as plain device executables), creates an AVD of its own, `agent-bc-file-system-android-api<api>`, boots it headless
# on the console port it is given, pushes the test binaries into the device directory the preset names
# (BC_FILE_SYSTEM_DEVICE_TEST_ROOT, read from the build's cache so the binaries and the script agree), runs each
# there, and prints every binary's output and exit status. Whatever happens, it then stops the emulator with
# `adb emu kill` and deletes the AVD. It never touches an AVD it did not create. The exit status is 0 only when every
# test binary exited 0.
#
# ANDROID_HOME names the SDK root and JAVA_HOME the JDK that runs avdmanager; the root holds the packages the
# presets and this script name: ndk;30.0.16248370, cmdline-tools;23.0, platform-tools, emulator, and the
# system-images;android-<api>;google_apis;arm64-v8a image of each API level.
set -euo pipefail

# The SDK pieces this script runs, by their place under the SDK root.
readonly CMDLINE_TOOLS_VERSION=23.0
readonly AVD_DEVICE=pixel_7
# The API levels with a preset, and the console ports the emulator accepts (an even number; adb names the device
# emulator-<port>).
readonly API_LEVELS="30 33 35"
readonly LOWEST_PORT=5554
readonly HIGHEST_PORT=5682
# How many times the boot is polled, two seconds apart, before the run gives up: ten minutes, where a cold boot
# takes under one.
readonly BOOT_POLLS=300
# Every test binary a build of the preset makes, by its place in the build tree.
readonly TEST_BINARIES=(
    core/tests/bc_file_system_path_test
    core/tests/bc_file_system_engine_test
    core/tests/bc_file_system_resolve_test
    cpp/bc_file_system_test
)

fail() {
    printf 'FAIL android-legs: %s\n' "$1" >&2
    exit 1
}

usage() {
    printf 'usage: bash scripts/android_legs.sh <api> <port>\n' >&2
    printf '  <api>: one of %s\n' "$API_LEVELS" >&2
    printf '  <port>: an even console port from %s to %s that no running emulator uses\n' \
        "$LOWEST_PORT" "$HIGHEST_PORT" >&2
    exit 2
}

# The value a CMake cache records for `name` (written as `name:TYPE=value`), or nothing.
cached_value() {
    local cache_file=$1
    local name=$2
    sed -n "s/^${name}:[A-Z]*=//p" "$cache_file"
}

build_preset() {
    local preset=$1
    printf '=== build: cmake --preset %s, then its build\n' "$preset"
    { cmake --preset "$preset" && cmake --build --preset "$preset" -- -k 0; } \
        || fail "the $preset preset did not configure or build"
}

create_avd() {
    local avdmanager=$1
    local java_home=$2
    local avd=$3
    local system_image=$4
    local device=$5
    printf '=== avd: creating %s on %s (%s)\n' "$avd" "$system_image" "$device"
    # avdmanager asks whether to make a custom hardware profile; the answer is the device's own profile, "no".
    JAVA_HOME="$java_home" "$avdmanager" create avd --name "$avd" --package "$system_image" --device "$device" \
        <<< 'no' || fail "avdmanager could not create $avd"
}

delete_avd() {
    local avdmanager=$1
    local java_home=$2
    local avd=$3
    printf '=== avd: deleting %s\n' "$avd"
    JAVA_HOME="$java_home" "$avdmanager" delete avd --name "$avd" \
        || printf 'WARNING android-legs: avdmanager could not delete %s; delete it by that name\n' "$avd" >&2
}

# Asks the emulator to exit through its console, then waits for the process this run started. A process that is
# still there after the request is this run's own child, so it is ended by the PID recorded when it started.
stop_emulator() {
    local adb=$1
    local serial=$2
    local emulator_pid=$3
    printf '=== emulator: adb -s %s emu kill\n' "$serial"
    "$adb" -s "$serial" emu kill || printf 'WARNING android-legs: emu kill was refused\n' >&2
    local waited=0
    while kill -0 "$emulator_pid" 2> /dev/null && [ "$waited" -lt 60 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$emulator_pid" 2> /dev/null; then
        printf 'WARNING android-legs: the emulator (pid %s) outlived emu kill by 60 s; ending that process\n' \
            "$emulator_pid" >&2
        kill "$emulator_pid" || true
    fi
    wait "$emulator_pid" 2> /dev/null || true
}

# Polls the device every two seconds until Android reports the boot complete, the emulator dies, or `polls` run
# out. A device that is still starting answers the property with an error or an empty line; both mean "not yet".
wait_for_boot() {
    local adb=$1
    local serial=$2
    local emulator_pid=$3
    local emulator_log=$4
    local polls=$5
    printf '=== emulator: waiting for sys.boot_completed on %s\n' "$serial"
    local poll
    for ((poll = 1; poll <= polls; poll++)); do
        if ! kill -0 "$emulator_pid" 2> /dev/null; then
            tail -n 20 "$emulator_log" >&2
            fail "the emulator exited before it booted (its log: $emulator_log)"
        fi
        local boot_completed=""
        boot_completed=$("$adb" -s "$serial" shell getprop sys.boot_completed 2> /dev/null | tr -d '\r') || true
        if [ "$boot_completed" = 1 ]; then
            printf '=== emulator: booted after %s polls, two seconds apart\n' "$poll"
            return 0
        fi
        sleep 2
    done
    fail "$serial did not finish booting after $polls polls (the emulator's log: $emulator_log)"
}

# What the legs ran on: the API level, the ABI and the build, the kernel, the user the tests run as (the shell
# user, never root, so permission checks apply), and the file system of /data, where the device root lives.
describe_device() {
    local adb=$1
    local serial=$2
    printf '=== device: %s\n' "$serial"
    printf 'api %s, abi %s, %s\n' \
        "$("$adb" -s "$serial" shell getprop ro.build.version.sdk | tr -d '\r')" \
        "$("$adb" -s "$serial" shell getprop ro.product.cpu.abi | tr -d '\r')" \
        "$("$adb" -s "$serial" shell getprop ro.build.fingerprint | tr -d '\r')"
    printf 'kernel %s\n' "$("$adb" -s "$serial" shell uname -r | tr -d '\r')"
    "$adb" -s "$serial" shell id
    "$adb" -s "$serial" shell grep "' /data '" /proc/mounts
}

# Runs one command on the device, printing its output and its exit status; answers the status.
run_on_device() {
    local adb=$1
    local serial=$2
    local name=$3
    local command=$4
    printf '\n=== run %s: %s\n' "$name" "$command"
    local exit_code=0
    "$adb" -s "$serial" shell "$command" || exit_code=$?
    printf '=== exit %s: %s\n' "$name" "$exit_code"
    return "$exit_code"
}

main() {
    if [ "$#" -ne 2 ]; then
        usage
    fi
    local api=$1
    local port=$2
    case " $API_LEVELS " in
        *" $api "*) ;;
        *) usage ;;
    esac
    if ! [[ "$port" =~ ^[0-9]+$ ]]; then
        usage
    fi
    if [ "$port" -lt "$LOWEST_PORT" ] || [ "$port" -gt "$HIGHEST_PORT" ] || [ $((port % 2)) -ne 0 ]; then
        usage
    fi
    local sdk_root java_home
    sdk_root=$(printenv ANDROID_HOME) || fail "ANDROID_HOME is not set; it names the Android SDK root"
    java_home=$(printenv JAVA_HOME) || fail "JAVA_HOME is not set; it names the JDK that runs avdmanager"

    local adb="$sdk_root/platform-tools/adb"
    local emulator="$sdk_root/emulator/emulator"
    local avdmanager="$sdk_root/cmdline-tools/$CMDLINE_TOOLS_VERSION/bin/avdmanager"
    local tool
    for tool in "$adb" "$emulator" "$avdmanager"; do
        [ -x "$tool" ] || fail "$tool is missing from the SDK root"
    done

    local root
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    cd "$root"

    local preset="android-arm64-api$api"
    local avd="agent-bc-file-system-android-api$api"
    local system_image="system-images;android-$api;google_apis;arm64-v8a"
    local serial="emulator-$port"
    local build_directory="$root/build/$preset"

    # Each listing is read whole before it is searched, so a search that stops early cannot fail the listing.
    local devices avds
    devices=$("$adb" devices) || fail "adb could not list the running devices"
    if grep -q "^${serial}[[:space:]]" <<< "$devices"; then
        fail "$serial is already running; choose another port"
    fi
    avds=$(JAVA_HOME="$java_home" "$avdmanager" list avd -c) || fail "avdmanager could not list the AVDs"
    if grep -qx "$avd" <<< "$avds"; then
        fail "$avd already exists (a run that did not finish left it); delete it first: avdmanager delete avd --name $avd"
    fi

    build_preset "$preset"
    local device_root
    device_root=$(cached_value "$build_directory/CMakeCache.txt" BC_FILE_SYSTEM_DEVICE_TEST_ROOT)
    [ -n "$device_root" ] || fail "the $preset build records no BC_FILE_SYSTEM_DEVICE_TEST_ROOT"

    create_avd "$avdmanager" "$java_home" "$avd" "$system_image" "$AVD_DEVICE"
    # From here the AVD exists, so every exit deletes it. The values are fixed now, when the trap is set.
    # shellcheck disable=SC2064
    trap "delete_avd '$avdmanager' '$java_home' '$avd'" EXIT

    local emulator_log="$build_directory/emulator-$port.log"
    printf '=== emulator: booting %s headless on port %s (its log: %s)\n' "$avd" "$port" "$emulator_log"
    "$emulator" -avd "$avd" -port "$port" -no-window -no-audio -no-boot-anim -no-snapshot > "$emulator_log" 2>&1 &
    local emulator_pid=$!
    # From here the emulator runs too: every exit stops it, then deletes the AVD.
    # shellcheck disable=SC2064
    trap "stop_emulator '$adb' '$serial' '$emulator_pid'; delete_avd '$avdmanager' '$java_home' '$avd'" EXIT

    wait_for_boot "$adb" "$serial" "$emulator_pid" "$emulator_log" "$BOOT_POLLS"
    describe_device "$adb" "$serial"

    local binary
    local pushed=()
    for binary in "${TEST_BINARIES[@]}"; do
        pushed+=("$build_directory/$binary")
    done
    "$adb" -s "$serial" shell mkdir -p "$device_root" || fail "$device_root could not be made on the device"
    "$adb" -s "$serial" push "${pushed[@]}" "$device_root/" || fail "the binaries could not be pushed to $device_root"

    local results=()
    local failures=0
    local name exit_code
    for binary in "${TEST_BINARIES[@]}"; do
        name=$(basename "$binary")
        local command="$device_root/$name"
        case "$name" in
            # The resolve test plants its tree under the root it is handed, the device directory.
            bc_file_system_resolve_test) command="$command $device_root" ;;
            # The suite's root is compiled in (test_paths.hpp); its XML report is pulled back below.
            bc_file_system_test) command="$command --gtest_output=xml:$device_root/bc_file_system_test.xml" ;;
        esac
        exit_code=0
        run_on_device "$adb" "$serial" "$name" "$command" || exit_code=$?
        results+=("$name: exit $exit_code")
        if [ "$exit_code" -ne 0 ]; then
            failures=$((failures + 1))
        fi
    done

    local report_directory="$build_directory/android-legs"
    mkdir -p "$report_directory"
    "$adb" -s "$serial" pull "$device_root/bc_file_system_test.xml" "$report_directory/bc_file_system_test.xml" \
        || printf 'WARNING android-legs: no GoogleTest XML report to pull\n' >&2

    printf '\n=== summary: %s on %s (%s)\n' "$preset" "$serial" "$avd"
    printf '%s\n' "${results[@]}"
    printf 'GoogleTest XML: %s\n' "$report_directory/bc_file_system_test.xml"
    if [ "$failures" -ne 0 ]; then
        printf 'FAIL android-legs %s: %s of %s test binaries exited non-zero\n' "$api" "$failures" "${#results[@]}"
        exit 1
    fi
    printf 'PASS android-legs %s\n' "$api"
}

main "$@"
