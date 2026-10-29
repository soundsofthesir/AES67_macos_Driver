//
// RTPTransmitter.cpp
// AES67 macOS Driver
// RTP packet transmitter with L16/L24 encoding, paced by the media clock
//

#include "RTPTransmitter.h"
#include "SimpleRTP.h"
#include "../../Driver/DebugLog.h"
#include "../Clock/HostTime.h"
#include "../NetworkInterfaceDetection.h"
#include <mach/mach_time.h>
#include <algorithm>
#include <cstring>
#include <random>
#include <chrono>
#include <cerrno>

namespace AES67 {

namespace {

uint32_t framesPerPacketFor(const SDPSession& sdp) {
    return framesPerPacket(sdp);
}

size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value) power <<= 1;
    return power;
}

int64_t floorDiv(int64_t value, int64_t divisor) {
    const int64_t quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// Core Audio writes output ahead of its media time by up to its IO buffer and
// safety offset; the buffer must also hold the packet being sent
constexpr size_t kMinBufferFrames = 16384;

} // namespace

RTPTransmitter::RTPTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    TxContext context,
    const std::string& networkInterface
)
    : sdp_(sdp)
    , mapping_(mapping)
    , context_(context)
    , networkInterface_(networkInterface)
    , framesPerPacket_(framesPerPacketFor(sdp))
{
    std::memset(&stats_, 0, sizeof(stats_));

    std::random_device rd;
    ssrc_ = rd();
    nextTimestamp_ = rd();  // RFC 3550: start the RTP timeline at a random value

    const size_t channels = std::max<size_t>(sdp_.numChannels, 1);
    buffer_ = std::make_unique<TimestampedAudioBuffer>(
        channels, nextPowerOfTwo(std::max<size_t>(kMinBufferFrames, 8 * static_cast<size_t>(framesPerPacket_))));

    // Pre-allocate so the transmit thread never allocates
    audioBuffer_.resize(static_cast<size_t>(framesPerPacket_) * channels);
    payloadBuffer_.resize(3 * static_cast<size_t>(framesPerPacket_) * channels);
}

RTPTransmitter::~RTPTransmitter() {
    stop();
}

