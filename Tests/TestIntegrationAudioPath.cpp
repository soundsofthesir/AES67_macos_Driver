//
// TestIntegrationAudioPath.cpp
// AES67 macOS Driver
// Integration tests for the full audio data path
//
// Tests exercise the complete chain:
//   RTP TX -> Network -> RTP RX -> Ring Buffers -> IO Handler
// using localhost multicast and real component instances.
//

#include "RxTestSupport.h"
#include "TestWait.h"
#include "../Driver/AudioThreadPriority.h"
#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../NetworkEngine/RTP/RTPTransmitter.h"
#include "../NetworkEngine/RTP/TxContext.h"
#include "../NetworkEngine/StreamChannelMapper.h"
#include "../Driver/SDPParser.h"
#include "../Shared/RingBuffer.hpp"
#include "../Shared/Types.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <array>
#include <atomic>
#include <cstring>
#include <numeric>
#include <sstream>
#include <algorithm>
#include <unistd.h>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

// ============================================================================
// Helper: Create initialized ring buffer array (128 channels)
// Same pattern as BenchmarkIOHandler.cpp
// ============================================================================

namespace {
    template<size_t... Is>
    auto MakeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
        return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
            ((void)Is, SPSCRingBuffer<float>(bufferSize))...
        };
    }

    template<size_t N>
    auto MakeRingBufferArray(size_t bufferSize) {
        return MakeRingBufferArray(bufferSize, std::make_index_sequence<N>{});
    }

    constexpr size_t kNumChannels = 128;
    constexpr size_t kRingBufferSize = 4096;

    // Plays Core Audio's output write: puts `frames` frames of a 128-channel
    // mix (value per device channel) into every published TX buffer, starting
    // at a packet boundary `aheadFrames` ahead of now. Returns that position.
    int64_t writeOutputAhead(TxRouting& routing, const MediaClock& clock, size_t frames,
                             const std::array<float, kNumChannels>& valueByChannel, int64_t aheadFrames) {
        const int64_t now = clock.snapshot().sampleAt(hostTimeNow());
        const int64_t start = (now / 48) * 48 + aheadFrames;
        std::vector<float> mix(frames * kNumChannels);
        for (size_t f = 0; f < frames; ++f) {
            std::copy(valueByChannel.begin(), valueByChannel.end(), mix.begin() + static_cast<ptrdiff_t>(f * kNumChannels));
        }
        routing.read([&](const TxRouting::Route& route) {
            route.buffer->write(start, frames, mix.data(), kNumChannels, route.deviceChannelStart);
        });
        return start;
    }

    constexpr size_t kPacketFrames = 48;

    // What may excuse an unheard packet, read while the receiver still runs.
    // The receiver judges lateness by arrival time, so a send the machine
    // delayed may arrive past its playout point and be dropped, or arrive
    // while its playout passes and be heard in part. A packet the receive
    // thread wrote after playout had passed it counts as missedPlayout (#185).
    // Each of those may cost up to one packet. With none, every count is exact.
    struct Excused {
        size_t packets{0};
        std::string summary;  // for failure messages

        Excused(const RTPReceiver& receiver, const TestSupport::SendPacer& pacer) {
            const auto stats = receiver.getPlacementStatistics();
            packets = pacer.lateSends() + static_cast<size_t>(stats.missedPlayout);
            std::ostringstream out;
            out << "late drops " << stats.lateDrops << ", early drops " << stats.earlyDrops << ", missed playout "
                << stats.missedPlayout << ", " << pacer.lateSends() << " sent late, worst " << pacer.maxBehindMs()
                << " ms behind";
            summary = out.str();
        }
    };

    bool playedAllBut(size_t frames, size_t packets, const Excused& excused) {
        const size_t want = packets * kPacketFrames;
        return frames <= want && frames + excused.packets * kPacketFrames >= want;
    }

    // Every sample is `value`. An excused packet plays as silence, so silence
    // passes only when a packet is excused.
    bool carries(const std::vector<float>& region, float value, float tolerance, const Excused& excused) {
        return std::all_of(region.begin(), region.end(), [&](float v) {
            return std::abs(v - value) < tolerance || (excused.packets > 0 && v == 0.0f);
        });
    }

    void resetClock(MediaClock& clock) {
        clock.reset(hostTimeNow(), 0, MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current()));
    }
}

// ============================================================================
// Helper: Create SDP session for testing
// ============================================================================

static SDPSession createTestSDP(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t channels,
    const std::string& encoding = "L24",
    uint32_t sampleRate = 48000,
    uint32_t framecount = 48
) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.encoding = encoding;
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = (encoding == "L16") ? PT_AES67_L16 : PT_AES67_L24;
    sdp.ptime = 1;
    sdp.framecount = framecount;
    sdp.originAddress = "127.0.0.1";
    sdp.ptpDomain = 0;
    sdp.ttl = 0;  // host-only: test streams never leave this machine
    return sdp;
}

