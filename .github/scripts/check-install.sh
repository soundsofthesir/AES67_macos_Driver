#!/usr/bin/env bash
#
# check-install.sh
# Installs the build into a temporary root with DESTDIR and asserts the layout
# the installer and the README describe. cmake --install is otherwise only
# exercised on a user's Mac, where a wrong destination is an interruption.
#
#   .github/scripts/check-install.sh [build-dir]

set -euo pipefail

BUILD="${1:-build}"
fail() { echo "ERROR: $1" >&2; exit 1; }

stage="$(mktemp -d "${TMPDIR:-/tmp}/aes67-install.XXXXXX")"
trap 'rm -rf "$stage"' EXIT

# DESTDIR also tells the install rules to skip configure-driver, which is what
# the .pkg's postinstall runs instead
DESTDIR="$stage" cmake --install "$BUILD" >/dev/null

expected=(
    "Library/Audio/Plug-Ins/HAL/AES67Driver.driver/Contents/MacOS/AES67Driver"
    "Library/Audio/Plug-Ins/HAL/AES67Driver.driver/Contents/Info.plist"
    "usr/local/bin/aes67ctl"
    "Applications/AES67Manager.app/Contents/MacOS/AES67Manager"
    "usr/local/share/doc/AES67Driver/README.md"
    "usr/local/share/doc/AES67Driver/VERSION.txt"
)
for path in "${expected[@]}"; do
    [ -e "$stage/$path" ] || fail "install is missing $path"
done

[ -x "$stage/usr/local/bin/aes67ctl" ] || fail "aes67ctl did not install executable"

shopt -s nullglob
examples=("$stage/usr/local/share/doc/AES67Driver/examples/"*.sdp)
shopt -u nullglob
[ "${#examples[@]}" -gt 0 ] || fail "no example SDP files installed"

codesign --verify --strict "$stage/Library/Audio/Plug-Ins/HAL/AES67Driver.driver" || \
    fail "installed driver bundle does not pass codesign --verify --strict"

echo "install layout: ${#expected[@]} paths and ${#examples[@]} example SDP files, signature valid"
