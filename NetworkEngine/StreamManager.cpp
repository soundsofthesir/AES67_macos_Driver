//
// StreamManager.cpp
// AES67 macOS Driver - Build #9
// Unified management of all AES67 streams with validation
//

#include "StreamManager.h"
#include "NetworkUtils.h"
#include "NetworkInterfaceDetection.h"
#include "../Driver/DebugLog.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#include <ctime>

namespace AES67 {

namespace {

/// A string member of a member of an entry's raw JSON ("" if absent or not a string).
std::string rawText(const JsonValue& raw, const char* object, const char* key) {
    const JsonValue* parent = raw.find(object);
    const JsonValue* value = parent ? parent->find(key) : nullptr;
    const std::string* text = value ? value->asString() : nullptr;
    return text ? *text : std::string();
}

/// A saved entry's ID, read from the raw JSON if the entry couldn't be read
/// as a whole (null if it has none).
StreamID savedStreamID(const SavedStreamEntry& entry) {
    if (entry.config) return entry.config->mapping.streamID;
    return StreamID(rawText(entry.raw, "mapping", "streamID"));
}

/// `mapping` for a stream of `numChannels` channels, covering all of them:
/// its receiver or transmitter routes every channel, whatever count the
/// caller or the file gives (#69).
ChannelMapping wholeStreamMapping(ChannelMapping mapping, uint16_t numChannels) {
    mapping.streamChannelCount = numChannels;
    mapping.deviceChannelCount = numChannels;
    return mapping;
}

/// The channels a saved entry holds while it isn't running: its mapping, or
/// for an entry that couldn't be read as a whole, the ID and channel range
/// read from its raw JSON. nullopt if the range can't be read; the mapper
/// rejects the rest (a null ID, a range past the device).
std::optional<ChannelMapping> keptMapping(const SavedStreamEntry& entry) {
    if (entry.config) return wholeStreamMapping(entry.config->mapping, entry.config->sdp.numChannels);

    const JsonValue* mapping = entry.raw.find("mapping");
    const JsonValue* start = mapping ? mapping->find("deviceChannelStart") : nullptr;
    const JsonValue* count = mapping ? mapping->find("deviceChannelCount") : nullptr;
    const std::optional<uint64_t> first = start ? start->asUInt64() : std::nullopt;
    const std::optional<uint64_t> channels = count ? count->asUInt64() : std::nullopt;
    if (!first || !channels || *first > StreamChannelMapper::kMaxDeviceChannels ||
        *channels > StreamChannelMapper::kMaxDeviceChannels) {
        return std::nullopt;
    }

    ChannelMapping held;
    held.streamID = savedStreamID(entry);
    held.streamName = rawText(entry.raw, "sdp", "sessionName");
    held.deviceChannelStart = static_cast<uint16_t>(*first);
    held.deviceChannelCount = static_cast<uint16_t>(*channels);
    held.streamChannelCount = held.deviceChannelCount;
    return held;
}

/// A saved entry's direction (an unreadable entry's as written, recvonly
/// if it has none, as when it is read whole).
StreamManager::Direction keptDirection(const SavedStreamEntry& entry) {
    if (entry.config) return StreamManager::directionOf(entry.config->sdp);
    SDPSession sdp;
    const std::string direction = rawText(entry.raw, "sdp", "direction");
    if (!direction.empty()) sdp.direction = direction;
    return StreamManager::directionOf(sdp);
}

/// A saved entry, enabled but not loaded, that a new stream on the same
/// group, port and direction replaces.
bool isFailedCopyOf(const SavedStreamEntry& entry, const SDPSession& sdp) {
    if (!entry.config || !entry.config->enabled) return false;
    const SDPSession& saved = entry.config->sdp;
    return saved.connectionAddress == sdp.connectionAddress && saved.port == sdp.port &&
           StreamManager::directionOf(saved) == StreamManager::directionOf(sdp);
}

/// Calls a function on leaving scope, unless dismissed first.
class UndoOnExit {
public:
    explicit UndoOnExit(std::function<void()> undo) : undo_(std::move(undo)) {}
    ~UndoOnExit() { if (undo_) undo_(); }
    UndoOnExit(const UndoOnExit&) = delete;
    UndoOnExit& operator=(const UndoOnExit&) = delete;
    void dismiss() { undo_ = nullptr; }

private:
    std::function<void()> undo_;
};

} // namespace

StreamManager::StreamManager(RxContext rxContext, TxContext txContext, SAPAnnouncer::Config sapConfig,
                             NetworkMonitor::Config networkConfig)
    : rxContext_(rxContext)
    , txContext_(txContext)
    , announcer_(std::move(sapConfig))
    , configManager_(std::make_unique<StreamConfigManager>())
{
    networkMonitor_ = std::make_unique<NetworkMonitor>(
        std::move(networkConfig), [this](const std::string& networkInterface) { onInterfaceChanged(networkInterface); });
}

StreamManager::~StreamManager() {
    networkMonitor_.reset();  // no interface change may be handled from here on
    removeAllStreams();
}

//
// Stream Management - RX
//

StreamID StreamManager::addStream(const SDPSession& sdp) {
    // Auto-create channel mapping
    auto optMapping = inputMapper_.createDefaultMapping(sdp);
    if (!optMapping) {
        AES67_LOGF("StreamManager::addStream: failed to create default mapping for '%s' (%u channels)",
                   sdp.sessionName.c_str(), sdp.numChannels);
        return StreamID::null();
    }

    return addStream(sdp, *optMapping);
}

StreamID StreamManager::addStream(const SDPSession& announced, const ChannelMapping& mapping) {
    return addStream(announced, mapping, std::string{});
}

StreamID StreamManager::addStream(const SDPSession& announced, const ChannelMapping& mapping,
                                  const std::string& networkInterface) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // A stored stream's direction is our role: the sender's SDP may say
    // sendonly (its own view), which would otherwise make this a transmitter
    // on reload, sending to the sender's group
    SDPSession sdp = announced;
    sdp.direction = "recvonly";

