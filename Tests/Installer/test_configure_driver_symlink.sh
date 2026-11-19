#!/bin/bash
#
# Regression test for Installer/scripts/configure-driver (#161).
# A symlink at the configuration directory must be replaced by a real
# directory without touching the symlink's target, and the script must not
# report success unless it prepared that directory.
#
# Paths and the root check are redirected to a temp directory, and chown and
# stat are stubbed, so the script runs unchanged on a non-macOS machine.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPT="$ROOT/Installer/scripts/configure-driver"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/bin" "$WORK/driver.driver" "$WORK/victim"
ln -s "$WORK/victim" "$WORK/cfg"
printf '#!/bin/sh\necho 0\n' > "$WORK/bin/id"
# chown records its operands, so the test can see which paths it was given.
printf '#!/bin/sh\necho "$*" >> "%s/chown.log"\nexit 0\n' "$WORK" > "$WORK/bin/chown"
# stat reports the owner and mode that a successful chown and chmod produce.
printf '#!/bin/sh\necho "_coreaudiod:wheel 755"\n' > "$WORK/bin/stat"
chmod +x "$WORK/bin/id" "$WORK/bin/chown" "$WORK/bin/stat"

# The checkout may use CRLF line endings; strip CRs so bash can parse the copy.
tr -d '\r' < "$SCRIPT" | sed -e "s|^DRIVER_PATH=.*|DRIVER_PATH=\"$WORK/driver.driver\"|" \
    -e "s|^CONFIG_DIR=.*|CONFIG_DIR=\"$WORK/cfg\"|" \
    -e "s|^LOG_FILE=.*|LOG_FILE=\"$WORK/install.log\"|" > "$WORK/configure-driver" || exit 1

out=$(PATH="$WORK/bin:$PATH" bash "$WORK/configure-driver" 2>&1)
status=$?

if [ -L "$WORK/cfg" ] && [ "$status" -eq 0 ]; then
    echo "FAIL: configure-driver reported success (exit 0) with a symlinked config directory"
    echo "$out"
    exit 1
fi
if grep -F -q "$WORK/victim" "$WORK/chown.log" 2>/dev/null; then
    echo "FAIL: configure-driver changed the owner of the symlink target"
    echo "$out"
    exit 1
fi
if [ "$status" -ne 0 ]; then
    echo "FAIL: configure-driver exited $status instead of preparing the replaced config directory"
    echo "$out"
    exit 1
fi
if [ ! -d "$WORK/cfg" ] || [ -L "$WORK/cfg" ]; then
    echo "FAIL: config path is not a real directory after install"
    echo "$out"
    exit 1
fi
echo "PASS: configure-driver replaced the symlink with a directory and left its target alone"
exit 0
