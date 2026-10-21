/// @file StreamManager.h
/// @brief Central coordinator for all AES67 RX and TX streams.

#pragma once

#include "../Shared/Types.h"
#include "../Driver/SDPParser.h"
#include "StreamChannelMapper.h"
#include "StreamConfig.h"
#include "RTP/RTPReceiver.h"
#include "RTP/RxContext.h"
#include "RTP/TxContext.h"
#include "Discovery/SAPAnnouncer.h"
#include "NetworkMonitor.h"
#include "RTP/RTPTransmitter.h"
#include <map>
#include <optional>
#include <random>
#include <memory>
#include <mutex>
#include <functional>

namespace AES67 {

/// Central coordinator for all AES67 streams (receivers, transmitters, channel mapping).
///
/// @warning NOT REAL-TIME SAFE. All public methods acquire a mutex. Never call from
/// the Core Audio IO thread or any RT-constrained thread. Safe to call from
/// initialization, UI/manager app, or control threads.
class StreamManager {
public:
    using StreamCallback = std::function<void(const StreamInfo&)>;

    /// Receive streams map to the device's input channels and transmit streams
    /// to its output channels: separate sets of 128, allocated independently.
    enum class Direction { Receive, Transmit };

    /// Transmit for SDPs saying sendonly or sendrecv (how TX streams are stored).
    static Direction directionOf(const SDPSession& sdp);

    /// Where and how far a TX stream is sent.
    struct TxOptions {
        std::string networkInterface;  // name ("en0") or IP; empty = default route
        uint8_t ttl{32};               // multicast TTL; 0 keeps the stream on this host
    };

    /// The PTP grandmaster the device clock follows.
    struct PtpGrandmaster {
        std::string identity;  // EUI-64, "00-1D-C1-FF-FE-D1-7B-F3"
        uint8_t domain{0};

        bool operator==(const PtpGrandmaster& o) const { return identity == o.identity && domain == o.domain; }
        bool operator!=(const PtpGrandmaster& o) const { return !(*this == o); }
    };

    /// The device clock follows this grandmaster (and TX timestamps are its
    /// time): TX streams announce it (ts-refclk:ptp) instead of this Mac's own
    /// clock. A change re-announces them with a new session version.
    void setPtpGrandmaster(const PtpGrandmaster& grandmaster);
    std::optional<PtpGrandmaster> ptpGrandmaster() const;

    /// @param rxContext Device state receivers place audio against (clock, routing, link offset).
    /// @param txContext Device state transmitters send from (clock, routing the IO thread writes).
    ///
    /// @param sapConfig Where TX streams are announced (SAP, RFC 2974).
    ///
    /// Receivers run only while Core Audio IO is active (setIOActive). Transmitters
    /// run whenever their stream is configured: AES67 senders send continuously,
    /// silence included, and each is announced over SAP while it runs.
    /// @param networkConfig How interface changes are watched (default: real
    ///        interfaces, checked every second).
    StreamManager(RxContext rxContext, TxContext txContext,
                  SAPAnnouncer::Config sapConfig = SAPAnnouncer::Config{},
                  NetworkMonitor::Config networkConfig = NetworkMonitor::Config{});
    ~StreamManager();

    // Prevent copy/move
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    //
    // Stream Management - RX
    //

    /// Add an RX stream with automatic channel assignment. Returns the new StreamID.
    StreamID addStream(const SDPSession& sdp);

    /// Add an RX stream with explicit channel mapping.
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping);

