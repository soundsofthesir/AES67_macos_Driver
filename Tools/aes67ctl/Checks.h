/// @file Checks.h
/// @brief What `aes67ctl doctor` checks, as pure functions over gathered facts.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace AES67 {
namespace Ctl {

struct Finding {
    enum class Level { Ok, Warn, Fail };
    Level level;
    std::string text;
};

/// What `doctor` looks at, gathered by System.h (or by a test).
struct Facts {
    std::string interfaceName;
    bool interfaceExists{false};
    bool interfaceUp{false};
    std::string ipv4;
    std::string bridge;              ///< the bridge the interface is a member of, or ""
    bool internetSharing{false};
    /// group -> the interface the routing table sends it to ("" if unknown)
    std::vector<std::pair<std::string, std::string>> groupRoutes;
    bool driverInstalled{false};
    bool configDirExists{false};
    bool configDirOwnedByDriver{false};
};

std::vector<Finding> diagnose(const Facts& facts);

/// True if any finding is a failure: `setup` stops on these.
bool hasFailure(const std::vector<Finding>& findings);

/// The bridge `interfaceName` is a member of, from `ifconfig -a` output.
std::string bridgeOf(const std::string& ifconfigOutput, const std::string& interfaceName);

/// The "interface:" line of `route -n get` output, or "".
std::string routeInterface(const std::string& routeGetOutput);

/// An interface name the tool will pass to other commands: letters and
/// digits only ("en10", "bridge100").
bool isPlainInterfaceName(const std::string& name);

}  // namespace Ctl
}  // namespace AES67
