//
// TestCriticalPathRegressions.cpp
// AES67 macOS Driver
// Regression tests for the critical-path defects found in review, kept
// current with the step 2 receive path (placement by RTP timestamp):
//   - Core Audio output never reaching the TX ring buffers (mixing mode)
//   - Device nominal sample rate defaulting to 44.1 kHz / rate changes ignored
//   - TX streams reloading as RX streams
//   - The device clock coming from the device's own MediaClock
//   - Input read by device time minus the link offset, for any buffer size,
//     identically for every client
//   - Receiver following a sender restart (ahead of or behind playout)
//   - Lost packets and outages keeping the stream's place on the timeline
//   - Late packets or a second sender on the group being ignored
//
// Uses a non-aborting CHECK so every failure is reported, and works under NDEBUG.
//

#include "RxTestSupport.h"
#include "SapTestSupport.h"
#include "PtpTestMaster.h"
#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "../NetworkEngine/Clock/PtpClockControl.h"
#include "../NetworkEngine/Discovery/SAPListener.h"
#include "../NetworkEngine/NetworkInterfaceDetection.h"
#include "../NetworkEngine/NetworkMonitor.h"
#include "../Driver/AES67Device.h"
#include "../Driver/AES67IOHandler.h"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../Driver/AudioThreadPriority.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../NetworkEngine/RTP/TxContext.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/Clock/TimestampedAudioBuffer.h"
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <aspl/Context.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace AES67;
using namespace AES67::TestSupport;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

constexpr size_t kNumChannels = 128;
// Point the driver's config search at a fresh file containing no streams.
// Test TX streams stay on this host: TTL 0, default interface
const StreamManager::TxOptions kHostOnly{"", 0};

// Test StreamManagers announce on a test port: even TTL 0 announcements on the
// real port 9875 would reach SAP listeners on this Mac (Dante Controller)
SAPAnnouncer::Config testSap() {
    SAPAnnouncer::Config config;
    config.port = kTestSapPort;
    config.interval = std::chrono::milliseconds(500);
    return config;
}

std::string useEmptyConfig(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp ? tmp : "/tmp/") + "aes67_regression_" + name + ".json";
    std::ofstream(path) << "{\n  \"version\": \"1.0\",\n  \"streams\": [\n  ]\n}\n";
    setenv("AES67_CONFIG_PATH", path.c_str(), 1);
    return path;
}

// Multicast listener used to observe what a transmitter actually sends.
class MulticastListener {
public:
    MulticastListener(const char* group, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(group);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        timeval tv{0, 20000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~MulticastListener() { if (fd_ >= 0) close(fd_); }

    size_t countPackets(std::chrono::milliseconds window) {
        size_t count = 0;
        uint8_t buf[2048];
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline) {
            if (recv(fd_, buf, sizeof(buf), 0) > 0) ++count;
        }
        return count;
    }

private:
    int fd_{-1};
};

SDPSession makeRxSDP(const char* group, uint16_t port, uint16_t channels) {
    SDPSession sdp;
    sdp.sessionName = "Regression RX";
    sdp.connectionAddress = group;
    sdp.port = port;
    sdp.numChannels = channels;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = kPayloadTypeL24;
    sdp.ptime = 1;
    sdp.framecount = kFramesPerPacket;
    sdp.ttl = 0;  // host-only, even if a regression made this stream transmit
    return sdp;
}

ChannelMapping makeMapping(uint16_t channels, uint16_t deviceStart) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Regression";
    mapping.streamChannelCount = channels;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = channels;
    return mapping;
}

// Runs a receiver against a fresh device context, sends with `send`, and
// returns what the IO thread would have heard on device channel 0.
template<typename SendFn>
std::vector<float> receiveAndListen(const char* group, uint16_t port, SendFn send, RTPReceiver::PlacementStatistics* stats = nullptr) {
    RxHarness harness;
    RTPReceiver receiver(makeRxSDP(group, port, 2), makeMapping(2, 0), harness.context());
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PlayoutReader reader(harness, 0);
    LoopbackSender sender(group, port);
    // Pace like a hardware sender: a starved test thread would send packets
    // after their timestamps, which the receiver rightly drops as late
    AudioThreadPriority::configureForRealTime();
    send(sender);
    AudioThreadPriority::restoreNormalPriority();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let playout pass the last packet
    auto samples = reader.stop();
    if (stats) *stats = receiver.getPlacementStatistics();
    receiver.stop();
    return samples;
}

// Sends `count` packets of `value` at 1 ms pacing, skipping indices for which skip(i) is true.
template<typename SkipFn>
void sendRun(LoopbackSender& sender, std::chrono::steady_clock::time_point& next, uint16_t firstSeq,
             uint32_t firstTimestamp, uint16_t count, float value, uint32_t ssrc, SkipFn skip) {
    for (uint16_t i = 0; i < count; ++i) {
        if (!skip(i)) {
            sender.sendL24(static_cast<uint16_t>(firstSeq + i), firstTimestamp + i * kFramesPerPacket, 2, value, ssrc);
        }
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
}

const auto kNoSkip = [](uint16_t) { return false; };

// RT-safe interface over a harness's receive context plus TX ring buffers
struct IOFixture {
    RxHarness harness;
    TxRouting txRouting;
    std::atomic<uint64_t> inputUnderruns{0};
    std::atomic<uint64_t> outputOverruns{0};
    std::atomic<bool> ioRunning{true};
    RTSafeStreamInterface rt{harness.routing, harness.clock, harness.linkOffsetFrames, txRouting,
                             inputUnderruns, outputOverruns, ioRunning};
    AES67IOHandler handler{rt, kNumChannels, sizeof(float)};
};

// ---------------------------------------------------------------------------
// C1: Core Audio output must reach the transmitters.
// With DeviceParameters::EnableMixing (the default) libASPL delivers output via
// OnWriteMixedOutput only; OnWriteClientOutput is never called. The mix is
// written into each TX stream's buffer at the samples' media positions.
// ---------------------------------------------------------------------------
void testMixedOutputReachesTxBuffers() {
    std::cout << "C1: mixed output reaches TX buffers at its media position" << std::endl;

    IOFixture io;
    TimestampedAudioBuffer tx(2, 16384);  // a 2-channel TX stream on device channels 8-9
    CHECK(io.txRouting.publish(&tx, 8), "TX route should publish");

    constexpr UInt32 kFrames = 64;
    std::vector<float> interleaved(kFrames * kNumChannels);
    for (UInt32 f = 0; f < kFrames; ++f) {
        for (size_t ch = 0; ch < kNumChannels; ++ch) {
            interleaved[f * kNumChannels + ch] = static_cast<float>(ch) * 0.001f + f * 1e-5f;
        }
    }

    aspl::IORequestHandler& base = io.handler;
    const Float64 sampleTime = 2048.0;
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, sampleTime, interleaved.data(),
                            static_cast<UInt32>(interleaved.size() * sizeof(float)));

    const int64_t position = io.harness.clock.snapshot().origin + static_cast<int64_t>(sampleTime);
    std::vector<float> stream(kFrames * 2);
    CHECK(tx.read(position, kFrames, stream.data(), 2, 0) == kFrames,
          "every mixed frame should be in the TX buffer at origin + sample time");
    bool matches = true;
    for (UInt32 f = 0; f < kFrames; ++f) {
        matches = matches && std::fabs(stream[f * 2] - (8 * 0.001f + f * 1e-5f)) < 1e-7f &&
                  std::fabs(stream[f * 2 + 1] - (9 * 0.001f + f * 1e-5f)) < 1e-7f;
    }
    CHECK(matches, "the stream's channels should come from its device channels 8-9");

    // Output buffers larger than the handler's scratch buffer are processed in chunks
    constexpr UInt32 kLarge = 5000;
    std::vector<float> large(kLarge * kNumChannels, 0.5f);
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, 8192.0, large.data(),
                            static_cast<UInt32>(large.size() * sizeof(float)));
    std::vector<float> largeRead(kLarge * 2);
    const int64_t largePosition = io.harness.clock.snapshot().origin + 8192;
    CHECK(tx.read(largePosition, kLarge, largeRead.data(), 2, 0) == kLarge,
          "all 5000 output frames should reach the TX buffer at consecutive positions");
    io.txRouting.unpublish(&tx);
}

