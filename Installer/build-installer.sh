#!/bin/bash
#
# build-installer.sh
# AES67 macOS Driver - Installer Package Builder
# Creates a macOS .pkg installer for the AES67 audio driver
#

set -e  # Exit on error

echo "========================================"
echo "AES67 Driver Installer Package Builder"
echo "========================================"
echo ""

# Paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
INSTALLER_DIR="$SCRIPT_DIR"
OUTPUT_DIR="$INSTALLER_DIR/output"
PAYLOAD_DIR="$INSTALLER_DIR/payload"
SCRIPTS_DIR="$INSTALLER_DIR/scripts"

# --build flag: build missing artifacts before packaging
if [ "$1" = "--build" ]; then
    echo "Auto-building artifacts..."
    echo ""

    if [ ! -d "$BUILD_DIR/AES67Driver.driver" ]; then
        echo "Building driver..."
        mkdir -p "$BUILD_DIR"
        (cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release && make AES67Driver)
        echo ""
    fi

    if [ ! -x "$BUILD_DIR/aes67ctl" ]; then
        echo "Building aes67ctl..."
        cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$BUILD_DIR" --target aes67ctl
        echo ""
    fi

    if [ ! -d "$PROJECT_ROOT/ManagerApp/AES67Manager.app" ]; then
        echo "Building Manager app..."
        (cd "$PROJECT_ROOT/ManagerApp" && ./build.sh)
        echo ""
    fi
fi

# Package info
PACKAGE_ID="com.aes67.driver"
PACKAGE_VERSION="1.0.0"
PACKAGE_NAME="AES67Driver-${PACKAGE_VERSION}-arm64.pkg"

# Driver info
DRIVER_NAME="AES67Driver.driver"
DRIVER_BUILD_PATH="$BUILD_DIR/$DRIVER_NAME"

# Manager app info
MANAGER_APP_NAME="AES67Manager.app"
MANAGER_APP_PATH="$PROJECT_ROOT/ManagerApp/$MANAGER_APP_NAME"

echo "Configuration:"
echo "  Project root: $PROJECT_ROOT"
echo "  Build dir: $BUILD_DIR"
echo "  Package ID: $PACKAGE_ID"
echo "  Package version: $PACKAGE_VERSION"
echo "  Output package: $PACKAGE_NAME"
echo ""

# Check if driver has been built
if [ ! -d "$DRIVER_BUILD_PATH" ]; then
    echo "ERROR: Driver not found at: $DRIVER_BUILD_PATH"
    echo ""
    echo "Please build the driver first:"
    echo "  cd $PROJECT_ROOT"
    echo "  mkdir -p build && cd build"
    echo "  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"
    echo "  make"
    echo ""
    exit 1
fi

echo "✓ Found driver at: $DRIVER_BUILD_PATH"

# Command-line setup tool
CTL_BUILD_PATH="$BUILD_DIR/aes67ctl"
if [ ! -x "$CTL_BUILD_PATH" ]; then
    echo "ERROR: aes67ctl not found at: $CTL_BUILD_PATH (cmake --build build --target aes67ctl)"
    exit 1
fi
echo "✓ Found aes67ctl at: $CTL_BUILD_PATH"

# Check if Manager app has been built
if [ ! -d "$MANAGER_APP_PATH" ]; then
    echo "ERROR: Manager app not found at: $MANAGER_APP_PATH"
    echo ""
    echo "Please build the Manager app first:"
    echo "  cd $PROJECT_ROOT/ManagerApp"
    echo "  ./build.sh"
    echo ""
    exit 1
fi

echo "✓ Found Manager app at: $MANAGER_APP_PATH"

# Clean and create output directories
echo "Preparing directories..."
chmod -R u+w "$PAYLOAD_DIR" 2>/dev/null || true
rm -rf "$OUTPUT_DIR" "$PAYLOAD_DIR"
mkdir -p "$OUTPUT_DIR"
mkdir -p "$PAYLOAD_DIR"

# Create payload directory structure
# The payload should mirror the final installation location
PAYLOAD_INSTALL_DIR="$PAYLOAD_DIR/Library/Audio/Plug-Ins/HAL"
PAYLOAD_APPS_DIR="$PAYLOAD_DIR/Applications"
mkdir -p "$PAYLOAD_INSTALL_DIR"
mkdir -p "$PAYLOAD_APPS_DIR"

# Copy driver to payload
echo "Copying driver to payload..."
cp -R "$DRIVER_BUILD_PATH" "$PAYLOAD_INSTALL_DIR/"

