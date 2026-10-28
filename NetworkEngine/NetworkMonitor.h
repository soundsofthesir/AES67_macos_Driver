/// @file NetworkMonitor.h
/// @brief Reports when an interface that streams use changes.

#pragma once

#include "NetworkInterfaceDetection.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace AES67 {

/// Watches interface settings ("en0", an IPv4 address, or "" for the primary
/// interface) and calls the handler when one's state changes: a new address
/// (DHCP), link down or up (Wi-Fi roam, cable), a new interface index (an
/// adapter replugged), or a different interface behind "" (auto). Polling
/// getifaddrs once a second needs no notification API inside the sandboxed
/// driver host and costs next to nothing.
class NetworkMonitor {
public:
    using StateProvider =
        std::function<std::optional<NetworkInterfaceDetection::InterfaceState>(const std::string& setting)>;
    using ChangeHandler = std::function<void(const std::string& setting)>;

    struct Config {
        std::chrono::milliseconds interval{1000};
        StateProvider provider;  // empty: NetworkInterfaceDetection::currentState
    };

    /// The handler runs on the monitor's thread, without the monitor's lock
    /// held, so it may call back into code that calls watch(). If it throws,
    /// the change is logged and reported again on the next poll.
    NetworkMonitor(Config config, ChangeHandler handler);
    ~NetworkMonitor();

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    /// Start watching `setting`, from its current state. Watching it again is harmless.
    void watch(const std::string& setting);

private:
    void run();
    std::optional<NetworkInterfaceDetection::InterfaceState> stateOf(const std::string& setting) const;

    const Config config_;
    const ChangeHandler handler_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::map<std::string, std::optional<NetworkInterfaceDetection::InterfaceState>> watched_;
    std::thread thread_;
};

} // namespace AES67
