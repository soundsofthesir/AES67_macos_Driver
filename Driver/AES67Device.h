//
// AES67Device.h
// AES67 macOS Driver - Build #1
// Core Audio device implementation using libASPL
// 128-channel input/output AudioServerPlugIn
//

#pragma once

#include "../Shared/Types.h"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../NetworkEngine/Clock/MediaClock.h"
#include "../NetworkEngine/Clock/RecoveredClockSource.h"
#include "../NetworkEngine/Clock/PtpClockSource.h"
#include "../NetworkEngine/RTP/RtpPlacement.h"
#include "../NetworkEngine/RTP/RxRouting.h"
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/Context.hpp>
#include <memory>
#include <array>
#include <atomic>
#include <mutex>

namespace AES67 {

class AES67IOHandler;

//
// AES67 Audio Device
//
// 128-channel bidirectional Core Audio device
// Integrates with macOS Core Audio via AudioServerPlugIn (libASPL)
//
class AES67Device : public aspl::Device {
public:
    static constexpr size_t kNumChannels = 128;

    // Nominal rate at startup (AES67 baseline). Must be passed to libASPL via
    // DeviceParameters::SampleRate, whose own default is 44.1 kHz.
    static constexpr UInt32 kDefaultSampleRate = 48000;

    // Frames between zero timestamps (HAL minimum 10923). See Docs/Spikes/S2-HAL-Clock.md.
    static constexpr UInt32 kZeroTimeStampPeriod = 16384;

    // Receive latency: playout reads this far behind now. 8 x the AES67 default
    // 1 ms packet time, matching the target network's receive buffer
    // (Docs/Step2-Clocking-Plan.md, decision 1).
    static constexpr double kLinkOffsetSeconds = 0.008;

    // Supported sample rates
    static constexpr std::array<Float64, 8> kSupportedSampleRates = {
        44100.0, 48000.0, 88200.0, 96000.0,
        176400.0, 192000.0, 352800.0, 384000.0
    };

    // Supported buffer sizes (in samples)
    static constexpr std::array<UInt32, 8> kSupportedBufferSizes = {
        16, 32, 48, 64, 128, 192, 288, 480
    };

    //
    // Constructor
    //
    explicit AES67Device(std::shared_ptr<aspl::Context> context);
    ~AES67Device();

    // Initialize device (must be called after construction)
    void Initialize();

    //
    // Device Configuration
    //

    // Current sample rate as seen by the driver (follows the HAL nominal rate)
    Float64 GetSampleRate() const;

    // Rates offered to the HAL: supported rates that every active stream can deliver
    std::vector<AudioValueRange> GetAvailableSampleRates() const override;

    // Get/Set buffer size
    UInt32 GetBufferSize() const;
    OSStatus SetBufferSize(UInt32 bufferSize);

    // Get available buffer sizes
    std::vector<UInt32> GetAvailableBufferSizes() const;

    //
    // Device Information
    //

    std::string GetDeviceName() const;
    std::string GetDeviceManufacturer() const;
    std::string GetDeviceUID() const override;

    // Get channel count
    UInt32 GetInputChannelCount() const { return kNumChannels; }
    UInt32 GetOutputChannelCount() const { return kNumChannels; }

    //
    // Stream Access
    //

    // Get input/output streams
    std::shared_ptr<aspl::Stream> GetInputStream() const { return inputStream_; }
    std::shared_ptr<aspl::Stream> GetOutputStream() const { return outputStream_; }

    //
    // RT-Safe Interface Access
    //

    // Returns the RT-safe interface for use by AES67IOHandler.
    // The interface is created during Initialize() and is valid for the
    // lifetime of this device. Only pass this to RT-safe code paths.
    RTSafeStreamInterface* GetRTInterface() { return rtInterface_.get(); }
    const RTSafeStreamInterface* GetRTInterface() const { return rtInterface_.get(); }

    //
    // Stream Manager Access
    //

    StreamManager* GetStreamManager() { return streamManager_.get(); }
    const StreamManager* GetStreamManager() const { return streamManager_.get(); }

    //
    // Clock
    //

    // The device's media clock: Core Audio's zero timestamps are computed from
    // it. Snapshots are real-time safe. Driven by PTP when ptp.json enables it
    // and a master is followed, otherwise by a received stream or the host.
    const MediaClock& GetMediaClock() const { return mediaClock_; }

    // PTP, if enabled (null otherwise)
    const PtpClockSource* GetPtpClockSource() const { return ptpClock_.get(); }

