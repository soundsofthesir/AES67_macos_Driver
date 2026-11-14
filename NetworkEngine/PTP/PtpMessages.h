/// @file PtpMessages.h
/// @brief PTP (IEEE 1588-2008, version 2) messages: parsing and building.
///
/// Covers what an ordinary slave clock using the end-to-end delay mechanism
/// needs: Sync, Follow_Up, Delay_Req, Delay_Resp and Announce. Other message
/// types parse their header only, so a slave can ignore them by type.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AES67 {
namespace Ptp {

enum class MessageType : uint8_t {
    Sync = 0x0,
    DelayReq = 0x1,
    PdelayReq = 0x2,
    PdelayResp = 0x3,
    FollowUp = 0x8,
    DelayResp = 0x9,
    PdelayRespFollowUp = 0xA,
    Announce = 0xB,
    Signaling = 0xC,
    Management = 0xD,
};

/// EUI-64 clock identity, written as AES67 SDP does: "00-1D-C1-FF-FE-D1-7B-F3".
struct ClockIdentity {
    std::array<uint8_t, 8> bytes{};

    std::string toString() const;
    bool operator==(const ClockIdentity& other) const { return bytes == other.bytes; }
    bool operator!=(const ClockIdentity& other) const { return bytes != other.bytes; }
    bool operator<(const ClockIdentity& other) const { return bytes < other.bytes; }
};

struct PortIdentity {
    ClockIdentity clock;
    uint16_t port{0};

    bool operator==(const PortIdentity& other) const { return clock == other.clock && port == other.port; }
    bool operator!=(const PortIdentity& other) const { return !(*this == other); }
    bool operator<(const PortIdentity& other) const {
        return clock < other.clock || (clock == other.clock && port < other.port);
    }
};

/// PTP time: 48-bit seconds and nanoseconds since the grandmaster's epoch
/// (TAI 1970 for most, power-on for some, such as a Behringer WING).
struct Timestamp {
    uint64_t seconds{0};
    uint32_t nanoseconds{0};

    /// Fits in 64 bits for seconds up to about 1.8e10; parse() rejects
    /// anything past 2^34, so only a hand-built Timestamp can overflow
    uint64_t totalNanoseconds() const { return seconds * 1000000000ULL + nanoseconds; }
    bool operator==(const Timestamp& other) const {
        return seconds == other.seconds && nanoseconds == other.nanoseconds;
    }
};

struct Header {
    uint8_t transportSpecific{0};
    MessageType type{MessageType::Sync};
    uint8_t version{2};
    uint16_t length{0};
    uint8_t domain{0};
    uint16_t flags{0};
    int64_t correction{0};  // nanoseconds x 2^16
    PortIdentity source;
    uint16_t sequenceId{0};
    uint8_t control{0};
    int8_t logMessageInterval{0};

    bool twoStep() const { return (flags & 0x0200) != 0; }
    bool unicast() const { return (flags & 0x0400) != 0; }
    bool ptpTimescale() const { return (flags & 0x0008) != 0; }
    double correctionNanoseconds() const { return static_cast<double>(correction) / 65536.0; }
};

struct AnnounceBody {
    Timestamp originTimestamp;
    int16_t currentUtcOffset{0};
    uint8_t grandmasterPriority1{128};
    uint8_t grandmasterClockClass{248};
    uint8_t grandmasterClockAccuracy{0xFE};
    uint16_t grandmasterOffsetScaledLogVariance{0xFFFF};
    uint8_t grandmasterPriority2{128};
    ClockIdentity grandmasterIdentity;
    uint16_t stepsRemoved{0};
    uint8_t timeSource{0xA0};
};

struct Message {
    Header header;
    /// Sync and Delay_Req: originTimestamp. Follow_Up: preciseOriginTimestamp.
    /// Delay_Resp: receiveTimestamp. Announce: originTimestamp.
    Timestamp timestamp;
    std::optional<PortIdentity> requestingPort;  // Delay_Resp
    std::optional<AnnounceBody> announce;        // Announce
};

/// Parse one PTP datagram. Returns nullopt for anything malformed: shorter
/// than its header or its type's body, a messageLength beyond the datagram,
/// or a version other than 2.
std::optional<Message> parse(const uint8_t* data, size_t length);

/// A Delay_Req (origin timestamp zero: t3 is taken locally when it is sent).
/// @param unicast Set the unicast flag (hybrid mode sends it to the master's address).
std::vector<uint8_t> buildDelayReq(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, bool unicast);

// Master-side messages, used to script a master in tests
std::vector<uint8_t> buildSync(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                               bool twoStep, const Timestamp& originTimestamp, int64_t correction = 0);
std::vector<uint8_t> buildFollowUp(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                   const Timestamp& preciseOriginTimestamp, int64_t correction = 0);
std::vector<uint8_t> buildDelayResp(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                    const Timestamp& receiveTimestamp, const PortIdentity& requestingPort,
                                    int64_t correction = 0);
std::vector<uint8_t> buildAnnounce(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                   const AnnounceBody& body);

} // namespace Ptp
} // namespace AES67
