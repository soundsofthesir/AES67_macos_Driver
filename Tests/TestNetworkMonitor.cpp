//
// TestNetworkMonitor.cpp
// AES67 macOS Driver
// Interface state for a stream's interface setting, and the monitor that
// reports when it changes (address, link, interface index, or which interface
// "auto" means). The monitor tests use a scripted state provider: real
// interfaces cannot be changed from a test.
//

#include "../NetworkEngine/NetworkMonitor.h"
#include "../NetworkEngine/NetworkInterfaceDetection.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace AES67;
using State = NetworkInterfaceDetection::InterfaceState;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

void testCurrentStateOfRealInterfaces() {
    std::cout << "Interface state resolves names, addresses and auto" << std::endl;
    const auto loopback = NetworkInterfaceDetection::currentState("lo0");
    CHECK(loopback && loopback->name == "lo0" && loopback->index > 0 && loopback->ipv4 == "127.0.0.1" && loopback->running,
          "lo0 should resolve to itself, its index and 127.0.0.1");
    const auto byAddress = NetworkInterfaceDetection::currentState("127.0.0.1");
    CHECK(byAddress && byAddress->name == "lo0", "an IPv4 address should resolve to the interface holding it");
    CHECK(!NetworkInterfaceDetection::currentState("en99"), "a missing interface has no state");
    CHECK(!NetworkInterfaceDetection::currentState("192.0.2.1"), "an address no interface holds has no state");
    const auto automatic = NetworkInterfaceDetection::currentState("");
    const auto autoWord = NetworkInterfaceDetection::currentState("auto");
    CHECK(automatic.has_value() == autoWord.has_value() && (!automatic || automatic->name == autoWord->name),
          "\"\" and \"auto\" both mean the primary interface");
}

// Monitors on several threads resolve interfaces at once. inet_ntoa's single
// static buffer let one lookup read another's address: a change that never
// happened, so streams and PTP restarted for nothing.
void testCurrentStateIsThreadSafe() {
    std::cout << "Interface state is resolved correctly from several threads at once" << std::endl;
    const auto other = NetworkInterfaceDetection::currentState("auto");
    if (!other || other->ipv4.empty() || other->ipv4 == "127.0.0.1") {
        std::cout << "  (skipped: no other interface with an IPv4 address)" << std::endl;
        return;
    }
    std::atomic<bool> stop{false};
    std::vector<std::thread> contenders;
    for (int i = 0; i < 3; ++i) {
        contenders.emplace_back([&stop] {
            while (!stop.load()) {
                NetworkInterfaceDetection::currentState("auto");
            }
        });
    }
    int wrong = 0;
    for (int i = 0; i < 20000; ++i) {
        const auto loopback = NetworkInterfaceDetection::currentState("lo0");
        if (!loopback || loopback->ipv4 != "127.0.0.1") {
            ++wrong;
        }
    }
    stop = true;
    for (auto& contender : contenders) {
        contender.join();
    }
    CHECK(wrong == 0, "lo0 always reads 127.0.0.1 while " << other->name << " is resolved concurrently (" << wrong
                                                           << " wrong in 20000)");
}

// Scripted interface states, changed by the test
class FakeNetwork {
public:
    void set(const std::string& setting, std::optional<State> state) {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[setting] = std::move(state);
    }
    std::optional<State> get(const std::string& setting) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(setting);
        return it == states_.end() ? std::nullopt : it->second;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::optional<State>> states_;
};