// ============================================================================
// Helper: Create channel mapping
// ============================================================================

static ChannelMapping createTestMapping(
    const StreamID& id,
    const std::string& name,
    uint16_t streamChannels,
    uint16_t deviceStart
) {
    ChannelMapping mapping;
    mapping.streamID = id;
    mapping.streamName = name;
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;
    return mapping;
}

// ============================================================================
// Helper: Send raw RTP packet via UDP to a multicast group
// Bypasses RTPTransmitter to have fine-grained control over packet content
// ============================================================================

static bool sendRawRTPPacket(
    const char* multicastIP,
    uint16_t port,
    uint16_t sequenceNumber,
    uint32_t timestamp,
    uint32_t ssrc,
    uint8_t payloadType,
    const uint8_t* payload,
    size_t payloadSize
) {
    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return false;

    // Host-only multicast (TTL 0): test traffic never leaves this machine
    uint8_t ttl = 0;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Enable loopback so the sender's own machine receives the packet
    uint8_t loopback = 1;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback));

    // Build destination address
    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_addr.s_addr = inet_addr(multicastIP);
    destAddr.sin_port = htons(port);

    // Build RTP header in network byte order
    RTPHeader header;
    header.version = 2;
    header.padding = 0;
    header.extension = 0;
    header.cc = 0;
    header.marker = 0;
    header.payloadType = payloadType;
    header.sequenceNumber = sequenceNumber;
    header.timestamp = timestamp;
    header.ssrc = ssrc;
    header.toNetworkOrder();

    // Send header + payload via scatter/gather
    struct iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<uint8_t*>(payload);
    iov[1].iov_len = payloadSize;

    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_name = &destAddr;
    msg.msg_namelen = sizeof(destAddr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ssize_t sent = sendmsg(sockfd, &msg, 0);
    ::close(sockfd);

    return sent > 0;
}

// ============================================================================
// Test 1: RTP Receive -> Ring Buffer
// ============================================================================

