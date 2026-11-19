/// @file Checks.cpp
/// @brief `aes67ctl doctor` checks.

#include "Checks.h"
#include <cctype>
#include <sstream>

namespace AES67 {
namespace Ctl {

namespace {

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

void add(std::vector<Finding>& findings, Finding::Level level, std::string text) {
    findings.push_back({level, std::move(text)});
}

}  // namespace

bool isPlainInterfaceName(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string bridgeOf(const std::string& ifconfigOutput, const std::string& interfaceName) {
    std::istringstream lines(ifconfigOutput);
    std::string line;
    std::string current;  // the interface whose block we are in
    while (std::getline(lines, line)) {
        if (!line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))) {
            current = line.substr(0, line.find(':'));
            continue;
        }
        const std::string text = trim(line);
        if (text.rfind("member: ", 0) != 0) continue;
        std::istringstream words(text.substr(8));
        std::string member;
        words >> member;
        if (member == interfaceName) return current;
    }
    return "";
}

std::string routeInterface(const std::string& routeGetOutput) {
    std::istringstream lines(routeGetOutput);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string text = trim(line);
        if (text.rfind("interface:", 0) == 0) return trim(text.substr(10));
    }
    return "";
}

std::vector<Finding> diagnose(const Facts& facts) {
    using L = Finding::Level;
    std::vector<Finding> findings;

    if (facts.driverInstalled) {
        add(findings, L::Ok, "driver installed");
    } else {
        add(findings, L::Fail, "driver not installed in /Library/Audio/Plug-Ins/HAL");
    }
    if (!facts.configDirExists) {
        add(findings, L::Warn, "no configuration directory yet; setup creates it");
    } else if (!facts.configDirOwnedByDriver) {
        add(findings, L::Warn,
            "configuration directory is not owned by _coreaudiod; the driver can't save changes "
            "(sudo chown _coreaudiod:wheel \"/Library/Application Support/AES67Driver\")");
    }

    if (facts.interfaceName.empty()) return findings;
    const std::string& name = facts.interfaceName;
    if (!facts.interfaceExists) {
        add(findings, L::Fail, name + " not found");
        return findings;
    }
    if (!facts.interfaceUp) add(findings, L::Fail, name + " is down (no link)");
    if (facts.ipv4.empty()) {
        add(findings, L::Fail, name + " has no IPv4 address");
    } else {
        add(findings, L::Ok, name + " " + facts.ipv4);
    }
    if (!facts.bridge.empty()) {
        add(findings, L::Fail,
            name + " is a member of " + facts.bridge +
                ": the Mac stops answering IGMP queries and the switch drops PTP and streams after a few minutes. "
                "Turn off Internet Sharing, then check " + name + "'s IP address");
    } else if (facts.internetSharing) {
        add(findings, L::Warn, "Internet Sharing is on; if it shares " + name + ", multicast stops after a few minutes");
    }
    for (const auto& [group, routed] : facts.groupRoutes) {
        if (!routed.empty() && routed != name) {
            add(findings, L::Warn,
                group + " routes via " + routed + ", not " + name +
                    ". The driver uses " + name + " directly; other multicast tools on this Mac won't");
        }
    }
    return findings;
}

bool hasFailure(const std::vector<Finding>& findings) {
    for (const auto& finding : findings) {
        if (finding.level == Finding::Level::Fail) return true;
    }
    return false;
}

}  // namespace Ctl
}  // namespace AES67
