/// @file TestAes67ctl.cpp
/// @brief aes67ctl's checks and packet parsing, from captured command output.

#include "../Tools/aes67ctl/Checks.h"
#include "../Tools/aes67ctl/PacketProbe.h"
#include <iostream>
#include <string>
#include <vector>

using namespace AES67::Ctl;

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

// `ifconfig -a` with Internet Sharing bridging en9 (abridged)
const char* kBridgedIfconfig = R"(lo0: flags=8049<UP,LOOPBACK,RUNNING,MULTICAST> mtu 16384
	inet 127.0.0.1 netmask 0xff000000
en9: flags=8863<UP,BROADCAST,SMART,RUNNING,SIMPLEX,MULTICAST> mtu 1500
	ether 00:11:22:33:44:55
	inet 192.0.2.10 netmask 0xffffff00 broadcast 192.0.2.255
bridge100: flags=8a63<UP,BROADCAST,SMART,RUNNING,ALLMULTI,SIMPLEX,MULTICAST> mtu 1500
	options=3<RXCSUM,TXCSUM>
	ether 00:11:22:33:44:64
	Configuration:
		id 0:0:0:0:0:0 priority 0 hellotime 0 fwddelay 0
	member: en19 flags=3<LEARNING,DISCOVER>
	        ifmaxaddr 0 port 22 priority 0 path cost 0
	member: en9 flags=3<LEARNING,DISCOVER>
	        ifmaxaddr 0 port 9 priority 0 path cost 0
	status: active
)";

const char* kRouteViaVpn = R"(   route to: 239.69.1.2
destination: 224.0.0.0
       mask: 240.0.0.0
  interface: utun4
      flags: <UP,DONE,STATIC,PRCLONING>
)";

Facts healthy() {
    Facts facts;
    facts.interfaceName = "en9";
    facts.interfaceExists = true;
    facts.interfaceUp = true;
    facts.ipv4 = "192.0.2.10";
    facts.driverInstalled = true;
    facts.configDirExists = true;
    facts.configDirOwnedByDriver = true;
    facts.groupRoutes = {{"239.69.1.2", "en9"}};
    return facts;
}

bool mentions(const std::vector<Finding>& findings, Finding::Level level, const std::string& text) {
    for (const auto& f : findings) {
        if (f.level == level && f.text.find(text) != std::string::npos) return true;
    }
    return false;
}

void testParsers() {
    std::cout << "ifconfig and route output are read for bridges and routes" << std::endl;
    CHECK(bridgeOf(kBridgedIfconfig, "en9") == "bridge100", "en9 is a member of bridge100");
    CHECK(bridgeOf(kBridgedIfconfig, "en19") == "bridge100", "en19 too");
    CHECK(bridgeOf(kBridgedIfconfig, "en1").empty(), "en1 is not (a prefix of en19 is no match)");
    CHECK(bridgeOf(kBridgedIfconfig, "lo0").empty(), "lo0 is not");
    CHECK(routeInterface(kRouteViaVpn) == "utun4", "the route's interface is read");
    CHECK(routeInterface("route: writing to routing socket: not in table\n").empty(), "no route reads as unknown");
    CHECK(isPlainInterfaceName("en10") && isPlainInterfaceName("bridge100"), "plain names pass");
    CHECK(!isPlainInterfaceName("") && !isPlainInterfaceName("en0; rm") && !isPlainInterfaceName("-v"),
          "anything else is refused");
}

void testHealthyMac() {
    std::cout << "A healthy Mac has no failures" << std::endl;
    const auto findings = diagnose(healthy());
    CHECK(!hasFailure(findings), "no failure");
    CHECK(mentions(findings, Finding::Level::Ok, "en9 192.0.2.10"), "the address is shown");
}

void testProblems() {
    std::cout << "Each known pitfall is reported" << std::endl;
    Facts bridged = healthy();
    bridged.bridge = "bridge100";
    bridged.internetSharing = true;
    auto findings = diagnose(bridged);
    CHECK(hasFailure(findings) && mentions(findings, Finding::Level::Fail, "member of bridge100"), "a bridged interface fails");
    CHECK(!mentions(findings, Finding::Level::Warn, "Internet Sharing is on"), "the bridge finding covers sharing");

    Facts sharing = healthy();
    sharing.internetSharing = true;
    findings = diagnose(sharing);
    CHECK(!hasFailure(findings) && mentions(findings, Finding::Level::Warn, "Internet Sharing"), "sharing alone warns");

    Facts noAddress = healthy();
    noAddress.ipv4.clear();
    CHECK(mentions(diagnose(noAddress), Finding::Level::Fail, "no IPv4 address"), "a missing address fails");

    Facts down = healthy();
    down.interfaceUp = false;
    CHECK(mentions(diagnose(down), Finding::Level::Fail, "is down"), "a down interface fails");

    Facts missing = healthy();
    missing.interfaceExists = false;
    findings = diagnose(missing);
    CHECK(mentions(findings, Finding::Level::Fail, "en9 not found") && findings.size() == 2,
          "a missing interface fails, with nothing more about it");

    Facts vpn = healthy();
    vpn.groupRoutes = {{"239.69.1.2", "utun4"}};
    CHECK(!hasFailure(diagnose(vpn)) && mentions(diagnose(vpn), Finding::Level::Warn, "routes via utun4"),
          "a VPN-routed group warns");

    Facts noDriver = healthy();
    noDriver.driverInstalled = false;
    CHECK(mentions(diagnose(noDriver), Finding::Level::Fail, "driver not installed"), "a missing driver fails");

    Facts rootOwned = healthy();
    rootOwned.configDirOwnedByDriver = false;
    CHECK(mentions(diagnose(rootOwned), Finding::Level::Warn, "_coreaudiod"), "a root-owned config directory warns");
}

std::vector<uint8_t> rtp(std::vector<uint8_t> payload, uint8_t firstByte = 0x80) {
    std::vector<uint8_t> packet = {firstByte, 97, 0, 1, 0, 0, 0, 48, 1, 2, 3, 4};
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

void testPacketParsing() {
    std::cout << "RTP payloads and L24 peaks are read" << std::endl;
    const auto packet = rtp({0x00, 0x00, 0x01, 0xFF, 0x00, 0x00, 0x7F, 0xFF, 0xFF});
    const auto [start, end] = rtpPayload(packet);
    CHECK(start == 12 && end == packet.size(), "the payload follows the 12-byte header");
    CHECK(peakL24(packet.data() + start, end - start) == 0x7FFFFF, "the loudest sample is full scale");
    const std::vector<uint8_t> negative = {0x80, 0x00, 0x00};
    CHECK(peakL24(negative.data(), negative.size()) == 0x800000, "negative full scale is read as a magnitude");
    const std::vector<uint8_t> quiet = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    CHECK(peakL24(quiet.data(), quiet.size()) == 1, "-1 is a peak of 1");

    auto withCsrc = rtp({0, 0, 0, 0xAA, 0xBB, 0xCC, 0x00, 0x10, 0x00}, 0x81);  // one CSRC
    CHECK(rtpPayload(withCsrc).first == 16, "CSRCs are skipped");
    CHECK(rtpPayload(rtp({}, 0x40)).second == 0, "not version 2: not RTP");
    CHECK(rtpPayload(std::vector<uint8_t>(5, 0x80)).second == 0, "too short: not RTP");
}

}  // namespace

int main() {
    testParsers();
    testHealthyMac();
    testProblems();
    testPacketParsing();
    std::cout << "aes67ctl: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