// ---------------------------------------------------------------------------
void testInputReadsByDeviceTime() {
    std::cout << "IO handler reads input by device time minus link offset" << std::endl;

    IOFixture io;
    aspl::IORequestHandler& base = io.handler;
    constexpr UInt32 kFrames = 5000;  // larger than the old 4096-frame limit
    constexpr int64_t kDeviceTime = 100000;
    const int64_t readPosition = io.harness.clock.snapshot().origin + kDeviceTime - kTestLinkOffset;

    TimestampedAudioBuffer stream(2, 8192);
    std::vector<float> ramp(kFrames * 2);
    for (UInt32 f = 0; f < kFrames; ++f) {
        ramp[f * 2] = static_cast<float>(f) * 1e-4f;
        ramp[f * 2 + 1] = -static_cast<float>(f) * 1e-4f;
    }
    stream.write(readPosition, kFrames, ramp.data(), 2, 0);
    CHECK(io.harness.routing.publish(&stream, 6), "route should publish");

    std::vector<float> clientA(kFrames * kNumChannels, -1.0f);
    std::vector<float> clientB(kFrames * kNumChannels, -2.0f);
    const auto bytes = static_cast<UInt32>(clientA.size() * sizeof(float));
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime,
                           clientA.data(), bytes);
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime,
                           clientB.data(), bytes);

    bool placed = true;
    bool othersSilent = true;
    for (UInt32 f = 0; f < kFrames; ++f) {
        placed = placed && clientA[f * kNumChannels + 6] == ramp[f * 2] && clientA[f * kNumChannels + 7] == ramp[f * 2 + 1];
        othersSilent = othersSilent && clientA[f * kNumChannels + 5] == 0.0f && clientA[f * kNumChannels + 8] == 0.0f &&
                       clientA[f * kNumChannels] == 0.0f;
    }
    CHECK(placed, "the stream should land on device channels 6-7 at device time minus link offset, all 5000 frames");
    CHECK(othersSilent, "channels no stream feeds should be silent");
    CHECK(clientA == clientB, "a second client in the same cycle should read identical input");
    CHECK(io.inputUnderruns.load() == 0, "fully covered reads should not count as underruns");

    std::vector<float> later(512 * kNumChannels, -1.0f);
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime + 50000,
                           later.data(), static_cast<UInt32>(later.size() * sizeof(float)));
    CHECK(std::all_of(later.begin(), later.end(), [](float v) { return v == 0.0f; }), "missing audio should read as silence");
    CHECK(io.inputUnderruns.load() == 1, "a read the stream does not cover should count one underrun");
    io.harness.routing.unpublish(&stream);
}

// ---------------------------------------------------------------------------
// C2: device nominal rate must match the stream formats and follow rate changes.
// ---------------------------------------------------------------------------
void testDeviceSampleRate() {
    std::cout << "C2: device nominal sample rate" << std::endl;
    useEmptyConfig("device");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    CHECK(device->GetNominalSampleRate() == 48000.0, "nominal rate should start at 48 kHz, not libASPL's 44.1 kHz default");
    CHECK(device->GetInputStream()->GetPhysicalFormat().mSampleRate == device->GetNominalSampleRate(),
          "input stream format should match the device nominal rate");

    // The default test streams are 48 kHz, so only 48 kHz can be offered.
    const auto rates = device->GetAvailableSampleRates();
    CHECK(rates.size() == 1 && rates[0].mMinimum == 48000.0,
          "available rates should be limited to what the active streams can deliver");
    device->SetNominalSampleRateAsync(96000.0);
    CHECK(device->GetNominalSampleRate() == 48000.0, "96 kHz should be refused while 48 kHz streams exist");

    // With no streams, a rate change must propagate everywhere.
    device->GetStreamManager()->removeAllStreams();
    device->SetNominalSampleRateAsync(96000.0);
    CHECK(device->GetNominalSampleRate() == 96000.0, "nominal rate should change to 96 kHz");
    CHECK(device->GetSampleRate() == 96000.0, "driver's own rate should follow the nominal rate");
    CHECK(device->GetStreamManager()->getDeviceSampleRate() == 96000.0, "StreamManager should follow the nominal rate");
    CHECK(device->GetInputStream()->GetPhysicalFormat().mSampleRate == 96000.0, "input physical format should follow");
    CHECK(device->GetOutputStream()->GetPhysicalFormat().mSampleRate == 96000.0, "output physical format should follow");
    CHECK(device->GetOutputStream()->GetVirtualFormat().mSampleRate == 96000.0, "output virtual format should follow");
}

