/// @file PacketProbe.h
/// @brief Listens to a stream's group for a few seconds (receive only).

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AES67 {
namespace Ctl {

struct ProbeResult {
    bool joined{false};   ///< false: the socket or join failed (Local Network permission?)
    size_t packets{0};    ///< RTP packets seen
    std::optional<double> peakDbfs;  ///< loudest L24 sample; nullopt when all silent
};

/// Count RTP packets on `group:port`, joined on `interfaceAddress`, for `duration`.
ProbeResult probe(const std::string& group, uint16_t port, const std::string& interfaceAddress,
                  std::chrono::milliseconds duration);

/// The RTP payload of `packet` (after the header, CSRCs and any extension),
/// or an empty range if it isn't RTP version 2. Exposed for tests.
std::pair<size_t, size_t> rtpPayload(const std::vector<uint8_t>& packet);

/// The largest absolute L24 sample in `payload` (big-endian 24-bit), 0..8388608.
uint32_t peakL24(const uint8_t* payload, size_t size);

}  // namespace Ctl
}  // namespace AES67
