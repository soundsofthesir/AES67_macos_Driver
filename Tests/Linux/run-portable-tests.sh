#!/usr/bin/env bash
# Builds and runs the unit tests that don't need Core Audio, libASPL or
# macOS-only socket options, on Linux, with AddressSanitizer and
# UndefinedBehaviorSanitizer. CI runs this; the full suite runs on macOS
# through CMake/ctest.
#
# The source lists mirror the targets in Tests/CMakeLists.txt. Add a test here
# when it builds without Apple headers beyond the stand-ins in shim/.
#
# Needs the libuuid headers (Debian/Ubuntu: uuid-dev) for Shared/Types.h.
#
# Usage: Tests/Linux/run-portable-tests.sh [build-dir]   (CXX picks the compiler)
#        COVERAGE=1 ... adds llvm-cov instrumentation and writes one .profraw
#        per test into the build dir (ci.yml merges them).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SHIM="$ROOT/Tests/Linux/shim"
OUT="${1:-$ROOT/build-linux}"
CXX="${CXX:-g++}"
mkdir -p "$OUT"
# Absolute from here on: the tests run with $OUT as their working directory,
# and LLVM_PROFILE_FILE below must not be resolved a second time
OUT="$(cd "$OUT" && pwd)"

# F_FULLFSYNC is macOS-only; -1 makes that fcntl() fail, so StreamConfig
# falls back to fsync() as it would on a filesystem without full sync.
# shellcheck disable=SC2054  # the commas are inside -fsanitize=..., not separators
FLAGS=(-std=c++17 -g -O1 -Wall -Wextra -UNDEBUG
       -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
       -pthread -I"$ROOT" -I"$SHIM" -DF_FULLFSYNC=-1)

# COVERAGE=1 instruments every binary for llvm-cov (see ci.yml).
if [[ "${COVERAGE:-0}" == "1" ]]; then
    FLAGS+=(-fprofile-instr-generate -fcoverage-mapping)
fi

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

failed=()

run() {
    local name="$1"; shift
    local sources=()
    for source in "$@"; do sources+=("$ROOT/$source"); done
    echo "::group::$name"
    if ! "$CXX" "${FLAGS[@]}" "${EXTRA_DEFS[@]}" "${sources[@]}" -o "$OUT/$name"; then
        echo "BUILD FAILED: $name"
        failed+=("$name (build)")
    elif ! (cd "$OUT" && LLVM_PROFILE_FILE="$OUT/$name-%p.profraw" timeout 600 "./$name"); then
        echo "TEST FAILED: $name"
        failed+=("$name")
    fi
    echo "::endgroup::"
}

EXTRA_DEFS=()
run TestSDPParser       Tests/TestSDPParser.cpp Driver/SDPParser.cpp Shared/Types.cpp
run TestChannelMapper   Tests/TestChannelMapper.cpp NetworkEngine/StreamChannelMapper.cpp Shared/Types.cpp
run TestRingBuffer      Tests/TestRingBuffer.cpp
run TestStreamManager   Tests/TestStreamManager.cpp Driver/SDPParser.cpp NetworkEngine/StreamChannelMapper.cpp Shared/Types.cpp
run TestStreamConfig    Tests/TestStreamConfig.cpp NetworkEngine/Json.cpp NetworkEngine/StreamConfig.cpp \
                        NetworkEngine/NetworkUtils.cpp NetworkEngine/StreamChannelMapper.cpp \
                        Driver/SDPParser.cpp Shared/Types.cpp
run TestStreamSetup     Tests/TestStreamSetup.cpp NetworkEngine/StreamSetup.cpp NetworkEngine/Json.cpp \
                        NetworkEngine/StreamConfig.cpp NetworkEngine/NetworkUtils.cpp \
                        NetworkEngine/StreamChannelMapper.cpp NetworkEngine/PTP/PtpSettings.cpp \
                        Driver/SDPParser.cpp Shared/Types.cpp
run TestAes67ctl       Tests/TestAes67ctl.cpp Tools/aes67ctl/Checks.cpp Tools/aes67ctl/PacketProbe.cpp
run TestRxTimestamp     Tests/TestRxTimestamp.cpp NetworkEngine/RTP/RtpPlacement.cpp \
                        NetworkEngine/Clock/TimestampedAudioBuffer.cpp
run TestSAPListener     Tests/TestSAPListener.cpp NetworkEngine/Discovery/SAPListener.cpp
run TestRtspClient      Tests/TestRtspClient.cpp NetworkEngine/Discovery/RTSPClient.cpp \
                        Driver/SDPParser.cpp Shared/Types.cpp
run TestPtpMessages     Tests/TestPtpMessages.cpp NetworkEngine/PTP/PtpMessages.cpp
run TestPtpBestMaster   Tests/TestPtpBestMaster.cpp NetworkEngine/PTP/PtpBestMaster.cpp NetworkEngine/PTP/PtpMessages.cpp
run TestPtpSyncPairer   Tests/TestPtpSyncPairer.cpp
run TestPtpServo        Tests/TestPtpServo.cpp NetworkEngine/PTP/PtpServo.cpp
# TestPtpServoScenarios has its own CI job (ci.yml, servo-scenarios)
run TestPtpClockControl Tests/TestPtpClockControl.cpp NetworkEngine/Clock/PtpClockControl.cpp NetworkEngine/PTP/PtpServo.cpp
run TestPtpSettings     Tests/TestPtpSettings.cpp NetworkEngine/PTP/PtpSettings.cpp
# Source-level check of the device's channel count (libASPL isn't available here)
run TestDeviceChannelCount Tests/TestDeviceChannelCount.cpp

# Shell tests that stand in for every system command they would change
echo "::group::Installer configure-driver"
if ! bash "$ROOT/Tests/Installer/test-configure-driver.sh"; then
    echo "TEST FAILED: Installer configure-driver"
    failed+=("Installer configure-driver")
fi
echo "::endgroup::"

if ((${#failed[@]})); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All portable tests passed"
