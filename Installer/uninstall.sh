#!/bin/bash
#
# uninstall.sh
# AES67 macOS Driver Uninstaller
# Removes the AES67 audio driver from the system
#

set -e

echo "===================================="
echo "AES67 Driver Uninstaller"
echo "===================================="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run with administrator privileges"
    echo "Please run: sudo $0"
    exit 1
fi

# Paths
DRIVER_NAME="AES67Driver.driver"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
DRIVER_PATH="${INSTALL_DIR}/${DRIVER_NAME}"
CONFIG_DIR="/Library/Application Support/AES67Driver"
MANAGER_APP="/Applications/AES67Manager.app"

# Check if driver is installed
if [ ! -d "$DRIVER_PATH" ]; then
    echo "AES67 Driver is not currently installed at:"
    echo "  $DRIVER_PATH"
    echo ""
    echo "Nothing to uninstall."
    exit 0
fi

echo "Found AES67 Driver at: $DRIVER_PATH"
echo ""
echo "This will remove:"
echo "  - Audio driver: $DRIVER_PATH"

# Check for optional components
REMOVE_CONFIG=false
REMOVE_MANAGER=false

if [ -d "$CONFIG_DIR" ]; then
    echo "  - Configuration: $CONFIG_DIR"
    REMOVE_CONFIG=true
fi

if [ -d "$MANAGER_APP" ]; then
    echo "  - Manager App: $MANAGER_APP"
    REMOVE_MANAGER=true
fi

echo ""
read -p "Continue with uninstallation? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstallation cancelled."
    exit 0
fi

echo ""
echo "Removing AES67 Driver..."

# Remove the driver
if [ -d "$DRIVER_PATH" ]; then
    echo "Removing driver bundle..."
    rm -rf "$DRIVER_PATH"
    echo "✓ Driver removed"
fi

# Remove the command-line tool
if [ -f /usr/local/bin/aes67ctl ]; then
    rm -f /usr/local/bin/aes67ctl
    echo "✓ aes67ctl removed"
fi

# Remove configuration (ask first)
if [ "$REMOVE_CONFIG" = true ]; then
    echo ""
    read -p "Remove configuration files? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$CONFIG_DIR"
        echo "✓ Configuration removed"
    else
        echo "Configuration files kept"
    fi
fi

# Remove Manager app (ask first)
if [ "$REMOVE_MANAGER" = true ]; then
    echo ""
    read -p "Remove AES67 Manager application? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$MANAGER_APP"
        echo "✓ Manager application removed"
    else
        echo "Manager application kept"
    fi
fi

# Restart Core Audio
echo ""
echo "Restarting Core Audio..."
killall coreaudiod 2>/dev/null || true
sleep 2

echo ""
echo "===================================="
echo "Uninstallation Complete"
echo "===================================="
echo ""
echo "The AES67 Driver has been removed from your system."
echo "Core Audio has been restarted."
echo ""
echo "You may need to restart your audio applications."
echo ""

exit 0
