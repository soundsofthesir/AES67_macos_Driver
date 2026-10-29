/// @file PtpMessages.cpp

#include "PtpMessages.h"
#include <cstdio>

namespace AES67 {
namespace Ptp {

namespace {

constexpr size_t kHeaderLength = 34;
constexpr size_t kTimestampLength = 10;
constexpr size_t kPortIdentityLength = 10;
constexpr size_t kAnnounceLength = 64;
constexpr size_t kDelayRespLength = kHeaderLength + kTimestampLength + kPortIdentityLength;  // 54
constexpr size_t kEventLength = kHeaderLength + kTimestampLength;                            // 44
constexpr uint32_t kNanosecondsPerSecond = 1000000000;

// controlField values (version 1 compatibility; still sent by version 2 devices)
constexpr uint8_t kControlSync = 0x00;
constexpr uint8_t kControlDelayReq = 0x01;
constexpr uint8_t kControlFollowUp = 0x02;
constexpr uint8_t kControlDelayResp = 0x03;
constexpr uint8_t kControlOther = 0x05;

constexpr uint16_t kFlagTwoStep = 0x0200;
constexpr uint16_t kFlagUnicast = 0x0400;

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint64_t readUnsigned(const uint8_t* p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value = (value << 8) | p[i];
    return value;
}

void write16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void writeUnsigned(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = bytes; i-- > 0;) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

PortIdentity readPortIdentity(const uint8_t* p) {
    PortIdentity id;
    for (size_t i = 0; i < 8; ++i) id.clock.bytes[i] = p[i];
    id.port = read16(p + 8);
    return id;
}

void writePortIdentity(std::vector<uint8_t>& out, const PortIdentity& id) {
    out.insert(out.end(), id.clock.bytes.begin(), id.clock.bytes.end());
    write16(out, id.port);
}

Timestamp readTimestamp(const uint8_t* p) {
    return Timestamp{readUnsigned(p, 6), static_cast<uint32_t>(readUnsigned(p + 6, 4))};
}

void writeTimestamp(std::vector<uint8_t>& out, const Timestamp& t) {
    writeUnsigned(out, t.seconds & 0xFFFFFFFFFFFFULL, 6);
    writeUnsigned(out, t.nanoseconds, 4);
}

std::vector<uint8_t> buildHeader(MessageType type, uint16_t length, uint8_t domain, uint16_t flags,
                                 int64_t correction, const PortIdentity& source, uint16_t sequenceId,
                                 uint8_t control, int8_t logInterval) {
    std::vector<uint8_t> out;
    out.reserve(length);
    out.push_back(static_cast<uint8_t>(type) & 0x0F);  // transportSpecific 0
    out.push_back(0x02);                                // versionPTP 2
    write16(out, length);
    out.push_back(domain);
    out.push_back(0x00);
    write16(out, flags);
    writeUnsigned(out, static_cast<uint64_t>(correction), 8);
    out.insert(out.end(), 4, 0x00);
    writePortIdentity(out, source);
    write16(out, sequenceId);
    out.push_back(control);
    out.push_back(static_cast<uint8_t>(logInterval));
    return out;
}

size_t minimumLength(MessageType type) {
    switch (type) {
        case MessageType::Sync:
        case MessageType::DelayReq:
        case MessageType::FollowUp:
            return kEventLength;
        case MessageType::DelayResp:
            return kDelayRespLength;
        case MessageType::Announce:
            return kAnnounceLength;
        default:
            return kHeaderLength;
    }
}

} // namespace

std::string ClockIdentity::toString() const {
    char text[24];
    std::snprintf(text, sizeof(text), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", bytes[0], bytes[1], bytes[2],
                  bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
    return text;
}

std::optional<Message> parse(const uint8_t* data, size_t length) {
    if (data == nullptr || length < kHeaderLength) {
        return std::nullopt;
    }
    Message m;
    Header& h = m.header;
    h.transportSpecific = static_cast<uint8_t>(data[0] >> 4);
    h.type = static_cast<MessageType>(data[0] & 0x0F);
    h.version = static_cast<uint8_t>(data[1] & 0x0F);
    h.length = read16(data + 2);
    h.domain = data[4];
    h.flags = read16(data + 6);
    h.correction = static_cast<int64_t>(readUnsigned(data + 8, 8));
    h.source = readPortIdentity(data + 20);
    h.sequenceId = read16(data + 30);
    h.control = data[32];
    h.logMessageInterval = static_cast<int8_t>(data[33]);

    if (h.version != 2 || h.length < kHeaderLength || h.length > length || h.length < minimumLength(h.type)) {
        return std::nullopt;
    }

    const uint8_t* body = data + kHeaderLength;
    switch (h.type) {
        case MessageType::Sync:
        case MessageType::DelayReq:
        case MessageType::FollowUp:
            m.timestamp = readTimestamp(body);
            break;
        case MessageType::DelayResp:
            m.timestamp = readTimestamp(body);
            m.requestingPort = readPortIdentity(body + kTimestampLength);
            break;
        case MessageType::Announce: {
            AnnounceBody a;
            a.originTimestamp = readTimestamp(body);
            m.timestamp = a.originTimestamp;
            const uint8_t* p = body + kTimestampLength;
            a.currentUtcOffset = static_cast<int16_t>(read16(p));
            a.grandmasterPriority1 = p[3];
            a.grandmasterClockClass = p[4];
            a.grandmasterClockAccuracy = p[5];
            a.grandmasterOffsetScaledLogVariance = read16(p + 6);
            a.grandmasterPriority2 = p[8];
            for (size_t i = 0; i < 8; ++i) a.grandmasterIdentity.bytes[i] = p[9 + i];
            a.stepsRemoved = read16(p + 17);
            a.timeSource = p[19];
            m.announce = a;
            break;
        }
        default:
            break;  // header only
    }
    if (m.timestamp.nanoseconds >= kNanosecondsPerSecond) {
        return std::nullopt;
    }
    return m;
}

std::vector<uint8_t> buildDelayReq(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, bool unicast) {
    // logMessageInterval 0x7F: "not used" for Delay_Req
    auto out = buildHeader(MessageType::DelayReq, kEventLength, domain, unicast ? kFlagUnicast : 0, 0, source,
                           sequenceId, kControlDelayReq, 0x7F);
    writeTimestamp(out, Timestamp{});
    return out;
}

std::vector<uint8_t> buildSync(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                               bool twoStep, const Timestamp& originTimestamp, int64_t correction) {
    auto out = buildHeader(MessageType::Sync, kEventLength, domain, twoStep ? kFlagTwoStep : 0, correction, source,
                           sequenceId, kControlSync, logInterval);
    writeTimestamp(out, originTimestamp);
    return out;
}

std::vector<uint8_t> buildFollowUp(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                   const Timestamp& preciseOriginTimestamp, int64_t correction) {
    auto out = buildHeader(MessageType::FollowUp, kEventLength, domain, 0, correction, source, sequenceId,
                           kControlFollowUp, logInterval);
    writeTimestamp(out, preciseOriginTimestamp);
    return out;
}

std::vector<uint8_t> buildDelayResp(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                    const Timestamp& receiveTimestamp, const PortIdentity& requestingPort,
                                    int64_t correction) {
    auto out = buildHeader(MessageType::DelayResp, kDelayRespLength, domain, 0, correction, source, sequenceId,
                           kControlDelayResp, logInterval);
    writeTimestamp(out, receiveTimestamp);
    writePortIdentity(out, requestingPort);
    return out;
}

std::vector<uint8_t> buildAnnounce(const PortIdentity& source, uint8_t domain, uint16_t sequenceId, int8_t logInterval,
                                   const AnnounceBody& body) {
    auto out = buildHeader(MessageType::Announce, kAnnounceLength, domain, 0, 0, source, sequenceId, kControlOther,
                           logInterval);
    writeTimestamp(out, body.originTimestamp);
    write16(out, static_cast<uint16_t>(body.currentUtcOffset));
    out.push_back(0x00);  // reserved
    out.push_back(body.grandmasterPriority1);
    out.push_back(body.grandmasterClockClass);
    out.push_back(body.grandmasterClockAccuracy);
    write16(out, body.grandmasterOffsetScaledLogVariance);
    out.push_back(body.grandmasterPriority2);
    out.insert(out.end(), body.grandmasterIdentity.bytes.begin(), body.grandmasterIdentity.bytes.end());
    write16(out, body.stepsRemoved);
    out.push_back(body.timeSource);
    return out;
}

} // namespace Ptp
} // namespace AES67
