#!/bin/bash
#
# Tests Installer/scripts/configure-driver without touching the machine.
# A copy of the script runs with every system path moved into a temp
# directory, and with stand-ins for the commands that change the system or
# need root (id, chown, stat, launchctl, killall, pgrep, sleep). Before it
# runs, the copy is checked for any system path left, so a new path in the
# script fails here instead of being written to as root.
#
# Cases:
#   - a symlink at the config directory is replaced by a real directory and
#     its target is left alone (#161)
#   - the install fails when the directory doesn't end up owned by
#     _coreaudiod:wheel with mode 755 (#161)
#   - a normal install succeeds and writes the keepalive service
#
# Runs on macOS (ctest) and Linux (Tests/Linux/run-portable-tests.sh).
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPT="$ROOT/Installer/scripts/configure-driver"
failures=0

fail() {
    echo "  FAIL: $1"
    [ -n "${2:-}" ] && echo "$2" | sed 's/^/    /'
    failures=$((failures + 1))
}

# Sets up $WORK with stand-ins and a redirected copy of the script.
# stat reports $STAT_RESULT, the owner and mode after chown and chmod.
setup() {
    WORK=$(mktemp -d)
    mkdir -p "$WORK/bin" "$WORK/driver.driver" "$WORK/LaunchDaemons" "$WORK/log"
    printf '#!/bin/sh\necho 0\n' > "$WORK/bin/id"
    # chown records its operands, so a case can see which paths it was given
    printf '#!/bin/sh\necho "$*" >> "%s/chown.log"\nexit 0\n' "$WORK" > "$WORK/bin/chown"
    printf '#!/bin/sh\necho "${STAT_RESULT:-_coreaudiod:wheel 755}"\n' > "$WORK/bin/stat"
    printf '#!/bin/sh\necho "$*" >> "%s/launchctl.log"\nexit 0\n' "$WORK" > "$WORK/bin/launchctl"
    printf '#!/bin/sh\necho "$*" >> "%s/killall.log"\nexit 0\n' "$WORK" > "$WORK/bin/killall"
    printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/pgrep"
    printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/sleep"
    printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/aes67ctl"
    chmod +x "$WORK"/bin/*

    # The checkout may use CRLF line endings; strip CRs so bash can parse the copy
    tr -d '\r' < "$SCRIPT" | sed \
        -e "s|^LOG_FILE=.*|LOG_FILE=\"$WORK/log/install.log\"|" \
        -e "s|^DRIVER_PATH=.*|DRIVER_PATH=\"$WORK/driver.driver\"|" \
        -e "s|^CONFIG_DIR=.*|CONFIG_DIR=\"$WORK/cfg\"|" \
        -e "s|^KEEPALIVE_PLIST=.*|KEEPALIVE_PLIST=\"$WORK/LaunchDaemons/\$KEEPALIVE_LABEL.plist\"|" \
        -e "s|^KEEPALIVE_LOG=.*|KEEPALIVE_LOG=\"$WORK/log/keepalive.log\"|" \
        -e "s|/usr/local/bin/aes67ctl|$WORK/bin/aes67ctl|g" \
        > "$WORK/configure-driver"

    # Any system path left outside comments, the plist's DTD and the temp
    # directory (under /var on macOS) would be used for real: stop first
    local left
    left=$(grep -v '^[[:space:]]*#' "$WORK/configure-driver" | grep -vF "$WORK" | grep -v 'DOCTYPE\|apple.com' |
           grep -nE '"/(Library|var|usr|etc|System)/|[[:space:]]/(Library|var|usr|etc|System)/' || true)
    if [ -n "$left" ]; then
        echo "ABORT: configure-driver uses a path this test doesn't redirect:"
        echo "$left"
        rm -rf "$WORK"
        exit 1
    fi
}

run_script() {
    OUT=$(PATH="$WORK/bin:$PATH" bash "$WORK/configure-driver" 2>&1)
    STATUS=$?
}

echo "A symlinked config directory is replaced and its target left alone"
setup
mkdir -p "$WORK/victim"
ln -s "$WORK/victim" "$WORK/cfg"
run_script
[ "$STATUS" -eq 0 ] || fail "exited $STATUS" "$OUT"
[ -d "$WORK/cfg" ] && [ ! -L "$WORK/cfg" ] || fail "config path is not a real directory" "$OUT"
[ -d "$WORK/victim" ] || fail "the symlink's target was removed" "$OUT"
grep -F -q "$WORK/victim" "$WORK/chown.log" 2>/dev/null && fail "chown was given the symlink's target" "$OUT"
rm -rf "$WORK"

echo "The install fails when the config directory has the wrong owner or mode"
setup
STAT_RESULT="root:wheel 755" run_script
[ "$STATUS" -ne 0 ] || fail "exited 0 with the directory owned by root" "$OUT"
STAT_RESULT="_coreaudiod:wheel 700" run_script
[ "$STATUS" -ne 0 ] || fail "exited 0 with the directory mode 700" "$OUT"
rm -rf "$WORK"

echo "A normal install succeeds, owns the config directory and starts the keepalive"
setup
run_script
[ "$STATUS" -eq 0 ] || fail "exited $STATUS" "$OUT"
grep -q "^_coreaudiod:wheel $WORK/cfg\$" "$WORK/chown.log" 2>/dev/null ||
    fail "chown wasn't given the config directory" "$(cat "$WORK/chown.log" 2>/dev/null)"
[ -f "$WORK/LaunchDaemons/com.aes67driver.keepalive.plist" ] || fail "keepalive plist not written" "$OUT"
grep -q "bootstrap system" "$WORK/launchctl.log" 2>/dev/null || fail "keepalive not started" "$OUT"
grep -q "coreaudiod" "$WORK/killall.log" 2>/dev/null || fail "Core Audio not restarted" "$OUT"
rm -rf "$WORK"

if [ "$failures" -ne 0 ]; then
    echo "configure-driver: $failures failed"
    exit 1
fi
echo "configure-driver: all passed"
