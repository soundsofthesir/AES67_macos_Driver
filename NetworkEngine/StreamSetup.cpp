/// @file StreamSetup.cpp
/// @brief Builds and merges streams.json entries.

#include "StreamSetup.h"
#include <arpa/inet.h>
#include <random>

namespace AES67 {
namespace Setup {

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr double kPacketTimeMs = 1.0;
constexpr uint8_t kPayloadType = 97;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool isIPv4(const std::string& text) {
    in_addr addr{};
    return inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

bool validate(const StreamRequest& request, const std::string& interfaceAddress, std::string* error) {
    if (!isMulticastIPv4(request.group)) return fail(error, request.group + " is not an IPv4 multicast address");
    if (request.port == 0) return fail(error, "the port must not be 0");
    if (request.channels == 0 || request.channels > kMaxStreamChannels) {
        return fail(error, "a stream carries 1 to " + std::to_string(kMaxStreamChannels) + " channels");
    }
    if (request.firstDeviceChannel + request.channels > StreamChannelMapper::kMaxDeviceChannels) {
        return fail(error, "the device has " + std::to_string(StreamChannelMapper::kMaxDeviceChannels) + " channels");
    }
    if (request.networkInterface.empty()) return fail(error, "an interface is required");
    if (request.direction == Direction::Send && !isIPv4(interfaceAddress)) {
        return fail(error, request.networkInterface + " has no IPv4 address");
    }
    return true;
}

uint32_t randomSessionID() {
    std::random_device seed;
    return std::uniform_int_distribution<uint32_t>(1, UINT32_MAX)(seed);
}

std::string rawDirection(const JsonValue& raw) {
    const JsonValue* sdp = raw.find("sdp");
    const JsonValue* direction = sdp ? sdp->find("direction") : nullptr;
    const std::string* text = direction ? direction->asString() : nullptr;
    return text ? *text : "";
}

std::string rawGroup(const SavedStreamEntry& entry) {
    if (entry.config) return entry.config->sdp.connectionAddress;
    const JsonValue* sdp = entry.raw.find("sdp");
    const JsonValue* address = sdp ? sdp->find("connectionAddress") : nullptr;
    const std::string* text = address ? address->asString() : nullptr;
    return text ? *text : "";
}

}  // namespace

bool isMulticastIPv4(const std::string& text) {
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
    return (ntohl(addr.s_addr) >> 28) == 0xE;
}

Direction directionOf(const SavedStreamEntry& entry) {
    const std::string direction = entry.config ? entry.config->sdp.direction : rawDirection(entry.raw);
    return (direction == "sendonly" || direction == "sendrecv") ? Direction::Send : Direction::Receive;
}

std::optional<PersistedStreamConfig> buildStream(const StreamRequest& request,
                                                 const std::string& interfaceAddress,
                                                 std::string* error) {
    if (!validate(request, interfaceAddress, error)) return std::nullopt;
    const bool send = request.direction == Direction::Send;

    SDPSession sdp;
    sdp.sessionName = request.name.empty() ? request.group : request.name;
    sdp.connectionAddress = request.group;
    sdp.port = request.port;
    sdp.ttl = request.ttl;
    sdp.encoding = "L24";
    sdp.payloadType = kPayloadType;
    sdp.sampleRate = kSampleRate;
    sdp.numChannels = request.channels;
    sdp.ptime = kPacketTimeMs;
    sdp.framecount = static_cast<uint32_t>(kSampleRate * kPacketTimeMs / 1000.0);
    sdp.direction = send ? "sendonly" : "recvonly";
    if (send) {
        sdp.sessionID = randomSessionID();
        sdp.sessionVersion = 1;
        sdp.originAddress = interfaceAddress;
        sdp.sourceAddress = interfaceAddress;
    }

    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = sdp.sessionName;
    mapping.streamChannelCount = request.channels;
    mapping.deviceChannelStart = request.firstDeviceChannel;
    mapping.deviceChannelCount = request.channels;

    PersistedStreamConfig config = StreamConfigManager::createConfig(sdp, mapping);
    config.networkInterface = request.networkInterface;
    if (!config.isValid()) {
        fail(error, "the stream is not valid");
        return std::nullopt;
    }
    return config;
}

std::optional<std::vector<SavedStreamEntry>> addStream(const std::vector<SavedStreamEntry>& existing,
                                                       const PersistedStreamConfig& stream,
                                                       std::string* error) {
    const SavedStreamEntry added{stream, StreamConfigManager::configToJSON(stream)};
    const Direction direction = directionOf(added);
    const uint16_t first = stream.mapping.deviceChannelStart;
    const uint16_t end = first + stream.sdp.numChannels;

    std::vector<SavedStreamEntry> result;
    result.reserve(existing.size() + 1);
    for (const auto& entry : existing) {
        if (entry.config && directionOf(entry) == direction &&
            entry.config->sdp.connectionAddress == stream.sdp.connectionAddress &&
            entry.config->sdp.port == stream.sdp.port) {
            continue;  // replaced
        }
        if (entry.config && entry.config->enabled && directionOf(entry) == direction) {
            const uint16_t otherFirst = entry.config->mapping.deviceChannelStart;
            const uint16_t otherEnd = otherFirst + entry.config->sdp.numChannels;
            if (first < otherEnd && otherFirst < end) {
                fail(error, "device channels " + std::to_string(first + 1) + "-" + std::to_string(end) +
                                " are used by " + entry.config->sdp.sessionName);
                return std::nullopt;
            }
        }
        result.push_back(entry);
    }
    result.push_back(added);
    return result;
}

std::vector<SavedStreamEntry> removeStreams(const std::vector<SavedStreamEntry>& existing,
                                            const std::string& group,
                                            size_t* removed) {
    std::vector<SavedStreamEntry> result;
    for (const auto& entry : existing) {
        if (rawGroup(entry) != group) result.push_back(entry);
    }
    if (removed) *removed = existing.size() - result.size();
    return result;
}

std::string toFileText(const std::vector<SavedStreamEntry>& entries) {
    std::vector<JsonValue> raw;
    raw.reserve(entries.size());
    for (const auto& entry : entries) {
        raw.push_back(entry.raw);  // as written: fields this version doesn't know survive
    }
    return StreamConfigManager::toJSON({}, raw);
}

std::string ptpFileText(const std::string& networkInterface, uint8_t domain, bool hybrid) {
    JsonValue settings = JsonValue::object();
    settings.add("enabled", JsonValue::boolean(true))
        .add("interface", JsonValue::string(networkInterface))
        .add("domain", JsonValue::number(static_cast<uint64_t>(domain)))
        .add("hybrid", JsonValue::boolean(hybrid));
    return settings.dump() + "\n";
}

}  // namespace Setup
}  // namespace AES67
