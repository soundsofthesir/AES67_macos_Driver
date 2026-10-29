//
// AES67Device.cpp
// AES67 macOS Driver - Build #9
// Core Audio device implementation
//

#include "AES67Device.h"
#include "AES67IOHandler.h"
#include "SDPParser.h"
#include "DebugLog.h"
#include "../NetworkEngine/Clock/DeviceTimeline.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/PTP/PtpSettings.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <cmath>
#include <utility>

namespace AES67 {

AES67Device::AES67Device(std::shared_ptr<aspl::Context> context)
    : aspl::Device(context, aspl::DeviceParameters{
        .Name = "AES67 Device",
        .Manufacturer = "AES67 Driver",
        .DeviceUID = "com.aes67.driver.device",
        .ModelUID = "com.aes67.driver.model",
        .CanBeDefault = true,
        .CanBeDefaultForSystemSounds = false,
        .SampleRate = kDefaultSampleRate,
        .ZeroTimeStampPeriod = kZeroTimeStampPeriod,
        .ClockIsStable = true,
        .ClockAlgorithm = kAudioDeviceClockAlgorithmRaw
    })
    , clockRecovery_(ClockServo::Config{}, [this](uint64_t, double ratio) { ApplyRecoveredRate(ratio); })
{
    AES67_LOG("AES67Device constructor: Starting initialization");
    linkOffsetFrames_.store(LinkOffsetFramesFor(currentSampleRate_.load()));
    AES67_LOGF("AES67Device: Initial sample rate = %.0f Hz", currentSampleRate_.load());

    // NOTE: Cannot call InitializeStreams() here because shared_from_this()
    // won't work until the shared_ptr is fully constructed
    // InitializeStreams() will be called from Initialize() method

    AES67_LOG("AES67Device constructor: Basic initialization complete");
}

void AES67Device::Initialize() {
    AES67_LOG("AES67Device::Initialize() called");

    // Valid clock from the start; restarted again whenever IO starts
    RestartTimeline(currentSampleRate_.load());

    // Initialize streams
    AES67_LOG("AES67Device: Calling InitializeStreams()");
    InitializeStreams();

    // Create RT-safe interface (compile-time boundary for IO handler)
    // Must be created before InitializeIOHandler() so it can be passed in.
    AES67_LOG("AES67Device: Creating RTSafeStreamInterface");
    rtInterface_ = std::make_unique<RTSafeStreamInterface>(
        rxRouting_,
        mediaClock_,
        linkOffsetFrames_,
        txRouting_,
        inputUnderruns_,
        outputUnderruns_,
        ioRunning_
    );
    AES67_LOG("AES67Device: RTSafeStreamInterface created successfully");

    // Initialize IO handler (uses RT-safe interface)
    AES67_LOG("AES67Device: Calling InitializeIOHandler()");
    InitializeIOHandler();

    // Initialize Stream Manager (manages all AES67 network streams)
    AES67_LOG("AES67Device: Creating StreamManager");
    streamManager_ = std::make_unique<StreamManager>(
        RxContext{mediaClock_, networkTime_, rxRouting_, linkOffsetFrames_, &clockRecovery_},
        TxContext{mediaClock_, txRouting_, &networkTime_});
    AES67_LOG("AES67Device: StreamManager created successfully");

    // Set device sample rate in StreamManager
    streamManager_->setDeviceSampleRate(currentSampleRate_.load());
    AES67_LOGF("AES67Device: StreamManager sample rate set to %.0f Hz", currentSampleRate_.load());

    // Load saved stream configurations from disk
    AES67_LOG("AES67Device: Attempting to load saved stream configurations");
    bool loadedSavedStreams = streamManager_->loadSavedStreams();

    // With no config file at all, create a test RX stream for initial testing.
    // A file whose streams didn't load is the user's: adding the test stream
    // would save over it
    if (!loadedSavedStreams && streamManager_->foundSavedConfig()) {
        AES67_LOG("AES67Device: Config found but no stream loaded; not adding the test stream");
    } else if (!loadedSavedStreams) {
        // Create test RX stream (Network → Core Audio) on channels 0-7
        AES67_LOG("AES67Device: No saved streams found, adding test RX stream (239.1.1.1:5004, 8ch @ 48kHz)");
        SDPSession testSDP;
        testSDP.sessionName = "Test AES67 Stream";
        testSDP.sessionInfo = "Hard-coded test stream for driver development";
        testSDP.connectionAddress = "239.1.1.1";  // AES67 multicast range
        testSDP.port = 5004;
        testSDP.numChannels = 8;
        testSDP.sampleRate = 48000.0;
        testSDP.encoding = "L24";
        testSDP.payloadType = 97;
        testSDP.ptime = 1.0;  // 1ms packets (48 samples @ 48kHz)
        testSDP.framecount = 48;
        testSDP.ptpDomain = 0;
        testSDP.sessionID = 123456;
        testSDP.sessionVersion = 1;
        testSDP.sourceAddress = "0.0.0.0";

        StreamID testStreamID = streamManager_->addStream(testSDP);
        if (!testStreamID.isNull()) {
            AES67_LOG("AES67Device: Test RX stream added successfully");
            AES67_LOGF("AES67Device: Test RX stream ID: %s", testStreamID.toString().c_str());
        } else {
            AES67_LOG("AES67Device: WARNING - Failed to add test RX stream");
        }

        // No built-in TX stream: a configured TX stream transmits
        // continuously, so one must never appear unasked
    }

    // PTP, only if ptp.json turns it on: it sends Delay_Req onto the network.
    // Last, so its thread finds the stream manager in place.
    StartPtp();

    AES67_LOG("AES67Device::Initialize() complete");
}

AES67Device::~AES67Device() {
    // First: its thread writes the media clock and notifies the HAL. Stopped
    // before ptpClock_ is cleared, as its listener (OnPtpChanged) reads
    // ptpClock_ (#58); cleared under clockWriteMutex_, as receive threads
    // read it in ApplyRecoveredRate.
    if (ptpClock_) {
        ptpClock_->stop();
        std::unique_ptr<PtpClockSource> stopped;
        {
            std::lock_guard<std::mutex> lock(clockWriteMutex_);
            stopped = std::move(ptpClock_);
        }
    }

    // Receivers reference mediaClock_, networkTime_ and rxRouting_, which are
    // declared after streamManager_ and so would be destroyed first
    streamManager_.reset();

    // Stream activity (kAudioStreamPropertyIsActive) belongs to the HAL, so it
    // is not touched here: setting it would notify the HAL during teardown.
    ioRunning_.store(false);
}

void AES67Device::InitializeStreams() {
    AES67_LOG("InitializeStreams: Creating input stream (Network → Core Audio)");
    // Create input stream (Network → Core Audio)
    aspl::StreamParameters inputParams;
    inputParams.Direction = aspl::Direction::Input;
    inputParams.StartingChannel = 1;
    inputParams.Latency = static_cast<UInt32>(linkOffsetFrames_.load());
    inputParams.Format.mSampleRate = currentSampleRate_.load();
    inputParams.Format.mFormatID = kAudioFormatLinearPCM;
    inputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    inputParams.Format.mBitsPerChannel = 32;
    inputParams.Format.mChannelsPerFrame = kNumChannels;
    inputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    inputParams.Format.mFramesPerPacket = 1;
    inputParams.Format.mBytesPerPacket = inputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Input stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    inputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        inputParams
    );
    AES67_LOG("InitializeStreams: Input stream created, adding to device");
    AddStreamAsync(inputStream_);
    AES67_LOG("InitializeStreams: Input stream added successfully");