    /// As above, received on a given interface ("en0", an IPv4 address, or ""
    /// for the primary interface), resolved each time the stream starts.
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping, const std::string& networkInterface);

    /// Import an RX stream from an SDP file on disk.
    StreamID importSDPFile(const std::string& filepath);

    /// Remove a stream, running or only saved (see getStreamsNotLoaded()).
    bool removeStream(const StreamID& id);

    // Remove all streams
    void removeAllStreams();

    //
    // Stream Management - TX
    //

    /// Create a TX stream that reads from device output channels and sends RTP.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping,
        const TxOptions& options
    );

    /// As above with default options (default route, TTL 32). An overload, as
    /// a nested struct's initialisers are not usable in a default argument.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping
    );

    // Export stream to SDP file
    bool exportSDPFile(const StreamID& id, const std::string& filepath);

    //
    // Channel Mapping
    //

    // Update channel mapping for a stream
    bool updateMapping(const StreamID& id, const ChannelMapping& newMapping);

    /// A running stream's mapping (a saved stream's is in getStreamsNotLoaded()).
    std::optional<ChannelMapping> getMapping(const StreamID& id) const;

    /// The running streams' mappings in one direction.
    std::vector<ChannelMapping> getAllMappings(Direction direction) const;

    //
    // Query
    //

    // Get all active streams
    std::vector<StreamInfo> getActiveStreams() const;

    // Get stream info
    std::optional<StreamInfo> getStreamInfo(const StreamID& id) const;

    /// Live statistics of a stream's receiver or transmitter.
    std::optional<StatisticsSnapshot> getStreamStatistics(const StreamID& id) const;

    /// Streams restarted because their interface changed (address, link,
    /// index, or which interface "auto" means).
    uint64_t getNetworkRestartCount() const { return networkRestarts_.load(); }

    // Check if stream exists
    bool hasStream(const StreamID& id) const;

    // Get stream count
    size_t getStreamCount() const;

    //
    // Validation
    //

    // Check if stream can be added
    bool canAddStream(const SDPSession& sdp, std::string* errorOut = nullptr) const;

    // Get detailed error message for why stream can't be added
    std::string getAddStreamError(const SDPSession& sdp) const;

    //
    // Device State
    //

    /// Notify that Core Audio IO has started or stopped.
    /// When active, starts all dormant receivers/transmitters; when inactive, stops them.
    void setIOActive(bool active);

    // Set current device sample rate (validates against streams)
    bool setDeviceSampleRate(double sampleRate);

    /// True if every active stream runs at this rate (always true with no streams).
    bool isSampleRateCompatible(double sampleRate) const;

    // Get current device sample rate
    double getDeviceSampleRate() const { return currentDeviceSampleRate_; }

    /// Channels free for a new stream: neither running streams nor saved
    /// ones (getStreamsNotLoaded()) hold them.
    size_t getAvailableChannelCount(Direction direction) const;

    //
    // Configuration Persistence
    //

    /// Load the saved streams (streams.json, see StreamConfigManager).
    /// Entries not loaded (disabled, unreadable, or failing to start) are
    /// kept and written back on every save.
    /// @return true if at least one stream was loaded.
    bool loadSavedStreams();

    /// Whether the last loadSavedStreams() found a config file, even one
    /// with no usable streams (or a corrupt one).
    bool foundSavedConfig() const;

    /// A saved stream that isn't running: disabled, failed to start, or
    /// unreadable. An unreadable entry has whatever could be read (the ID
    /// may be null).
    ///
    /// It holds its channels whenever no running stream uses them, so new
    /// streams are not placed there. Where channels clash, running streams
    /// win, then saved ones in file order; channelsReserved says whether this
    /// one holds its channels now. An entry whose ID or channels can't be
    /// read holds none.
    struct SavedStreamInfo {
        StreamID id;
        std::string name;
        bool enabled{false};
        bool readable{false};
        bool channelsReserved{false};
        Direction direction{Direction::Receive};
        uint16_t deviceChannelStart{0};
        uint16_t deviceChannelCount{0};
    };

    /// The saved streams that aren't running. removeStream() takes their IDs.
    std::vector<SavedStreamInfo> getStreamsNotLoaded() const;

    /// Persist all current streams to disk.
    bool saveAllStreams();

    // Enable/disable auto-save (automatically save after add/remove/update)
    void setAutoSave(bool enabled) { autoSaveEnabled_ = enabled; }

    // Get auto-save state
    bool isAutoSaveEnabled() const { return autoSaveEnabled_; }

    //
    // Callbacks
    //

    // Register callback for stream added
    void setStreamAddedCallback(StreamCallback callback) {
        streamAddedCallback_ = callback;
    }

    // Register callback for stream removed
    void setStreamRemovedCallback(StreamCallback callback) {
        streamRemovedCallback_ = callback;
    }

    // Register callback for stream status changed
    void setStreamStatusCallback(StreamCallback callback) {
        streamStatusCallback_ = callback;
    }