// ---------------------------------------------------------------------------
// Phase 1: the device's Core Audio clock comes from its MediaClock.
// ---------------------------------------------------------------------------
void testDeviceClockFromMediaClock() {
    std::cout << "Device clock driven by MediaClock" << std::endl;
    useEmptyConfig("deviceclock");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    CHECK(device->GetClockAlgorithm() == kAudioDeviceClockAlgorithmRaw,
          "clock algorithm should be Raw: timestamps come from the clock model, not HAL smoothing");
    CHECK(device->GetZeroTimeStampPeriod() == 16384, "zero timestamp period should be 16384 frames");
    CHECK(device->GetClockIsStable(), "the host-clock source should report a stable clock");

    CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should start");
    const double hostTicksPerSecond = HostTimebase::current().ticksPerSecond();
    Float64 sampleTime = -1;
    UInt64 hostTime = 0;
    UInt64 seed = 0;
    const uint64_t before = hostTimeNow();
    CHECK(device->GetZeroTimeStamp(device->GetID(), 0, &sampleTime, &hostTime, &seed) == kAudioHardwareNoError,
          "GetZeroTimeStamp should succeed while running");
    CHECK(std::fmod(sampleTime, 16384.0) == 0.0, "zero timestamp should sit on a period boundary");
    CHECK(hostTime <= before && static_cast<double>(before - hostTime) < 16384.0 / 48000.0 * hostTicksPerSecond,
          "zero timestamp should be the latest boundary before now at 48 kHz");
    CHECK(seed != 0, "seed should be set");

    // The timestamp must come from the device's own MediaClock: its host time
    // is exactly where that sample starts on the clock, and the seed is the
    // clock's generation (libASPL's default clock would match neither)
    const auto clock = device->GetMediaClock().snapshot();
    CHECK(clock.sampleAt(hostTime) - clock.origin == static_cast<int64_t>(sampleTime) &&
              clock.sampleAt(hostTime - 1) - clock.origin == static_cast<int64_t>(sampleTime) - 1,
          "zero timestamp should land exactly on the device's MediaClock");
    CHECK(seed == clock.generation, "seed should be the MediaClock generation");

    // Rate change: new timeline (seed) at the new rate
    device->StopIO(device->GetID(), 0);
    device->GetStreamManager()->removeAllStreams();
    device->SetNominalSampleRateAsync(96000.0);
    device->StartIO(device->GetID(), 0);
    Float64 sampleTime96 = -1;
    UInt64 hostTime96 = 0;
    UInt64 seed96 = 0;
    const uint64_t before96 = hostTimeNow();
    device->GetZeroTimeStamp(device->GetID(), 0, &sampleTime96, &hostTime96, &seed96);
    CHECK(seed96 != seed, "a sample rate change should start a new timeline (seed)");
    CHECK(static_cast<double>(before96 - hostTime96) < 16384.0 / 96000.0 * hostTicksPerSecond,
          "after the change, boundaries should be spaced for 96 kHz");
    device->StopIO(device->GetID(), 0);
}

// ---------------------------------------------------------------------------
// Step 2 phase 3: the device clock follows its received stream, and a
// timeline restart returns it to the nominal rate.
// ---------------------------------------------------------------------------
void testDeviceClockFollowsReceivedStream() {
    std::cout << "Device clock follows a received stream" << std::endl;
    useEmptyConfig("recovered");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();  // empty config: the built-in RX stream on 239.1.1.1:5004, 8 ch
    CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should start");
    const double nominal = MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current());

    // A sender 1500 ppm fast for 3 s: the device clock must speed up towards it
    LoopbackSender sender("239.1.1.1", 5004);
    AudioThreadPriority::configureForRealTime();
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e6 / 1.0015));
    auto next = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 3000; ++i) {
        sender.sendL24(static_cast<uint16_t>(i), i * kFramesPerPacket, 8, 0.5f);
        std::this_thread::sleep_until(next += period);
    }
    AudioThreadPriority::restoreNormalPriority();

    const double steered = device->GetMediaClock().snapshot().samplesPerTick / nominal - 1.0;
    CHECK(steered > 300e-6 && steered <= 2000e-6,
          "the device clock should speed up towards a fast sender (" << steered * 1e6 << " ppm)");

    // IO restart = new timeline at the nominal rate, recovery starts over
    const uint32_t generation = device->GetMediaClock().snapshot().generation;
    device->StopIO(device->GetID(), 0);
    CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should restart");
    const auto restarted = device->GetMediaClock().snapshot();
    CHECK(restarted.generation != generation && restarted.samplesPerTick == nominal,
          "a timeline restart should return the device clock to nominal");
    device->StopIO(device->GetID(), 0);
}

// ---------------------------------------------------------------------------
// PTP: off unless ptp.json turns it on; when on and a master is followed, it
// has the device clock and a received stream no longer steers it.
// ---------------------------------------------------------------------------
void usePtpSettings(const std::string& json) {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp/") + "aes67_regression_ptp_settings.json";
    std::ofstream(path) << json;
    setenv("AES67_PTP_CONFIG_PATH", path.c_str(), 1);
}

void noPtpSettings() { setenv("AES67_PTP_CONFIG_PATH", "/nonexistent/aes67-ptp.json", 1); }

void testPtpIsOffByDefault() {
    std::cout << "PTP is off without ptp.json" << std::endl;
    useEmptyConfig("ptpoff");
    noPtpSettings();
    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();
    CHECK(device->GetPtpClockSource() == nullptr, "no PTP clock source: nothing is sent");
    CHECK(device->GetClockIsStable() && device->GetClockDomain() == 0, "a stable clock in no domain, as before");
    usePtpSettings(R"({"enabled": false, "interface": "lo0"})");
    auto disabled = std::make_shared<AES67Device>(context);
    disabled->Initialize();
    CHECK(disabled->GetPtpClockSource() == nullptr, "nor with enabled false");
    noPtpSettings();
}