    // Create output stream (Core Audio → Network)
    AES67_LOG("InitializeStreams: Creating output stream (Core Audio → Network)");
    aspl::StreamParameters outputParams;
    outputParams.Direction = aspl::Direction::Output;
    outputParams.StartingChannel = 1;
    outputParams.Format.mSampleRate = currentSampleRate_.load();
    outputParams.Format.mFormatID = kAudioFormatLinearPCM;
    outputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outputParams.Format.mBitsPerChannel = 32;
    outputParams.Format.mChannelsPerFrame = kNumChannels;
    outputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    outputParams.Format.mFramesPerPacket = 1;
    outputParams.Format.mBytesPerPacket = outputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Output stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    outputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        outputParams
    );
    AES67_LOG("InitializeStreams: Output stream created, adding to device");
    AddStreamAsync(outputStream_);
    AES67_LOG("InitializeStreams: Output stream added successfully");

    AES67_LOG("InitializeStreams: Complete");
}

void AES67Device::InitializeIOHandler() {
    AES67_LOG("InitializeIOHandler: Creating AES67IOHandler with RTSafeStreamInterface");
    ioHandler_ = std::make_shared<AES67IOHandler>(
        *rtInterface_,
        kNumChannels,           // Cache channel count for RT-safe access
        sizeof(Float32)         // Cache bytes per sample for RT-safe access
    );
    AES67_LOG("InitializeIOHandler: IOHandler created successfully");

    // Register IO handler with device
    AES67_LOG("InitializeIOHandler: Registering IOHandler with device");
    SetIOHandler(ioHandler_);
    AES67_LOG("InitializeIOHandler: Complete");
}