    // A saved copy of this stream that failed to load lends it its channels,
    // and holds them again if this add fails
    releaseKeptCopiesOf(sdp);
    UndoOnExit reserveAgain([this] { reserveKeptChannels(); });

    // Validate stream can be added
    std::string error;
    if (!canAddStream(sdp, &error)) {
        AES67_LOGF("StreamManager::addStream: validation failed for '%s': %s",
                   sdp.sessionName.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate unique stream ID
    StreamID id = StreamID::generate();

    // Check if stream already exists (shouldn't happen with UUIDs but be safe)
    if (streams_.find(id) != streams_.end()) {
        AES67_LOGF("StreamManager::addStream: duplicate stream ID for '%s'",
                   sdp.sessionName.c_str());
        return StreamID::null();
    }

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = wholeStreamMapping(mapping, sdp.numChannels);
    completeMapping.streamID = id;
    completeMapping.streamName = sdp.sessionName;

    // Add mapping to mapper
    if (!inputMapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::addStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   sdp.sessionName.c_str(), mapping.deviceChannelStart, sdp.numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = false;
    managed.networkInterface = networkInterface;

    // Create RTP receiver
    managed.receiver = createReceiver(sdp, completeMapping, 0, networkInterface);
    if (!managed.receiver) {
        AES67_LOGF("StreamManager::addStream: failed to create RTP receiver for '%s'",
                   sdp.sessionName.c_str());
        inputMapper_.removeMapping(id);
        return StreamID::null();
    }

    // Only start receiver if IO is active (a Core Audio client has called StartIO).
    // Otherwise the stream is created dormant and will be started by setIOActive(true).
    if (ioActive_.load()) {
        if (!managed.receiver->start()) {
            AES67_LOGF("StreamManager::addStream: failed to start RTP receiver for '%s' (%s:%u)",
                       sdp.sessionName.c_str(), sdp.connectionAddress.c_str(), sdp.port);
            inputMapper_.removeMapping(id);
            return StreamID::null();
        }
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = sdp.sessionName;
    managed.info.description = sdp.sessionInfo;

    // Network addresses
    managed.info.source.ip = sdp.sourceAddress;
    managed.info.source.port = sdp.port;
    managed.info.multicast.ip = sdp.connectionAddress;
    managed.info.multicast.port = sdp.port;
    managed.info.multicast.ttl = sdp.ttl;

    // Audio format
    if (sdp.encoding == "L16") {
        managed.info.encoding = AudioEncoding::L16;
    } else if (sdp.encoding == "L24") {
        managed.info.encoding = AudioEncoding::L24;
    } else {
        managed.info.encoding = AudioEncoding::Unknown;
    }

    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = sdp.numChannels;
    managed.info.payloadType = sdp.payloadType;

    // Timing
    managed.info.ptime = static_cast<uint32_t>(std::llround(sdp.ptime * 1000.0));  // microseconds
    managed.info.framecount = sdp.framecount;

    // PTP
    managed.info.ptp.domain = sdp.ptpDomain;

    // State
    managed.info.isActive = true;
    managed.info.isConnected = false;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);
    networkMonitor_->watch(streams_[id].networkInterface);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    reserveAgain.dismiss();
    dropKeptCopiesOf(streams_[id].sdp);
    autoSaveIfEnabled();

    return id;
}

StreamID StreamManager::importSDPFile(const std::string& filepath) {
    // Read SDP file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return StreamID::null();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string sdpContent = buffer.str();

    // Parse SDP
    auto sdpSession = SDPParser::parseString(sdpContent);
    if (!sdpSession) {
        return StreamID::null();
    }

    // Add stream with auto-mapping
    return addStream(*sdpSession);
}

bool StreamManager::removeStream(const StreamID& id) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        // A saved stream that isn't running: remove it from the file
        const auto kept = std::find_if(keptEntries_.begin(), keptEntries_.end(),
                                       [&](const KeptStream& kept) { return savedStreamID(kept.entry) == id; });
        if (id.isNull() || kept == keptEntries_.end()) {
            return false;
        }
        releaseKeptChannels(*kept);
        keptEntries_.erase(kept);
        reserveKeptChannels();
        autoSaveIfEnabled();
        return true;
    }

    // Save info for callback before deletion
    StreamInfo info = it->second.info;

    // Stop receiver/transmitter
    if (it->second.receiver) {
        it->second.receiver->stop();
    }
    if (it->second.transmitter) {
        it->second.transmitter->stop();
        announcer_.withdraw(id);
    }

    // Remove from mapper; a saved stream may now take the freed channels
    mapperFor(it->second).removeMapping(id);

    // Remove from map
    streams_.erase(it);
    reserveKeptChannels();

    // Notify callback
    notifyStreamRemoved(info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return true;
}

void StreamManager::removeAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Stop all streams
    for (auto& pair : streams_) {
        if (pair.second.receiver) {
            pair.second.receiver->stop();
        }
        if (pair.second.transmitter) {
            pair.second.transmitter->stop();
            announcer_.withdraw(pair.first);
        }
        mapperFor(pair.second).removeMapping(pair.first);

        notifyStreamRemoved(pair.second.info);
    }

    // Saved streams not running keep their channels
    streams_.clear();
    reserveKeptChannels();
}

//
// Stream Management - TX
//

StreamID StreamManager::createTxStream(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    const ChannelMapping& mapping
) {
    return createTxStream(name, multicastIP, port, numChannels, mapping, TxOptions{});
}

StreamID StreamManager::createTxStream(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    const ChannelMapping& mapping,
    const TxOptions& options
) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Build SDP session for transmit stream
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.numChannels = numChannels;
    sdp.sampleRate = currentDeviceSampleRate_.load();
    sdp.encoding = "L24"; // Use L24 for best quality
    sdp.payloadType = 97; // Dynamic payload type
    sdp.direction = "sendonly"; // loadSavedStreams() relies on this to recreate a transmitter
    // A random session ID, saved with the stream: unique even for streams
    // created in the same second, and stable across reloads, so SAP receivers
    // list each stream exactly once
    sdp.sessionID = std::uniform_int_distribution<uint32_t>(1, UINT32_MAX)(sessionIdRandom_);
    sdp.sessionVersion = 1;
    sdp.ttl = options.ttl;
    sdp.framecount = framesPerPacket(sdp);