void testPtpTakesTheDeviceClock() {
    std::cout << "PTP on lo0: takes the device clock; a received stream no longer steers it" << std::endl;
    useEmptyConfig("ptp");  // the built-in RX stream on 239.1.1.1:5004
    usePtpSettings(R"({"enabled": true, "interface": "lo0"})");
    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();
    CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should start");
    CHECK(device->GetPtpClockSource() != nullptr, "a PTP clock source");
    CHECK(!device->GetClockIsStable() && device->GetClockDomain() == 0, "not stable, no domain, before a master");

    // A master on the real PTP ports, on lo0 only (TTL 0)
    PtpTest::ScriptedMaster::Settings settings;
    settings.eventPort = 319;
    settings.generalPort = 320;
    PtpTest::ScriptedMaster master(settings);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!device->GetClockIsStable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(device->GetClockIsStable(), "stable once the master is followed");
    CHECK(device->GetClockDomain() == PtpClockSource::clockDomainFor(master.identity().clock, 0),
          "in the grandmaster's clock domain");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint32_t generation = device->GetMediaClock().snapshot().generation;
    const double nominal = MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current());
    auto rateNow = [&] { return (device->GetMediaClock().snapshot().samplesPerTick / nominal - 1.0) * 1e6; };
    // Just after lock the servo's line still moves (on lo0 its slope by up to
    // ~25 ppm a refit, and its value now by up to ~200 us), and PTP slews each
    // move out at up to maxSlewPpm. So the clock is judged against the
    // servo's rate, within that slew; the margin allows for the rate and the
    // status being a step apart. The received stream moves it by hundreds
    auto servoRate = [&] {
        const auto estimate = device->GetPtpClockSource()->status().receiver.estimate;
        return estimate ? (estimate->rate - 1.0) * 1e6 : 0.0;
    };
    const double allowed = PtpClockControl::Config{}.maxSlewPpm + 25.0;
    CHECK(std::fabs(servoRate() - 100.0) < 50, "the servo at the master's +100 ppm (" << servoRate() << ")");
    CHECK(std::fabs(rateNow() - servoRate()) < allowed,
          "the clock at the servo's rate (" << rateNow() << " vs " << servoRate() << ")");

    // A sender 1500 ppm fast: the recovered clock would chase it; PTP must not let it
    LoopbackSender sender("239.1.1.1", 5004);
    AudioThreadPriority::configureForRealTime();
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e6 / 1.0015));
    auto next = std::chrono::steady_clock::now();
    double worst = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        sender.sendL24(static_cast<uint16_t>(i), i * kFramesPerPacket, 8, 0.5f);
        if (i % 100 == 0) worst = std::max(worst, std::fabs(rateNow() - servoRate()));
        std::this_thread::sleep_until(next += period);
    }
    AudioThreadPriority::restoreNormalPriority();
    CHECK(worst < allowed, "still at the servo's rate with a fast stream arriving (worst " << worst << " ppm off)");
    CHECK(device->GetMediaClock().snapshot().generation == generation, "and never a new timeline");

    // TX on PTP time: each packet's RTP timestamp (mediaclk offset 0) is the
    // master's time in samples at its first frame, so when it arrives the
    // master's time is about one packet on
    const auto grandmaster = device->GetStreamManager()->ptpGrandmaster();
    CHECK(grandmaster && grandmaster->identity == master.identity().clock.toString() && grandmaster->domain == 0,
          "TX streams are told the grandmaster to announce");
    RTP::RTPSocket capture;
    CHECK(capture.openReceiver("239.69.99.49", 55088), "capture socket should open");
    const StreamID tx =
        device->GetStreamManager()->createTxStream("PTP TX", "239.69.99.49", 55088, 2, makeMapping(2, 0), kHostOnly);
    CHECK(!tx.isNull(), "a TX stream");
    std::vector<int64_t> lag;  // master's time at arrival minus the packet's end, samples
    std::vector<uint8_t> buffer(2048);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < until) {
        RTP::RTPPacket packet;
        uint64_t arrival = 0;
        if (capture.receive(packet, buffer.data(), buffer.size(), &arrival) <= 0 || arrival == 0) continue;
        const MediaPosition ptp =
            ptpSamples(master.clockAt(HostTimebase::current().ticksToNanos(arrival)), 48000.0);
        lag.push_back(static_cast<int32_t>(static_cast<uint32_t>(ptp.sample) - (packet.header.timestamp + kFramesPerPacket)));
    }
    device->GetStreamManager()->removeStream(tx);
    capture.close();
    std::sort(lag.begin(), lag.end());
    const int64_t median = lag.empty() ? 1 << 30 : lag[lag.size() / 2];
    std::cout << "  " << lag.size() << " TX packets; master's time at arrival minus packet end: median " << median
              << " samples, range " << (lag.empty() ? 0 : lag.front()) << " to " << (lag.empty() ? 0 : lag.back())
              << std::endl;
    CHECK(lag.size() > 800, "TX packets arrive (" << lag.size() << ")");
    CHECK(!lag.empty() && lag.front() > -5 && median < 48,
          "stamped with the master's time: none early, most within a millisecond (median " << median << ")");
    device->StopIO(device->GetID(), 0);
    noPtpSettings();
}

// ---------------------------------------------------------------------------
// TX streams must reload as transmitters.
// ---------------------------------------------------------------------------
void testTxStreamSurvivesReload() {
    std::cout << "TX stream survives save/reload and transmits continuously" << std::endl;
    useEmptyConfig("txreload");

    constexpr const char* kGroup = "239.69.99.2";
    constexpr uint16_t kPort = 55010;
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};

    StreamID txID;
    {
        StreamManager manager(harness.context(), txContext, testSap());
        txID = manager.createTxStream("Regression TX", kGroup, kPort, 8, makeMapping(8, 8), kHostOnly);
        CHECK(!txID.isNull(), "TX stream should be created");
    }

    StreamManager reloaded(harness.context(), txContext, testSap());
    CHECK(reloaded.loadSavedStreams(), "saved TX stream should load");

    // AES67 senders send continuously: no Core Audio client is running here
    MulticastListener listener(kGroup, kPort);
    const size_t packets = listener.countPackets(std::chrono::milliseconds(200));
    CHECK(packets > 150, "a configured TX stream should transmit without IO (saw " << packets << " packets in 200 ms)");

    reloaded.removeAllStreams();
    const size_t afterRemoval = listener.countPackets(std::chrono::milliseconds(100));
    CHECK(afterRemoval <= 1, "a removed TX stream should stop (saw " << afterRemoval << " packets)");
}

// ---------------------------------------------------------------------------
// A TX stream's interface and TTL are part of its configuration: both must
// survive a save (saving used to drop every stream's interface).
// ---------------------------------------------------------------------------
void testTxOptionsAreSaved() {
    std::cout << "TX interface and TTL are saved" << std::endl;
    const std::string path = useEmptyConfig("txoptions");
    RxHarness harness;
    TxRouting txRouting;
    {
        StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
        const StreamID id = manager.createTxStream("Options TX", "239.69.99.6", 55026, 2, makeMapping(2, 0),
                                                   StreamManager::TxOptions{"127.0.0.1", 0});
        CHECK(!id.isNull(), "TX stream should be created");
    }
    std::ifstream file(path);
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(json.find("\"networkInterface\": \"127.0.0.1\"") != std::string::npos, "the TX stream's interface should be saved");
    CHECK(json.find("\"ttl\": 0") != std::string::npos, "the TX stream's TTL should be saved");
}

// ---------------------------------------------------------------------------
// A stream's interface setting is kept as written. Loading used to replace
// "lo0" or "en0" with its address at that moment, and the next save made the
// address permanent, so a later DHCP change broke the stream for good.
// ---------------------------------------------------------------------------
void testInterfaceSettingSurvivesReloadAndSave() {
    std::cout << "Interface settings are kept as written through reload and save" << std::endl;
    const std::string path = useEmptyConfig("ifacesetting");
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    {
        StreamManager manager(harness.context(), txContext, testSap());
        CHECK(!manager.createTxStream("Iface TX", "239.69.99.16", 55044, 2, makeMapping(2, 0),
                                      StreamManager::TxOptions{"lo0", 0}).isNull(),
              "a TX stream on lo0 should be created");
    }
    {
        StreamManager reloaded(harness.context(), txContext, testSap());
        CHECK(reloaded.loadSavedStreams(), "the TX stream should reload");
        // Adding a stream saves every stream's configuration again
        CHECK(!reloaded.addStream(makeRxSDP("239.69.99.17", 55046, 2), makeMapping(2, 0)).isNull(),
              "an RX stream should be added");
    }
    std::ifstream file(path);
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(json.find("\"networkInterface\": \"lo0\"") != std::string::npos,
          "the TX stream's interface should still read lo0");
    CHECK(json.find("127.0.0.1") == std::string::npos, "no interface address should have been written in its place");
}