Float64 AES67Device::GetSampleRate() const {
    return currentSampleRate_.load();
}

bool AES67Device::IsSupportedSampleRate(Float64 sampleRate) {
    for (auto validRate : kSupportedSampleRates) {
        if (std::abs(sampleRate - validRate) < 0.1) {
            return true;
        }
    }
    return false;
}

OSStatus AES67Device::SetNominalSampleRateImpl(Float64 rate) {
    if (!IsSupportedSampleRate(rate)) {
        return kAudioHardwareUnsupportedOperationError;
    }

    const Float64 previousRate = currentSampleRate_.load();
    AES67_LOGF("SetNominalSampleRateImpl: Changing from %.0f Hz to %.0f Hz", previousRate, rate);

    // StreamManager refuses rates its active streams cannot deliver (no SRC yet)
    if (streamManager_ && !streamManager_->setDeviceSampleRate(rate)) {
        AES67_LOGF("SetNominalSampleRateImpl: Refused %.0f Hz - active streams run at a different rate", rate);
        return kAudioDeviceUnsupportedFormatError;
    }

    const OSStatus status = aspl::Device::SetNominalSampleRateImpl(rate);
    if (status != kAudioHardwareNoError) {
        if (streamManager_) {
            streamManager_->setDeviceSampleRate(previousRate);
        }
        return status;
    }

    currentSampleRate_.store(rate);
    linkOffsetFrames_.store(LinkOffsetFramesFor(rate));
    if (inputStream_) {
        inputStream_->SetLatencyAsync(static_cast<UInt32>(linkOffsetFrames_.load()));
    }

    // Media positions are counted in samples, so a new rate is a new timeline
    RestartTimeline(rate);

    // Streams must present the same rate as the device. We are inside a HAL
    // configuration change, so these Async setters apply in place.
    if (inputStream_) {
        ApplyStreamSampleRate(*inputStream_, rate);
    }
    if (outputStream_) {
        ApplyStreamSampleRate(*outputStream_, rate);
    }

    AES67_LOGF("SetNominalSampleRateImpl: Now running at %.0f Hz", rate);
    return kAudioHardwareNoError;
}

void AES67Device::RestartTimeline(Float64 sampleRate) {
    // First, so no rate steered against the old timeline lands on the new one
    clockRecovery_.reset();

    std::lock_guard<std::mutex> lock(clockWriteMutex_);
    // Media positions are never reused: the new timeline starts beyond any
    // position the old one (or Core Audio writing output ahead on it) can have
    // touched, so no buffer slot from before reads as current. Device sample
    // time still starts at 0, counted from the new origin.
    const MediaClock::Snapshot previous = mediaClock_.snapshot();
    const uint64_t now = hostTimeNow();
    const int64_t start = previous.valid() ? previous.sampleAt(now) + kTimelineGapFrames : 0;
    mediaClock_.reset(now, start, MediaClock::samplesPerTick(sampleRate, 1.0, HostTimebase::current()));
    // Network time was anchored to the old timeline; receivers re-anchor on
    // their next packet (they also see the new clock generation)
    networkTime_.reset();
}