    // Dante receivers only subscribe to AES67 multicast inside the prefix in
    // their AES67 settings (default 239.69.0.0/16). Outside it a stream is
    // listed in Dante Controller but never subscribes (seen with a Behringer
    // WING); other AES67 receivers have no such limit, so warn, don't refuse.
    if (multicastIP.rfind("239.69.", 0) != 0) {
        AES67_LOGF("StreamManager::createTxStream: %s is outside 239.69.0.0/16; Dante receivers with the default "
                   "AES67 prefix will list '%s' but not subscribe to it", multicastIP.c_str(), name.c_str());
    }

    // As in addStream: a failed saved copy lends its channels until this succeeds
    releaseKeptCopiesOf(sdp);
    UndoOnExit reserveAgain([this] { reserveKeptChannels(); });

    // Validate
    std::string error;
    if (!canAddStream(sdp, &error)) {
        AES67_LOGF("StreamManager::createTxStream: validation failed for '%s': %s",
                   name.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate stream ID
    StreamID id = StreamID::generate();

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = wholeStreamMapping(mapping, numChannels);
    completeMapping.streamID = id;
    completeMapping.streamName = name;

    // Add mapping
    if (!outputMapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::createTxStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   name.c_str(), mapping.deviceChannelStart, numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = true;
    managed.networkInterface = options.networkInterface;

    // Create RTP transmitter
    managed.transmitter = createTransmitter(sdp, completeMapping, options.networkInterface);
    if (!managed.transmitter) {
        AES67_LOGF("StreamManager::createTxStream: failed to create RTP transmitter for '%s'",
                   name.c_str());
        outputMapper_.removeMapping(id);
        return StreamID::null();
    }

    // Transmit continuously from now on, whether or not Core Audio IO runs
    if (!managed.transmitter->start()) {
        AES67_LOGF("StreamManager::createTxStream: failed to start RTP transmitter for '%s' (%s:%u)",
                   name.c_str(), multicastIP.c_str(), port);
        outputMapper_.removeMapping(id);
        return StreamID::null();
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = name;
    managed.info.multicast.ip = multicastIP;
    managed.info.multicast.port = port;
    managed.info.encoding = AudioEncoding::L24;
    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = numChannels;
    managed.info.payloadType = sdp.payloadType;
    managed.info.isActive = true;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);
    networkMonitor_->watch(streams_[id].networkInterface);
    announceTx(id, streams_[id]);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    reserveAgain.dismiss();
    dropKeptCopiesOf(streams_[id].sdp);
    autoSaveIfEnabled();

    return id;
}

bool StreamManager::exportSDPFile(const StreamID& id, const std::string& filepath) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Generate SDP content
    std::string sdpContent = SDPParser::generate(it->second.sdp);
    if (sdpContent.empty()) {
        return false;
    }

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << sdpContent;
    return true;
}

//
// Channel Mapping
//

bool StreamManager::updateMapping(const StreamID& id, const ChannelMapping& newMapping) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }
    ManagedStream& managed = it->second;
    if (!managed.receiver && !managed.transmitter) {
        return false;
    }

