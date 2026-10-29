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
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

// Point the driver's config search at a file that doesn't exist: a device
// then starts with its built-in RX test stream on 239.1.1.1:5004.
std::string useNoConfig(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp ? tmp : "/tmp/") + "aes67_regression_" + name + ".json";
    std::remove(path.c_str());
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
    useNoConfig("device");

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
    useNoConfig("recovered");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();  // no config: the built-in RX stream on 239.1.1.1:5004, 8 ch
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
    useNoConfig("ptp");  // the built-in RX stream on 239.1.1.1:5004
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
// Restarting the streams on a changed interface used to hold the streams lock
// for every stream at once, so StartIO waited out all the thread joins (#71).
// Each stream now restarts under the lock on its own.
// ---------------------------------------------------------------------------
void testInterfaceRestartLetsOthersIn() {
    std::cout << "#71: restarting streams on an interface change doesn't hold the streams lock throughout" << std::endl;
    useEmptyConfig("ifacelock");
    std::mutex stateMutex;
    auto loopback = NetworkInterfaceDetection::currentState("lo0");
    CHECK(loopback.has_value(), "lo0 should exist");
    NetworkMonitor::Config network;
    network.interval = std::chrono::milliseconds(20);
    network.provider = [&](const std::string& setting) -> std::optional<NetworkInterfaceDetection::InterfaceState> {
        std::lock_guard<std::mutex> lock(stateMutex);
        return setting == "lo0" ? loopback : NetworkInterfaceDetection::currentState(setting);
    };

    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap(), network);
    constexpr int kStreams = 16;
    for (int i = 0; i < kStreams; ++i) {
        const std::string group = "239.69.99." + std::to_string(120 + i);
        CHECK(!manager.addStream(makeRxSDP(group.c_str(), static_cast<uint16_t>(55150 + 2 * i), 2),
                                 makeMapping(2, static_cast<uint16_t>(2 * i)), "lo0").isNull(),
              "RX stream " << i << " should be added");
    }
    manager.setIOActive(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Time a cheap locked call over and over while the streams restart
    using Clock = std::chrono::steady_clock;
    std::atomic<bool> probing{true};
    Clock::duration longestWait{};
    std::thread probe([&] {
        while (probing.load()) {
            const auto start = Clock::now();
            manager.getStreamCount();
            longestWait = std::max(longestWait, Clock::now() - start);
        }
    });
    const auto changed = Clock::now();
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        loopback->ipv4 = "127.0.0.2";
    }
    // From the first stream restarted to the last: the monitor's polling delay doesn't count
    std::optional<Clock::time_point> firstRestarted;
    while (manager.getNetworkRestartCount() < kStreams && Clock::now() - changed < std::chrono::seconds(5)) {
        if (!firstRestarted && manager.getNetworkRestartCount() > 0) firstRestarted = Clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const auto restartTook = Clock::now() - firstRestarted.value_or(changed);
    probing = false;
    probe.join();
    manager.setIOActive(false);

    const auto us = [](Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    };
    CHECK(manager.getNetworkRestartCount() == kStreams, "every stream restarts");
    CHECK(longestWait * 4 < restartTook, "the longest wait for the lock (" << us(longestWait)
                                             << " us) is a fraction of the whole restart (" << us(restartTook)
                                             << " us)");
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
    useNoConfig("defaults");
    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();
    MulticastListener listener("239.1.1.2", 5004);
    CHECK(listener.countPackets(std::chrono::milliseconds(100)) == 0, "nothing should be sent to the old TX test group");
    CHECK(device->GetStreamManager()->getStreamCount() == 1, "only the RX test stream should exist (got "
                                                                  << device->GetStreamManager()->getStreamCount() << ")");
}

// ---------------------------------------------------------------------------
// streams.json entries the driver doesn't load (#63, #64, #66) stay in the
// file: disabled or unreadable entries are written back on every save, and a
// config whose streams all fail to load is never replaced by the test stream.
// ---------------------------------------------------------------------------
std::string savedEntry(const SDPSession& sdp, const ChannelMapping& mapping, bool enabled) {
    PersistedStreamConfig config = StreamConfigManager::createConfig(sdp, mapping);
    config.enabled = enabled;
    return StreamConfigManager::configToJSON(config).dump();
}

void writeStreamsFile(const std::string& path, const std::vector<std::string>& entries) {
    std::string json = "{\"version\": \"1.0\", \"streams\": [";
    for (size_t i = 0; i < entries.size(); ++i) json += (i ? ", " : "") + entries[i];
    std::ofstream(path) << json << "]}";
}

// `text` with the first occurrence of `from` replaced by `to`.
std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

