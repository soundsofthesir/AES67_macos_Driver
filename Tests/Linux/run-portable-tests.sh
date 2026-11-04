#!/usr/bin/env bash
# Builds and runs the unit tests that don't need Core Audio, libASPL or
# macOS-only socket options, on Linux, with AddressSanitizer and
# UndefinedBehaviorSanitizer. CI runs this; the full suite runs on macOS
# through CMake/ctest.
#
# The source lists mirror the targets in Tests/CMakeLists.txt. Add a test here
# when it builds without Apple headers beyond the stand-ins in shim/.
#
# Usage: Tests/Linux/run-portable-tests.sh [build-dir]   (CXX picks the compiler)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SHIM="$ROOT/Tests/Linux/shim"
OUT="${1:-$ROOT/build-linux}"
CXX="${CXX:-g++}"
mkdir -p "$OUT"

# F_FULLFSYNC is macOS-only; -1 makes that fcntl() fail, so StreamConfig
# falls back to fsync() as it would on a filesystem without full sync
FLAGS=(-std=c++17 -g -O1 -Wall -Wextra -UNDEBUG
       -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
       -pthread -I"$ROOT" -I"$SHIM" -DF_FULLFSYNC=-1)

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
    elif ! (cd "$OUT" && timeout 600 "./$name"); then
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
run TestRxTimestamp     Tests/TestRxTimestamp.cpp NetworkEngine/RTP/RtpPlacement.cpp \
                        NetworkEngine/Clock/TimestampedAudioBuffer.cpp
run TestSAPListener     Tests/TestSAPListener.cpp NetworkEngine/Discovery/SAPListener.cpp
run TestPtpMessages     Tests/TestPtpMessages.cpp NetworkEngine/PTP/PtpMessages.cpp
run TestPtpBestMaster   Tests/TestPtpBestMaster.cpp NetworkEngine/PTP/PtpBestMaster.cpp NetworkEngine/PTP/PtpMessages.cpp
run TestPtpSyncPairer   Tests/TestPtpSyncPairer.cpp
run TestPtpServo        Tests/TestPtpServo.cpp NetworkEngine/PTP/PtpServo.cpp
run TestPtpClockControl Tests/TestPtpClockControl.cpp NetworkEngine/Clock/PtpClockControl.cpp NetworkEngine/PTP/PtpServo.cpp
run TestPtpSettings     Tests/TestPtpSettings.cpp NetworkEngine/PTP/PtpSettings.cpp

if ((${#failed[@]})); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All portable tests passed"