    const ChannelMapping previous = managed.mapping;
    ChannelMapping completeMapping = wholeStreamMapping(newMapping, managed.sdp.numChannels);
    completeMapping.streamID = id;
    completeMapping.streamName = previous.streamName;

    StreamChannelMapper& mapper = mapperFor(managed);
    if (!mapper.updateMapping(completeMapping)) {
        return false;
    }

    // Move the receiver or transmitter, running again afterwards if it ran before
    const bool wasRunning = managed.receiver ? managed.receiver->isRunning() : managed.transmitter->isRunning();
    const auto routeTo = [&managed, wasRunning](const ChannelMapping& mapping) {
        const auto route = [&](auto& endpoint) {
            return endpoint.updateMapping(mapping) && (!wasRunning || endpoint.isRunning() || endpoint.start());
        };
        return managed.receiver ? route(*managed.receiver) : route(*managed.transmitter);
    };

    if (!routeTo(completeMapping)) {
        // Nothing has taken the old channels while we hold the lock: put the stream back on them (#70)
        mapper.updateMapping(previous);
        if (!routeTo(previous)) {
            AES67_LOGF("StreamManager::updateMapping: '%s' could not restart on its old channels",
                       previous.streamName.c_str());
        }
        return false;
    }

    // A saved stream may take the channels it left
    managed.mapping = completeMapping;
    reserveKeptChannels();
    notifyStreamStatusChanged(managed.info);

    // Auto-save configuration
    autoSaveIfEnabled();
    return true;
}

std::optional<ChannelMapping> StreamManager::getMapping(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    const auto it = streams_.find(id);
    if (it == streams_.end()) {
        return std::nullopt;
    }
    return it->second.mapping;
}

std::vector<ChannelMapping> StreamManager::getAllMappings(Direction direction) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    // The mappers also hold saved streams' channels (getStreamsNotLoaded())
    std::vector<ChannelMapping> mappings = mapperFor(direction).getAllMappings();
    mappings.erase(std::remove_if(mappings.begin(), mappings.end(),
                                  [&](const ChannelMapping& m) { return streams_.count(m.streamID) == 0; }),
                   mappings.end());
    return mappings;
}

//
// Query
//

std::vector<StreamInfo> StreamManager::getActiveStreams() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<StreamInfo> activeStreams;
    for (const auto& pair : streams_) {
        if (pair.second.info.isActive) {
            activeStreams.push_back(pair.second.info);
        }
    }

    return activeStreams;
}

std::optional<StreamInfo> StreamManager::getStreamInfo(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it != streams_.end()) {
        return it->second.info;
    }

    return std::nullopt;
}

bool StreamManager::hasStream(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.find(id) != streams_.end();
}

size_t StreamManager::getStreamCount() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.size();
}

//
// Validation
//

bool StreamManager::canAddStream(const SDPSession& sdp, std::string* errorOut) const {
    if (!validateSampleRate(sdp, errorOut)) {
        return false;
    }

    if (!validateChannelAvailability(sdp.numChannels, directionOf(sdp), errorOut)) {
        return false;
    }

    if (!validateNetworkConfig(sdp, errorOut)) {
        return false;
    }

    return true;
}

std::string StreamManager::getAddStreamError(const SDPSession& sdp) const {
    std::string error;
    canAddStream(sdp, &error);
    return error;
}

//
// Device State
//

void StreamManager::setIOActive(bool active) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    bool wasActive = ioActive_.exchange(active);
    if (wasActive == active) {
        return; // No state change
    }

    // Receivers follow Core Audio IO; transmitters run whenever configured
    if (active) {
        AES67_LOGF("StreamManager::setIOActive: Starting receivers (%zu stream(s))", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver && !managed.receiver->start()) {
                AES67_LOGF("StreamManager::setIOActive: failed to start the receiver for '%s'",
                           managed.sdp.sessionName.c_str());
            }
        }
    } else {
        AES67_LOGF("StreamManager::setIOActive: Stopping receivers (%zu stream(s))", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver) {
                managed.receiver->stop();
            }
        }
    }
}

bool StreamManager::setDeviceSampleRate(double sampleRate) {
    if (sampleRate < 44100 || sampleRate > 384000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(streamsMutex_);

    if (!streamsSupportSampleRate(sampleRate)) {
        return false;
    }

    currentDeviceSampleRate_.store(sampleRate);
    return true;
}

bool StreamManager::isSampleRateCompatible(double sampleRate) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streamsSupportSampleRate(sampleRate);
}