std::string readText(const std::string& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void testUnloadedStreamsStayInTheConfig() {
    std::cout << "#64: disabled and unreadable streams stay in streams.json across saves" << std::endl;
    const std::string path = useEmptyConfig("keptentries");
    SDPSession enabledSdp = makeRxSDP("239.69.99.40", 55070, 2);
    enabledSdp.sessionName = "Enabled One";
    SDPSession disabledSdp = makeRxSDP("239.69.99.41", 55071, 2);
    disabledSdp.sessionName = "Disabled One";
    std::string unreadable = savedEntry(makeRxSDP("239.69.99.42", 55072, 2), makeMapping(2, 4), true);
    const size_t domain = unreadable.find("\"ptpDomain\": 0");
    unreadable.replace(domain, std::strlen("\"ptpDomain\": 0"), "\"ptpDomain\": 99999999999");
    writeStreamsFile(path, {savedEntry(enabledSdp, makeMapping(2, 0), true),
                            savedEntry(disabledSdp, makeMapping(2, 2), false), unreadable});

    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamManager manager(harness.context(), txContext, testSap());
    CHECK(manager.loadSavedStreams(), "the enabled stream should load");
    CHECK(manager.getStreamCount() == 1, "only the enabled stream should run (got " << manager.getStreamCount() << ")");

    SDPSession added = makeRxSDP("239.69.99.43", 55073, 2);
    added.sessionName = "Added Later";
    const StreamID addedID = manager.addStream(added, makeMapping(2, 6));
    CHECK(!addedID.isNull(), "a stream should be added (and saved)");
    std::string json = readText(path);
    CHECK(json.find("Added Later") != std::string::npos && json.find("Enabled One") != std::string::npos,
          "the running streams should be saved");
    CHECK(json.find("Disabled One") != std::string::npos, "the disabled stream should survive the save");
    CHECK(json.find("99999999999") != std::string::npos, "the unreadable entry should survive the save");

    CHECK(manager.removeStream(addedID), "the added stream should be removed (and saved)");
    json = readText(path);
    CHECK(json.find("Added Later") == std::string::npos, "the removed stream should be gone from the file");
    CHECK(json.find("Disabled One") != std::string::npos && json.find("99999999999") != std::string::npos,
          "the entries not loaded should survive a removal too");

    const auto reloaded = StreamConfigManager::fromJSON(json);
    size_t disabledCount = 0;
    for (const auto& entry : reloaded ? *reloaded : std::vector<SavedStreamEntry>{}) {
        if (entry.config && !entry.config->enabled) ++disabledCount;
    }
    CHECK(disabledCount == 1, "the disabled stream should still be disabled");
}

void testConfigWithNoLoadableStreamIsKept() {
    std::cout << "#63: a config whose streams don't load is not replaced by the test stream" << std::endl;
    const std::string path = useEmptyConfig("allunloadable");
    SDPSession a = makeRxSDP("239.69.99.44", 55074, 2);
    a.sessionName = "User Stream A";
    SDPSession b = makeRxSDP("239.69.99.45", 55075, 2);
    b.sessionName = "User Stream B";
    writeStreamsFile(path, {savedEntry(a, makeMapping(2, 0), false), savedEntry(b, makeMapping(2, 2), false)});
    const std::string before = readText(path);

    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        CHECK(device->GetStreamManager()->getStreamCount() == 0,
              "no stream should run (got " << device->GetStreamManager()->getStreamCount() << ")");
    }
    CHECK(readText(path) == before, "streams.json should be untouched");

    // An empty list is the user's choice too
    useEmptyConfig("allunloadable");
    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        CHECK(device->GetStreamManager()->getStreamCount() == 0, "an empty stream list should stay empty");
    }

    // A corrupt file is kept for the user to fix, not replaced
    std::ofstream(path) << "{\"streams\": [";
    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        CHECK(device->GetStreamManager()->getStreamCount() == 0, "a corrupt config should not get the test stream");
    }
    CHECK(readText(path) == "{\"streams\": [", "the corrupt file should still be there");
    CHECK(readText(path + ".bak") == "{\"streams\": [", "and copied to .bak");
    std::remove((path + ".bak").c_str());

    // A file the driver can't read (permissions, sandbox) is still the user's
    writeStreamsFile(path, {savedEntry(a, makeMapping(2, 0), true)});
    const std::string locked = readText(path);
    chmod(path.c_str(), 0000);
    size_t lockedCount = 0;
    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        lockedCount = device->GetStreamManager()->getStreamCount();
    }
    chmod(path.c_str(), 0644);
    if (geteuid() != 0) {
        CHECK(lockedCount == 0, "an unreadable config should not get the test stream");
        CHECK(readText(path) == locked, "and should not be saved over");
    }
}

// ---------------------------------------------------------------------------
// Behaviour changes in the streams.json rework, end to end.
// ---------------------------------------------------------------------------

// The streams in a saved file, parsed.
std::vector<SavedStreamEntry> savedEntries(const std::string& path) {
    auto entries = StreamConfigManager::fromJSON(readText(path));
    return entries ? *entries : std::vector<SavedStreamEntry>{};
}

