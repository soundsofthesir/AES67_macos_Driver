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
#include <ctime>

namespace AES67 {

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
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = sdp.sessionName;
    completeMapping.streamChannelCount = sdp.numChannels;
    completeMapping.deviceChannelCount = sdp.numChannels;

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
        return false;
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

    // Remove from mapper
    mapperFor(it->second).removeMapping(id);

    // Remove from map
    streams_.erase(it);

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

        notifyStreamRemoved(pair.second.info);
    }

    streams_.clear();
    inputMapper_.clearAll();
    outputMapper_.clearAll();
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
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = name;
    completeMapping.streamChannelCount = numChannels;
    completeMapping.deviceChannelCount = numChannels;

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

    // Update mapper
    ChannelMapping completeMapping = newMapping;
    completeMapping.streamID = id;
    completeMapping.streamName = it->second.mapping.streamName;
    completeMapping.streamChannelCount = it->second.sdp.numChannels;

    if (!mapperFor(it->second).updateMapping(completeMapping)) {
        return false;
    }

    // Update managed stream
    it->second.mapping = completeMapping;

    // Update receiver/transmitter
    bool updated = false;
    if (it->second.receiver) {
        updated = it->second.receiver->updateMapping(completeMapping);
    } else if (it->second.transmitter) {
        updated = it->second.transmitter->updateMapping(completeMapping);
    }

    if (updated) {
        notifyStreamStatusChanged(it->second.info);

        // Auto-save configuration
        autoSaveIfEnabled();
    }

    return updated;
}

std::optional<ChannelMapping> StreamManager::getMapping(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    if (auto mapping = inputMapper_.getMapping(id)) {
        return mapping;
    }
    return outputMapper_.getMapping(id);
}

std::vector<ChannelMapping> StreamManager::getAllMappings(Direction direction) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapperFor(direction).getAllMappings();
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
            if (managed.receiver) {
                managed.receiver->start();
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

void StreamManager::onInterfaceChanged(const std::string& networkInterface) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    for (auto& [id, managed] : streams_) {
        if (managed.networkInterface != networkInterface) {
            continue;
        }
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
            keptEntries_.push_back(std::move(entry.raw));
            failedCount++;
            continue;
        }

        const PersistedStreamConfig& config = *entry.config;
        if (!config.enabled) {
            AES67_LOGF("StreamManager: Skipping disabled stream: %s", config.sdp.sessionName.c_str());
            keptEntries_.push_back(std::move(entry.raw));
            continue;
        }

        if (streams_.find(config.mapping.streamID) != streams_.end()) {
            AES67_LOGF("StreamManager: Stream already exists: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        if (!loadSavedStream(config)) {
            keptEntries_.push_back(std::move(entry.raw));
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

    return loadedCount > 0;
}

bool StreamManager::foundSavedConfig() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return savedConfigFound_;
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
    const StreamID id = config.mapping.streamID;

    // Add mapping to mapper
    StreamChannelMapper& mapper = mapperFor(directionOf(config.sdp));
    if (!mapper.addMapping(config.mapping)) {
        AES67_LOGF("StreamManager: Failed to add mapping for stream: %s",
                  config.sdp.sessionName.c_str());
        return false;
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = config.sdp;
    managed.mapping = config.mapping;
    managed.isTransmit = (config.sdp.direction == "sendonly" || config.sdp.direction == "sendrecv");
    managed.networkInterface = config.networkInterface;

    // Transmitters start now; receivers start only if IO is active
    if (managed.isTransmit) {
        managed.transmitter = createTransmitter(config.sdp, config.mapping, config.networkInterface);
        if (!managed.transmitter || !managed.transmitter->start()) {
            mapper.removeMapping(id);
            return false;
        }
    } else {
        managed.receiver = createReceiver(config.sdp, config.mapping, config.jitterBufferDepth, config.networkInterface);
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
    bool success = configManager_->saveConfig(configs, keptEntries_);

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