class ChangeLog {
public:
    void add(const std::string& setting) {
        std::lock_guard<std::mutex> lock(mutex_);
        changes_.push_back(setting);
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto changes = changes_;
        changes_.clear();
        return changes;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> changes_;
};

State state(const std::string& name, unsigned index, const std::string& ipv4, bool running = true) {
    return State{name, index, ipv4, running};
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }

void testMonitorReportsChanges() {
    std::cout << "The monitor reports each change of a watched interface, once" << std::endl;
    FakeNetwork network;
    network.set("en0", state("en0", 4, "10.46.75.146"));
    network.set("en1", state("en1", 5, "192.168.1.20"));
    ChangeLog log;
    NetworkMonitor::Config config;
    config.interval = std::chrono::milliseconds(20);
    config.provider = [&](const std::string& setting) { return network.get(setting); };
    NetworkMonitor monitor(config, [&](const std::string& setting) { log.add(setting); });
    monitor.watch("en0");
    monitor.watch("en1");
    monitor.watch("en0");  // watching twice is harmless

    settle();
    CHECK(log.take().empty(), "nothing is reported while nothing changes");

    network.set("en0", state("en0", 4, "10.46.75.200"));  // DHCP gave a new address
    settle();
    CHECK(log.take() == std::vector<std::string>{"en0"}, "a new address is reported, for that interface only");

    network.set("en0", state("en0", 4, "10.46.75.200", false));  // link down (Wi-Fi roam)
    settle();
    network.set("en0", state("en0", 4, "10.46.75.200", true));   // and back up
    settle();
    CHECK(log.take().size() == 2, "link down and link up are each reported");

    network.set("en0", std::nullopt);                      // adapter unplugged
    settle();
    network.set("en0", state("en0", 9, "10.46.75.200"));   // replugged with a new index
    settle();
    CHECK(log.take().size() == 2, "an interface disappearing and returning (new index) is reported");
}

void testMonitorFollowsAuto() {
    std::cout << "The monitor follows which interface \"auto\" means" << std::endl;
    FakeNetwork network;
    network.set("", state("en1", 5, "192.168.1.20"));  // Wi-Fi is primary
    ChangeLog log;
    NetworkMonitor::Config config;
    config.interval = std::chrono::milliseconds(20);
    config.provider = [&](const std::string& setting) { return network.get(setting); };
    NetworkMonitor monitor(config, [&](const std::string& setting) { log.add(setting); });
    monitor.watch("");
    settle();
    network.set("", state("en0", 4, "10.46.75.146"));  // Ethernet plugged in, now primary
    settle();
    CHECK(log.take() == std::vector<std::string>{""}, "a different primary interface is reported for \"auto\"");
}

// A handler that throws used to escape the monitor's thread and terminate
// the driver host (#71). The monitor keeps running and reports the change
// again on its next poll.
void testMonitorSurvivesAThrowingHandler() {
    std::cout << "#71: a handler that throws neither stops the monitor nor loses the change" << std::endl;
    FakeNetwork network;
    network.set("en0", state("en0", 4, "10.46.75.146"));
    ChangeLog log;
    std::atomic<int> throwsLeft{1};
    NetworkMonitor::Config config;
    config.interval = std::chrono::milliseconds(20);
    config.provider = [&](const std::string& setting) { return network.get(setting); };
    NetworkMonitor monitor(config, [&](const std::string& setting) {
        if (throwsLeft.fetch_sub(1) > 0) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        log.add(setting);
    });
    monitor.watch("en0");

    network.set("en0", state("en0", 4, "10.46.75.200"));
    settle();
    CHECK(log.take() == std::vector<std::string>{"en0"}, "the change is reported again once the handler succeeds");

    network.set("en0", state("en0", 4, "10.46.75.201"));
    settle();
    CHECK(log.take() == std::vector<std::string>{"en0"}, "and later changes are still reported, once");
}

void testMonitorStopsPromptly() {
    std::cout << "The monitor stops promptly" << std::endl;
    NetworkMonitor::Config config;
    config.interval = std::chrono::seconds(10);
    const auto start = std::chrono::steady_clock::now();
    {
        NetworkMonitor monitor(config, [](const std::string&) {});
        monitor.watch("lo0");
    }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500),
          "destruction should not wait out the polling interval");
}

} // namespace

int main() {
    testCurrentStateOfRealInterfaces();
    testCurrentStateIsThreadSafe();
    testMonitorReportsChanges();
    testMonitorFollowsAuto();
    testMonitorSurvivesAThrowingHandler();
    testMonitorStopsPromptly();

    std::cout << "\nNetwork monitor: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