void testConfigPathEnvironmentIsAuthoritative() {
    std::cout << "AES67_CONFIG_PATH is used even when it doesn't exist and a user config does" << std::endl;
    const char* tmp = std::getenv("TMPDIR");
    const std::string home = std::string(tmp ? tmp : "/tmp/") + "aes67_regression_home";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home + "/Library/Application Support/AES67Driver");
    const std::string userPath = home + "/Library/Application Support/AES67Driver/streams.json";
    SDPSession user = makeRxSDP("239.69.99.50", 55080, 2);
    user.sessionName = "User Config Stream";
    writeStreamsFile(userPath, {savedEntry(user, makeMapping(2, 0), true)});
    const std::string userBefore = readText(userPath);

    const char* realHome = std::getenv("HOME");
    const std::string savedHome = realHome ? realHome : "";
    setenv("HOME", home.c_str(), 1);

    const std::string envPath = useNoConfig("envpath");
    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        const auto streams = device->GetStreamManager()->getActiveStreams();
        CHECK(streams.size() == 1 && streams[0].name == "Test AES67 Stream",
              "a missing AES67_CONFIG_PATH file means no config: only the test stream");
    }
    CHECK(readText(userPath) == userBefore, "the user's config should be untouched");
    CHECK(readText(envPath).find("Test AES67 Stream") != std::string::npos, "the test stream should be saved at AES67_CONFIG_PATH");

    // Without the variable the search still finds the user's file
    unsetenv("AES67_CONFIG_PATH");
    {
        auto context = std::make_shared<aspl::Context>();
        auto device = std::make_shared<AES67Device>(context);
        device->Initialize();
        const auto streams = device->GetStreamManager()->getActiveStreams();
        CHECK(streams.size() == 1 && streams[0].name == "User Config Stream", "the user's config should load");
    }

    setenv("HOME", savedHome.c_str(), 1);
    std::filesystem::remove_all(home);
    std::remove(envPath.c_str());
}

void testOldFileSurvivesLoadAndSave() {
    std::cout << "A streams.json from the old writer keeps every field through load and save" << std::endl;
    const std::string path = useEmptyConfig("upgrade");
    // As the old writer wrote it: a raw control character in a name, uppercase UUID
    std::ofstream(path) << R"({
  "version": "1.0",
  "streams": [
    {
      "enabled": true,
      "description": "",
      "createdTimestamp": 1759400000,
      "modifiedTimestamp": 1759400000,
      "jitterBufferDepth": 0,
      "networkInterface": "lo0",
      "sdp": {
        "sessionName": "Old)" "\x01" R"(Name [with] {brackets}",
        "sessionInfo": "Old writer",
        "sessionID": 18446744073709551615,
        "sessionVersion": 9007199254740993,
        "originUsername": "riedel",
        "originAddress": "192.168.1.20",
        "connectionAddress": "239.69.99.60",
        "ttl": 0,
        "port": 55090,
        "payloadType": 98,
        "encoding": "L16",
        "sampleRate": 48000,
        "numChannels": 4,
        "ptime": 0.333,
        "framecount": 16,
        "sourceAddress": "192.168.1.20",
        "ptpDomain": -1,
        "ptpMasterMAC": "00-1D-C1-FF-FE-12-34-56",
        "mediaClockType": "direct",
        "direction": "recvonly"
      },
      "mapping": {
        "streamID": "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0",
        "streamName": "Old mapping",
        "streamChannelCount": 4,
        "streamChannelOffset": 0,
        "deviceChannelStart": 20,
        "deviceChannelCount": 4
      }
    }
  ]
})";
    const auto before = savedEntries(path);
    CHECK(before.size() == 1 && before[0].config, "the old file should parse");
    if (before.empty() || !before[0].config) return;

    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    {
        StreamManager manager(harness.context(), txContext, testSap());
        CHECK(manager.loadSavedStreams(), "the old stream should load");
        CHECK(manager.saveAllStreams(), "and save");
    }
    const auto after = savedEntries(path);
    CHECK(after.size() == 1 && after[0].config, "the saved file should parse");
    if (after.empty() || !after[0].config) return;

    const JsonValue was = StreamConfigManager::configToJSON(*before[0].config);
    const JsonValue now = StreamConfigManager::configToJSON(*after[0].config);
    CHECK(was.find("sdp")->dump() == now.find("sdp")->dump(),
          "every SDP field should survive\nwas " << was.find("sdp")->dump() << "\nnow " << now.find("sdp")->dump());
    CHECK(was.find("mapping")->dump() == now.find("mapping")->dump(),
          "every mapping field should survive\nwas " << was.find("mapping")->dump() << "\nnow " << now.find("mapping")->dump());
    CHECK(after[0].config->networkInterface == "lo0", "the interface setting should survive");
    CHECK(after[0].config->sdp.sessionID == UINT64_MAX && after[0].config->sdp.sessionVersion == 9007199254740993ull,
          "64-bit IDs above 2^53 should be exact");
}