bool StreamManager::streamsSupportSampleRate(double sampleRate) const {
    for (const auto& pair : streams_) {
        if (std::abs(pair.second.sdp.sampleRate - sampleRate) > 0.1) {
            return false;
        }
    }
    return true;
}

size_t StreamManager::getAvailableChannelCount(Direction direction) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapperFor(direction).getAvailableChannelCount();
}

StreamManager::Direction StreamManager::directionOf(const SDPSession& sdp) {
    return (sdp.direction == "sendonly" || sdp.direction == "sendrecv") ? Direction::Transmit : Direction::Receive;
}

//
// Validation Helpers
//

bool StreamManager::validateSampleRate(const SDPSession& sdp, std::string* errorOut) const {
    const double deviceRate = currentDeviceSampleRate_.load();

    if (std::abs(sdp.sampleRate - deviceRate) > 0.1) {
        if (errorOut) {
            *errorOut = "Sample rate mismatch: stream=" + std::to_string(static_cast<int>(sdp.sampleRate)) +
                       " Hz, device=" + std::to_string(static_cast<int>(deviceRate)) + " Hz";
        }
        return false;
    }

    return true;
}

bool StreamManager::validateChannelAvailability(uint16_t numChannels, Direction direction, std::string* errorOut) const {
    if (numChannels == 0 || numChannels > 128) {
        if (errorOut) {
            *errorOut = "Invalid channel count: " + std::to_string(numChannels) + " (must be 1-128)";
        }
        return false;
    }

    size_t available = mapperFor(direction).getAvailableChannelCount();
    if (numChannels > available) {
        if (errorOut) {
            *errorOut = "Insufficient channels: need " + std::to_string(numChannels) +
                       ", have " + std::to_string(available);
        }
        return false;
    }

    return true;
}

bool StreamManager::validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const {
    if (sdp.connectionAddress.empty()) {
        if (errorOut) {
            *errorOut = "Missing multicast IP address";
        }
        return false;
    }

    // Check for valid multicast range (239.x.x.x for AES67)
    if (sdp.connectionAddress.substr(0, 4) != "239.") {
        if (errorOut) {
            *errorOut = "Invalid multicast IP: " + sdp.connectionAddress +
                       " (AES67 requires 239.x.x.x)";
        }
        return false;
    }

    if (sdp.port == 0) {
        if (errorOut) {
            *errorOut = "Invalid port: 0";
        }
        return false;
    }

    // Check multicast routing configuration
    // Get primary ethernet interface for the check
    std::string primaryIface = NetworkUtils::getPrimaryEthernetInterface();

    if (!NetworkUtils::hasMulticastRoute(primaryIface)) {
        if (errorOut) {
            std::string iface = primaryIface.empty() ? "en0" : primaryIface;
            *errorOut = "WARNING: Multicast routing not configured. Audio may not flow.\n"
                       "To fix, run: " + NetworkUtils::getMulticastRouteCommand(iface) + "\n"
                       "Stream will be added but may not receive traffic.";
        }
        // Log warning but don't block stream addition
        AES67_LOG("WARNING: Multicast routing not configured - audio may not flow");
        if (!primaryIface.empty()) {
            AES67_LOGF("Run: %s", NetworkUtils::getMulticastRouteCommand(primaryIface).c_str());
        }
        // Return true to allow stream addition with warning
    }

    return true;
}

//
// Stream Creation Helpers
//

std::unique_ptr<RTPReceiver> StreamManager::createReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    size_t jitterBufferDepth,
    const std::string& networkInterface
) {
    // Receivers place decoded network audio by timestamp (Network → Core Audio).
    // jitterBufferDepth is a legacy setting: the playout buffer is sized from
    // the link offset instead.
    (void)jitterBufferDepth;
    return std::make_unique<RTPReceiver>(sdp, mapping, rxContext_, networkInterface);
}

void StreamManager::announceTx(const StreamID& id, const ManagedStream& managed) {
    const std::string address = NetworkInterfaceDetection::resolveIPv4Address(managed.networkInterface);
    if (address.empty()) {
        AES67_LOGF("StreamManager: no IPv4 address for interface '%s'; '%s' is not announced",
                   managed.networkInterface.c_str(), managed.sdp.sessionName.c_str());
        return;
    }

    // The description receivers subscribe from
    SDPSession sdp = managed.sdp;
    sdp.direction = "recvonly";  // AES67: what a receiver does with the stream
    sdp.originNetworkType = "IN";
    sdp.originAddressType = "IP4";
    sdp.originAddress = address;
    sdp.framecount = managed.transmitter->framesPerPacket();

    if (ptpGrandmaster_) {
        // RTP timestamps are the grandmaster's time plus the mediaclk offset
        sdp.ptpMasterMAC = ptpGrandmaster_->identity;
        sdp.ptpDomain = ptpGrandmaster_->domain;
        sdp.customAttributes.erase("ts-refclk");
        if (sdp.mediaClockType.empty()) {
            sdp.mediaClockType = "direct=0";
        }
    } else {
        // No PTP: the media clock is this Mac's own. Say so (RFC 7273
        // localmac) rather than claim a PTP reference
        sdp.ptpDomain = -1;
        sdp.ptpMasterMAC.clear();
        const std::string mac = NetworkInterfaceDetection::getInterfaceMACAddress(
            NetworkInterfaceDetection::getInterfaceForIPAddress(address));
        if (!mac.empty()) {
            sdp.customAttributes["ts-refclk"] = "localmac=" + mac;
        }
    }

    announcer_.announce(id, SDPParser::generate(sdp), address, sdp.ttl);
}