    // Stable unless PTP is enabled and not yet followed (it will steer the rate);
    // the clock domain is the grandmaster's while it is followed
    bool GetClockIsStable() const override;
    UInt32 GetClockDomain() const override;

    //
    // Control
    //

    // Start/Stop IO (overrides from aspl::Device)
    OSStatus StartIOImpl(UInt32 clientID, UInt32 startCount) override;
    OSStatus StopIOImpl(UInt32 clientID, UInt32 startCount) override;

    //
    // Statistics
    //

    uint64_t GetInputUnderrunCount() const { return inputUnderruns_.load(); }
    uint64_t GetOutputUnderrunCount() const { return outputUnderruns_.load(); }
    void ResetStatistics();

protected:
    // Invoked by libASPL (inside a HAL configuration change) when a client sets
    // kAudioDevicePropertyNominalSampleRate. Applies the rate to the driver,
    // StreamManager and both streams' formats.
    OSStatus SetNominalSampleRateImpl(Float64 rate) override;

    // Zero timestamps from the media clock (Raw algorithm: the HAL uses them as-is)
    OSStatus GetZeroTimeStampImpl(UInt32 clientID, Float64* outSampleTime, UInt64* outHostTime,
                                  UInt64* outSeed) override;

private:
    // Internal handlers
    OSStatus OnSetBufferSize(UInt32 bufferSize);

    static bool IsSupportedSampleRate(Float64 sampleRate);

    // Host clock source: start a new timeline at device time 0, running at the
    // nominal rate on the host clock (new seed)
    void RestartTimeline(Float64 sampleRate);

    // Put the driver's rate, link offset, timeline and both streams' formats
    // on `rate` (the HAL and StreamManager are set by the caller)
    void ApplySampleRate(Float64 rate);

    // Lock a stream's physical and virtual formats to the given sample rate
    static void ApplyStreamSampleRate(aspl::Stream& stream, Float64 sampleRate);

    // Initialize streams and IO handler
    void InitializeStreams();
    void InitializeIOHandler();


    // Streams
    std::shared_ptr<aspl::Stream> inputStream_;
    std::shared_ptr<aspl::Stream> outputStream_;

    // IO Handler
    std::shared_ptr<AES67IOHandler> ioHandler_;

    // Stream Manager (manages all AES67 network streams)
    std::unique_ptr<StreamManager> streamManager_;

    // RT-safe interface (compile-time boundary for IO handler)
    // Created during Initialize(), references the receive routing, media clock,
    // link offset, transmit routing and atomics
    std::unique_ptr<RTSafeStreamInterface> rtInterface_;

    // Current configuration
    std::atomic<Float64> currentSampleRate_{static_cast<Float64>(kDefaultSampleRate)};
    std::atomic<UInt32> currentBufferSize_{64};

    // State
    std::atomic<bool> ioRunning_{false};

    // Media clock. Writers (timeline restarts, recovered rate) are serialised by clockWriteMutex_;
    // readers, including GetZeroTimeStampImpl on the IO thread, never lock.
    MediaClock mediaClock_;
    std::mutex clockWriteMutex_;

    // Receive path (step 2 phase 2): network time -> media position, where the
    // IO thread finds each stream's buffer, and the playout delay in frames
    NetworkTimeMapping networkTime_;
    RxRouting rxRouting_;
    std::atomic<int64_t> linkOffsetFrames_{0};

    // Transmit path: where the IO thread writes each TX stream's output
    TxRouting txRouting_;

    // Frames between one timeline's positions and the next's (about 22 s at
    // 48 kHz): beyond any buffer and any output written ahead
    static constexpr int64_t kTimelineGapFrames = int64_t{1} << 20;

    // Stream-recovered clock (step 2 phase 3): steers mediaClock_'s rate to
    // the reference receive stream. Takes clockWriteMutex_ inside its own lock,
    // so it must be reset without clockWriteMutex_ held.
    RecoveredClockSource clockRecovery_;
    void ApplyRecoveredRate(double ratio);

    // PTP clock source (step 2 phase 5a), when ptp.json enables it. Takes
    // precedence over the recovered clock while active. Declared last so it
    // stops first.
    void StartPtp();
    void ApplyPtpRate(uint32_t clockGeneration, double samplesPerTick);
    void ApplyPtpOffset(uint32_t clockGeneration, int64_t offset);
    void OnPtpChanged();

    static int64_t LinkOffsetFramesFor(Float64 sampleRate);

    // Statistics
    std::atomic<uint64_t> inputUnderruns_{0};
    std::atomic<uint64_t> outputUnderruns_{0};

    std::unique_ptr<PtpClockSource> ptpClock_;
};

} // namespace AES67