void testStreamThatFailsToLoadIsKeptEnabled() {
    std::cout << "A stream that fails to load stays enabled in the file and loads once it can" << std::endl;
    const std::string path = useEmptyConfig("failedload");
    SDPSession first = makeRxSDP("239.69.99.61", 55091, 2);
    first.sessionName = "First";
    SDPSession clash = makeRxSDP("239.69.99.62", 55092, 2);
    clash.sessionName = "Clashing";
    // Both want device channels 0-1: the second can't be mapped
    const std::string clashing = replaced(savedEntry(clash, makeMapping(2, 0), true), "\"enabled\": true",
                                          "\"enabled\": true, \"futureField\": {\"x\": [1, 2]}");
    writeStreamsFile(path, {savedEntry(first, makeMapping(2, 0), true), clashing});

    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamID firstID;
    {
        StreamManager manager(harness.context(), txContext, testSap());
        CHECK(manager.loadSavedStreams(), "the first stream should load");
        const auto streams = manager.getActiveStreams();
        CHECK(streams.size() == 1 && streams[0].name == "First", "only the first stream should run");
        if (!streams.empty()) firstID = streams[0].id;
        CHECK(manager.removeStream(firstID), "removing the first stream frees its channels (and saves)");
    }
    const auto entries = savedEntries(path);
    CHECK(entries.size() == 1 && entries[0].config && entries[0].config->enabled &&
              entries[0].config->sdp.sessionName == "Clashing",
          "the clashing stream should still be saved, enabled");
    CHECK(readText(path).find("futureField") != std::string::npos, "with fields this version doesn't know");

    StreamManager reloaded(harness.context(), txContext, testSap());
    CHECK(reloaded.loadSavedStreams() && reloaded.getStreamCount() == 1, "with its channels free it should load now");
}

// #76: saved streams that aren't running can be listed and removed, and
// re-adding one that failed to start replaces it rather than doubling it.
void testStreamsNotLoadedCanBeListedAndRemoved() {
    std::cout << "#76: streams not loaded are listed, removable, and replaced when re-added" << std::endl;
    const std::string path = useEmptyConfig("notloaded");
    SDPSession running = makeRxSDP("239.69.99.70", 55100, 2);
    running.sessionName = "Running";
    SDPSession disabled = makeRxSDP("239.69.99.71", 55101, 2);
    disabled.sessionName = "Disabled";
    SDPSession failed = makeRxSDP("239.69.99.72", 55102, 2);
    failed.sessionName = "Failed";
    const ChannelMapping disabledMapping = makeMapping(2, 10);
    const ChannelMapping failedMapping = makeMapping(2, 0);  // clashes with Running
    std::string unreadable = savedEntry(makeRxSDP("239.69.99.73", 55103, 2), makeMapping(2, 20), true);
    unreadable = replaced(unreadable, "\"port\": 55103", "\"port\": \"55103\"");
    writeStreamsFile(path, {savedEntry(running, makeMapping(2, 0), true), savedEntry(disabled, disabledMapping, false),
                            savedEntry(failed, failedMapping, true), unreadable});

    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamManager manager(harness.context(), txContext, testSap());
    CHECK(manager.loadSavedStreams() && manager.getStreamCount() == 1, "only Running should load");

    const auto notLoaded = manager.getStreamsNotLoaded();
    CHECK(notLoaded.size() == 3, "three saved streams aren't running (got " << notLoaded.size() << ")");
    const auto find = [&](const std::string& name) -> const StreamManager::SavedStreamInfo* {
        for (const auto& s : notLoaded) if (s.name == name) return &s;
        return nullptr;
    };
    const auto* d = find("Disabled");
    CHECK(d && !d->enabled && d->readable && d->id == disabledMapping.streamID &&
              d->direction == StreamManager::Direction::Receive && d->deviceChannelStart == 10 && d->deviceChannelCount == 2,
          "the disabled stream is listed with its ID, direction and channels");
    const auto* f = find("Failed");
    CHECK(f && f->enabled && f->readable && f->id == failedMapping.streamID, "the stream that failed to load is listed, enabled");
    size_t unreadableCount = 0;
    for (const auto& s : notLoaded) if (!s.readable) ++unreadableCount;
    CHECK(unreadableCount == 1, "the unreadable entry is listed as unreadable");

    // Removing a disabled stream takes it out of the file
    CHECK(manager.removeStream(disabledMapping.streamID), "a disabled stream can be removed");
    CHECK(readText(path).find("\"Disabled\"") == std::string::npos, "and is gone from streams.json");
    CHECK(manager.getStreamsNotLoaded().size() == 2, "and from the list");
    CHECK(!manager.removeStream(disabledMapping.streamID), "removing it again finds nothing");

    // Re-adding the failed stream (same group, port, direction) replaces its saved copy
    SDPSession again = failed;
    const StreamID againID = manager.addStream(again, makeMapping(2, 4));
    CHECK(!againID.isNull(), "the failed stream can be added again on free channels");
    size_t failedEntries = 0;
    for (const auto& entry : savedEntries(path)) {
        if (entry.config && entry.config->sdp.sessionName == "Failed") ++failedEntries;
    }
    CHECK(failedEntries == 1, "streams.json should hold it once (holds " << failedEntries << ")");
    CHECK(readText(path).find("\"55103\"") != std::string::npos, "the unreadable entry is still kept");
}

