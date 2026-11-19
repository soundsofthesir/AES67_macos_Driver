/// @file KeepAlive.h
/// @brief aes67ctl keepalive: keeps the device's IO running while a send
/// stream is configured.
///
/// With no client running IO, Core Audio lowers the driver's process to
/// background priority and its transmit thread loses real-time scheduling,
/// so send streams skip packets (#201). An output-only IOProc that writes
/// silence keeps IO running. A LaunchDaemon runs this at boot.

#pragma once

#include <functional>
#include <optional>

namespace AES67 {
namespace Ctl {

/// Runs until killed. Every few seconds asks `wantsIO` (nullopt: the
/// streams file can't be read, keep the current state) and starts or stops
/// IO to match, and follows the device across coreaudiod restarts.
int keepAlive(const std::function<std::optional<bool>()>& wantsIO);

}  // namespace Ctl
}  // namespace AES67
