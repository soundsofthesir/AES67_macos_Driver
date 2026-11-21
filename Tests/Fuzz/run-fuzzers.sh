#!/usr/bin/env bash
# Builds the libFuzzer targets for the parsers that read untrusted input
# (network packets and hand-edited config files) and runs them.
#
#   Tests/Fuzz/run-fuzzers.sh [seconds] [build-dir]
#
# seconds = 0 (the default) replays the committed corpus once: deterministic,
# for every PR. seconds > 0 fuzzes each target for that long, starting from
# the corpus, with one worker per core (JOBS overrides); new inputs go to the
# build dir, crashes to <build-dir>/crashes. When a crash is fixed, add its
# input to Tests/Fuzz/corpus/<target>/.
#
# Needs clang with the libFuzzer runtime (the clang package on Linux; Xcode's
# clang or Homebrew LLVM on macOS) and, on Linux, the libuuid headers.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECONDS_PER_TARGET="${1:-0}"
OUT="${2:-$ROOT/build-fuzz}"
CXX="${CXX:-clang++}"
mkdir -p "$OUT/crashes"

# The shim and the F_FULLFSYNC override are Linux-only; on macOS the real
# headers are used.
# shellcheck disable=SC2054  # the commas are inside -fsanitize=..., not separators
FLAGS=(-std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
       -fno-omit-frame-pointer -pthread -I"$ROOT")
if [[ "$(uname -s)" != "Darwin" ]]; then
    FLAGS+=(-I"$ROOT/Tests/Linux/shim" -DF_FULLFSYNC=-1)
fi

# One worker per core: the open-ended run gets every core for the same wall
# clock, and each worker keeps its own corpus and artifacts.
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)}"
[[ "$JOBS" =~ ^[0-9]+$ ]] || JOBS=1

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export AES67_QUIET_LOG=1  # the streams.json target would log every rejected input

failed=()

fuzz() {
    local name="$1" corpus="$2"; shift 2
    local sources=()
    for source in "$@"; do sources+=("$ROOT/$source"); done
    echo "::group::$name"
    if ! "$CXX" "${FLAGS[@]}" "${sources[@]}" -o "$OUT/$name"; then
        failed+=("$name (build)")
        echo "::endgroup::"
        return
    fi
    local seeds="$ROOT/Tests/Fuzz/corpus/$corpus"
    local status=0
    if [[ "$SECONDS_PER_TARGET" == 0 ]]; then
        "$OUT/$name" -runs=0 -artifact_prefix="$OUT/crashes/$name-" "$seeds" || status=$?
    else
        mkdir -p "$OUT/corpus/$corpus"
        "$OUT/$name" -fork="$JOBS" -max_total_time="$SECONDS_PER_TARGET" -max_len=4096 -timeout=10 \
            -print_final_stats=1 -artifact_prefix="$OUT/crashes/$name-" \
            "$OUT/corpus/$corpus" "$seeds" || status=$?
    fi
    ((status == 0)) || failed+=("$name")
    echo "::endgroup::"
}

fuzz FuzzSdpParser       sdp          Tests/Fuzz/FuzzSdpParser.cpp Driver/SDPParser.cpp Shared/Types.cpp
fuzz FuzzPtpMessages     ptp          Tests/Fuzz/FuzzPtpMessages.cpp NetworkEngine/PTP/PtpMessages.cpp
fuzz FuzzSapAnnouncement sap          Tests/Fuzz/FuzzSapAnnouncement.cpp NetworkEngine/Discovery/SAPListener.cpp \
                                      Driver/SDPParser.cpp Shared/Types.cpp
fuzz FuzzRtspClient      rtsp         Tests/Fuzz/FuzzRtspClient.cpp NetworkEngine/Discovery/RTSPClient.cpp \
                                      Driver/SDPParser.cpp Shared/Types.cpp
fuzz FuzzJson            json         Tests/Fuzz/FuzzJson.cpp NetworkEngine/Json.cpp
fuzz FuzzStreamConfig    streamconfig Tests/Fuzz/FuzzStreamConfig.cpp NetworkEngine/StreamConfig.cpp NetworkEngine/Json.cpp \
                                      NetworkEngine/NetworkUtils.cpp NetworkEngine/StreamChannelMapper.cpp \
                                      Driver/SDPParser.cpp Shared/Types.cpp
fuzz FuzzPtpSettings     ptpsettings  Tests/Fuzz/FuzzPtpSettings.cpp NetworkEngine/PTP/PtpSettings.cpp

if ((${#failed[@]})); then
    echo "Failed: ${failed[*]}"
    ls -1 "$OUT/crashes" 2>/dev/null || true
    exit 1
fi
echo "All fuzz targets passed"