// ---------------------------------------------------------------------------
// When a stream's interface changes (new address, link down/up, replugged
// adapter), its streams rejoin and resend on it as it is now, so a DHCP
// renewal or Wi-Fi roam no longer stops them until coreaudiod restarts. The
// interface state is scripted; the restarted streams use lo0 for real.
// ---------------------------------------------------------------------------
void testStreamsRestartWhenTheirInterfaceChanges() {
    std::cout << "Streams rejoin when their interface changes" << std::endl;
    useEmptyConfig("ifacechange");
    std::mutex stateMutex;
    auto loopback = NetworkInterfaceDetection::currentState("lo0");
    CHECK(loopback.has_value(), "lo0 should exist");
    NetworkMonitor::Config network;
    network.interval = std::chrono::milliseconds(30);
    network.provider = [&](const std::string& setting) -> std::optional<NetworkInterfaceDetection::InterfaceState> {
        std::lock_guard<std::mutex> lock(stateMutex);
        return setting == "lo0" ? loopback : NetworkInterfaceDetection::currentState(setting);
    };

    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap(), network);
    CHECK(!manager.createTxStream("Net TX", "239.69.99.18", 55048, 2, makeMapping(2, 0),
                                  StreamManager::TxOptions{"lo0", 0}).isNull(),
          "a TX stream on lo0 should be created");
    CHECK(!manager.createTxStream("Other TX", "239.69.99.19", 55050, 2, makeMapping(2, 2), kHostOnly).isNull(),
          "a TX stream on the default interface should be created");
    const StreamID rx = manager.addStream(makeRxSDP("239.69.99.18", 55048, 2), makeMapping(2, 0), "lo0");
    CHECK(!rx.isNull(), "an RX stream on lo0 should be added");
    manager.setIOActive(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(manager.getNetworkRestartCount() == 0, "nothing restarts while nothing changes");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        loopback->ipv4 = "127.0.0.2";  // as if DHCP had renewed with a new address
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(manager.getNetworkRestartCount() == 2, "the RX and TX streams on lo0 restart, the other TX does not (got "
                                                     << manager.getNetworkRestartCount() << ")");

    // Both restarted streams work: the RX on lo0 still hears the TX on lo0
    const auto before = manager.getStreamStatistics(rx);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto after = manager.getStreamStatistics(rx);
    CHECK(before && after && after->packetsReceived > before->packetsReceived + 100,
          "after restarting, the RX on lo0 should still receive the TX on lo0");
    manager.setIOActive(false);
}

// ---------------------------------------------------------------------------
// A fractional packet time (#22) is kept through save and reload, and
// StreamInfo reports it in microseconds as documented.
// ---------------------------------------------------------------------------
void testFractionalPtimeSurvivesReload() {
    std::cout << "Fractional packet times survive save and reload" << std::endl;
    const std::string path = useEmptyConfig("ptime");
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    SDPSession sdp = makeRxSDP("239.69.99.20", 55052, 2);
    sdp.ptime = 0.125;
    sdp.framecount = 6;
    StreamID id;
    {
        StreamManager manager(harness.context(), txContext, testSap());
        id = manager.addStream(sdp, makeMapping(2, 0));
        CHECK(!id.isNull(), "a 125 us stream should be added");
        const auto info = manager.getStreamInfo(id);
        CHECK(info && info->ptime == 125, "StreamInfo::ptime is in microseconds (got " << (info ? info->ptime : 0) << ")");
    }
    std::ifstream file(path);
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(json.find("\"ptime\": 0.125") != std::string::npos, "the saved config should keep 0.125 ms");

    StreamManager reloaded(harness.context(), txContext, testSap());
    CHECK(reloaded.loadSavedStreams(), "the stream should reload");
    const auto reloadedInfo = reloaded.getStreamInfo(id);
    CHECK(reloadedInfo && reloadedInfo->ptime == 125, "the reloaded stream should still be 125 us");
}

// ---------------------------------------------------------------------------
// SAP: a TX stream is announced while it is configured, with an SDP a
// receiver can subscribe from, and deleted when it goes. Its session ID is
// unique and survives a reload, so receivers do not list it twice.
// ---------------------------------------------------------------------------
std::vector<SDPSession> announcedSessions(const std::vector<Received>& packets, const std::string& name) {
    std::vector<SDPSession> sessions;
    for (const auto& p : packets) {
        if (isDeletion(p)) continue;
        const auto sap = SAPListener::parseAnnouncement(reinterpret_cast<const char*>(p.bytes.data()), p.bytes.size(), "");
        auto sdp = SDPParser::parseString(sap.sessionDescription);
        if (sdp && sdp->sessionName == name) sessions.push_back(*sdp);
    }
    return sessions;
}