void testSavedStreamsHoldTheirChannels() {
    std::cout << "Saved streams that aren't running hold their channels" << std::endl;
    const std::string path = useEmptyConfig("reserve");
    const auto rx = [](const char* name, const char* group, uint16_t port) {
        SDPSession sdp = makeRxSDP(group, port, 2);
        sdp.sessionName = name;
        return sdp;
    };
    // Written before reservations: a disabled stream listing the channels a running one uses
    const ChannelMapping oldClash = makeMapping(2, 0);
    const ChannelMapping running = makeMapping(2, 0);
    const ChannelMapping disabled = makeMapping(2, 2);
    const ChannelMapping unreadableMapping = makeMapping(2, 4);
    const ChannelMapping failed = makeMapping(2, 6);
    const ChannelMapping failedAgain = makeMapping(2, 10);
    const ChannelMapping disabledTx = makeMapping(2, 0);
    SDPSession failedSdp = rx("Failed", "239.69.99.84", 55114);
    failedSdp.sampleRate = 44100;  // the device runs at 48 kHz, so it fails to load
    SDPSession failedAgainSdp = rx("FailedAgain", "239.69.99.85", 55115);
    failedAgainSdp.sampleRate = 44100;
    SDPSession txSdp = rx("DisabledTx", "239.69.99.86", 55116);
    txSdp.direction = "sendonly";
    std::string unreadable = savedEntry(rx("Unreadable", "239.69.99.83", 55113), unreadableMapping, true);
    unreadable = replaced(unreadable, "\"port\": 55113", "\"port\": \"55113\"");
    writeStreamsFile(path, {savedEntry(rx("OldClash", "239.69.99.80", 55110), oldClash, false),
                            savedEntry(rx("Running", "239.69.99.81", 55111), running, true),
                            savedEntry(rx("Disabled", "239.69.99.82", 55112), disabled, false), unreadable,
                            savedEntry(failedSdp, failed, true), savedEntry(failedAgainSdp, failedAgain, true),
                            savedEntry(txSdp, disabledTx, false)});

    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamManager manager(harness.context(), txContext, testSap());
    CHECK(manager.loadSavedStreams() && manager.getStreamCount() == 1, "only Running should load");

    const auto saved = [&](const std::string& name) {
        for (const auto& s : manager.getStreamsNotLoaded()) if (s.name == name) return std::optional(s);
        return std::optional<StreamManager::SavedStreamInfo>();
    };
    CHECK(manager.getMapping(running.streamID), "Running keeps its channels over the earlier disabled entry");
    CHECK(saved("OldClash") && !saved("OldClash")->channelsReserved, "the disabled entry on Running's channels holds none");
    CHECK(saved("Disabled") && saved("Disabled")->channelsReserved, "a disabled stream holds its channels");
    CHECK(saved("Failed") && saved("Failed")->channelsReserved, "a stream that failed to load holds its channels");
    size_t unreadableReserved = 0;
    for (const auto& s : manager.getStreamsNotLoaded()) {
        if (!s.readable && s.channelsReserved && s.id == unreadableMapping.streamID && s.deviceChannelStart == 4 &&
            s.deviceChannelCount == 2) ++unreadableReserved;
    }
    CHECK(unreadableReserved == 1, "an unreadable entry with readable channels holds them");
    CHECK(saved("DisabledTx") && saved("DisabledTx")->channelsReserved &&
              saved("DisabledTx")->direction == StreamManager::Direction::Transmit,
          "a disabled TX stream holds output channels");
    CHECK(manager.getAvailableChannelCount(StreamManager::Direction::Receive) == 118,
          "inputs held: Running, Disabled, Unreadable, Failed, FailedAgain (free "
              << manager.getAvailableChannelCount(StreamManager::Direction::Receive) << ")");
    CHECK(manager.getAvailableChannelCount(StreamManager::Direction::Transmit) == 126, "outputs held: DisabledTx");
    CHECK(manager.getAllMappings(StreamManager::Direction::Receive).size() == 1 && !manager.getMapping(disabled.streamID),
          "mapping queries list running streams only");

    // New streams go around held channels
    const StreamID added = manager.addStream(rx("New", "239.69.99.87", 55117));
    const auto addedMapping = manager.getMapping(added);
    CHECK(addedMapping && addedMapping->deviceChannelStart == 8,
          "a new stream takes the first free block, 8 (got " << (addedMapping ? addedMapping->deviceChannelStart : -1) << ")");
    CHECK(manager.addStream(rx("OnDisabled", "239.69.99.88", 55118), makeMapping(2, 2)).isNull(),
          "a new stream can't take a disabled stream's channels");

    // The stream that failed to load, added again, gets its own channels back
    failedSdp.sampleRate = 48000;
    CHECK(!manager.addStream(failedSdp, makeMapping(2, 6)).isNull(), "a failed stream re-added takes its saved channels");
    CHECK(!saved("Failed"), "and replaces its saved copy");

    // A re-add that fails leaves the saved copy holding its channels
    CHECK(manager.addStream(failedAgainSdp, makeMapping(2, 10)).isNull(), "a re-add at the wrong rate fails");
    CHECK(saved("FailedAgain") && saved("FailedAgain")->channelsReserved, "and the saved copy holds its channels again");
    CHECK(manager.addStream(rx("OnFailedAgain", "239.69.99.89", 55119), makeMapping(2, 10)).isNull(),
          "so another stream still can't take them");

    // Removing a saved stream frees its channels
    CHECK(manager.removeStream(disabled.streamID), "a disabled stream can be removed");
    CHECK(!manager.addStream(rx("OnFreed", "239.69.99.88", 55118), makeMapping(2, 2)).isNull(),
          "and its channels can then be used");

    // Channels a running stream frees go to a saved stream listing them
    CHECK(manager.removeStream(running.streamID), "Running can be removed");
    CHECK(saved("OldClash") && saved("OldClash")->channelsReserved, "and the disabled entry on its channels takes them");

    manager.removeAllStreams();
    CHECK(saved("Unreadable") && saved("Unreadable")->channelsReserved, "removing every running stream keeps saved ones");
    CHECK(!manager.addStream(rx("OnFree", "239.69.99.91", 55121), makeMapping(2, 20)).isNull(),
          "free channels can be used after removing every stream");
    CHECK(manager.addStream(rx("OnUnreadable", "239.69.99.90", 55120), makeMapping(2, 4)).isNull(),
          "which still hold their channels");
}