# Verify driver payload
if [ ! -d "$PAYLOAD_INSTALL_DIR/$DRIVER_NAME" ]; then
    echo "ERROR: Failed to copy driver to payload"
    exit 1
fi

echo "✓ Driver copied to payload"

# Copy aes67ctl to payload
mkdir -p "$PAYLOAD_DIR/usr/local/bin"
cp "$CTL_BUILD_PATH" "$PAYLOAD_DIR/usr/local/bin/"
echo "✓ aes67ctl copied to payload"

# Copy Manager app to payload
echo "Copying Manager app to payload..."
cp -R "$MANAGER_APP_PATH" "$PAYLOAD_APPS_DIR/"

# Verify Manager app payload
if [ ! -d "$PAYLOAD_APPS_DIR/$MANAGER_APP_NAME" ]; then
    echo "ERROR: Failed to copy Manager app to payload"
    exit 1
fi

echo "✓ Manager app copied to payload"

# Check scripts exist and are executable
echo "Checking installer scripts..."

if [ ! -f "$SCRIPTS_DIR/preinstall" ]; then
    echo "ERROR: preinstall script not found at: $SCRIPTS_DIR/preinstall"
    exit 1
fi

if [ ! -f "$SCRIPTS_DIR/postinstall" ]; then
    echo "ERROR: postinstall script not found at: $SCRIPTS_DIR/postinstall"
    exit 1
fi

if [ ! -x "$SCRIPTS_DIR/preinstall" ]; then
    echo "Making preinstall script executable..."
    chmod +x "$SCRIPTS_DIR/preinstall"
fi

if [ ! -x "$SCRIPTS_DIR/postinstall" ]; then
    echo "Making postinstall script executable..."
    chmod +x "$SCRIPTS_DIR/postinstall"
fi

echo "✓ Installer scripts ready"

# Build component package with pkgbuild
echo ""
echo "Building component package..."

COMPONENT_PKG="$OUTPUT_DIR/AES67Driver.pkg"

pkgbuild \
    --root "$PAYLOAD_DIR" \
    --component-plist "$INSTALLER_DIR/component.plist" \
    --identifier "$PACKAGE_ID" \
    --version "$PACKAGE_VERSION" \
    --scripts "$SCRIPTS_DIR" \
    --install-location "/" \
    "$COMPONENT_PKG"

if [ ! -f "$COMPONENT_PKG" ]; then
    echo "ERROR: Failed to create component package"
    exit 1
fi

echo "✓ Component package created: $COMPONENT_PKG"

# Build product package with productbuild
echo ""
echo "Building product package..."

FINAL_PKG="$OUTPUT_DIR/$PACKAGE_NAME"

# Check if Distribution.xml exists
if [ -f "$INSTALLER_DIR/Distribution.xml" ]; then
    echo "Using Distribution.xml..."

    productbuild \
        --distribution "$INSTALLER_DIR/Distribution.xml" \
        --package-path "$OUTPUT_DIR" \
        --resources "$INSTALLER_DIR" \
        "$FINAL_PKG"
else
    echo "No Distribution.xml found, creating simple package..."

    # Create simple product package without distribution file
    productbuild \
        --package "$COMPONENT_PKG" \
        "$FINAL_PKG"
fi

if [ ! -f "$FINAL_PKG" ]; then
    echo "ERROR: Failed to create final package"
    exit 1
fi

echo "✓ Product package created: $FINAL_PKG"

# Get package size
PKG_SIZE=$(du -h "$FINAL_PKG" | awk '{print $1}')

echo ""
echo "========================================"
echo "Package Build Complete!"
echo "========================================"
echo ""
echo "Output package:"
echo "  Location: $FINAL_PKG"
echo "  Size: $PKG_SIZE"
echo ""
echo "Package contents:"
pkgutil --payload-files "$FINAL_PKG" | head -10
echo ""
echo "To install (requires sudo):"
echo "  sudo installer -pkg \"$FINAL_PKG\" -target /"
echo ""
echo "Or double-click the package to install via GUI."
echo ""

# Optional: Sign the package
echo "========================================"
echo "Code Signing"
echo "========================================"
echo ""
echo "This package is UNSIGNED."
echo ""
echo "To sign the package, you need an Apple Developer certificate."
echo "See the code signing documentation for details."
echo ""
echo "To sign manually:"
echo "  productsign --sign \"Developer ID Installer: Your Name\" \\"
echo "    \"$FINAL_PKG\" \\"
echo "    \"${FINAL_PKG%.pkg}-signed.pkg\""
echo ""

echo "Build complete!"