void testTxStreamIsAnnounced() {
    std::cout << "TX streams are announced over SAP while configured" << std::endl;
    useEmptyConfig("sap");
    const std::string address = NetworkInterfaceDetection::resolveIPv4Address("");
    if (address.empty()) {
        std::cout << "  skipped: no network interface" << std::endl;
        return;
    }
    SapCapture capture("239.255.255.255", kTestSapPort, address);
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    size_t deletionsWhileRunning = 0;
    size_t announcementsOfBAfterRemoval = 0;
    {
        StreamManager manager(harness.context(), txContext, testSap());
        CHECK(!manager.createTxStream("SAP TX A", "239.69.99.7", 55028, 2, makeMapping(2, 0), kHostOnly).isNull(),
              "TX stream A should be created");
        const StreamID idB = manager.createTxStream("SAP TX B", "239.69.99.8", 55030, 2, makeMapping(2, 2), kHostOnly);
        CHECK(!idB.isNull(), "TX stream B should be created");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Removing a stream withdraws it at once, while the manager runs
        const size_t before = capture.packets().size();
        manager.removeStream(idB);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // over two re-announce intervals
        const auto during = capture.packets();
        for (size_t i = before; i < during.size(); ++i) {
            deletionsWhileRunning += isDeletion(during[i]) ? 1 : 0;
        }
        announcementsOfBAfterRemoval =
            announcedSessions(std::vector<Received>(during.begin() + static_cast<ptrdiff_t>(before), during.end()), "SAP TX B").size();
    }
    CHECK(deletionsWhileRunning == 1, "removing a stream should send its deletion (got " << deletionsWhileRunning << ")");
    CHECK(announcementsOfBAfterRemoval == 0, "a removed stream should no longer be announced");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto first = capture.packets();

    const auto a = announcedSessions(first, "SAP TX A");
    const auto b = announcedSessions(first, "SAP TX B");
    CHECK(!a.empty() && !b.empty(), "both streams should be announced");
    if (!a.empty() && !b.empty()) {
        const SDPSession& sdp = a.front();
        CHECK(sdp.connectionAddress == "239.69.99.7" && sdp.port == 55028, "the announcement carries the stream's group and port");
        CHECK(sdp.direction == "recvonly", "receivers are told to receive (a=recvonly)");
        CHECK(sdp.originNetworkType == "IN" && sdp.originAddressType == "IP4" && sdp.originAddress == address,
              "o= names this Mac's address (got " << sdp.originAddress << ")");
        CHECK(sdp.framecount == 48 && sdp.ptime == 1, "1 ms packets of 48 frames at 48 kHz");
        CHECK(sdp.ptpDomain == -1, "no PTP reference is claimed without PTP");
        const auto refclk = sdp.customAttributes.find("ts-refclk");
        CHECK(refclk != sdp.customAttributes.end() && refclk->second.rfind("localmac=", 0) == 0,
              "the reference clock is signalled as this Mac's own (ts-refclk:localmac=)");
        CHECK(a.front().sessionID != b.front().sessionID && a.front().sessionID != 0,
              "each stream has its own session ID");
    }
    size_t deletions = 0;
    for (const auto& p : first) deletions += isDeletion(p) ? 1 : 0;
    CHECK(deletions == 2, "each stream is deleted exactly once (got " << deletions << ")");

    {
        StreamManager reloaded(harness.context(), txContext, testSap());
        CHECK(reloaded.loadSavedStreams(), "saved TX streams should load");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const auto all = capture.packets();
    const auto reloadedA = announcedSessions(std::vector<Received>(all.begin() + static_cast<ptrdiff_t>(first.size()), all.end()),
                                             "SAP TX A");
    CHECK(!reloadedA.empty() && !a.empty() && reloadedA.front().sessionID == a.front().sessionID,
          "a reloaded stream keeps its session ID");
}

// With PTP followed, TX streams name the grandmaster (ts-refclk:ptp, in the
// AES67 form Dante and RAVENNA use) and re-announce with a new version when
// it changes. Until then they keep ts-refclk:localmac.
std::vector<std::string> announcedTexts(const std::vector<Received>& packets, const std::string& name) {
    std::vector<std::string> texts;
    for (const auto& p : packets) {
        if (isDeletion(p)) continue;
        const auto sap = SAPListener::parseAnnouncement(reinterpret_cast<const char*>(p.bytes.data()), p.bytes.size(), "");
        auto sdp = SDPParser::parseString(sap.sessionDescription);
        if (sdp && sdp->sessionName == name) texts.push_back(sap.sessionDescription);
    }
    return texts;
}

void testTxAnnouncesThePtpGrandmaster() {
    std::cout << "TX streams announce the PTP grandmaster once it is followed, re-announcing when it changes" << std::endl;
    useEmptyConfig("sapptp");
    const std::string address = NetworkInterfaceDetection::resolveIPv4Address("");
    if (address.empty()) {
        std::cout << "  skipped: no network interface" << std::endl;
        return;
    }
    SapCapture capture("239.255.255.255", kTestSapPort, address);
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamManager manager(harness.context(), txContext, testSap());
    CHECK(!manager.createTxStream("SAP PTP", "239.69.99.9", 55032, 2, makeMapping(2, 0), kHostOnly).isNull(),
          "TX stream should be created");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const size_t beforePtp = capture.packets().size();
    const auto local = announcedSessions(capture.packets(), "SAP PTP");
    CHECK(!local.empty() && local.back().ptpDomain == -1, "before PTP: no PTP reference");

    manager.setPtpGrandmaster(StreamManager::PtpGrandmaster{"00-1D-C1-FF-FE-12-34-56", 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto packets = capture.packets();
    const std::vector<Received> afterFirst(packets.begin() + static_cast<ptrdiff_t>(beforePtp), packets.end());
    const auto texts = announcedTexts(afterFirst, "SAP PTP");
    const auto sessions = announcedSessions(afterFirst, "SAP PTP");
    CHECK(!texts.empty() && texts.back().find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n") !=
                                std::string::npos,
          "names the grandmaster and domain, AES67 form");
    CHECK(!texts.empty() && texts.back().find("localmac") == std::string::npos, "and no longer localmac");
    CHECK(!texts.empty() && texts.back().find("a=mediaclk:direct=0") != std::string::npos,
          "with the mediaclk offset its RTP timestamps use");
    CHECK(!sessions.empty() && !local.empty() && sessions.back().sessionVersion > local.back().sessionVersion &&
              sessions.back().sessionID == local.back().sessionID,
          "same session, new version");
    size_t deletions = 0;
    for (const auto& p : afterFirst) deletions += isDeletion(p) ? 1 : 0;
    CHECK(deletions == 1, "the old description is withdrawn (" << deletions << ")");

    // The same grandmaster again (lock regained): nothing to re-announce
    const size_t beforeSame = capture.packets().size();
    manager.setPtpGrandmaster(StreamManager::PtpGrandmaster{"00-1D-C1-FF-FE-12-34-56", 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    packets = capture.packets();
    size_t sameDeletions = 0;
    for (size_t i = beforeSame; i < packets.size(); ++i) sameDeletions += isDeletion(packets[i]) ? 1 : 0;
    CHECK(sameDeletions == 0, "the same grandmaster changes nothing");

    // Another grandmaster: another version
    const size_t beforeNew = capture.packets().size();
    manager.setPtpGrandmaster(StreamManager::PtpGrandmaster{"AC-DE-48-FF-FE-00-11-22", 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    packets = capture.packets();
    const std::vector<Received> afterNew(packets.begin() + static_cast<ptrdiff_t>(beforeNew), packets.end());
    const auto newTexts = announcedTexts(afterNew, "SAP PTP");
    const auto newSessions = announcedSessions(afterNew, "SAP PTP");
    CHECK(!newTexts.empty() && newTexts.back().find("IEEE1588-2008:AC-DE-48-FF-FE-00-11-22:0") != std::string::npos,
          "names the new grandmaster");
    CHECK(!newSessions.empty() && !sessions.empty() && newSessions.back().sessionVersion > sessions.back().sessionVersion,
          "with a newer version");
}

// ---------------------------------------------------------------------------
// Input and output channels are separate: a Core Audio device has 128 of
// each, in different buffers, so RX on inputs 1-2 and TX from outputs 1-2 is
// the ordinary setup, not an overlap.
// ---------------------------------------------------------------------------
void testInputAndOutputChannelsAreSeparate() {
    std::cout << "Input and output channels are allocated separately" << std::endl;
    useEmptyConfig("iochannels");
    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
    using Direction = StreamManager::Direction;

    const StreamID rx = manager.addStream(makeRxSDP("239.69.99.10", 55032, 2), makeMapping(2, 0));
    CHECK(!rx.isNull(), "RX on input channels 0-1 should be added");
    const StreamID tx = manager.createTxStream("IO TX", "239.69.99.11", 55034, 2, makeMapping(2, 0), kHostOnly);
    CHECK(!tx.isNull(), "TX from output channels 0-1 should not clash with RX on input channels 0-1");
    CHECK(manager.getAvailableChannelCount(Direction::Receive) == 126, "2 input channels in use");
    CHECK(manager.getAvailableChannelCount(Direction::Transmit) == 126, "2 output channels in use");

    CHECK(manager.createTxStream("IO TX 2", "239.69.99.12", 55036, 2, makeMapping(2, 1), kHostOnly).isNull(),
          "a second TX overlapping output channels 0-1 should be refused");
    CHECK(manager.addStream(makeRxSDP("239.69.99.13", 55038, 2), makeMapping(2, 1)).isNull(),
          "a second RX overlapping input channels 0-1 should be refused");

    CHECK(manager.getAllMappings(Direction::Receive).size() == 1 && manager.getAllMappings(Direction::Transmit).size() == 1,
          "each direction lists its own mappings");
    CHECK(manager.getMapping(tx).has_value() && manager.getMapping(rx).has_value(), "both streams' mappings are found");

    manager.removeStream(tx);
    CHECK(manager.getAvailableChannelCount(Direction::Transmit) == 128, "removing the TX frees its output channels");
    CHECK(manager.getAvailableChannelCount(Direction::Receive) == 126, "and leaves the inputs alone");
}

// ---------------------------------------------------------------------------
// A sender's SDP may describe its own role (a=sendonly or sendrecv). Added as
// a receive stream it must stay one: on input channels, and still a receiver
// (not a transmitter) after a reload.
// ---------------------------------------------------------------------------
void testSenderSdpDirectionDoesNotMakeATransmitter() {
    std::cout << "A received stream stays a receiver whatever its SDP's direction" << std::endl;
    useEmptyConfig("rxdirection");
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    using Direction = StreamManager::Direction;

    SDPSession senderSdp = makeRxSDP("239.69.99.14", 55040, 2);
    senderSdp.direction = "sendonly";  // as the sender sees it
    {
        StreamManager manager(harness.context(), txContext, testSap());
        CHECK(!manager.addStream(senderSdp, makeMapping(2, 0)).isNull(), "the RX stream should be added");
        CHECK(manager.getAvailableChannelCount(Direction::Receive) == 126 &&
                  manager.getAvailableChannelCount(Direction::Transmit) == 128,
              "it should take input channels, not output channels");
    }
    StreamManager reloaded(harness.context(), txContext, testSap());
    CHECK(reloaded.loadSavedStreams(), "the RX stream should reload");
    CHECK(reloaded.getAvailableChannelCount(Direction::Receive) == 126 &&
              reloaded.getAvailableChannelCount(Direction::Transmit) == 128,
          "after a reload it should still be a receive stream");
    MulticastListener listener("239.69.99.14", 55040);
    CHECK(listener.countPackets(std::chrono::milliseconds(100)) == 0, "nothing should be transmitted to its group");
}

// ---------------------------------------------------------------------------
// Timeline restarts (IO start, sample rate change) never reuse media
// positions, so no buffer slot written on an old timeline can be read as
// current on the new one (TX would replay old output).
// ---------------------------------------------------------------------------
void testTimelineRestartNeverReusesPositions() {
    std::cout << "Timeline restarts never reuse media positions" << std::endl;
    useEmptyConfig("monotonic");
    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    int64_t previous = device->GetMediaClock().snapshot().sampleAt(hostTimeNow());
    bool increasing = true;
    for (int i = 0; i < 3; ++i) {
        const uint32_t generation = device->GetMediaClock().snapshot().generation;
        CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should start");
        const auto clock = device->GetMediaClock().snapshot();
        CHECK(clock.generation != generation, "IO start should begin a new timeline");
        const int64_t now = clock.sampleAt(hostTimeNow());
        increasing = increasing && now > previous + 16384;
        previous = now;
        device->StopIO(device->GetID(), 0);
    }
    CHECK(increasing, "each new timeline should start beyond every position the previous one could have used");
}

// ---------------------------------------------------------------------------
// With no configuration the device creates its RX test stream only: a TX test
// stream would now transmit (and be announced) continuously on every install.
// ---------------------------------------------------------------------------
void testNoDefaultTxStream() {
    std::cout << "No built-in TX stream without a configuration" << std::endl;
    useEmptyConfig("defaults");
    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();
    MulticastListener listener("239.1.1.2", 5004);
    CHECK(listener.countPackets(std::chrono::milliseconds(100)) == 0, "nothing should be sent to the old TX test group");
    CHECK(device->GetStreamManager()->getStreamCount() == 1, "only the RX test stream should exist (got "
                                                                  << device->GetStreamManager()->getStreamCount() << ")");
}

// ---------------------------------------------------------------------------
// Receive path (step 2 phase 2): audio is placed by RTP timestamp. Tests
// judge the stream as heard, from its first to its last non-silent sample.
// ---------------------------------------------------------------------------
void testReceiverFollowsSenderRestart() {
    std::cout << "RX follows a sender restart" << std::endl;

    RTPReceiver::PlacementStatistics stats{};
    const auto samples = receiveAndListen("239.69.99.3", 55012, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 1000, 0, 40, 0.25f, 0x12345678, kNoSkip);
        // Sender restarts with a new SSRC and unrelated sequence and timestamp bases
        sendRun(sender, next, 40000, 900000, 200, -0.5f, 0x0BADF00D, kNoSkip);
    }, &stats);

    const size_t restarted = countNear(samples, -0.5f);
    CHECK(restarted >= 150 * kFramesPerPacket,
          "audio from the restarted sender should play promptly (got " << restarted / kFramesPerPacket << " packets)");
    CHECK(stats.reanchors >= 1, "the restart should re-anchor");
}

void testLostPacketKeepsTimeline() {
    std::cout << "RX lost packet leaves exactly its own gap" << std::endl;

    constexpr uint16_t kPackets = 30;
    RTPReceiver::PlacementStatistics stats{};
    const auto region = heardRegion(receiveAndListen("239.69.99.4", 55014, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 2000, 0, kPackets, 0.25f, 0x12345678, [](uint16_t i) { return i == 15; });
    }, &stats));

    CHECK(countNear(region, 0.25f) == (kPackets - 1) * kFramesPerPacket,
          "every received packet should be played (late drops: " << stats.lateDrops << ")");
    CHECK(region.size() == kPackets * kFramesPerPacket,
          "the stream should keep its length (got " << region.size() << " samples, want " << kPackets * kFramesPerPacket << ")");
    CHECK(countNear(region, 0.0f) == kFramesPerPacket, "the lost packet should leave exactly one packet of silence");
}

void testLatePacketIsIgnored() {
    std::cout << "RX ignores a single very late packet" << std::endl;

    constexpr uint16_t kPackets = 60;
    const auto region = heardRegion(receiveAndListen("239.69.99.5", 55016, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        for (uint16_t i = 0; i < kPackets; ++i) {
            sender.sendL24(static_cast<uint16_t>(3000 + i), 400 * kFramesPerPacket + i * kFramesPerPacket, 2, 0.25f);
            if (i == 30) {
                sender.sendL24(static_cast<uint16_t>(3000 - 400), 0, 2, 0.9f);  // 400 packets late
            }
            std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
        }
    }));

    CHECK(countNear(region, 0.9f) == 0, "the late packet should not be played");
    CHECK(countNear(region, 0.25f) == kPackets * kFramesPerPacket, "every in-order packet should still be played");
    CHECK(countNear(region, 0.0f) == 0, "a late packet should not cause silence");
}

void testInterleavedSecondSourceIsIgnored() {
    std::cout << "RX ignores an interleaved second source" << std::endl;

    constexpr uint16_t kPackets = 100;
    const auto region = heardRegion(receiveAndListen("239.69.99.6", 55018, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        for (uint16_t i = 0; i < kPackets; ++i) {
            sender.sendL24(static_cast<uint16_t>(5000 + i), i * kFramesPerPacket, 2, 0.25f, 0xAAAA0001);
            sender.sendL24(static_cast<uint16_t>(20000 + i), 777 + i * kFramesPerPacket, 2, 0.9f, 0xBBBB0002);
            std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
        }
    }));

    CHECK(countNear(region, 0.9f) == 0, "the second source should never be played");
    CHECK(countNear(region, 0.25f) == kPackets * kFramesPerPacket,
          "the first source should play uninterrupted (got " << countNear(region, 0.25f) / kFramesPerPacket << " packets)");
}

void runRestartBehindPlayout(const char* group, uint16_t port, uint32_t restartSsrc, const char* label) {
    const auto samples = receiveAndListen(group, port, [restartSsrc](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 6000, 200 * kFramesPerPacket, 60, 0.25f, 0x12345678, kNoSkip);
        // Restart with timestamps ~150 packets behind where playout has reached
        sendRun(sender, next, 5900, 110 * kFramesPerPacket, 150, -0.5f, restartSsrc, kNoSkip);
    });

    CHECK(countNear(samples, -0.5f) >= 100 * kFramesPerPacket,
          label << ": the restarted stream should play (got " << countNear(samples, -0.5f) / kFramesPerPacket
                << " of 150 packets)");
}

