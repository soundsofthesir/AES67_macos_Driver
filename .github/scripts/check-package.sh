#!/usr/bin/env bash
#
# check-package.sh
# Expands the unsigned .pkg built by Installer/build-installer.sh and asserts
# what it installs, so a packaging change that drops the driver or aes67ctl
# fails here instead of on a user's Mac.
#
#   .github/scripts/check-package.sh [pkg]

set -euo pipefail

fail() { echo "ERROR: $1" >&2; exit 1; }

pkg="${1:-}"
if [ -z "$pkg" ]; then
    pkg="$(ls -1 Installer/output/AES67Driver-*.pkg 2>/dev/null | head -1 || true)"
fi
[ -n "$pkg" ] && [ -f "$pkg" ] || fail "no package: run Installer/build-installer.sh first"

version="$(sed -E 's/-build\.[0-9]+//' VERSION.txt | tr -d '[:space:]')"
case "$(basename "$pkg")" in
    "AES67Driver-$version-"*.pkg) ;;
    *) fail "$(basename "$pkg") does not carry version $version from VERSION.txt" ;;
esac

files="$(pkgutil --payload-files "$pkg")"
for expected in \
    "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver" \
    "/usr/local/bin/aes67ctl"; do
    grep -Fq -- "$expected" <<<"$files" || fail "package payload is missing $expected"
done

# The install scripts are what create the keepalive daemon and its config
expanded="$(mktemp -d "${TMPDIR:-/tmp}/aes67-pkg.XXXXXX")"
trap 'rm -rf "$expanded"' EXIT
pkgutil --expand "$pkg" "$expanded/pkg"
[ -f "$expanded/pkg/Distribution" ] || fail "expanded package has no Distribution"
scripts="$(find "$expanded/pkg" -maxdepth 2 -type d -name Scripts | head -1)"
[ -n "$scripts" ] || fail "expanded package has no install scripts"
for script in preinstall postinstall configure-driver; do
    [ -f "$scripts/$script" ] || fail "install scripts are missing $script"
done

echo "package $pkg contains the driver, aes67ctl and the install scripts"
