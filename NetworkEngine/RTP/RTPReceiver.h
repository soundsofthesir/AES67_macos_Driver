/// @file RTPReceiver.h
/// @brief RTP packet receiver: places decoded L16/L24 audio by RTP timestamp.

#pragma once

#include "../../Shared/Types.h"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "../NetworkInterfaceDetection.h"
#include "../Clock/TimestampedAudioBuffer.h"
#include "SimpleRTP.h"
#include "RtpPlacement.h"
#include "RxContext.h"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>

namespace AES67 {

/// Receives one RTP audio stream from a multicast group.
///
/// A single receive thread decodes each packet straight into this stream's
/// TimestampedAudioBuffer at the media position its RTP timestamp gives
/// (RtpPlacement). While running, the buffer is published in the device's
/// RxRouting, where the IO thread reads it at device time minus the link
/// offset. There is no FIFO: loss, outages and restarts keep their place on
/// the timeline. See Docs/Step2-Clocking-Plan.md.
class RTPReceiver {
public:
    /// Placement outcomes, for diagnostics.
    struct PlacementStatistics {
        uint64_t lateDrops{0};     // arrived after their playout time
        uint64_t earlyDrops{0};    // too far ahead of playout
        uint64_t foreignDrops{0};  // from another SSRC on the group
        uint64_t reanchors{0};     // timeline re-anchored (restart or drift)
    };

    /// @param sdp SDP session describing the stream to receive.
    /// @param mapping Channel mapping from stream channels to device channels.
    /// @param context Device clock, network time mapping, IO routing and link offset.
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = INADDR_ANY.
    RTPReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        RxContext context,
        const std::string& networkInterface = ""
    );

    ~RTPReceiver();

    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    //
    // Control
    //

    /// Open the socket, publish this stream's buffer and start receiving.
    bool start();

    /// Stop receiving and unpublish the buffer (waits for in-flight IO reads).
    void stop();

    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    StatisticsSnapshot getStatistics() const;
    PlacementStatistics getPlacementStatistics() const;
    void resetStatistics();

    bool isConnected() const;
    int64_t getTimeSinceLastPacket() const;

    //
    // Configuration
    //

    /// Update channel mapping (stops and restarts receiver)
    bool updateMapping(const ChannelMapping& newMapping);

    const SDPSession& getSDPSession() const { return sdp_; }
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    static constexpr size_t kMaxFramesPerPacket = AES67::kMaxFramesPerPacket;

    void receiveLoop();
    void processPacket(const RTP::RTPPacket& packet, uint64_t arrivalHostTime);
    bool validatePacket(const RTP::RTPPacket& packet);

    /// Decode into decodeBuffer_ (interleaved floats); returns frames, 0 if rejected.
    size_t decode(const uint8_t* payload, size_t payloadSize);

    void updateStats(uint16_t sequenceNumber, size_t payloadSize);
    void publishPlacementStatistics();

    /// The a=mediaclk:direct=<offset> value, or 0.
    static uint32_t parseMediaClockOffset(const std::string& mediaClockType);

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    RxContext context_;
    size_t bytesPerSample_;

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Playout buffer (allocated in start(), published in context_.routing while running)
    std::unique_ptr<TimestampedAudioBuffer> buffer_;

    // Receive thread state
    std::unique_ptr<RtpPlacement> placement_;
    uint32_t lastClockGeneration_{0};
    int64_t lastNetworkOffset_{NetworkTimeMapping::kUnset};  // shared mapping seen by the last packet
    int64_t linkOffsetFrames_{0};                            // fixed for this run (set by start)
    std::vector<float> decodeBuffer_;
    uint8_t receiveBuffer_[2048];

    // Threading
    std::thread receiveThread_;
    std::atomic<bool> running_{false};

    // Statistics
    Statistics stats_;
    std::atomic<uint16_t> lastSequenceNumber_{0};
    std::atomic<uint64_t> lateDrops_{0};
    std::atomic<uint64_t> earlyDrops_{0};
    std::atomic<uint64_t> foreignDrops_{0};
    std::atomic<uint64_t> reanchors_{0};

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastPacketTimeNs_{0};

    // Network interface binding
    std::string networkInterface_;      // as configured: name, IPv4 address, or "" (auto)
    std::string interfaceDescription_;  // what the last start() resolved it to (logs)
};

} // namespace AES67