void testRestartBehindPlayoutIsFollowed() {
    std::cout << "RX follows a restart that lands behind playout" << std::endl;
    runRestartBehindPlayout("239.69.99.7", 55020, 0x12345678, "same SSRC");
    runRestartBehindPlayout("239.69.99.8", 55022, 0x0BADF00D, "new SSRC");
}

void testOutageKeepsTimeline() {
    std::cout << "RX outage plays as its own length of silence" << std::endl;

    constexpr uint16_t kOutagePackets = 40;
    RTPReceiver::PlacementStatistics stats{};
    const auto region = heardRegion(receiveAndListen("239.69.99.9", 55024, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        // Packets 30..69 are lost in a 40 ms outage
        sendRun(sender, next, 7000, 0, 100, 0.25f, 0x12345678,
                [](uint16_t i) { return i >= 30 && i < 30 + kOutagePackets; });
    }, &stats));

    CHECK(countNear(region, 0.25f) == (100 - kOutagePackets) * kFramesPerPacket,
          "every received packet should play (late drops: " << stats.lateDrops << ")");
    CHECK(region.size() == 100 * kFramesPerPacket,
          "the outage must not add latency: the stream should keep its length (got " << region.size() << " samples)");
    CHECK(countNear(region, 0.0f) == kOutagePackets * kFramesPerPacket, "the outage should be exactly its own length of silence");
}