void StreamManager::setPtpGrandmaster(const PtpGrandmaster& grandmaster) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    if (ptpGrandmaster_ == grandmaster) {
        return;
    }
    ptpGrandmaster_ = grandmaster;
    AES67_LOGF("StreamManager: PTP grandmaster %s, domain %u: re-announcing TX streams", grandmaster.identity.c_str(),
               static_cast<unsigned>(grandmaster.domain));
    for (auto& [id, managed] : streams_) {
        if (managed.transmitter) {
            ++managed.sdp.sessionVersion;  // RFC 4566: a changed description gets a new version
            announceTx(id, managed);
        }
    }
}

std::optional<StreamManager::PtpGrandmaster> StreamManager::ptpGrandmaster() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return ptpGrandmaster_;
}

namespace {
// Between streams restarted on an interface change: long enough for a thread
// woken on the streams lock to take it
constexpr auto kInterfaceRestartPause = std::chrono::microseconds(500);
}

void StreamManager::onInterfaceChanged(const std::string& networkInterface) {
    std::vector<StreamID> affected;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        for (const auto& [id, managed] : streams_) {
            if (managed.networkInterface == networkInterface) {
                affected.push_back(id);
            }
        }
    }

    // One stream at a time under the lock, so StartIO and the rest wait for
    // one stream's thread joins at most, not every stream's (#71). The mutex
    // isn't fair: without a pause between streams this thread takes it back
    // before a woken waiter runs
    bool first = true;
    for (const StreamID& id : affected) {
        if (!first) std::this_thread::sleep_for(kInterfaceRestartPause);
        first = false;
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(id);
        if (it == streams_.end() || it->second.networkInterface != networkInterface) {
            continue;  // removed or moved meanwhile
        }
        ManagedStream& managed = it->second;
        // Rejoin (a fresh IGMP report, so switches forward at once) and
        // resend on the interface as it is now
        if (managed.receiver && ioActive_.load()) {
            managed.receiver->stop();
            if (!managed.receiver->start()) {
                AES67_LOGF("StreamManager: '%s' could not rejoin on interface '%s'; retried on its next change",
                           managed.sdp.sessionName.c_str(), networkInterface.c_str());
            }
            networkRestarts_.fetch_add(1);
        }
        if (managed.transmitter) {
            managed.transmitter->stop();
            if (managed.transmitter->start()) {
                announceTx(id, managed);  // its address may have changed
            } else {
                announcer_.withdraw(id);
                AES67_LOGF("StreamManager: '%s' could not restart on interface '%s'; retried on its next change",
                           managed.sdp.sessionName.c_str(), networkInterface.c_str());
            }
            networkRestarts_.fetch_add(1);
        }
    }
    AES67_LOGF("StreamManager: interface '%s' changed; its streams restarted",
               networkInterface.empty() ? "auto" : networkInterface.c_str());
}

std::optional<StatisticsSnapshot> StreamManager::getStreamStatistics(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return std::nullopt;
    }
    if (it->second.receiver) {
        return it->second.receiver->getStatistics();
    }
    if (it->second.transmitter) {
        return it->second.transmitter->getStatistics();
    }
    return std::nullopt;
}

std::unique_ptr<RTPTransmitter> StreamManager::createTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    const std::string& networkInterface
) {
    // Transmitters read audio from OUTPUT buffers (Core Audio → Network)
    return std::make_unique<RTPTransmitter>(sdp, mapping, txContext_, networkInterface);
}

//
// Callback Invocation
//

void StreamManager::notifyStreamAdded(const StreamInfo& info) {
    if (streamAddedCallback_) {
        streamAddedCallback_(info);
    }
}

void StreamManager::notifyStreamRemoved(const StreamInfo& info) {
    if (streamRemovedCallback_) {
        streamRemovedCallback_(info);
    }
}

void StreamManager::notifyStreamStatusChanged(const StreamInfo& info) {
    if (streamStatusCallback_) {
        streamStatusCallback_(info);
    }
}

//
// Configuration Persistence
//

