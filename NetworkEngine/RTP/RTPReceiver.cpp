//
// RTPReceiver.cpp
// AES67 macOS Driver
// RTP packet receiver: L16/L24 decoding, placed by RTP timestamp
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include "../Clock/HostTime.h"
#include "../../Driver/DebugLog.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/select.h>

namespace AES67 {

namespace {

size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value) power <<= 1;
    return power;
}

} // namespace

RTPReceiver::RTPReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    RxContext context,
    const std::string& networkInterface
)
    : sdp_(sdp)
    , mapping_(mapping)
    , context_(context)
    , bytesPerSample_(sdp.encoding == "L16" ? 2 : 3)
    , networkInterface_(networkInterface)
{
    // The interface is resolved at each start(), never here: its address or
    // index may change while the stream exists (DHCP, Wi-Fi roam, replug)
}

RTPReceiver::~RTPReceiver() {
    stop();
}

uint32_t RTPReceiver::parseMediaClockOffset(const std::string& mediaClockType) {
    return AES67::parseMediaClockOffset(mediaClockType);
}

bool RTPReceiver::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPReceiver::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false;
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPReceiver::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPReceiver::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // Join on the configured interface as it is now
    unsigned interfaceIndex = 0;
    if (!NetworkInterfaceDetection::socketInterfaceIndex(networkInterface_, interfaceIndex, interfaceDescription_)) {
        AES67_LOGF("RTPReceiver::start: interface %s (stream=%s)", interfaceDescription_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }
    if (!rtpSocket_.openReceiver(sdp_.connectionAddress.c_str(), sdp_.port, interfaceIndex)) {
        AES67_LOGF("RTPReceiver::start: socket open failed for %s:%u on %s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, interfaceDescription_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    // Playout buffer: room for the playable window (two link offsets ahead of
    // the read point) plus packets in flight, with margin
    const int64_t linkOffset = context_.linkOffsetFrames.load(std::memory_order_relaxed);
    const size_t packetFrames = sdp_.framecount > 0 ? sdp_.framecount : kMaxFramesPerPacket;
    const size_t capacity = nextPowerOfTwo(std::max<size_t>(4096, 4 * static_cast<size_t>(linkOffset) + 4 * packetFrames));
    buffer_ = std::make_unique<TimestampedAudioBuffer>(sdp_.numChannels, capacity);

    RtpPlacement::Config placementConfig;
    placementConfig.mediaClockOffset = parseMediaClockOffset(sdp_.mediaClockType);
    placementConfig.linkOffsetFrames = linkOffset;
    placementConfig.sourceSwitchFrames =
        static_cast<int64_t>(std::llround(sdp_.sampleRate * RtpPlacement::kSourceSwitchSeconds));
    linkOffsetFrames_ = linkOffset;
    lastNetworkOffset_ = NetworkTimeMapping::kUnset;
    placement_ = std::make_unique<RtpPlacement>(placementConfig, context_.networkTime);
    lastClockGeneration_ = 0;
    decodeBuffer_.assign(kMaxFramesPerPacket * sdp_.numChannels, 0.0f);

    if (!context_.routing.publish(buffer_.get(), static_cast<uint32_t>(mapping_.deviceChannelStart))) {
        AES67_LOGF("RTPReceiver::start: cannot route %u channels at device channel %zu (stream=%s)",
                   sdp_.numChannels, mapping_.deviceChannelStart, sdp_.sessionName.c_str());
        rtpSocket_.close();
        return false;
    }

    AES67_LOGF("RTPReceiver::start: %s:%u on %s, link offset %lld frames, buffer %zu frames (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port,
               interfaceDescription_.c_str(),
               static_cast<long long>(linkOffset), capacity, sdp_.sessionName.c_str());

    running_ = true;
    // Wakes per packet, and at least every 1 ms (the select timeout)
    const auto wakePeriod = std::min<std::chrono::nanoseconds>(
        std::chrono::nanoseconds(std::llround(sdp_.ptime * 1e6)), std::chrono::milliseconds(1));
    receiveThread_ = std::thread([this, wakePeriod]() {
        if (!AudioThreadPriority::configureForRealTime(wakePeriod)) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on receive thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        receiveLoop();
    });

    return true;
}

void RTPReceiver::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    // After this returns no IO read can still be using the buffer
    context_.routing.unpublish(buffer_.get());

    if (context_.clockRecovery) {
        context_.clockRecovery->streamStopped(this);
    }

    rtpSocket_.close();
    connected_ = false;
}

StatisticsSnapshot RTPReceiver::getStatistics() const {
    return stats_.snapshot();
}

RTPReceiver::PlacementStatistics RTPReceiver::getPlacementStatistics() const {
    return {lateDrops_.load(std::memory_order_relaxed), earlyDrops_.load(std::memory_order_relaxed),
            foreignDrops_.load(std::memory_order_relaxed), reanchors_.load(std::memory_order_relaxed)};
}

void RTPReceiver::resetStatistics() {
    stats_.packetsReceived.store(0, std::memory_order_relaxed);
    stats_.packetsLost.store(0, std::memory_order_relaxed);
    stats_.malformedPackets.store(0, std::memory_order_relaxed);
    stats_.outOfOrderPackets.store(0, std::memory_order_relaxed);
    stats_.underruns.store(0, std::memory_order_relaxed);
    stats_.overruns.store(0, std::memory_order_relaxed);
    stats_.jitterNs.store(0, std::memory_order_relaxed);
    stats_.latencyNs.store(0, std::memory_order_relaxed);
    stats_.bytesReceived.store(0, std::memory_order_relaxed);
    stats_.bytesSent.store(0, std::memory_order_relaxed);
    lastSequenceNumber_.store(0, std::memory_order_relaxed);
}

bool RTPReceiver::isConnected() const {
    if (!connected_) {
        return false;
    }

    // Consider disconnected if no packet in last 1 second
    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return false;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) < 1000000000LL; // 1 second in ns
}