bool RTPTransmitter::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPTransmitter::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPTransmitter::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPTransmitter::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // Send on the configured interface as it is now (resolved at each start)
    unsigned interfaceIndex = 0;
    std::string interfaceDescription;
    if (!NetworkInterfaceDetection::socketInterfaceIndex(networkInterface_, interfaceIndex, interfaceDescription)) {
        AES67_LOGF("RTPTransmitter::start: interface %s (stream=%s)", interfaceDescription.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }
    if (!rtpSocket_.openTransmitter(sdp_.connectionAddress.c_str(), sdp_.port, interfaceIndex, sdp_.ttl)) {
        AES67_LOGF("RTPTransmitter::start: socket open failed for %s:%u on %s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, interfaceDescription.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    // The IO thread writes this stream's output channels into the buffer
    if (!context_.routing.publish(buffer_.get(), static_cast<uint32_t>(mapping_.deviceChannelStart))) {
        AES67_LOGF("RTPTransmitter::start: no route for device channels %zu-%zu (stream=%s)",
                   mapping_.deviceChannelStart, mapping_.deviceChannelStart + sdp_.numChannels - 1,
                   sdp_.sessionName.c_str());
        rtpSocket_.close();
        return false;
    }
    published_ = true;

    running_ = true;
    // Wakes per packet, and at least every 1 ms (see transmitLoop)
    const auto wakePeriod = std::min<std::chrono::nanoseconds>(
        std::chrono::nanoseconds(static_cast<int64_t>(framesPerPacket_) * 1000000000 / sdp_.sampleRate),
        std::chrono::milliseconds(1));
    transmitThread_ = std::thread([this, wakePeriod]() {
        if (!AudioThreadPriority::configureForRealTime(wakePeriod)) {
            AES67_LOGF("RTPTransmitter: failed to set RT priority on transmit thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        transmitLoop();
    });

    AES67_LOGF("RTPTransmitter::start: %s:%u, %u frames per packet (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port, framesPerPacket_, sdp_.sessionName.c_str());
    return true;
}

void RTPTransmitter::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (transmitThread_.joinable()) {
        transmitThread_.join();
    }

    // After this returns no IO write can still be using the buffer
    if (published_) {
        context_.routing.unpublish(buffer_.get());
        published_ = false;
    }

    rtpSocket_.close();
}

void RTPTransmitter::transmitLoop() {
    const auto frames = static_cast<int64_t>(framesPerPacket_);
    const auto pollTicks = static_cast<uint64_t>(HostTimebase::current().ticksPerSecond() * 0.001);

    uint32_t generation = 0;
    bool haveTimeline = false;
    int64_t position = 0;     // media position of the next packet's first frame
    uint32_t rtpOffset = 0;   // RTP timestamp = position + rtpOffset (mod 2^32), without PTP
    bool onPtp = false;
    const uint32_t mediaClockOffset = parseMediaClockOffset(sdp_.mediaClockType);

    while (running_) {
        const MediaClock::Snapshot clock = context_.clock.snapshot();
        if (!clock.valid()) {
            mach_wait_until(hostTimeNow() + pollTicks);
            continue;
        }

        if (!haveTimeline || clock.generation != generation) {
            // Start at the packet now in progress; keep the RTP timeline
            // continuous across the media clock's restarts
            position = floorDiv(clock.sampleAt(hostTimeNow()), frames) * frames;
            rtpOffset = nextTimestamp_ - static_cast<uint32_t>(position);
            if (haveTimeline) {
                timelineRestarts_.fetch_add(1, std::memory_order_relaxed);
            }
            generation = clock.generation;
            haveTimeline = true;
        }

        // Due when the media clock reaches the packet's end. Wait in short
        // steps so stop() and clock changes are noticed promptly.
        const uint64_t due = clock.hostAt(position + frames);
        const uint64_t now = hostTimeNow();
        if (due > now) {
            mach_wait_until(std::min(due, now + pollTicks));
            continue;
        }

        // Too far behind (the thread was descheduled): skip to the packet in
        // progress rather than bursting late packets at receivers
        const int64_t current = floorDiv(clock.sampleAt(now), frames) * frames;
        if (current - position > kMaxLatePackets * frames) {
            packetsSkipped_.fetch_add(static_cast<uint64_t>((current - position) / frames), std::memory_order_relaxed);
            position = current;
            continue;
        }

        // With PTP: PTP time plus the SDP's mediaclk offset. Without: carry on
        // from the last timestamp sent, so leaving PTP does not jump
        const int64_t ptpOffset = context_.networkTime && context_.networkTime->fixed()
            ? context_.networkTime->offset()
            : NetworkTimeMapping::kUnset;
        uint32_t timestamp = 0;
        if (ptpOffset != NetworkTimeMapping::kUnset) {
            timestamp = static_cast<uint32_t>(position - ptpOffset) + mediaClockOffset;
            onPtp = true;
        } else {
            if (onPtp) {
                rtpOffset = nextTimestamp_ - static_cast<uint32_t>(position);
                onPtp = false;
            }
            timestamp = static_cast<uint32_t>(position) + rtpOffset;
        }
        sendPosition(position, timestamp);
        position += frames;
    }
}

void RTPTransmitter::sendPosition(int64_t position, uint32_t timestamp) {
    const size_t frames = framesPerPacket_;
    // Unwritten positions (no client running, or not yet written) read as silence
    buffer_->read(position, frames, audioBuffer_.data(), sdp_.numChannels, 0);

    uint8_t* payload = payloadBuffer_.data();
    size_t payloadSize = 0;
    if (sdp_.encoding == "L16") {
        RTP::L16Codec::encode(audioBuffer_.data(), frames * sdp_.numChannels, payload);
        payloadSize = frames * sdp_.numChannels * 2;
    } else if (sdp_.encoding == "L24") {
        RTP::L24Codec::encode(audioBuffer_.data(), frames * sdp_.numChannels, payload);
        payloadSize = frames * sdp_.numChannels * 3;
    } else {
        return;  // rejected at start-up by StreamManager validation
    }

    sendPacket(payload, payloadSize, timestamp);
    nextTimestamp_ = timestamp + framesPerPacket_;
    packetsSent_.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesSent.fetch_add(payloadSize, std::memory_order_relaxed);
}

RTPTransmitter::TransmitStatistics RTPTransmitter::getTransmitStatistics() const {
    TransmitStatistics s;
    s.packetsSent = packetsSent_.load(std::memory_order_relaxed);
    s.packetsSkipped = packetsSkipped_.load(std::memory_order_relaxed);
    s.timelineRestarts = timelineRestarts_.load(std::memory_order_relaxed);
    return s;
}

bool RTPTransmitter::updateMapping(const ChannelMapping& newMapping) {
    // Validate mapping
    if (newMapping.deviceChannelStart + sdp_.numChannels > 128) {
        return false;
    }

    // Stop, update, restart (republishes the buffer on the new channels)
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

StatisticsSnapshot RTPTransmitter::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPTransmitter::resetStatistics() {
    // Reset all atomic counters
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
}

void RTPTransmitter::sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp) {
    if (!rtpSocket_.isOpen() || !payload || payloadSize == 0) {
        return;
    }

    // Build RTP packet
    RTP::RTPPacket packet;
    packet.header.version = 2;
    packet.header.padding = 0;
    packet.header.extension = 0;
    packet.header.cc = 0;
    packet.header.marker = 0;
    packet.header.payloadType = sdp_.payloadType;
    packet.header.sequenceNumber = sequenceNumber_++;
    packet.header.timestamp = timestamp;
    packet.header.ssrc = ssrc_;
    packet.payload = const_cast<uint8_t*>(payload);
    packet.payloadSize = payloadSize;

    // Send packet
    ssize_t bytesSent = rtpSocket_.send(packet);

    if (bytesSent < 0) {
        // Send failed — count via malformedPackets (repurposed as send error counter for TX)
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPTransmitter::sendPacket: send failed #%llu (seq=%u, errno=%d: %s) stream=%s",
                       (unsigned long long)count, sequenceNumber_ - 1,
                       errno, strerror(errno), sdp_.sessionName.c_str());
        }
    }
}


} // namespace AES67