// ---------------------------------------------------------------------------
// A stream's mapping covers all of its channels, whatever count a caller or
// the file gives (#69); a mapping that is refused changes nothing (#70); and
// channelMap, which never routed anything, no longer decides ownership (#68).
// ---------------------------------------------------------------------------
using Routes = std::vector<std::pair<uint32_t, uint32_t>>;

// The device channels receivers publish to, as {start, count}, sorted.
Routes liveRoutes(const RxHarness& harness) {
    Routes routes;
    harness.routing.read([&](const RxRouting::Route& route) {
        routes.emplace_back(route.deviceChannelStart, static_cast<uint32_t>(route.buffer->channels()));
    });
    std::sort(routes.begin(), routes.end());
    return routes;
}

SDPSession namedRx(const char* name, const char* group, uint16_t port, uint16_t channels) {
    SDPSession sdp = makeRxSDP(group, port, channels);
    sdp.sessionName = name;
    return sdp;
}

void testMappingCoversTheWholeStream() {
    std::cout << "#69, #70: a new mapping covers the whole stream, and a refused one changes nothing" << std::endl;
    const std::string path = useEmptyConfig("fullmapping");
    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
    manager.setIOActive(true);
    using Direction = StreamManager::Direction;

    const StreamID a = manager.addStream(namedRx("A", "239.69.99.100", 55130, 8), makeMapping(8, 0));
    const StreamID b = manager.addStream(namedRx("B", "239.69.99.101", 55131, 8));
    CHECK(!a.isNull() && !b.isNull(), "A and B should be added");

    // #69: a mapping that asks for 1 channel still reserves all 8 the stream publishes
    CHECK(manager.updateMapping(b, makeMapping(1, 120)), "B should move to 120");
    const auto moved = manager.getMapping(b);
    CHECK(moved && moved->deviceChannelStart == 120 && moved->deviceChannelCount == 8 &&
              moved->streamChannelCount == 8,
          "B holds 120-127 (got " << (moved ? moved->deviceChannelStart : -1) << " x "
                                  << (moved ? moved->deviceChannelCount : -1) << ")");
    CHECK(manager.addStream(namedRx("C", "239.69.99.102", 55132, 4), makeMapping(4, 124)).isNull(),
          "a stream on 124-127 is refused: B uses them");
    CHECK(manager.getAvailableChannelCount(Direction::Receive) == 112,
          "16 inputs in use (free " << manager.getAvailableChannelCount(Direction::Receive) << ")");
    CHECK(liveRoutes(harness) == (Routes{{0, 8}, {120, 8}}), "A routes 0-7, B 120-127");

    // #70: 8 channels at 121 don't fit; nothing changes
    CHECK(!manager.updateMapping(b, makeMapping(1, 121)), "B can't move to 121");
    const auto kept = manager.getMapping(b);
    CHECK(kept && kept->deviceChannelStart == 120 && kept->deviceChannelCount == 8, "B's mapping stays 120-127");
    CHECK(manager.addStream(namedRx("D", "239.69.99.103", 55133, 1), makeMapping(1, 120)).isNull(),
          "120 is still B's");
    CHECK(liveRoutes(harness) == (Routes{{0, 8}, {120, 8}}), "B still routes 120-127");
    CHECK(manager.saveAllStreams(), "streams should save");
    bool savedAt120 = false;
    for (const auto& entry : savedEntries(path)) {
        if (entry.config && entry.config->mapping.streamID == b) {
            savedAt120 = entry.config->mapping.deviceChannelStart == 120 &&
                         entry.config->mapping.deviceChannelCount == 8;
        }
    }
    CHECK(savedAt120, "B is saved at 120-127");

    // The same for a transmitter, whose mapping the transmitter itself used to refuse
    const StreamID tx = manager.createTxStream("Mapping TX", "239.69.99.104", 55134, 2, makeMapping(2, 0), kHostOnly);
    CHECK(!tx.isNull(), "TX should be created");
    CHECK(!manager.updateMapping(tx, makeMapping(1, 127)), "2 output channels at 127 don't fit");
    const auto txKept = manager.getMapping(tx);
    CHECK(txKept && txKept->deviceChannelStart == 0 && txKept->deviceChannelCount == 2, "TX's mapping stays 0-1");
    CHECK(manager.getAvailableChannelCount(Direction::Transmit) == 126, "TX holds 2 outputs");
}