int64_t RTPReceiver::getTimeSinceLastPacket() const {
    if (!connected_) {
        return -1;
    }

    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return -1;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) / 1000000; // ns to ms
}

bool RTPReceiver::updateMapping(const ChannelMapping& newMapping) {
    if (newMapping.deviceChannelStart + sdp_.numChannels > 128) {
        return false;
    }

    // Stop, update, restart (republishes the route at the new channels)
    const bool wasRunning = running_;
    if (wasRunning) {
        stop();
    }

    mapping_ = newMapping;

    if (wasRunning) {
        return start();
    }

    return true;
}

void RTPReceiver::receiveLoop() {
    fd_set readfds;
    struct timeval tv;

    RTP::RTPPacket packet;

    while (running_) {
        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout

        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            uint64_t arrivalHostTime = 0;
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_), &arrivalHostTime);
            if (bytesReceived > 0) {
                // The kernel timestamp is when the packet reached the host,
                // regardless of how late this thread was scheduled
                processPacket(packet, arrivalHostTime != 0 ? arrivalHostTime : hostTimeNow());
            }
        }
    }
}

void RTPReceiver::processPacket(const RTP::RTPPacket& packet, uint64_t arrivalHostTime) {
    const size_t bytesPerFrame = bytesPerSample_ * sdp_.numChannels;
    const size_t frames = bytesPerFrame > 0 ? packet.payloadSize / bytesPerFrame : 0;

    if (!validatePacket(packet) || !packet.payload || frames == 0 || frames > kMaxFramesPerPacket) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: malformed packet #%llu (ver=%u pt=%u size=%zu, expected pt=%u) stream=%s",
                       (unsigned long long)count, packet.header.version,
                       packet.header.payloadType, packet.payloadSize,
                       sdp_.payloadType, sdp_.sessionName.c_str());
        }
        return;
    }

    connected_ = true;
    auto now = std::chrono::steady_clock::now();
    lastPacketTimeNs_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
        std::memory_order_release);
    updateStats(packet.header.sequenceNumber, packet.payloadSize);

    const MediaClock::Snapshot clock = context_.clock.snapshot();
    if (!clock.valid()) {
        return;
    }
    bool marginsJumped = false;
    if (clock.generation != lastClockGeneration_) {
        // The local timeline restarted: earlier placement no longer applies
        placement_->reset();
        lastClockGeneration_ = clock.generation;
        marginsJumped = true;
    }

    // Another stream re-anchoring the shared mapping moves this stream's
    // positions too, without this stream's placement reporting it. Read before
    // placing: a re-anchor in between is then seen (one packet late) rather
    // than missed.
    const int64_t networkOffset = context_.networkTime.offset();
    if (networkOffset != lastNetworkOffset_) {
        marginsJumped = true;
        if (context_.networkTime.fixed()) {
            // PTP set a new offset: network time's true place. Start over
            // against it, dropping any anchor of this stream's own
            placement_->reset();
        }
    }
    lastNetworkOffset_ = networkOffset;

    const MediaPosition arrival = clock.positionAt(arrivalHostTime);
    const RtpPlacement::Result placed = placement_->place(
        packet.header.timestamp, packet.header.ssrc, static_cast<uint32_t>(frames), arrival.sample);
    publishPlacementStatistics();

    if (placed.reanchored) {
        AES67_LOGF("RTPReceiver: re-anchored timeline at ssrc=%08x ts=%u (stream=%s)",
                   packet.header.ssrc, packet.header.timestamp, sdp_.sessionName.c_str());
    }
    if (context_.clockRecovery && (marginsJumped || placed.reanchored)) {
        context_.clockRecovery->streamReanchored(this);
    }
    if (placed.verdict != RtpPlacement::Verdict::Accepted) {
        return;
    }

    if (context_.clockRecovery) {
        // How far ahead of the read point the packet's end landed (samples).
        // The servo holds this constant, locking the device clock to the sender.
        const int64_t wholeMargin = placed.position + static_cast<int64_t>(frames) - (arrival.sample - linkOffsetFrames_);
        context_.clockRecovery->observe(this, arrivalHostTime, static_cast<double>(wholeMargin) - arrival.fraction);
    }

    if (decode(packet.payload, packet.payloadSize) == frames) {
        buffer_->write(placed.position, frames, decodeBuffer_.data(), sdp_.numChannels, 0);
    }
}

