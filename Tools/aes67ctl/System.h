/// @file System.h
/// @brief The parts of aes67ctl that look at or change this Mac.

#pragma once

#include "Checks.h"
#include <CoreAudio/CoreAudio.h>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace AES67 {
namespace Ctl {

/// Run `argv` (no shell) and return its exit status, with stdout in `output`.
int run(const std::vector<std::string>& argv, std::string* output = nullptr);

struct InterfaceInfo {
    bool exists{false};
    bool up{false};
    std::string ipv4;
};
InterfaceInfo interfaceInfo(const std::string& name);

/// Facts for `doctor`. `groups` are checked for a route that bypasses the interface.
Facts gatherFacts(const std::string& interfaceName, const std::vector<std::string>& groups);

/// Where the driver reads its files when it runs inside coreaudiod. Tests
/// redirect them with AES67_CONFIG_PATH and AES67_PTP_CONFIG_PATH.
std::string streamsPath();
std::string ptpPath();
/// True when the paths are the system's (not redirected), so writing needs root.
bool usingSystemPaths();

/// Create the configuration directory, owned by _coreaudiod (as postinstall does).
bool prepareConfigDirectory(std::string* error);

/// Write `contents` to `path` atomically.
bool writeFile(const std::string& path, const std::string& contents, std::string* error);

/// Restart coreaudiod so the driver reads its files again.
bool restartCoreAudio(std::string* error);

/// The driver's device, by UID, if Core Audio has it.
std::optional<AudioObjectID> findDevice();

/// The installed driver bundle's version, "0.1.0 build 19", or "" if the
/// driver isn't installed.
std::string installedDriverVersion();

/// The version the driver running in coreaudiod reports, "0.1.0 build 19
/// (abc1234)"; "" if it reports none (drivers before 0.1.0), nullopt if
/// Core Audio has no AES67 device.
std::optional<std::string> runningDriverVersion();

/// Wait for the driver's device to appear; false on timeout.
bool waitForDevice(std::chrono::seconds timeout);

/// Make the driver's device the default output.
bool setDefaultOutput(std::string* error);

/// Whether the driver's device is the default output.
bool isDefaultOutput();

/// When coreaudiod (and so the driver) last started, as `log show --start`
/// takes it ("2026-11-18 15:08:16"), or "" if it isn't running.
std::string coreAudioStartTime();

/// The driver's last PTP lock change since `start` (see coreAudioStartTime),
/// without the "PTP: " prefix, or "".
std::string lastPtpLockChange(const std::string& start);

/// This Mac's short host name, for stream names.
std::string shortHostName();

}  // namespace Ctl
}  // namespace AES67
