# Installer

Builds `output/AES67Driver-<version>-arm64.pkg`. The package installs:

- `/Library/Audio/Plug-Ins/HAL/AES67Driver.driver`
- `/usr/local/bin/aes67ctl`
- the `com.aes67driver.keepalive` LaunchDaemon, written and started by `scripts/configure-driver`

The Manager app isn't in the package until it controls the driver. `sudo cmake --install build` still installs it.

## Build

```bash
Installer/build-installer.sh --build
```

`--build` rebuilds the driver and `aes67ctl` first. Without it the script packages whatever is in `build/`. The version comes from `VERSION.txt`.

## Signing and notarization

The script signs whatever it can with the certificates in the keychain:

| Certificate | Signs |
|---|---|
| Developer ID Application | the driver and `aes67ctl`, with the hardened runtime |
| Developer ID Installer | the `.pkg` |

It notarizes and staples the package when `NOTARY_PROFILE` names a stored notarytool profile. Store one once:

```bash
xcrun notarytool store-credentials aes67-notary \
  --apple-id <apple id> --team-id KBUT93GX9U --password <app-specific password>
```

A release build:

```bash
REQUIRE_SIGNING=1 NOTARY_PROFILE=aes67-notary Installer/build-installer.sh --build
```

`REQUIRE_SIGNING=1` stops the build if either certificate or the profile is missing. Set `DEVELOPER_ID_APP` or `DEVELOPER_ID_INSTALLER` to choose a certificate when the keychain has more than one.

## Install

```bash
sudo installer -pkg Installer/output/AES67Driver-<version>-arm64.pkg -target /
```

Core Audio restarts at the end. The install log is `/var/log/aes67driver_install.log`.

## Scripts

| Script | Does |
|---|---|
| `scripts/preinstall` | Checks for macOS 13 or later and 50 MB free |
| `scripts/postinstall` | Runs `configure-driver` |
| `scripts/configure-driver` | Gives the driver its config directory, starts the keepalive service, restarts Core Audio. `cmake --install` runs it too. |
| `uninstall.sh` | Removes the driver, `aes67ctl` and the keepalive service, and asks before removing the config and the Manager app |