bool RTPReceiver::validatePacket(const RTP::RTPPacket& packet) {
    // Check RTP version (should be 2)
    if (packet.header.version != 2) {
        return false;
    }

    // Check payload type matches SDP
    if (packet.header.payloadType != sdp_.payloadType) {
        return false;
    }

    // Check payload size is reasonable
    if (packet.payloadSize == 0 || packet.payloadSize > 1500) {
        return false;
    }

    return true;
}

size_t RTPReceiver::decode(const uint8_t* payload, size_t payloadSize) {
    const size_t channels = sdp_.numChannels;
    const size_t frames = payloadSize / (bytesPerSample_ * channels);
    const size_t samples = frames * channels;
    if (frames == 0 || samples > decodeBuffer_.size()) {
        return 0;
    }

    if (bytesPerSample_ == 2) {
        // L16: 16-bit big-endian signed PCM
        for (size_t i = 0; i < samples; ++i) {
            const uint16_t raw = (static_cast<uint16_t>(payload[i * 2]) << 8) | payload[i * 2 + 1];
            decodeBuffer_[i] = static_cast<int16_t>(raw) / 32768.0f;
        }
    } else {
        // L24: 24-bit big-endian signed PCM. Assemble in the top bits of a
        // uint32_t, then arithmetic-shift to sign-extend (no signed overflow)
        for (size_t i = 0; i < samples; ++i) {
            const uint32_t raw = (static_cast<uint32_t>(payload[i * 3]) << 24) |
                                 (static_cast<uint32_t>(payload[i * 3 + 1]) << 16) |
                                 (static_cast<uint32_t>(payload[i * 3 + 2]) << 8);
            decodeBuffer_[i] = (static_cast<int32_t>(raw) >> 8) / 8388608.0f;
        }
    }
    return frames;
}

void RTPReceiver::publishPlacementStatistics() {
    lateDrops_.store(placement_->lateDrops(), std::memory_order_relaxed);
    earlyDrops_.store(placement_->earlyDrops(), std::memory_order_relaxed);
    foreignDrops_.store(placement_->foreignDrops(), std::memory_order_relaxed);
    reanchors_.store(placement_->reanchors(), std::memory_order_relaxed);
}

void RTPReceiver::updateStats(uint16_t sequenceNumber, size_t payloadSize) {
    // Detect packet loss (sequence number gaps). Signed 16-bit arithmetic
    // distinguishes forward gaps (lost packets) from backward gaps
    // (reordered/duplicate packets), even across 16-bit wraparound.
    uint64_t currentPacketCount = stats_.packetsReceived.load(std::memory_order_relaxed);
    if (currentPacketCount > 0) {
        uint16_t expected = lastSequenceNumber_.load(std::memory_order_relaxed) + 1;
        if (sequenceNumber != expected) {
            int16_t gap = static_cast<int16_t>(sequenceNumber - expected);
            if (gap > 0) {
                stats_.packetsLost.fetch_add(static_cast<uint64_t>(gap), std::memory_order_relaxed);
            } else {
                stats_.outOfOrderPackets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    lastSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesReceived.fetch_add(payloadSize, std::memory_order_relaxed);
}

} // namespace AES67
