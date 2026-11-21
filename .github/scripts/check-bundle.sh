#!/usr/bin/env bash
#
# check-bundle.sh
# Fails when the built driver is not the bundle the README promises: a
# universal binary whose minimum macOS is 13.0 and whose Info.plist version
# came from VERSION.txt.
#
#   .github/scripts/check-bundle.sh [build-dir] [source-dir]
#
# Both checks have regressed silently before: CMake ignores
# CMAKE_OSX_DEPLOYMENT_TARGET set after project(), which left the build host's
# macOS version as the driver's minimum, and an empty
# CMAKE_OSX_ARCHITECTURES makes the "universal" build host-only.

set -euo pipefail

BUILD="${1:-build}"
SOURCE="${2:-.}"
fail() { echo "ERROR: $1" >&2; exit 1; }

binary="$BUILD/AES67Driver.driver/Contents/MacOS/AES67Driver"
plist="$BUILD/AES67Driver.driver/Contents/Info.plist"
[ -f "$binary" ] || fail "no driver binary at $binary: the build failed?"
[ -f "$plist" ] || fail "no Info.plist at $plist"

archs="$(lipo -archs "$binary")"
for arch in arm64 x86_64; do
    case " $archs " in
        *" $arch "*) ;;
        *) fail "driver is missing the $arch slice (has: $archs)" ;;
    esac
done
echo "driver architectures: $archs"

minos="$(xcrun vtool -show-build "$binary" | awk '/minos/ {print $2; exit}')"
[ "$minos" = "13.0" ] || fail "driver minimum macOS is ${minos:-unknown}, expected 13.0"
echo "driver minimum macOS: $minos"

short_version="$(sed -E 's/-build\..*$//' "$SOURCE/VERSION.txt" | tr -d '[:space:]')"
build_number="$(sed -nE 's/.*-build\.([0-9]+).*/\1/p' "$SOURCE/VERSION.txt")"
[ -n "$short_version" ] && [ -n "$build_number" ] || fail "cannot read a version from VERSION.txt"

plist_short="$(plutil -extract CFBundleShortVersionString raw "$plist")"
plist_build="$(plutil -extract CFBundleVersion raw "$plist")"
[ "$plist_short" = "$short_version" ] || \
    fail "Info.plist short version is $plist_short, VERSION.txt says $short_version"
[ "$plist_build" = "$build_number" ] || \
    fail "Info.plist build is $plist_build, VERSION.txt says $build_number"
echo "driver version: $plist_short (build $plist_build)"

# The Manager app is built by its own script, not CMake, but ships from the
# same tree and must run on the same Macs
manager="$SOURCE/ManagerApp/AES67Manager.app/Contents/MacOS/AES67Manager"
if [ -f "$manager" ]; then
    manager_archs="$(lipo -archs "$manager")"
    for arch in arm64 x86_64; do
        case " $manager_archs " in
            *" $arch "*) ;;
            *) fail "Manager app is missing the $arch slice (has: $manager_archs)" ;;
        esac
    done
    echo "Manager app architectures: $manager_archs"
fi