private:
    // Internal stream state
    struct ManagedStream {
        SDPSession sdp;
        ChannelMapping mapping;
        std::unique_ptr<RTPReceiver> receiver;
        std::unique_ptr<RTPTransmitter> transmitter;
        StreamInfo info;
        bool isTransmit{false};
        std::string networkInterface;  // as configured (saved with the stream)
    };

    // Validation helpers
    bool validateSampleRate(const SDPSession& sdp, std::string* errorOut) const;
    bool validateChannelAvailability(uint16_t numChannels, Direction direction, std::string* errorOut) const;
    bool validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const;
    bool streamsSupportSampleRate(double sampleRate) const;  // Caller must hold streamsMutex_

    // Stream creation helpers
    std::unique_ptr<RTPReceiver> createReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );

    std::unique_ptr<RTPTransmitter> createTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& networkInterface = ""
    );

    // Callback invocation
    void notifyStreamAdded(const StreamInfo& info);
    void notifyStreamRemoved(const StreamInfo& info);
    void notifyStreamStatusChanged(const StreamInfo& info);

    // Announce a running TX stream over SAP (caller holds streamsMutex_)
    void announceTx(const StreamID& id, const ManagedStream& managed);

    // An interface streams use changed: restart those streams on it as it
    // is now (called on the network monitor's thread)
    void onInterfaceChanged(const std::string& networkInterface);


    // A saved entry not loaded, and whether its channels are held in its
    // direction's mapper (under its saved ID)
    struct KeptStream {
        SavedStreamEntry entry;
        bool channelsReserved{false};
    };

    // Configuration helpers (all: caller holds streamsMutex_)
    bool loadSavedStream(const PersistedStreamConfig& config);
    void dropKeptCopiesOf(const SDPSession& sdp);
    void releaseKeptCopiesOf(const SDPSession& sdp);  // their channels, for the stream replacing them
    void releaseKeptChannels(KeptStream& kept);
    void reserveKeptChannels();  // every kept entry whose channels are free takes them
    void autoSaveIfEnabled();
    bool saveAllStreamsInternal();  // Internal version without locking

    // Data members
    RxContext rxContext_;                   // RTP receivers place audio here (Network → Core Audio)
    TxContext txContext_;                   // RTP transmitters send from here (Core Audio → Network)
    SAPAnnouncer announcer_;                // announces TX streams while they run
    std::mt19937 sessionIdRandom_{std::random_device{}()};  // guarded by streamsMutex_
    StreamChannelMapper inputMapper_;   // RX streams -> device input channels
    StreamChannelMapper outputMapper_;  // TX streams <- device output channels
    StreamChannelMapper& mapperFor(Direction direction) {
        return direction == Direction::Transmit ? outputMapper_ : inputMapper_;
    }
    const StreamChannelMapper& mapperFor(Direction direction) const {
        return direction == Direction::Transmit ? outputMapper_ : inputMapper_;
    }
    StreamChannelMapper& mapperFor(const ManagedStream& managed) {
        return mapperFor(managed.isTransmit ? Direction::Transmit : Direction::Receive);
    }
    std::map<StreamID, ManagedStream> streams_;
    mutable std::mutex streamsMutex_;

    // Configuration management
    std::unique_ptr<StreamConfigManager> configManager_;
    bool autoSaveEnabled_{true};
    std::vector<KeptStream> keptEntries_;  // saved entries not loaded, in file order; guarded by streamsMutex_
    bool savedConfigFound_{false};        // guarded by streamsMutex_

    // IO lifecycle state — true when Core Audio IO is active (StartIO/StopIO)
    std::atomic<bool> ioActive_{false};
    std::atomic<uint64_t> networkRestarts_{0};

    // Last member: destroyed first, so no change is handled mid-destruction
    std::unique_ptr<NetworkMonitor> networkMonitor_;

    // Device state
    std::atomic<double> currentDeviceSampleRate_{48000.0};
    std::optional<PtpGrandmaster> ptpGrandmaster_;  // guarded by streamsMutex_

    // Callbacks
    StreamCallback streamAddedCallback_;
    StreamCallback streamRemovedCallback_;
    StreamCallback streamStatusCallback_;
};

} // namespace AES67