// ---------------------------------------------------------------------------
// kAudioStreamPropertyIsActive belongs to the HAL: it activates the streams
// its clients use. The device must not change it on IO start/stop, because
// libASPL's Stream::SetIsActive() sends a property-change notification, and
// AudioServerPlugIn.h forbids devices from notifying changes that affect IO.
// ---------------------------------------------------------------------------
void testIOStartStopLeavesStreamActivityToHAL() {
    std::cout << "IO start/stop leaves stream activity to the HAL" << std::endl;
    useEmptyConfig("streamactive");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    // The HAL deactivates a stream no client uses (here: the output)
    device->GetOutputStream()->SetIsActive(false);

    device->StartIO(device->GetID(), 0);
    CHECK(!device->GetOutputStream()->GetIsActive(), "starting IO must not reactivate a stream the HAL deactivated");
    CHECK(device->GetInputStream()->GetIsActive(), "starting IO must leave the input stream as the HAL set it");

    device->StopIO(device->GetID(), 0);
    CHECK(device->GetInputStream()->GetIsActive(), "stopping IO must not deactivate streams (the HAL owns that property)");
}

} // namespace

// ---------------------------------------------------------------------------
// configureForRealTime must report success when the real-time policies are
// applied. It used to return the result of THREAD_AFFINITY_POLICY, which Apple
// Silicon never supports, so every receive thread logged a false failure.
// ---------------------------------------------------------------------------
void testRealTimePriorityReportsSuccess() {
    std::cout << "Real-time thread priority reports success when applied" << std::endl;
    bool reported = false;
    boolean_t getDefault = FALSE;
    thread_extended_policy_data_t extended{TRUE};
    mach_msg_type_number_t count = THREAD_EXTENDED_POLICY_COUNT;
    std::thread([&] {
        reported = AudioThreadPriority::configureForRealTime();
        thread_policy_get(mach_thread_self(), THREAD_EXTENDED_POLICY,
                          reinterpret_cast<thread_policy_t>(&extended), &count, &getDefault);
    }).join();
    CHECK(!extended.timeshare, "the thread should have left time-sharing");
    CHECK(reported, "configureForRealTime should report the success");
}

int main() {
    // No test may pick up a real ptp.json: PTP only where a test asks for it
    noPtpSettings();
    testMixedOutputReachesTxBuffers();
    testInputReadsByDeviceTime();
    testDeviceSampleRate();
    testDeviceClockFromMediaClock();
    testRealTimePriorityReportsSuccess();
    testIOStartStopLeavesStreamActivityToHAL();
    testDeviceClockFollowsReceivedStream();
    testPtpIsOffByDefault();
    testPtpTakesTheDeviceClock();
    testTxOptionsAreSaved();
    testInterfaceSettingSurvivesReloadAndSave();
    testFractionalPtimeSurvivesReload();
    testStreamsRestartWhenTheirInterfaceChanges();
    testInputAndOutputChannelsAreSeparate();
    testSenderSdpDirectionDoesNotMakeATransmitter();
    testTxStreamIsAnnounced();
    testTxAnnouncesThePtpGrandmaster();
    testTimelineRestartNeverReusesPositions();
    testNoDefaultTxStream();
    testTxStreamSurvivesReload();
    testReceiverFollowsSenderRestart();
    testLostPacketKeepsTimeline();
    testLatePacketIsIgnored();
    testInterleavedSecondSourceIsIgnored();
    testRestartBehindPlayoutIsFollowed();
    testOutageKeepsTimeline();

    std::cout << "\nCritical path regressions: " << checksPassed << " passed, "
              << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