void AES67Device::ApplyRecoveredRate(double ratio) {
    // From now on: rebasing at the packet's (earlier) arrival would move
    // positions the HAL has already been given
    std::lock_guard<std::mutex> lock(clockWriteMutex_);
    if (ptpClock_ && ptpClock_->active()) {
        return;  // PTP has the clock
    }
    mediaClock_.setRate(hostTimeNow(),
                        MediaClock::samplesPerTick(currentSampleRate_.load(), ratio, HostTimebase::current()));
}

void AES67Device::StartPtp() {
    std::string path;
    std::string error;
    const Ptp::Settings settings = Ptp::Settings::load(&path, &error);
    if (!error.empty()) {
        AES67_LOGF("PTP: off: %s is not valid (%s)", path.c_str(), error.c_str());
        return;
    }
    if (!settings.enabled) {
        AES67_LOGF("PTP: off (%s)", path.empty() ? "no ptp.json" : path.c_str());
        return;
    }

    PtpClockSource::Config config;
    config.receiver.networkInterface = settings.networkInterface;
    config.receiver.domain = settings.domain;
    config.receiver.unicastDelayRequests = settings.unicastDelayRequests;
    ptpClock_ = std::make_unique<PtpClockSource>(
        config, mediaClock_, currentSampleRate_,
        [this](uint32_t generation, double samplesPerTick) { ApplyPtpRate(generation, samplesPerTick); },
        [this] { OnPtpChanged(); },
        [this](uint32_t generation, int64_t offset) { ApplyPtpOffset(generation, offset); });
    ptpClock_->start();
    AES67_LOGF("PTP: on (%s): interface '%s', domain %u, %s Delay_Req", path.c_str(),
               settings.networkInterface.empty() ? "primary" : settings.networkInterface.c_str(),
               static_cast<unsigned>(settings.domain), settings.unicastDelayRequests ? "unicast" : "multicast");
}

void AES67Device::ApplyPtpRate(uint32_t clockGeneration, double samplesPerTick) {
    std::lock_guard<std::mutex> lock(clockWriteMutex_);
    if (mediaClock_.snapshot().generation != clockGeneration) {
        return;  // computed for a timeline that has since restarted
    }
    mediaClock_.setRate(hostTimeNow(), samplesPerTick);
}

void AES67Device::ApplyPtpOffset(uint32_t clockGeneration, int64_t offset) {
    std::lock_guard<std::mutex> lock(clockWriteMutex_);
    if (mediaClock_.snapshot().generation != clockGeneration) {
        return;  // for a timeline that has since restarted
    }
    // Network time is now PTP time: receivers place by it, transmitters stamp by it
    networkTime_.fix(offset);
}

void AES67Device::OnPtpChanged() {
    NotifyPropertyChanged(kAudioDevicePropertyClockIsStable);
    NotifyPropertyChanged(kAudioDevicePropertyClockDomain);

    // TX timestamps are now the followed grandmaster's time: announce it. On
    // losing lock the last one stays announced until another is followed.
    if (!ptpClock_->locked()) {
        return;
    }
    const auto status = ptpClock_->status();
    if (status.receiver.master && streamManager_) {
        streamManager_->setPtpGrandmaster(StreamManager::PtpGrandmaster{
            status.receiver.master->announce.grandmasterIdentity.toString(), ptpClock_->domain()});
    }
}

bool AES67Device::GetClockIsStable() const {
    return ptpClock_ ? ptpClock_->locked() : aspl::Device::GetClockIsStable();
}

UInt32 AES67Device::GetClockDomain() const {
    return ptpClock_ ? ptpClock_->clockDomain() : aspl::Device::GetClockDomain();
}

