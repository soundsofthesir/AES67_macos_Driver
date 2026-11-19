# AES67 Driver Installer Package

This directory contains the installer package infrastructure for the AES67 macOS Audio Driver.

## Contents

```
Installer/
├── build-installer.sh      # Main build script - creates the .pkg file
├── Distribution.xml        # Package distribution configuration
├── welcome.html            # Installer welcome screen
├── license.txt             # MIT license text
├── readme.html             # Installation information
├── conclusion.html         # Post-installation message
├── uninstall.sh            # Uninstaller script for users
├── scripts/
│   ├── preinstall          # Pre-installation checks
│   └── postinstall         # Post-installation setup (copies driver, restarts Core Audio)
├── output/                 # Generated packages (created during build)
└── payload/                # Temporary payload directory (created during build)
```

## Building the Installer Package

### Prerequisites

1. **Build the driver first:**
   ```bash
   cd /path/to/AES67_macos_Driver
   mkdir -p build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
   make
   ```

2. **Verify the driver exists:**
   ```bash
   ls -la build/AES67Driver.driver
   ```

### Create the Package

Run the build script:

```bash
cd Installer
./build-installer.sh
```

This will create:
- **Component package**: `output/AES67Driver.pkg`
- **Final package**: `output/AES67Driver-1.0.0-arm64.pkg`

### Package Output

The installer package will be created at:
```
Installer/output/AES67Driver-1.0.0-arm64.pkg
```

## Installing the Package

### GUI Installation (Recommended)

1. Double-click `AES67Driver-1.0.0-arm64.pkg`
2. Follow the on-screen instructions
3. Enter your admin password when prompted
4. Core Audio will restart automatically

### Command Line Installation

```bash
sudo installer -pkg Installer/output/AES67Driver-1.0.0-arm64.pkg -target /
```

### Installation Log

Check the installation log for troubleshooting:
```bash
cat /var/log/aes67driver_install.log
```

## What the Installer Does

1. **Pre-installation** (`scripts/preinstall`):
   - Checks macOS version (requires 13.0+)
   - Verifies system architecture (Apple Silicon recommended)
   - Checks available disk space
   - Displays warnings about experimental nature

2. **Installation**:
   - Copies `AES67Driver.driver` to `/Library/Audio/Plug-Ins/HAL/`
   - Sets ownership to `root:wheel`
   - Sets permissions to `755`

3. **Post-installation** (`scripts/postinstall`):
   - Verifies driver installation
   - Restarts Core Audio (`killall coreaudiod`)
   - Writes installation log

## Uninstalling

### Using the Uninstall Script

```bash
sudo ./uninstall.sh
```

The script will:
- Remove the driver from `/Library/Audio/Plug-Ins/HAL/`
- Optionally remove configuration files
- Optionally remove Manager application
- Restart Core Audio

### Manual Uninstallation

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/AES67Driver.driver
sudo killall coreaudiod
```

## Code Signing (Optional but Recommended)

### Why Sign?

- Allows installation without Gatekeeper warnings
- Required for distribution outside App Store
- Provides verification of package authenticity

### Signing the Package

1. **Get a Developer ID Installer certificate** from Apple Developer Program

2. **List available certificates:**
   ```bash
   security find-identity -v -p codesigning
   ```

3. **Sign the package:**
   ```bash
   productsign --sign "Developer ID Installer: Your Name (TEAMID)" \
     output/AES67Driver-1.0.0-arm64.pkg \
     output/AES67Driver-1.0.0-arm64-signed.pkg
   ```

4. **Verify signature:**
   ```bash
   pkgutil --check-signature output/AES67Driver-1.0.0-arm64-signed.pkg
   ```

### Notarization (Required for macOS 10.15+)

After signing, submit for notarization:

```bash
# Submit for notarization
xcrun notarytool submit output/AES67Driver-1.0.0-arm64-signed.pkg \
  --apple-id "your-apple-id@example.com" \
  --team-id "TEAMID" \
  --password "app-specific-password" \
  --wait

# Staple the notarization ticket
xcrun stapler staple output/AES67Driver-1.0.0-arm64-signed.pkg
```

See `CODE_SIGNING.md` for detailed code signing and notarization instructions.

## Package Contents

The package installs:

```
/Library/Audio/Plug-Ins/HAL/AES67Driver.driver/
├── Contents/
│   ├── Info.plist
│   └── MacOS/
│       └── AES67Driver (executable)
```

### Verify Package Contents

```bash
pkgutil --payload-files output/AES67Driver-1.0.0-arm64.pkg
```

### Verify Installation

After installing:

```bash
# Check if driver is installed
ls -la /Library/Audio/Plug-Ins/HAL/AES67Driver.driver

# Check Core Audio loaded plugins
system_profiler SPAudioDataType

# Check Audio MIDI Setup for "AES67 Device"
```

## Troubleshooting

### Driver Not Appearing

1. Check installation log:
   ```bash
   cat /var/log/aes67driver_install.log
   ```

2. Verify driver exists:
   ```bash
   ls -la /Library/Audio/Plug-Ins/HAL/AES67Driver.driver
   ```

3. Restart Core Audio:
   ```bash
   sudo killall coreaudiod
   ```

4. Restart your Mac (if needed)

### Build Errors

**Error: "Driver not found"**
- Solution: Build the driver first (see Prerequisites above)

**Error: "pkgbuild: command not found"**
- Solution: Install Xcode Command Line Tools:
  ```bash
  xcode-select --install
  ```

**Error: "Permission denied"**
- Solution: Make scripts executable:
  ```bash
  chmod +x build-installer.sh scripts/*
  ```

### Installation Errors

**Error: "Requires macOS 13.0 or later"**
- Solution: Upgrade macOS or build on compatible system

**Error: "Insufficient disk space"**
- Solution: Free up at least 50MB on system drive

**Error: "Package is damaged"**
- Solution: Re-download or rebuild package, may need code signing

## Distribution

### Local Distribution

1. Build package as described above
2. Share the `.pkg` file directly
3. Recipients double-click to install

### Signed Distribution

1. Sign the package with Developer ID certificate
2. Notarize with Apple
3. Distribute signed and notarized package

### Important: Package Warnings

**Unsigned packages will show security warnings on macOS 10.15+**

To bypass Gatekeeper for testing (NOT recommended for distribution):
```bash
sudo spctl --master-disable  # Disable Gatekeeper (risky!)
# Install package
sudo spctl --master-enable   # Re-enable Gatekeeper
```

## Notes

- **Apple Silicon Only**: Package optimized for arm64 architecture
- **Experimental Software**: Include warnings in all distribution materials
- **Open Source**: MIT licensed, source available on GitHub
- **No Warranty**: Provided as-is without warranty of any kind

## Version Information

- **Package Version**: 1.0.0
- **Build**: #18
- **Bundle Identifier**: com.aes67.driver
- **Minimum macOS**: 13.0 (Ventura)
- **Architecture**: arm64 (Apple Silicon)

## Support

For issues or questions:
- GitHub Issues: https://github.com/yourusername/AES67_macos_Driver/issues
- Documentation: See main README.md
- Installation Logs: `/var/log/aes67driver_install.log`