bool StreamManager::loadSavedStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Load configurations from disk
    LoadedStreamConfig saved = configManager_->loadConfig();
    savedConfigFound_ = saved.status != LoadedStreamConfig::Status::Missing;
    for (auto& kept : keptEntries_) {
        releaseKeptChannels(kept);
    }
    keptEntries_.clear();
    if (saved.status != LoadedStreamConfig::Status::Loaded) {
        AES67_LOG("StreamManager: No saved stream configurations found");
        return false;
    }

    AES67_LOGF("StreamManager: Loading %zu saved stream configurations", saved.entries.size());

    int loadedCount = 0;
    int failedCount = 0;

    // Whatever isn't loaded is written back on the next save, so the file
    // keeps it: only a second copy of a stream already running is dropped
    for (auto& entry : saved.entries) {
        if (!entry.config) {
            AES67_LOG("StreamManager: Keeping an unreadable stream entry as it is");
            keptEntries_.push_back({std::move(entry)});
            failedCount++;
            continue;
        }

        const PersistedStreamConfig& config = *entry.config;
        if (!config.enabled) {
            AES67_LOGF("StreamManager: Skipping disabled stream: %s", config.sdp.sessionName.c_str());
            keptEntries_.push_back({std::move(entry)});
            continue;
        }

        if (streams_.find(config.mapping.streamID) != streams_.end()) {
            AES67_LOGF("StreamManager: Stream already exists: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        if (!loadSavedStream(config)) {
            keptEntries_.push_back({std::move(entry)});
            failedCount++;
            continue;
        }

        loadedCount++;
        AES67_LOGF("StreamManager: Loaded stream: %s (%s)",
                  config.sdp.sessionName.c_str(),
                  config.mapping.streamID.toString().c_str());
    }

    AES67_LOGF("StreamManager: Loaded %d streams successfully, %d failed",
              loadedCount, failedCount);

    // After every enabled stream, so a running stream keeps channels a
    // disabled one also lists (as files written before reservations can)
    reserveKeptChannels();
    for (const auto& kept : keptEntries_) {
        if (!kept.channelsReserved) {
            AES67_LOGF("StreamManager: Saved stream '%s' holds no channels: they are in use or unreadable",
                       rawText(kept.entry.raw, "sdp", "sessionName").c_str());
        }
    }

    return loadedCount > 0;
}

bool StreamManager::foundSavedConfig() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return savedConfigFound_;
}

std::vector<StreamManager::SavedStreamInfo> StreamManager::getStreamsNotLoaded() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<SavedStreamInfo> result;
    result.reserve(keptEntries_.size());
    for (const auto& kept : keptEntries_) {
        const SavedStreamEntry& entry = kept.entry;
        SavedStreamInfo info;
        info.id = savedStreamID(entry);
        info.channelsReserved = kept.channelsReserved;
        info.direction = keptDirection(entry);
        if (const auto mapping = keptMapping(entry)) {
            info.deviceChannelStart = mapping->deviceChannelStart;
            info.deviceChannelCount = mapping->deviceChannelCount;
        }
        if (entry.config) {
            info.name = entry.config->sdp.sessionName;
            info.enabled = entry.config->enabled;
            info.readable = true;
        } else {
            info.name = rawText(entry.raw, "sdp", "sessionName");
            const JsonValue* enabled = entry.raw.find("enabled");
            info.enabled = enabled && enabled->asBool().value_or(false);
        }
        result.push_back(std::move(info));
    }
    return result;
}

void StreamManager::dropKeptCopiesOf(const SDPSession& sdp) {
    // NOTE: Caller must hold streamsMutex_ lock

    // A saved stream that failed to start, added again: the new one replaces
    // it. Disabled entries stay; they are the user's to remove.
    keptEntries_.erase(std::remove_if(keptEntries_.begin(), keptEntries_.end(),
                                      [&](KeptStream& kept) {
                                          if (!isFailedCopyOf(kept.entry, sdp)) return false;
                                          AES67_LOGF("StreamManager: '%s' replaces its saved copy that failed to load",
                                                     sdp.sessionName.c_str());
                                          releaseKeptChannels(kept);
                                          return true;
                                      }),
                       keptEntries_.end());
}

void StreamManager::releaseKeptCopiesOf(const SDPSession& sdp) {
    // NOTE: Caller must hold streamsMutex_ lock
    for (auto& kept : keptEntries_) {
        if (isFailedCopyOf(kept.entry, sdp)) {
            releaseKeptChannels(kept);
        }
    }
}

void StreamManager::releaseKeptChannels(KeptStream& kept) {
    // NOTE: Caller must hold streamsMutex_ lock
    if (!kept.channelsReserved) return;
    mapperFor(keptDirection(kept.entry)).removeMapping(savedStreamID(kept.entry));
    kept.channelsReserved = false;
}

void StreamManager::reserveKeptChannels() {
    // NOTE: Caller must hold streamsMutex_ lock

    // In file order, so of two saved streams on the same channels the
    // earlier one holds them
    for (auto& kept : keptEntries_) {
        if (kept.channelsReserved) continue;
        const auto mapping = keptMapping(kept.entry);
        if (!mapping) continue;
        // The mapper is keyed by ID: never displace a running stream's (or
        // another entry's) mapping saved under the same ID
        StreamChannelMapper& mapper = mapperFor(keptDirection(kept.entry));
        if (mapping->streamID.isNull() || mapper.getMapping(mapping->streamID)) continue;
        kept.channelsReserved = mapper.addMapping(*mapping);
    }
}

