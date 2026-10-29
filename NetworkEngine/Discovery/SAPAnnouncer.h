/// @file SAPAnnouncer.h
/// @brief Session Announcement Protocol (RFC 2974) sender for TX streams.
///
/// Receivers that discover streams by SAP (Dante Controller among them) list a
/// stream only while it is announced. Each session is announced at once, then
/// re-announced at a randomised interval, and a deletion is sent when it is
/// withdrawn or the announcer is destroyed.

#pragma once

#include "../../Shared/Types.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AES67 {

class SAPAnnouncer {
public:
    struct Config {
        // The group AES67 devices announce on (administratively scoped
        // 239.255.0.0/16, as Dante and RAVENNA use) and the SAP port
        std::string group{"239.255.255.255"};
        uint16_t port{9875};
        // Nominal re-announcement period; each one is randomised by +/-1/3
        // (RFC 2974). AES67 devices commonly use about 30 s.
        std::chrono::milliseconds interval{30000};
    };

    // Delegating rather than `Config config = Config{}`: a nested struct's
    // default member initializers are not usable in the enclosing class's
    // default arguments.
    SAPAnnouncer() : SAPAnnouncer(Config{}) {}
    explicit SAPAnnouncer(Config config);
    ~SAPAnnouncer();

    SAPAnnouncer(const SAPAnnouncer&) = delete;
    SAPAnnouncer& operator=(const SAPAnnouncer&) = delete;

    /// Announce `sdp` for `id` now and keep re-announcing it. Replacing a
    /// session with different content deletes the old announcement first.
    /// @param interfaceAddress IPv4 address to send from (also the SAP
    ///        originating source); empty uses the default route.
    /// @param ttl Multicast TTL, normally the session's own (0 = this host only).
    void announce(const StreamID& id, const std::string& sdp, const std::string& interfaceAddress, uint8_t ttl);

    /// Send a deletion for `id` and stop announcing it.
    void withdraw(const StreamID& id);

    size_t sessionCount() const;

    /// 16-bit message ID hash of an SDP: changes whenever the SDP does, never 0.
    static uint16_t messageIdHash(const std::string& sdp);

    /// A complete SAP packet: header, IPv4 originating source (network byte
    /// order), "application/sdp" payload type, then the SDP.
    static std::vector<uint8_t> buildPacket(const std::string& sdp, uint32_t originNetworkOrder, bool deletion);

private:
    struct Session {
        std::string sdp;
        std::string interfaceAddress;
        uint8_t ttl;
        std::chrono::steady_clock::time_point nextDue;
    };

    void run();
    void send(const Session& session, bool deletion);  // caller holds mutex_
    int socketFor(const std::string& interfaceAddress, uint8_t ttl);  // caller holds mutex_
    void closeUnusedSockets();  // caller holds mutex_
    std::chrono::steady_clock::duration jitteredInterval();  // caller holds mutex_

    const Config config_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::map<StreamID, Session> sessions_;
    std::map<std::pair<std::string, uint8_t>, int> sockets_;
    std::mt19937 random_{std::random_device{}()};
    std::thread thread_;
};

} // namespace AES67