int64_t AES67Device::LinkOffsetFramesFor(Float64 sampleRate) {
    return static_cast<int64_t>(std::llround(sampleRate * kLinkOffsetSeconds));
}

OSStatus AES67Device::GetZeroTimeStampImpl(UInt32 clientID, Float64* outSampleTime, UInt64* outHostTime,
                                           UInt64* outSeed) {
    // RT-SAFE: lock-free snapshot, no allocation
    const ZeroTimeStamp zts = zeroTimeStampAt(mediaClock_.snapshot(), hostTimeNow(), kZeroTimeStampPeriod);
    *outSampleTime = zts.sampleTime;
    *outHostTime = zts.hostTime;
    *outSeed = zts.seed;
    (void)clientID;
    return kAudioHardwareNoError;
}

void AES67Device::ApplyStreamSampleRate(aspl::Stream& stream, Float64 sampleRate) {
    auto format = stream.GetPhysicalFormat();
    format.mSampleRate = sampleRate;

    const AudioStreamRangedDescription ranged{format, {sampleRate, sampleRate}};
    stream.SetAvailablePhysicalFormatsAsync({ranged});
    stream.SetAvailableVirtualFormatsAsync({ranged});
    stream.SetPhysicalFormatAsync(format);
    stream.SetVirtualFormatAsync(format);
}

std::vector<AudioValueRange> AES67Device::GetAvailableSampleRates() const {
    std::vector<AudioValueRange> ranges;
    for (auto rate : kSupportedSampleRates) {
        if (!streamManager_ || streamManager_->isSampleRateCompatible(rate)) {
            ranges.push_back({rate, rate});
        }
    }
    return ranges;
}

UInt32 AES67Device::GetBufferSize() const {
    return currentBufferSize_.load();
}

OSStatus AES67Device::SetBufferSize(UInt32 bufferSize) {
    // Validate buffer size
    bool isValid = false;
    for (auto validSize : kSupportedBufferSizes) {
        if (bufferSize == validSize) {
            isValid = true;
            break;
        }
    }

    if (!isValid) {
        return kAudioHardwareUnsupportedOperationError;
    }

    // Update current buffer size
    currentBufferSize_.store(bufferSize);

    return kAudioHardwareNoError;
}

std::vector<UInt32> AES67Device::GetAvailableBufferSizes() const {
    return std::vector<UInt32>(kSupportedBufferSizes.begin(), kSupportedBufferSizes.end());
}

std::string AES67Device::GetDeviceName() const {
    return "AES67 Device";
}

std::string AES67Device::GetDeviceManufacturer() const {
    return "AES67 Driver";
}

std::string AES67Device::GetDeviceUID() const {
    return "com.aes67.driver.device";
}

OSStatus AES67Device::StartIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means first client starting IO (device transitions to running)
    if (startCount == 0) {
        // Stream activity is left to the HAL, which activates the streams its
        // clients use. Setting it here would send a property-change
        // notification affecting IO, which AudioServerPlugIn.h forbids.
        ioRunning_.store(true);

        // New timeline for this IO session (new seed for the HAL)
        RestartTimeline(currentSampleRate_.load());

        // Start RTP network threads now that a client needs audio
        if (streamManager_) {
            streamManager_->setIOActive(true);
        }
    }

    return aspl::Device::StartIOImpl(clientID, startCount);
}

OSStatus AES67Device::StopIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means last client stopped IO (device transitions to not running)
    if (startCount == 0) {
        // Stop RTP network threads — no client needs audio anymore
        if (streamManager_) {
            streamManager_->setIOActive(false);
        }

        ioRunning_.store(false);
    }

    return aspl::Device::StopIOImpl(clientID, startCount);
}

void AES67Device::ResetStatistics() {
    inputUnderruns_.store(0);
    outputUnderruns_.store(0);
}

OSStatus AES67Device::OnSetBufferSize(UInt32 bufferSize) {
    return SetBufferSize(bufferSize);
}

} // namespace AES67
