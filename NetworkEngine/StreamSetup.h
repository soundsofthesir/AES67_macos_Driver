/// @file StreamSetup.h
/// @brief Builds streams.json entries from a few choices (aes67ctl, Manager).
///
/// The driver reads streams.json only at load. These helpers turn "send the
/// Mac's output to this group on this interface" into an entry the driver
/// accepts, and merge it into the file's existing entries without losing any.
/// They never touch the network or the file system.

#pragma once

#include "StreamConfig.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AES67 {
namespace Setup {

enum class Direction { Send, Receive };

/// AES67 (and ST 2110-30) streams carry at most 8 channels (#171).
constexpr uint16_t kMaxStreamChannels = 8;
constexpr uint16_t kDefaultPort = 5004;

struct StreamRequest {
    Direction direction{Direction::Send};
    std::string group;               ///< IPv4 multicast address
    uint16_t port{kDefaultPort};
    uint16_t channels{2};            ///< 2: the Mac's stereo output
    uint16_t firstDeviceChannel{0};  ///< 0-based; outputs 0-1 carry the Mac's stereo
    std::string networkInterface;    ///< "en10"; required
    std::string name;                ///< session name; empty = a default from the group
    uint8_t ttl{32};
};

/// A complete entry for `request`: L24, 48 kHz, 1 ms packets, a new stream
/// ID and, for a send stream, a random session ID and `interfaceAddress` as
/// its source. nullopt with `error` set if the request can't be a stream.
std::optional<PersistedStreamConfig> buildStream(const StreamRequest& request,
                                                 const std::string& interfaceAddress,
                                                 std::string* error);

/// `existing` with `stream` added. An entry on the same group, port and
/// direction is replaced (as the driver does); every other entry, readable
/// or not, is kept as written. nullopt with `error` set if `stream`'s
/// device channels overlap another entry of the same direction.
std::optional<std::vector<SavedStreamEntry>> addStream(const std::vector<SavedStreamEntry>& existing,
                                                       const PersistedStreamConfig& stream,
                                                       std::string* error);

/// `existing` without the entries on `group` (any port, both directions).
/// `removed` counts them.
std::vector<SavedStreamEntry> removeStreams(const std::vector<SavedStreamEntry>& existing,
                                            const std::string& group,
                                            size_t* removed = nullptr);

/// The file text for `entries`, in their order.
std::string toFileText(const std::vector<SavedStreamEntry>& entries);

/// ptp.json text for following PTP on `networkInterface`.
std::string ptpFileText(const std::string& networkInterface, uint8_t domain, bool hybrid);

/// A dotted-quad IPv4 multicast address (224.0.0.0/4), nothing looser.
bool isMulticastIPv4(const std::string& text);

/// "send"/"receive" for an entry, as the driver will treat it.
Direction directionOf(const SavedStreamEntry& entry);

/// Whether the driver will run a send stream from these entries: an enabled
/// one it can read. aes67ctl keepalive keeps the device's IO running only then.
bool hasEnabledSend(const std::vector<SavedStreamEntry>& entries);

}  // namespace Setup
}  // namespace AES67