void testSavedMappingCoversTheWholeStream() {
    std::cout << "#69: a saved mapping covers the whole stream, whatever count the file gives" << std::endl;
    const std::string path = useEmptyConfig("savedfullmapping");
    const auto narrowEntry = [](const SDPSession& sdp, uint16_t start) {
        ChannelMapping mapping = makeMapping(8, start);
        mapping.deviceChannelCount = 1;
        mapping.streamChannelCount = 1;
        return savedEntry(sdp, mapping, true);
    };
    writeStreamsFile(path, {narrowEntry(namedRx("Fits", "239.69.99.105", 55135, 8), 0),
                            narrowEntry(namedRx("PastTheEnd", "239.69.99.106", 55136, 8), 121)});

    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
    CHECK(manager.loadSavedStreams() && manager.getStreamCount() == 1, "only Fits should load");
    manager.setIOActive(true);

    const auto mappings = manager.getAllMappings(StreamManager::Direction::Receive);
    CHECK(mappings.size() == 1 && mappings[0].deviceChannelStart == 0 && mappings[0].deviceChannelCount == 8,
          "Fits holds 0-7");
    CHECK(manager.addStream(namedRx("OnFits", "239.69.99.107", 55137, 2), makeMapping(2, 4)).isNull(),
          "a stream on 4-5 is refused: Fits uses them");
    CHECK(liveRoutes(harness) == (Routes{{0, 8}}), "only Fits routes");

    bool pastTheEndHoldsNothing = false;
    for (const auto& saved : manager.getStreamsNotLoaded()) {
        if (saved.name == "PastTheEnd") {
            pastTheEndHoldsNothing = !saved.channelsReserved && saved.deviceChannelCount == 8;
        }
    }
    CHECK(pastTheEndHoldsNothing, "PastTheEnd, 8 channels at 121, isn't loaded and holds no channels");
    CHECK(!manager.addStream(namedRx("OnPastTheEnd", "239.69.99.108", 55138, 1), makeMapping(1, 121)).isNull(),
          "so 121 is free");
}

void testChannelMapIsIgnored() {
    std::cout << "#68: an old channelMap is ignored and dropped on save" << std::endl;
    const std::string path = useEmptyConfig("channelmap");
    const std::string entry = savedEntry(namedRx("Mapped", "239.69.99.109", 55139, 2), makeMapping(2, 0), true);
    const std::string withMap =
        replaced(entry, "\"deviceChannelCount\": 2", "\"deviceChannelCount\": 2, \"channelMap\": [10, 11]");
    CHECK(withMap != entry, "the test should have added a channelMap");
    writeStreamsFile(path, {withMap});

    RxHarness harness;
    TxRouting txRouting;
    StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
    CHECK(manager.loadSavedStreams() && manager.getStreamCount() == 1, "the stream should load");
    CHECK(manager.addStream(namedRx("On0", "239.69.99.110", 55140, 2), makeMapping(2, 0)).isNull(),
          "0-1, where the stream routes, are taken");
    CHECK(!manager.addStream(namedRx("On10", "239.69.99.111", 55141, 2), makeMapping(2, 10)).isNull(),
          "10-11, from the channelMap, are free");
    CHECK(manager.saveAllStreams(), "streams should save");
    CHECK(readText(path).find("channelMap") == std::string::npos, "the saved file has no channelMap");
}

