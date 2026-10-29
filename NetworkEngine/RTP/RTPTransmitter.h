/// @file RTPTransmitter.h
/// @brief RTP packet transmitter with L16/L24 encoding, paced by the media clock.

#pragma once

#include "../../Shared/Types.h"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "../Clock/TimestampedAudioBuffer.h"
#include "SimpleRTP.h"
#include "TxContext.h"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

namespace AES67 {

/// Transmits one stream from the device's output on the media clock.
///
/// The IO thread writes the output mix into this stream's TimestampedAudioBuffer
/// at each sample's media position (through TxRouting). The transmit thread
/// sends the packet holding media positions [M, M + F) when the media clock
/// reaches M + F, as hardware senders do: neither early nor late at receivers.
/// Positions nothing wrote (no Core Audio client running) are sent as silence,
/// so the stream flows continuously whenever the transmitter runs.
///
/// RTP timestamps: while PTP has the clock (the device's network time
/// mapping is fixed at PTP's offset K), PTP time plus the SDP's mediaclk
/// offset, M - K + mediaclk, as AES67 receivers expect. Otherwise M plus an
/// offset, adjusted at each timeline restart (new clock generation) and on
/// leaving PTP so the timestamps continue without a jump.
class RTPTransmitter {
public:
    struct TransmitStatistics {
        uint64_t packetsSent{0};
        uint64_t packetsSkipped{0};    // fell too far behind and were not sent
        uint64_t timelineRestarts{0};  // media clock generation changes followed
    };

    /// @param sdp SDP session describing the TX stream configuration.
    /// @param mapping Channel mapping from device output channels to stream channels.
    /// @param context Media clock and the routing the IO thread writes through.
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = default.
    RTPTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        TxContext context,
        const std::string& networkInterface = ""
    );

    ~RTPTransmitter();

    // Prevent copy/move
    RTPTransmitter(const RTPTransmitter&) = delete;
    RTPTransmitter& operator=(const RTPTransmitter&) = delete;

    //
    // Control
    //

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    StatisticsSnapshot getStatistics() const;
    void resetStatistics();
    TransmitStatistics getTransmitStatistics() const;

    /// Frames per packet: the sample rate times ptime.
    uint32_t framesPerPacket() const { return framesPerPacket_; }

    /// Multicast TTL in use (from the SDP; 0 keeps the stream on this host).
    uint8_t multicastTtl() const { return rtpSocket_.multicastTTL(); }

    //
    // Configuration
    //

    bool updateMapping(const ChannelMapping& newMapping);
    const SDPSession& getSDPSession() const { return sdp_; }
    const ChannelMapping& getMapping() const { return mapping_; }

    /// Packets more than this many behind are skipped rather than sent late,
    /// so a descheduled thread leaves a gap instead of a burst.
    static constexpr int64_t kMaxLatePackets = 4;

private:
    void transmitLoop();
    void sendPosition(int64_t position, uint32_t timestamp);

    // Send RTP packet
    void sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp);

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    TxContext context_;
    std::string networkInterface_;
    const uint32_t framesPerPacket_;

    // Output samples by media position, written by the IO thread
    std::unique_ptr<TimestampedAudioBuffer> buffer_;
    bool published_{false};

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Threading
    std::thread transmitThread_;
    std::atomic<bool> running_{false};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;
    std::atomic<uint64_t> packetsSent_{0};
    std::atomic<uint64_t> packetsSkipped_{0};
    std::atomic<uint64_t> timelineRestarts_{0};

    // RTP state (transmit thread only while running)
    uint16_t sequenceNumber_{0};
    uint32_t nextTimestamp_{0};
    uint32_t ssrc_{0};

    // Reused to avoid allocation on the transmit thread
    std::vector<float> audioBuffer_;
    std::vector<uint8_t> payloadBuffer_;
};

} // namespace AES67