bool testRTPReceiveToRingBuffer() {
    std::cout << "Test: RTP Receive -> Playout by timestamp... ";

    // Configure a 2-channel L16 RX stream, mapped to device channels 0-1
    const uint16_t rxChannels = 2;
    const uint16_t rxPort = 15004; // Use high port to avoid conflicts
    const char* rxAddr = "239.69.69.1";

    SDPSession rxSDP = createTestSDP("RX Test", rxAddr, rxPort, rxChannels, "L16");
    ChannelMapping rxMapping = createTestMapping(StreamID::generate(), "RX Test", rxChannels, 0);

    TestSupport::RxHarness harness;
    RTPReceiver receiver(rxSDP, rxMapping, harness.context());
    TEST_ASSERT(!receiver.isRunning(), "Receiver should not be running before start");
    TEST_ASSERT(receiver.start(), "Receiver should start successfully");
    TEST_ASSERT(receiver.isRunning(), "Receiver should be running after start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TestSupport::PlayoutReader reader(harness, {0, 1, 2, 3});

    // 48 frames per packet: channel 0 = 0.5, channel 1 = -0.5
    const size_t frameCount = 48;
    std::vector<float> audioData(frameCount * rxChannels);
    for (size_t f = 0; f < frameCount; ++f) {
        audioData[f * rxChannels + 0] = 0.5f;
        audioData[f * rxChannels + 1] = -0.5f;
    }
    std::vector<uint8_t> l16Payload(audioData.size() * 2);
    L16Codec::encode(audioData.data(), audioData.size(), l16Payload.data());

    // Real-time pacing: one 1 ms packet per millisecond
    const int numPackets = 20;
    TestSupport::SendPacer pacer;
    for (int i = 0; i < numPackets; ++i) {
        bool sent = sendRawRTPPacket(rxAddr, rxPort, static_cast<uint16_t>(i), static_cast<uint32_t>(i * 48),
                                     0x12345678, PT_AES67_L16, l16Payload.data(), l16Payload.size());
        TEST_ASSERT(sent, "RTP packet should be sent successfully");
        pacer.wait();
    }

    // Let playout pass the last packet, then see what the IO thread heard
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto heard = reader.stopAll();

    const Excused excused(receiver, pacer);
    const size_t played0 = TestSupport::countNonSilent(heard[0]);
    const size_t played1 = TestSupport::countNonSilent(heard[1]);
    TEST_ASSERT(playedAllBut(played0, numPackets, excused),
                "Channel 0 should play every received frame (got " << played0 << " of " << numPackets * frameCount
                                                                   << "; " << excused.summary << ")");
    TEST_ASSERT(playedAllBut(played1, numPackets, excused),
                "Channel 1 should play every received frame (got " << played1 << " of " << numPackets * frameCount
                                                                   << "; " << excused.summary << ")");

    const auto ch0 = TestSupport::heardRegion(heard[0]);
    const auto ch1 = TestSupport::heardRegion(heard[1]);
    TEST_ASSERT(carries(ch0, 0.5f, 0.01f, excused), "Channel 0 samples should be ~0.5f (" << excused.summary << ")");
    TEST_ASSERT(carries(ch1, -0.5f, 0.01f, excused), "Channel 1 samples should be ~-0.5f (" << excused.summary << ")");

    TEST_ASSERT(TestSupport::countNonSilent(heard[2]) == 0, "Unmapped channel 2 should be silent");
    TEST_ASSERT(TestSupport::countNonSilent(heard[3]) == 0, "Unmapped channel 3 should be silent");

    StatisticsSnapshot stats = receiver.getStatistics();
    TEST_ASSERT(stats.packetsReceived > 0, "Should report received packets");

    receiver.stop();
    TEST_ASSERT(!receiver.isRunning(), "Receiver should stop");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 2: Ring Buffer -> RTP Transmit
// ============================================================================

bool testRingBufferToRTPTransmit() {
    std::cout << "Test: TX buffer -> RTP Transmit... ";

    MediaClock clock;
    resetClock(clock);
    TxRouting routing;

    // Configure a 2-channel L16 TX stream on 239.69.69.2:15006, device channels 8-9
    const uint16_t txChannels = 2;
    const uint16_t txPort = 15006;
    const char* txAddr = "239.69.69.2";

    SDPSession txSDP = createTestSDP("TX Test", txAddr, txPort, txChannels, "L16");
    StreamID txID = StreamID::generate();
    ChannelMapping txMapping = createTestMapping(txID, "TX Test", txChannels, 8);

    RTPTransmitter transmitter(txSDP, txMapping, TxContext{clock, routing});
    TEST_ASSERT(!transmitter.isRunning(), "Transmitter should not be running before start");
    TEST_ASSERT(transmitter.framesPerPacket() == 48, "48 kHz at 1 ms should be 48 frames per packet");

    bool started = transmitter.start();
    TEST_ASSERT(started, "Transmitter should start successfully");
    TEST_ASSERT(transmitter.isRunning(), "Transmitter should be running after start");

    // Core Audio writes 10 packets of output: channel 8 at 0.75, channel 9 at -0.25
    std::array<float, kNumChannels> values{};
    values[8] = 0.75f;
    values[9] = -0.25f;
    writeOutputAhead(routing, clock, 480, values, 96);

    // Let the transmitter send packets for a brief period
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    transmitter.stop();
    TEST_ASSERT(!transmitter.isRunning(), "Transmitter should stop");

    StatisticsSnapshot stats = transmitter.getStatistics();
    TEST_ASSERT(stats.bytesSent > 0, "Transmitter should have sent bytes");
    const auto sent = transmitter.getTransmitStatistics().packetsSent;
    // Wall-clock throughput: a loaded or instrumented (ThreadSanitizer) runner
    // sends fewer packets in the same 100 ms, and can send a burst after it is
    // descheduled. Ordering and continuity are checked elsewhere; this is only
    // a limit on timing accuracy (Tests/TestWait.h).
    const auto leastSent = TestWait::timingLimit(90u, 30u);
    const auto mostSent = TestWait::timingLimit(110u, 160u);
    TEST_ASSERT(sent >= leastSent && sent <= mostSent,
                "about 100 packets should be sent in 100 ms (got " << sent << ")");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 3: Full Loopback (TX -> Network -> RX)
// ============================================================================

bool testFullLoopback() {
    std::cout << "Test: Full Loopback (TX -> Network -> RX)... ";

    // TX and RX on one media clock: TX sends by media position, RX places by timestamp
    TestSupport::RxHarness harness;
    TxRouting txRouting;

    const char* loopAddr = "239.69.69.3";
    const uint16_t loopPort = 15008;
    const uint16_t channels = 2;

    SDPSession txSDP = createTestSDP("Loopback TX", loopAddr, loopPort, channels, "L24");
    ChannelMapping txMapping = createTestMapping(StreamID::generate(), "Loopback TX", channels, 0);
    SDPSession rxSDP = createTestSDP("Loopback RX", loopAddr, loopPort, channels, "L24");
    ChannelMapping rxMapping = createTestMapping(StreamID::generate(), "Loopback RX", channels, 0);

    RTPReceiver receiver(rxSDP, rxMapping, harness.context());
    TEST_ASSERT(receiver.start(), "Loopback receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TestSupport::PlayoutReader reader(harness, {0, 1});

    RTPTransmitter transmitter(txSDP, txMapping, TxContext{harness.clock, txRouting});
    TEST_ASSERT(transmitter.start(), "Loopback transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 20 packets of a recognisable pattern; around it the transmitter sends
    // silence (AES67 requires continuous packets). Written 50 ms ahead of the
    // transmitter. Any part it had sent before the write finished (the test
    // thread was descheduled) went out as silence, and is excused below.
    const size_t patternFrames = 960;
    std::array<float, kNumChannels> values{};
    values[0] = 0.25f;
    values[1] = -0.75f;
    const int64_t patternStart = writeOutputAhead(txRouting, harness.clock, patternFrames, values, 2400);
    const int64_t writtenBy = harness.clock.snapshot().sampleAt(hostTimeNow());
    const auto writtenLate = static_cast<size_t>(
        std::clamp<int64_t>(writtenBy - patternStart, 0, static_cast<int64_t>(patternFrames)));

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    transmitter.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto heard = reader.stopAll();
    receiver.stop();

    TEST_ASSERT(transmitter.getStatistics().bytesSent > 0, "TX should have sent bytes");
    TEST_ASSERT(receiver.getStatistics().packetsReceived > 0, "RX should have received packets");

    // The pattern must arrive intact and in one piece: exactly the 960
    // written frames, with the transmitter's silence around them. Both ends
    // run on wall-clock threads: a transmitter descheduled past its link
    // offset skips packets or sends them too late to play, and a late receive
    // thread misses playout. Each such packet may leave up to one packet of
    // silence, as may a late write (above); with none, the check is exact.
    // Wrong samples are never excused.
    const auto tx = transmitter.getTransmitStatistics();
    const auto rx = receiver.getPlacementStatistics();
    const size_t excusedFrames =
        static_cast<size_t>(tx.packetsSkipped + rx.lateDrops + rx.missedPlayout) * kPacketFrames +
        (writtenLate > 0 ? writtenLate + kPacketFrames : 0);
    std::ostringstream timing;
    timing << "skipped " << tx.packetsSkipped << ", late drops " << rx.lateDrops << ", missed playout "
           << rx.missedPlayout << ", written " << writtenLate << " frames late";
    const auto ch0 = TestSupport::heardRegion(heard[0]);
    const auto ch1 = TestSupport::heardRegion(heard[1]);
    const size_t heard0 = TestSupport::countNear(ch0, 0.25f);
    const size_t heard1 = TestSupport::countNear(ch1, -0.75f);
    TEST_ASSERT(heard0 == TestSupport::countNonSilent(ch0) && heard1 == TestSupport::countNonSilent(ch1),
                "Loopback should carry only the written values (" << timing.str() << ")");
    TEST_ASSERT(ch0.size() <= patternFrames && ch1.size() <= patternFrames,
                "Loopback should not stretch the 960 written frames (" << ch0.size() << ", " << ch1.size() << ")");
    TEST_ASSERT(heard0 + excusedFrames >= patternFrames && heard1 + excusedFrames >= patternFrames,
                "Loopback should deliver the 960 written frames (heard " << heard0 << " and " << heard1 << "; "
                                                                         << timing.str() << ")");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Underrun Behavior
// ============================================================================

bool testUnderrunBehavior() {
    std::cout << "Test: Underrun Behavior (stream stops -> silence)... ";

    // Empty ring buffers (still used for TX) read nothing
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(kRingBufferSize);
    std::vector<float> readBuffer(48, 999.0f);
    TEST_ASSERT(deviceBuffers[0].read(readBuffer.data(), 48) == 0, "Read from empty buffer should return 0");

    // A receiver whose sender stops: playout goes silent at exactly the point
    // the audio ends, rather than stalling or repeating
    SDPSession sdp = createTestSDP("Underrun RX", "239.69.69.4", 15010, 2, "L16");
    ChannelMapping mapping = createTestMapping(StreamID::generate(), "Underrun RX", 2, 0);
    TestSupport::RxHarness harness;
    RTPReceiver receiver(sdp, mapping, harness.context());
    TEST_ASSERT(receiver.start(), "Underrun test receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TestSupport::PlayoutReader reader(harness, 0);

    std::vector<float> audio(48 * 2, 0.1f);
    std::vector<uint8_t> payload(audio.size() * 2);
    L16Codec::encode(audio.data(), audio.size(), payload.data());
    TestSupport::SendPacer pacer;
    for (int i = 0; i < 3; ++i) {
        sendRawRTPPacket("239.69.69.4", 15010, i, i * 48, 0xAABBCCDD, PT_AES67_L16, payload.data(), payload.size());
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto heard = reader.stop();
    const Excused excused(receiver, pacer);
    receiver.stop();

    const auto region = TestSupport::heardRegion(heard);
    const size_t played = TestSupport::countNonSilent(heard);
    TEST_ASSERT(playedAllBut(played, 3, excused) && region.size() <= 3 * 48,
                "The three packets should play as 144 frames (got " << played << " over " << region.size() << "; "
                                                                    << excused.summary << ")");
    const auto last = std::find_if(heard.rbegin(), heard.rend(), [](float v) { return v != 0.0f; });
    const size_t trailingSilence = static_cast<size_t>(last - heard.rbegin());
    TEST_ASSERT(trailingSilence > 48 * 100, "Playout should continue in silence after the sender stops");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 5: Overrun Behavior
// ============================================================================

bool testOverrunBehavior() {
    std::cout << "Test: Overrun Behavior (ring buffer overflow)... ";

    // Create ring buffers with a SMALL capacity to easily trigger overrun
    const size_t smallCapacity = 64;

    // We need the full 128-channel array but with small buffers
    auto deviceBuffers = MakeRingBufferArray<kNumChannels>(smallCapacity);

    // Fill the ring buffer to capacity
    std::vector<float> fillData(smallCapacity, 0.5f);
    size_t written = deviceBuffers[0].write(fillData.data(), smallCapacity);
    TEST_ASSERT(written == smallCapacity, "Should fill buffer to capacity");
    TEST_ASSERT(deviceBuffers[0].isFull(), "Buffer should be full after fill");

    // Try to write more -- should return 0 (overrun)
    float extraSample = 0.99f;
    size_t overflowWritten = deviceBuffers[0].write(&extraSample, 1);
    TEST_ASSERT(overflowWritten == 0, "Write to full buffer should return 0 (overrun)");

    // Verify the buffer data is intact (old data preserved, not corrupted)
    std::vector<float> readBack(smallCapacity);
    size_t readCount = deviceBuffers[0].read(readBack.data(), smallCapacity);
    TEST_ASSERT(readCount == smallCapacity, "Should read back all original data");

    bool dataIntact = true;
    for (size_t i = 0; i < readCount; ++i) {
        if (std::abs(readBack[i] - 0.5f) > 0.001f) {
            dataIntact = false;
            break;
        }
    }
    TEST_ASSERT(dataIntact, "Original data should be preserved after overrun");

    // Test overrun tracking: simulate Core Audio writing to output buffers
    // when the network consumer is too slow. Fill all channels, then try
    // to write more.
    auto outputBuffers = MakeRingBufferArray<kNumChannels>(smallCapacity);
    std::atomic<uint64_t> overrunCounter{0};

    // Fill channel 0 and 1
    outputBuffers[0].write(fillData.data(), smallCapacity);
    outputBuffers[1].write(fillData.data(), smallCapacity);

    // Simulate processOutput behavior: try to write and track overruns
    float newData[48];
    std::memset(newData, 0, sizeof(newData));

    for (size_t ch = 0; ch < 2; ++ch) {
        size_t samplesWritten = outputBuffers[ch].write(newData, 48);
        if (samplesWritten < 48) {
            overrunCounter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    TEST_ASSERT(overrunCounter.load() == 2,
                "Should detect overrun on both channels");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 6: Multi-Stream Channel Isolation
// ============================================================================

bool testMultiStreamChannelIsolation() {
    std::cout << "Test: Multi-Stream Channel Isolation... ";

    // Stream A on device channels 0-1, stream B on 4-5, sharing one device context
    const uint16_t channels = 2;
    SDPSession sdpA = createTestSDP("Stream A", "239.69.69.5", 15012, channels, "L16");
    ChannelMapping mappingA = createTestMapping(StreamID::generate(), "Stream A", channels, 0);
    SDPSession sdpB = createTestSDP("Stream B", "239.69.69.6", 15014, channels, "L16");
    ChannelMapping mappingB = createTestMapping(StreamID::generate(), "Stream B", channels, 4);

    TestSupport::RxHarness harness;
    RTPReceiver receiverA(sdpA, mappingA, harness.context());
    RTPReceiver receiverB(sdpB, mappingB, harness.context());
    TEST_ASSERT(receiverA.start(), "Receiver A should start");
    TEST_ASSERT(receiverB.start(), "Receiver B should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TestSupport::PlayoutReader reader(harness, {0, 1, 2, 3, 4, 5});

    std::vector<float> audioA(48 * channels, 0.3f);
    std::vector<float> audioB(48 * channels, -0.7f);
    std::vector<uint8_t> payloadA(audioA.size() * 2);
    std::vector<uint8_t> payloadB(audioB.size() * 2);
    L16Codec::encode(audioA.data(), audioA.size(), payloadA.data());
    L16Codec::encode(audioB.data(), audioB.size(), payloadB.data());

    const int numPackets = 20;
    // One wait paces both streams, so a late send may cost a packet on each
    TestSupport::SendPacer pacer;
    for (int i = 0; i < numPackets; ++i) {
        sendRawRTPPacket("239.69.69.5", 15012, i, i * 48, 0x11111111, PT_AES67_L16, payloadA.data(), payloadA.size());
        sendRawRTPPacket("239.69.69.6", 15014, i, i * 48, 0x22222222, PT_AES67_L16, payloadB.data(), payloadB.size());
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto heard = reader.stopAll();
    const Excused excusedA(receiverA, pacer);
    const Excused excusedB(receiverB, pacer);
    receiverA.stop();
    receiverB.stop();

    const size_t expected = numPackets * 48;
    for (uint32_t ch : {0u, 1u, 4u, 5u}) {
        const Excused& excused = ch < 2 ? excusedA : excusedB;
        const size_t played = TestSupport::countNonSilent(heard[ch]);
        TEST_ASSERT(playedAllBut(played, numPackets, excused),
                    "Stream " << (ch < 2 ? "A" : "B") << ": channel " << ch << " should play every frame (got "
                              << played << " of " << expected << "; " << excused.summary << ")");
    }
    TEST_ASSERT(TestSupport::countNonSilent(heard[2]) == 0, "Channel 2 (gap between streams) should be silent");
    TEST_ASSERT(TestSupport::countNonSilent(heard[3]) == 0, "Channel 3 (gap between streams) should be silent");

    const auto a = TestSupport::heardRegion(heard[0]);
    const auto b = TestSupport::heardRegion(heard[4]);
    TEST_ASSERT(carries(a, 0.3f, 0.02f, excusedA), "Stream A channel 0 values should be ~0.3f (" << excusedA.summary << ")");
    TEST_ASSERT(carries(b, -0.7f, 0.02f, excusedB), "Stream B channel 4 values should be ~-0.7f (" << excusedB.summary << ")");
    TEST_ASSERT(!receiverA.isRunning() && !receiverB.isRunning(), "Receivers should stop");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 7: L16 vs L24 Encoding Round-Trip via Ring Buffers
// ============================================================================

bool testEncodingRoundTrip() {
    std::cout << "Test: L16/L24 Encoding Round-Trip via Ring Buffers... ";

    // Test that encoding -> ring buffer -> decoding preserves audio data
    // at the expected precision for each codec.

    const size_t numSamples = 256;
    std::vector<float> original(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        original[i] = std::sin(2.0f * M_PI * i / numSamples);
    }

    // L16 round-trip via codec
    {
        std::vector<uint8_t> encoded(numSamples * 2);
        std::vector<float> decoded(numSamples);

        L16Codec::encode(original.data(), numSamples, encoded.data());
        L16Codec::decode(encoded.data(), encoded.size(), decoded.data());

        // Write decoded to ring buffer and read back
        SPSCRingBuffer<float> ringBuffer(kRingBufferSize);
        size_t written = ringBuffer.write(decoded.data(), numSamples);
        TEST_ASSERT(written == numSamples, "L16: should write all samples to ring buffer");

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        TEST_ASSERT(readCount == numSamples, "L16: should read all samples from ring buffer");

        // Verify L16 precision (~0.01 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        TEST_ASSERT(maxError < 0.01, "L16 round-trip error should be < 0.01");
    }

    // L24 round-trip via codec
    {
        std::vector<uint8_t> encoded(numSamples * 3);
        std::vector<float> decoded(numSamples);

        L24Codec::encode(original.data(), numSamples, encoded.data());
        L24Codec::decode(encoded.data(), encoded.size(), decoded.data());

        // Write decoded to ring buffer and read back
        SPSCRingBuffer<float> ringBuffer(kRingBufferSize);
        size_t written = ringBuffer.write(decoded.data(), numSamples);
        TEST_ASSERT(written == numSamples, "L24: should write all samples to ring buffer");

        std::vector<float> readBack(numSamples);
        size_t readCount = ringBuffer.read(readBack.data(), numSamples);
        TEST_ASSERT(readCount == numSamples, "L24: should read all samples from ring buffer");

        // Verify L24 precision (~0.001 tolerance)
        double maxError = 0.0;
        for (size_t i = 0; i < numSamples; ++i) {
            maxError = std::max(maxError, static_cast<double>(std::abs(readBack[i] - original[i])));
        }
        TEST_ASSERT(maxError < 0.001, "L24 round-trip error should be < 0.001");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 8: Channel Mapping Correctness Through Receiver
// ============================================================================

bool testChannelMappingThroughReceiver() {
    std::cout << "Test: Channel Mapping Correctness (offset mapping)... ";

    // A 4-channel stream mapped to device channels 16-19
    const uint16_t rxChannels = 4;
    SDPSession sdp = createTestSDP("Mapped RX", "239.69.69.7", 15016, rxChannels, "L16");
    ChannelMapping mapping = createTestMapping(StreamID::generate(), "Mapped RX", rxChannels, 16);

    TestSupport::RxHarness harness;
    RTPReceiver receiver(sdp, mapping, harness.context());
    TEST_ASSERT(receiver.start(), "Mapped receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<uint32_t> listen;
    for (uint32_t ch = 0; ch < 24; ++ch) listen.push_back(ch);
    TestSupport::PlayoutReader reader(harness, listen);

    // Stream channel n carries 0.1 * (n + 1)
    std::vector<float> audio(48 * rxChannels);
    for (size_t f = 0; f < 48; ++f) {
        for (size_t c = 0; c < rxChannels; ++c) audio[f * rxChannels + c] = 0.1f * (c + 1);
    }
    std::vector<uint8_t> payload(audio.size() * 2);
    L16Codec::encode(audio.data(), audio.size(), payload.data());

    TestSupport::SendPacer pacer;
    for (int i = 0; i < 15; ++i) {
        sendRawRTPPacket("239.69.69.7", 15016, i, i * 48, 0x33333333, PT_AES67_L16, payload.data(), payload.size());
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto heard = reader.stopAll();
    const Excused excused(receiver, pacer);
    receiver.stop();

    for (uint32_t ch = 0; ch < 24; ++ch) {
        const bool mapped = ch >= 16 && ch < 20;
        if (!mapped) {
            TEST_ASSERT(TestSupport::countNonSilent(heard[ch]) == 0, "Channels outside the mapping should be silent");
            continue;
        }
        const float expected = 0.1f * (ch - 16 + 1);
        const auto region = TestSupport::heardRegion(heard[ch]);
        const size_t played = TestSupport::countNonSilent(heard[ch]);
        TEST_ASSERT(playedAllBut(played, 15, excused) && region.size() <= 15 * 48,
                    "Mapped channels should play every frame (channel " << ch << " got " << played << " over "
                                                                        << region.size() << "; " << excused.summary << ")");
        TEST_ASSERT(carries(region, expected, 0.02f, excused),
                    "Device channel " << ch << " should carry stream channel " << ch - 16 << " (" << excused.summary << ")");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 9: Receiver Statistics Accuracy
// ============================================================================

bool testReceiverStatistics() {
    std::cout << "Test: Receiver Statistics Accuracy... ";

    SDPSession sdp = createTestSDP("Stats RX", "239.69.69.8", 15018, 2, "L16");
    ChannelMapping mapping = createTestMapping(StreamID::generate(), "Stats RX", 2, 0);
    TestSupport::RxHarness harness;
    RTPReceiver receiver(sdp, mapping, harness.context());
    receiver.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<float> audio(48 * 2, 0.0f);
    std::vector<uint8_t> payload(audio.size() * 2);
    L16Codec::encode(audio.data(), audio.size(), payload.data());

    // Packets 0-9, then 10-14 are lost and 15 arrives on schedule
    TestSupport::SendPacer pacer;
    for (int i = 0; i < 16; ++i) {
        if (i < 10 || i == 15) {
            sendRawRTPPacket("239.69.69.8", 15018, i, i * 48, 0x44444444, PT_AES67_L16, payload.data(), payload.size());
        }
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    StatisticsSnapshot stats = receiver.getStatistics();
    TEST_ASSERT(stats.packetsReceived == 11, "Should receive all 11 packets sent");
    TEST_ASSERT(stats.bytesReceived == 11 * payload.size(), "Should count every payload byte");
    TEST_ASSERT(stats.malformedPackets == 0, "Should have no malformed packets");
    TEST_ASSERT(stats.packetsLost == 5, "Should count exactly the 5 lost packets");
    // Only a packet the machine sent late may be dropped as late
    const auto placement = receiver.getPlacementStatistics();
    TEST_ASSERT(placement.lateDrops <= pacer.lateSends() && placement.earlyDrops == 0,
                "On-time packets should not be dropped (" << Excused(receiver, pacer).summary << ")");

    receiver.stop();
    receiver.resetStatistics();
    TEST_ASSERT(receiver.getStatistics().packetsReceived == 0, "Reset should clear statistics");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 9b: Loss accounting survives reordering, duplicates and wraparound (#155)
// ============================================================================

bool testReceiverLossWithReordering() {
    std::cout << "Test: Receiver loss count with reordered, duplicate and late packets... ";

    SDPSession sdp = createTestSDP("Reorder RX", "239.69.69.10", 15022, 2, "L16");
    ChannelMapping mapping = createTestMapping(StreamID::generate(), "Reorder RX", 2, 0);
    TestSupport::RxHarness harness;
    RTPReceiver receiver(sdp, mapping, harness.context());
    receiver.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<float> audio(48 * 2, 0.0f);
    std::vector<uint8_t> payload(audio.size() * 2);
    L16Codec::encode(audio.data(), audio.size(), payload.data());

    // Packet n carries sequence 65530 + n, so the run crosses the 16-bit wrap.
    // 4 and 5 swap, 6 is duplicated, 8 arrives late after 11, and 13 is the
    // only packet really lost.
    const int order[] = {0, 1, 2, 3, 5, 4, 6, 6, 7, 9, 10, 11, 8, 12, 14, 15};
    TestSupport::SendPacer pacer;
    for (const int n : order) {
        const auto seq = static_cast<uint16_t>(65530 + n);
        sendRawRTPPacket("239.69.69.10", 15022, seq, static_cast<uint32_t>(n) * 48, 0x55555555, PT_AES67_L16,
                         payload.data(), payload.size());
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();

    const StatisticsSnapshot stats = receiver.getStatistics();
    TEST_ASSERT(stats.packetsReceived == 16, "Should receive all 16 packets sent (got " << stats.packetsReceived << ")");
    TEST_ASSERT(stats.malformedPackets == 0, "Should have no malformed packets");
    TEST_ASSERT(stats.packetsLost == 1, "Only packet 13 is lost (got " << stats.packetsLost << ")");
    TEST_ASSERT(stats.outOfOrderPackets == 3,
                "The swapped, duplicate and late packets are out of order (got " << stats.outOfOrderPackets << ")");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 9c: Loss accounting resyncs after a sender restart (#155)
// ============================================================================

bool testReceiverLossAfterSenderRestart() {
    std::cout << "Test: Receiver loss count after sender sequence restarts... ";

    SDPSession sdp = createTestSDP("Restart RX", "239.69.69.11", 15024, 2, "L16");
    ChannelMapping mapping = createTestMapping(StreamID::generate(), "Restart RX", 2, 0);
    TestSupport::RxHarness harness;
    RTPReceiver receiver(sdp, mapping, harness.context());
    receiver.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<float> audio(48 * 2, 0.0f);
    std::vector<uint8_t> payload(audio.size() * 2);
    L16Codec::encode(audio.data(), audio.size(), payload.data());

    // Ten packets from 20000, then the sender restarts 10000 lower and loses
    // only 10005, then jumps 5000 forward. The timestamps stay continuous.
    std::vector<uint16_t> sequences;
    for (uint16_t s = 20000; s < 20010; ++s) sequences.push_back(s);
    for (uint16_t s = 10000; s < 10020; ++s) {
        if (s != 10005) sequences.push_back(s);
    }
    for (uint16_t s = 15020; s < 15030; ++s) sequences.push_back(s);

    TestSupport::SendPacer pacer;
    uint32_t timestamp = 0;
    for (const uint16_t seq : sequences) {
        sendRawRTPPacket("239.69.69.11", 15024, seq, timestamp, 0x66666666, PT_AES67_L16, payload.data(),
                         payload.size());
        timestamp += 48;
        pacer.wait();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();

    const StatisticsSnapshot stats = receiver.getStatistics();
    TEST_ASSERT(stats.packetsReceived == sequences.size(),
                "Should receive all " << sequences.size() << " packets sent (got " << stats.packetsReceived << ")");
    TEST_ASSERT(stats.malformedPackets == 0, "Should have no malformed packets");
    TEST_ASSERT(stats.packetsLost == 1, "Only 10005 is lost (got " << stats.packetsLost << ")");

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Test 10: Transmitter Continuous Packet Flow
// ============================================================================

bool testTransmitterContinuousFlow() {
    std::cout << "Test: Transmitter Continuous Packet Flow (silence when nothing is written)... ";

    // AES67 requires continuous packets even when Core Audio writes nothing.
    // The transmitter should send silence-filled packets in that case.
    MediaClock clock;
    resetClock(clock);
    TxRouting routing;

    SDPSession sdp = createTestSDP("Continuous TX", "239.69.69.9", 15020, 2, "L16");
    StreamID id = StreamID::generate();
    ChannelMapping mapping = createTestMapping(id, "Continuous TX", 2, 0);

    RTPTransmitter transmitter(sdp, mapping, TxContext{clock, routing});
    bool started = transmitter.start();
    TEST_ASSERT(started, "Transmitter should start with nothing written");

    // Let it run for 50ms -- it should still be sending packets (silence)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    StatisticsSnapshot stats = transmitter.getStatistics();
    TEST_ASSERT(stats.bytesSent > 0,
                "Transmitter should send silence packets when nothing is written");

    transmitter.stop();

    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    // Send test packets like a hardware sender: a starved test thread would
    // send them after their timestamps, which the receiver rightly drops as late
    AudioThreadPriority::configureForRealTime();

    std::cout << "========================================" << std::endl;
    std::cout << "AES67 Integration Tests: Full Audio Path" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "RTP Receive Path Tests:" << std::endl;
    std::cout << "----------------------" << std::endl;
    testRTPReceiveToRingBuffer();
    std::cout << std::endl;

    std::cout << "RTP Transmit Path Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testRingBufferToRTPTransmit();
    std::cout << std::endl;

    std::cout << "Full Loopback Tests:" << std::endl;
    std::cout << "-------------------" << std::endl;
    testFullLoopback();
    std::cout << std::endl;

    std::cout << "Buffer Edge Case Tests:" << std::endl;
    std::cout << "----------------------" << std::endl;
    testUnderrunBehavior();
    testOverrunBehavior();
    std::cout << std::endl;

    std::cout << "Channel Isolation Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testMultiStreamChannelIsolation();
    std::cout << std::endl;

    std::cout << "Encoding Round-Trip Tests:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    testEncodingRoundTrip();
    std::cout << std::endl;

    std::cout << "Channel Mapping Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testChannelMappingThroughReceiver();
    std::cout << std::endl;

    std::cout << "Statistics Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testReceiverStatistics();
    testReceiverLossWithReordering();
    testReceiverLossAfterSenderRestart();
    std::cout << std::endl;

    std::cout << "Continuous Flow Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testTransmitterContinuousFlow();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
