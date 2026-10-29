#!/bin/bash
# Build script for AES67 Manager App

set -e

cd "$(dirname "$0")"

FORCE=0
if [ "$1" = "--force" ]; then
    FORCE=1
fi

# Incremental build: skip if binary is newer than all Swift sources
BINARY="AES67Manager.app/Contents/MacOS/AES67Manager"
if [ "$FORCE" -eq 0 ] && [ -f "$BINARY" ]; then
    CHANGED=$(find . -name '*.swift' -newer "$BINARY" 2>/dev/null)
    if [ -z "$CHANGED" ]; then
        echo "AES67 Manager: Up to date"
        exit 0
    fi
fi

echo "Building AES67 Manager..."

# Compile Swift files - include all Views and Models
# Note: SDPProcessor.swift disabled - needs compatibility fixes
SOURCES=(
  Models/StreamInfo.swift
  Models/DriverManager.swift
  Models/MenuBarManager.swift
  Views/ContentView.swift
  Views/StreamListView.swift
  Views/StreamDetailView.swift
  Views/AddStreamView.swift
  Views/SettingsView.swift
  Views/ChannelMappingView.swift
  Views/ChannelMapDiagnosticView.swift
  Views/QuickStartView.swift
  Views/PTPDiagnosticView.swift
  Views/AudioStatusView.swift
  AES67ManagerApp.swift
)

# Universal binary, like the driver
SDK="$(xcrun --show-sdk-path --sdk macosx)"
for ARCH in arm64 x86_64; do
  swiftc -o "AES67Manager-$ARCH" \
    -target "$ARCH-apple-macos13.0" \
    -sdk "$SDK" \
    -framework SwiftUI \
    -framework Foundation \
    -framework AppKit \
    -framework CoreAudio \
    -framework UniformTypeIdentifiers \
    "${SOURCES[@]}"
done
lipo -create -output AES67Manager AES67Manager-arm64 AES67Manager-x86_64
rm AES67Manager-arm64 AES67Manager-x86_64

# Create app bundle structure
echo "Creating app bundle..."
mkdir -p AES67Manager.app/Contents/MacOS
mkdir -p AES67Manager.app/Contents/Resources

# Move executable
mv AES67Manager AES67Manager.app/Contents/MacOS/

# Copy Info.plist
cp Resources/Info.plist AES67Manager.app/Contents/

# Sign the app
echo "Signing app..."
codesign --force --deep --sign - --options runtime AES67Manager.app

echo "Build complete: AES67Manager.app"