void testDuplicateEntryIsSavedOnce() {
    std::cout << "A stream listed twice runs once and is saved once" << std::endl;
    const std::string path = useEmptyConfig("duplicate");
    const std::string entry = savedEntry(makeRxSDP("239.69.99.63", 55093, 2), makeMapping(2, 0), true);
    writeStreamsFile(path, {entry, entry});
    RxHarness harness;
    TxRouting txRouting;
    const TxContext txContext{harness.clock, txRouting};
    StreamManager manager(harness.context(), txContext, testSap());
    CHECK(manager.loadSavedStreams() && manager.getStreamCount() == 1, "one copy should run");
    CHECK(manager.saveAllStreams(), "and save");
    CHECK(savedEntries(path).size() == 1, "the file should hold it once (holds " << savedEntries(path).size() << ")");
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
// Real-time threads get the time-constraint policy, with the period they ask
// for (#62). The extended + precedence policy they had before still woke
// about 115 us late on every timer, a whole packet at 125 us packet time.
// configureForRealTime reports success only when the policy is applied.
// ---------------------------------------------------------------------------
namespace {

// The thread's time-constraint policy, or nullopt if it has none.
std::optional<thread_time_constraint_policy_data_t> timeConstraintOf(thread_act_t thread) {
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t getDefault = FALSE;
    if (thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), &count,
                          &getDefault) != KERN_SUCCESS || getDefault) {
        return std::nullopt;
    }
    return policy;
}

int64_t ticksToMicroseconds(uint32_t ticks) {
    return static_cast<int64_t>(std::llround(static_cast<double>(ticks) / HostTimebase::current().ticksPerSecond() * 1e6));
}

// The time-constraint periods of every thread in this process, in microseconds, sorted.
std::vector<int64_t> timeConstraintPeriods() {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    std::vector<int64_t> periods;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return periods;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (const auto policy = timeConstraintOf(threads[i])) periods.push_back(ticksToMicroseconds(policy->period));
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    std::sort(periods.begin(), periods.end());
    return periods;
}

} // namespace

void testRealTimePriorityReportsSuccess() {
    std::cout << "#62: real-time threads get the time-constraint policy they ask for" << std::endl;
    bool reported = false;
    std::optional<thread_time_constraint_policy_data_t> applied;
    std::optional<thread_time_constraint_policy_data_t> restored;
    std::thread([&] {
        reported = AudioThreadPriority::configureForRealTime(std::chrono::microseconds(250));
        applied = timeConstraintOf(pthread_mach_thread_np(pthread_self()));
        AudioThreadPriority::restoreNormalPriority();
        restored = timeConstraintOf(pthread_mach_thread_np(pthread_self()));
    }).join();
    CHECK(reported, "configureForRealTime should report the success");
    CHECK(applied && ticksToMicroseconds(applied->period) == 250 && applied->computation <= applied->constraint &&
              applied->constraint <= applied->period && applied->computation > 0,
          "the thread should run with a 250 us time-constraint policy (period "
              << (applied ? ticksToMicroseconds(applied->period) : -1) << " us)");
    CHECK(!restored, "restoreNormalPriority should return the thread to the standard policy");

    // The receive and transmit threads, at their packet time (capped at the 1 ms they poll at)
    useEmptyConfig("rtpolicy");
    const std::vector<int64_t> before = timeConstraintPeriods();
    RxHarness harness;
    TxRouting txRouting;
    {
        StreamManager manager(harness.context(), TxContext{harness.clock, txRouting}, testSap());
        SDPSession fast = makeRxSDP("239.69.99.140", 55170, 2);
        fast.ptime = 0.125;
        fast.framecount = 6;
        CHECK(!manager.addStream(fast, makeMapping(2, 0)).isNull(), "a 125 us RX stream should be added");
        CHECK(!manager.createTxStream("RT TX", "239.69.99.141", 55172, 2, makeMapping(2, 0), kHostOnly).isNull(),
              "a TX stream should be created");
        manager.setIOActive(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::vector<int64_t> added = timeConstraintPeriods();
        for (int64_t period : before) {
            const auto it = std::find(added.begin(), added.end(), period);
            if (it != added.end()) added.erase(it);
        }
        std::ostringstream periods;
        for (int64_t period : added) periods << period << " ";
        CHECK(added == (std::vector<int64_t>{125, 1000}),
              "the RX thread runs at 125 us and the TX thread at 1000 us (got: " << periods.str() << ")");
        manager.setIOActive(false);
    }
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
    testInterfaceRestartLetsOthersIn();
    testInputAndOutputChannelsAreSeparate();
    testSenderSdpDirectionDoesNotMakeATransmitter();
    testTxStreamIsAnnounced();
    testTxAnnouncesThePtpGrandmaster();
    testTimelineRestartNeverReusesPositions();
    testNoDefaultTxStream();
    testUnloadedStreamsStayInTheConfig();
    testConfigWithNoLoadableStreamIsKept();
    testConfigPathEnvironmentIsAuthoritative();
    testOldFileSurvivesLoadAndSave();
    testStreamThatFailsToLoadIsKeptEnabled();
    testDuplicateEntryIsSavedOnce();
    testStreamsNotLoadedCanBeListedAndRemoved();
    testSavedStreamsHoldTheirChannels();
    testMappingCoversTheWholeStream();
    testSavedMappingCoversTheWholeStream();
    testChannelMapIsIgnored();
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