bool StreamManager::loadSavedStream(const PersistedStreamConfig& config) {
    // NOTE: Caller must hold streamsMutex_ lock

    // Validate config
    if (!config.isValid()) {
        AES67_LOGF("StreamManager: Invalid stream config: %s", config.sdp.sessionName.c_str());
        return false;
    }

    // Check if stream can be added (validate sample rate, channels, etc.)
    std::string error;
    if (!canAddStream(config.sdp, &error)) {
        AES67_LOGF("StreamManager: Cannot add stream '%s': %s",
                  config.sdp.sessionName.c_str(), error.c_str());
        return false;
    }

    // Use the StreamID from the saved mapping
    const ChannelMapping mapping = wholeStreamMapping(config.mapping, config.sdp.numChannels);
    const StreamID id = mapping.streamID;

    // Add mapping to mapper
    StreamChannelMapper& mapper = mapperFor(directionOf(config.sdp));
    if (!mapper.addMapping(mapping)) {
        AES67_LOGF("StreamManager: Failed to add mapping for stream: %s",
                  config.sdp.sessionName.c_str());
        return false;
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = config.sdp;
    managed.mapping = mapping;
    managed.isTransmit = (config.sdp.direction == "sendonly" || config.sdp.direction == "sendrecv");
    managed.networkInterface = config.networkInterface;

    // Transmitters start now; receivers start only if IO is active
    if (managed.isTransmit) {
        managed.transmitter = createTransmitter(config.sdp, mapping, config.networkInterface);
        if (!managed.transmitter || !managed.transmitter->start()) {
            mapper.removeMapping(id);
            return false;
        }
    } else {
        managed.receiver = createReceiver(config.sdp, mapping, config.jitterBufferDepth, config.networkInterface);
        if (!managed.receiver || (ioActive_.load() && !managed.receiver->start())) {
            mapper.removeMapping(id);
            return false;
        }
    }

    // Build stream info (same as in addStream)
    managed.info.id = id;
    managed.info.name = config.sdp.sessionName;
    managed.info.description = config.sdp.sessionInfo;
    managed.info.source.ip = config.sdp.sourceAddress;
    managed.info.source.port = config.sdp.port;
    managed.info.multicast.ip = config.sdp.connectionAddress;
    managed.info.multicast.port = config.sdp.port;
    managed.info.multicast.ttl = config.sdp.ttl;

    if (config.sdp.encoding == "L16") {
        managed.info.encoding = AudioEncoding::L16;
    } else if (config.sdp.encoding == "L24") {
        managed.info.encoding = AudioEncoding::L24;
    } else {
        managed.info.encoding = AudioEncoding::Unknown;
    }

    managed.info.sampleRate = config.sdp.sampleRate;
    managed.info.numChannels = config.sdp.numChannels;
    managed.info.payloadType = config.sdp.payloadType;
    managed.info.ptime = static_cast<uint32_t>(std::llround(config.sdp.ptime * 1000.0));  // microseconds
    managed.info.framecount = config.sdp.framecount;
    managed.info.ptp.domain = config.sdp.ptpDomain;
    managed.info.isActive = true;
    managed.info.isConnected = false;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);
    networkMonitor_->watch(streams_[id].networkInterface);
    if (streams_[id].transmitter) {
        announceTx(id, streams_[id]);
    }

    // Notify callback
    notifyStreamAdded(streams_[id].info);
    return true;
}

bool StreamManager::saveAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return saveAllStreamsInternal();
}

bool StreamManager::saveAllStreamsInternal() {
    // NOTE: Caller must hold streamsMutex_ lock

    std::vector<PersistedStreamConfig> configs;
    configs.reserve(streams_.size());

    // Convert all streams to persisted configs
    for (const auto& pair : streams_) {
        const auto& managed = pair.second;

        PersistedStreamConfig config = StreamConfigManager::createConfig(
            managed.sdp,
            managed.mapping,
            managed.info.description
        );
        config.networkInterface = managed.networkInterface;

        configs.push_back(config);
    }

    // Save to disk
    std::vector<JsonValue> kept;
    kept.reserve(keptEntries_.size());
    for (const auto& entry : keptEntries_) {
        kept.push_back(entry.entry.raw);
    }

    bool success = configManager_->saveConfig(configs, kept);

    if (success) {
        AES67_LOGF("StreamManager: Saved %zu stream configurations to disk", configs.size());
    } else {
        AES67_LOG("StreamManager: Failed to save stream configurations");
    }

    return success;
}

void StreamManager::autoSaveIfEnabled() {
    // NOTE: We're already holding streamsMutex_ when this is called
    // from addStream/removeStream/updateMapping, so use the internal version
    if (autoSaveEnabled_) {
        saveAllStreamsInternal();
    }
}

} // namespace AES67
