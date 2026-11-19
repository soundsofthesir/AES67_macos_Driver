#!/bin/bash
#
# build-installer.sh
# Builds the .pkg that installs the driver, aes67ctl and the keepalive
# LaunchDaemon (the daemon is written by scripts/configure-driver).
#
#   ./build-installer.sh            package what is already built
#   ./build-installer.sh --build    rebuild first (incrementally)
#
# Signing is automatic when the keychain has the certificates:
#   "Developer ID Application"  signs the driver and aes67ctl
#   "Developer ID Installer"    signs the .pkg
# Set DEVELOPER_ID_APP / DEVELOPER_ID_INSTALLER to pick one when there are
# several. Notarization runs when NOTARY_PROFILE names a profile made with
#   xcrun notarytool store-credentials <profile> --apple-id ... --team-id ...
# A release must be signed and notarized; set REQUIRE_SIGNING=1 to fail
# instead of building an unsigned package.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
OUTPUT_DIR="$SCRIPT_DIR/output"
PAYLOAD_DIR="$SCRIPT_DIR/payload"
SCRIPTS_DIR="$SCRIPT_DIR/scripts"

DRIVER_NAME="AES67Driver.driver"
DRIVER_BUILD_PATH="$BUILD_DIR/$DRIVER_NAME"
CTL_BUILD_PATH="$BUILD_DIR/aes67ctl"

PACKAGE_ID="com.aes67.driver"
# 0.1.0 from VERSION.txt's "0.1.0-build.19"
PACKAGE_VERSION="$(sed -E 's/-build\.[0-9]+//' "$PROJECT_ROOT/VERSION.txt" | tr -d '[:space:]')"
FINAL_PKG="$OUTPUT_DIR/AES67Driver-${PACKAGE_VERSION}-arm64.pkg"
COMPONENT_PKG="$OUTPUT_DIR/AES67Driver.pkg"

fail() { echo "ERROR: $1" >&2; exit 1; }

# First valid identity whose name starts with $1, or "". $2 is an optional
# find-identity policy (-p codesigning); installer certificates need none.
find_identity() {
    security find-identity -v ${2:+"$2"} 2>/dev/null \
        | sed -nE "s/.*\"($1[^\"]*)\".*/\1/p" | head -1
}

# Rebuild so a stale binary never ships, for example an aes67ctl without the
# keepalive command (#201).
if [ "${1:-}" = "--build" ]; then
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --target AES67Driver aes67ctl
fi

[ -d "$DRIVER_BUILD_PATH" ] || fail "driver not found at $DRIVER_BUILD_PATH (run with --build)"
[ -x "$CTL_BUILD_PATH" ] || fail "aes67ctl not found at $CTL_BUILD_PATH (run with --build)"
for s in preinstall postinstall configure-driver; do
    [ -x "$SCRIPTS_DIR/$s" ] || fail "installer script missing or not executable: $SCRIPTS_DIR/$s"
done

APP_ID="${DEVELOPER_ID_APP:-$(find_identity "Developer ID Application" "-pcodesigning")}"
INSTALLER_ID="${DEVELOPER_ID_INSTALLER:-$(find_identity "Developer ID Installer")}"
if [ "${REQUIRE_SIGNING:-0}" = "1" ]; then
    [ -n "$APP_ID" ] || fail "no Developer ID Application certificate"
    [ -n "$INSTALLER_ID" ] || fail "no Developer ID Installer certificate"
    [ -n "${NOTARY_PROFILE:-}" ] || fail "NOTARY_PROFILE is not set"
fi

echo "Packaging $PACKAGE_ID $PACKAGE_VERSION"

# Payload mirrors the install locations.
chmod -R u+w "$PAYLOAD_DIR" 2>/dev/null || true
rm -rf "$OUTPUT_DIR" "$PAYLOAD_DIR"
mkdir -p "$OUTPUT_DIR" "$PAYLOAD_DIR/Library/Audio/Plug-Ins/HAL" "$PAYLOAD_DIR/usr/local/bin"
cp -R "$DRIVER_BUILD_PATH" "$PAYLOAD_DIR/Library/Audio/Plug-Ins/HAL/"
cp "$CTL_BUILD_PATH" "$PAYLOAD_DIR/usr/local/bin/"
# Drop quarantine and other extended attributes. com.apple.provenance is
# protected and stays; pkgutil lists it as ._ entries, which install as
# attributes, not files.
xattr -cr "$PAYLOAD_DIR"

if [ -n "$APP_ID" ]; then
    echo "Signing binaries with: $APP_ID"
    for target in "$PAYLOAD_DIR/usr/local/bin/aes67ctl" \
                  "$PAYLOAD_DIR/Library/Audio/Plug-Ins/HAL/$DRIVER_NAME"; do
        codesign --force --options runtime --timestamp --sign "$APP_ID" "$target"
        codesign --verify --strict "$target"
    done
fi

pkgbuild \
    --root "$PAYLOAD_DIR" \
    --component-plist "$SCRIPT_DIR/component.plist" \
    --identifier "$PACKAGE_ID" \
    --version "$PACKAGE_VERSION" \
    --scripts "$SCRIPTS_DIR" \
    --install-location "/" \
    "$COMPONENT_PKG"

DIST_XML="$OUTPUT_DIR/Distribution.xml"
sed "s/@PACKAGE_VERSION@/$PACKAGE_VERSION/" "$SCRIPT_DIR/Distribution.xml" > "$DIST_XML"

PRODUCT_SIGN_ARGS=()
if [ -n "$INSTALLER_ID" ]; then
    echo "Signing package with: $INSTALLER_ID"
    PRODUCT_SIGN_ARGS=(--sign "$INSTALLER_ID" --timestamp)
fi
productbuild \
    --distribution "$DIST_XML" \
    --package-path "$OUTPUT_DIR" \
    --resources "$SCRIPT_DIR" \
    ${PRODUCT_SIGN_ARGS[@]+"${PRODUCT_SIGN_ARGS[@]}"} \
    "$FINAL_PKG"
rm -f "$COMPONENT_PKG" "$DIST_XML"

if [ -n "$INSTALLER_ID" ] && [ -n "${NOTARY_PROFILE:-}" ]; then
    echo "Notarizing..."
    xcrun notarytool submit "$FINAL_PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$FINAL_PKG"
    spctl --assess --type install -vv "$FINAL_PKG"
fi

echo ""
echo "Package: $FINAL_PKG"
pkgutil --payload-files "$FINAL_PKG" | grep -E 'aes67ctl$|\.driver$'
if [ -z "$INSTALLER_ID" ]; then
    echo "UNSIGNED: no Developer ID Installer certificate in the keychain."
elif [ -z "${NOTARY_PROFILE:-}" ]; then
    echo "Signed but NOT notarized: set NOTARY_PROFILE to notarize."
else
    echo "Signed, notarized and stapled."
fi
