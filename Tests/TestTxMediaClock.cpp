//
// TestTxMediaClock.cpp
// AES67 macOS Driver
// Transmit on the media clock: the IO thread writes the output mix into each
// TX stream's buffer at its media position, and the transmitter sends each
// packet when the media clock reaches its end, whether or not anything was
// written. Samples here carry their own media position (as a 24-bit value),
// so every captured packet shows exactly which positions it held.
//

#include "../NetworkEngine/RTP/RTPTransmitter.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../NetworkEngine/RTP/TxContext.h"
#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "RxTestSupport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
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

constexpr double kRate = 48000.0;
constexpr uint32_t kChannels = 2;
constexpr int64_t kFramesPerPacket = 48;
constexpr size_t kDeviceChannels = 128;

// A sample whose 24-bit PCM value is exactly its media position (the L24
// encoder truncates, hence the half step)
float positionValue(int64_t position) {
    return static_cast<float>((static_cast<double>(position) + 0.5) / 8388607.0);
}

SDPSession txSdp(const char* group, uint16_t port) {
    SDPSession sdp;
    sdp.sessionName = "TX media clock";
    sdp.connectionAddress = group;
    sdp.port = port;
    sdp.numChannels = kChannels;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = kPayloadTypeL24;
    sdp.ptime = 1;
    sdp.framecount = kFramesPerPacket;
    sdp.direction = "sendonly";
    sdp.ttl = 0;  // host-only: test streams never leave this machine
    return sdp;
}

ChannelMapping txMapping(uint32_t deviceStart) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "TX media clock";
    mapping.streamChannelCount = kChannels;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = kChannels;
    return mapping;
}

// Plays the Core Audio IO thread: keeps every published TX buffer written a
// little ahead of now, from a 128-channel mix whose every channel carries the
// sample's media position. Follows timeline restarts as the HAL would.
class PositionWriter {
public:
    PositionWriter(const MediaClock& clock, TxRouting& routing)
        : clock_(clock), routing_(routing), thread_([this] { run(); }) {}
    ~PositionWriter() { stop(); }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        AudioThreadPriority::configureForRealTime();
        std::vector<float> mix(512 * kDeviceChannels);
        uint32_t generation = 0;
        int64_t next = 0;
        while (running_) {
            const MediaClock::Snapshot snapshot = clock_.snapshot();
            const int64_t now = snapshot.sampleAt(hostTimeNow());
            if (snapshot.generation != generation) {
                generation = snapshot.generation;
                next = now;
            }
            const int64_t target = now + 256;  // output is written ahead of its time
            while (next < target) {
                const auto frames = static_cast<size_t>(std::min<int64_t>(target - next, 512));
                for (size_t f = 0; f < frames; ++f) {
                    std::fill_n(mix.begin() + static_cast<ptrdiff_t>(f * kDeviceChannels), kDeviceChannels,
                                positionValue(next + static_cast<int64_t>(f)));
                }
                routing_.read([&](const TxRouting::Route& route) {
                    route.buffer->write(next, frames, mix.data(), kDeviceChannels, route.deviceChannelStart);
                });
                next += static_cast<int64_t>(frames);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    const MediaClock& clock_;
    TxRouting& routing_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

struct CapturedPacket {
    uint64_t arrivalHost;
    uint16_t sequence;
    uint32_t timestamp;
    std::vector<int32_t> channel0;  // PCM of the first channel, per frame
    std::vector<int32_t> channel1;
};

int32_t decodeL24(const uint8_t* p) {
    const uint32_t raw = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) << 8);
    return static_cast<int32_t>(raw) >> 8;
}

// Receives the group with kernel arrival timestamps
class PacketCapture {
public:
    PacketCapture(const char* group, uint16_t port) {
        CHECK(socket_.openReceiver(group, port), "capture socket should open");
        thread_ = std::thread([this] { run(); });
    }
    ~PacketCapture() { stop(); }

    std::vector<CapturedPacket> stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        socket_.close();
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_;
    }

private:
    void run() {
        AudioThreadPriority::configureForRealTime();
        std::vector<uint8_t> buffer(2048);
        while (running_) {
            RTP::RTPPacket packet;
            uint64_t arrival = 0;
            if (socket_.receive(packet, buffer.data(), buffer.size(), &arrival) <= 0 || !packet.payload) continue;
            CapturedPacket captured{arrival != 0 ? arrival : hostTimeNow(), packet.header.sequenceNumber,
                                    packet.header.timestamp, {}, {}};
            const size_t frames = packet.payloadSize / (3 * kChannels);
            for (size_t f = 0; f < frames; ++f) {
                captured.channel0.push_back(decodeL24(packet.payload + (f * kChannels) * 3));
                captured.channel1.push_back(decodeL24(packet.payload + (f * kChannels + 1) * 3));
            }
            std::lock_guard<std::mutex> lock(mutex_);
            packets_.push_back(std::move(captured));
        }
    }

    RTP::RTPSocket socket_;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<CapturedPacket> packets_;
};

struct TxHarness {
    MediaClock clock;
    TxRouting routing;
    NetworkTimeMapping networkTime;  // fixed at PTP's offset while PTP has the clock
    TxHarness() { clock.reset(hostTimeNow(), 0, MediaClock::samplesPerTick(kRate, 1.0, HostTimebase::current())); }
    TxContext context() { return TxContext{clock, routing, &networkTime}; }
};

// Packets after the first `skip`, which may predate the writer's first write
std::vector<CapturedPacket> settled(std::vector<CapturedPacket> packets, size_t skip = 20) {
    if (packets.size() <= skip) return {};
    return std::vector<CapturedPacket>(packets.begin() + static_cast<ptrdiff_t>(skip), packets.end());
}

bool consecutive(const std::vector<CapturedPacket>& packets) {
    for (size_t i = 1; i < packets.size(); ++i) {
        if (static_cast<uint16_t>(packets[i].sequence - packets[i - 1].sequence) != 1) return false;
        if (packets[i].timestamp - packets[i - 1].timestamp != static_cast<uint32_t>(kFramesPerPacket)) return false;
    }
    return true;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(p * static_cast<double>(values.size() - 1))];
}

void testPacketsCarryTheirMediaPositions() {
    std::cout << "TX packets carry the media positions they are stamped with, and leave on time" << std::endl;
    TxHarness h;
    PositionWriter writer(h.clock, h.routing);
    PacketCapture capture("239.69.99.40", 55070);
    RTPTransmitter tx(txSdp("239.69.99.40", 55070), txMapping(4), h.context());
    CHECK(tx.framesPerPacket() == kFramesPerPacket, "48 kHz at 1 ms should be 48 frames per packet");
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    tx.stop();
    writer.stop();
    const auto packets = settled(capture.stop());

    CHECK(packets.size() > 1300, "about 1500 packets should arrive (got " << packets.size() << ")");
    CHECK(consecutive(packets), "sequence numbers and timestamps should advance by one packet each");

    bool contiguous = true;
    bool sameOffset = true;
    bool channelsMatch = true;
    std::vector<double> lateness;
    const auto clock = h.clock.snapshot();
    const double ticksPerSecond = HostTimebase::current().ticksPerSecond();
    const int64_t offset = static_cast<int64_t>(packets.front().timestamp) - packets.front().channel0.front();
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& p = packets[i];
        const int64_t position = p.channel0.front();
        for (size_t f = 0; f < p.channel0.size(); ++f) {
            contiguous = contiguous && p.channel0[f] == position + static_cast<int64_t>(f);
        }
        channelsMatch = channelsMatch && p.channel0 == p.channel1;
        sameOffset = sameOffset && static_cast<int64_t>(p.timestamp) - position == offset;
        if (i > 0) contiguous = contiguous && position == packets[i - 1].channel0.front() + kFramesPerPacket;
        const uint64_t due = clock.hostAt(position + kFramesPerPacket);
        lateness.push_back((static_cast<double>(p.arrivalHost) - static_cast<double>(due)) / ticksPerSecond * 1e6);
    }
    CHECK(contiguous, "each packet should hold the next 48 media positions, in order");
    CHECK(channelsMatch, "both stream channels should carry their mapped device channels");
    CHECK(sameOffset, "the RTP timestamp should stay a fixed offset from the media position");

    const double median = percentile(lateness, 0.5);
    const double p99 = percentile(lateness, 0.99);
    const double earliest = percentile(lateness, 0.0);
    std::cout << "  departure after the packet's last sample: median " << median << " us, p99 " << p99
              << " us, earliest " << earliest << " us" << std::endl;
    CHECK(earliest > -100.0, "no packet should leave before its last sample's media time");
    CHECK(median < 500.0, "packets should leave within half a packet of their media time (median " << median << " us)");
    CHECK(p99 < 2000.0, "almost no packet should be more than 2 ms late (p99 " << p99 << " us)");
}

void testSilenceFlowsWithoutTheIOThread() {
    std::cout << "TX sends silence, continuously, when nothing writes the buffer" << std::endl;
    TxHarness h;
    PacketCapture capture("239.69.99.41", 55072);
    RTPTransmitter tx(txSdp("239.69.99.41", 55072), txMapping(0), h.context());
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    tx.stop();
    const auto packets = settled(capture.stop(), 2);

    CHECK(packets.size() > 900 && packets.size() < 1100, "about 1000 packets per second (got " << packets.size() << ")");
    CHECK(consecutive(packets), "silent packets should still advance one packet at a time");
    bool silent = true;
    for (const auto& p : packets) {
        silent = silent && std::all_of(p.channel0.begin(), p.channel0.end(), [](int32_t v) { return v == 0; });
    }
    CHECK(silent, "unwritten media positions should be sent as silence");
}

void testFollowsTheMediaClockRate() {
    std::cout << "TX follows the media clock's rate (e.g. a recovered clock)" << std::endl;
    TxHarness h;
    h.clock.setRate(hostTimeNow(), MediaClock::samplesPerTick(kRate, 1.0 + 1000e-6, HostTimebase::current()));
    PacketCapture capture("239.69.99.42", 55074);
    RTPTransmitter tx(txSdp("239.69.99.42", 55074), txMapping(0), h.context());
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    tx.stop();
    const auto packets = settled(capture.stop());

    // Least-squares packets per host second, against the nominal 1000
    const double ticksPerSecond = HostTimebase::current().ticksPerSecond();
    double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
    const double n = static_cast<double>(packets.size());
    for (size_t i = 0; i < packets.size(); ++i) {
        const double x = static_cast<double>(packets[i].arrivalHost - packets.front().arrivalHost) / ticksPerSecond;
        const double y = static_cast<double>(i);
        sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
    }
    const double packetsPerSecond = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
    const double ppm = (packetsPerSecond / 1000.0 - 1.0) * 1e6;
    std::cout << "  packet rate " << ppm << " ppm against nominal (clock at +1000 ppm)" << std::endl;
    CHECK(std::fabs(ppm - 1000.0) < 100.0, "the packet rate should follow the media clock (" << ppm << " ppm)");
}

void testTimelineRestartKeepsRtpContinuous() {
    std::cout << "A timeline restart keeps the RTP stream continuous" << std::endl;
    TxHarness h;
    PositionWriter writer(h.clock, h.routing);
    PacketCapture capture("239.69.99.43", 55076);
    RTPTransmitter tx(txSdp("239.69.99.43", 55076), txMapping(0), h.context());
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // IO restart, as the device does it: a new timeline (generation) whose
    // media positions continue beyond any the old one used
    const uint64_t restartHost = hostTimeNow();
    h.clock.reset(restartHost, h.clock.snapshot().sampleAt(restartHost) + 65536,
                  MediaClock::samplesPerTick(kRate, 1.0, HostTimebase::current()));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    tx.stop();
    writer.stop();
    const auto packets = settled(capture.stop());

    CHECK(consecutive(packets), "RTP timestamps and sequence numbers must not jump at a timeline restart");
    // The writer (like the HAL) starts on the new timeline within a packet or
    // two, mid-packet, so a packet or two around the restart is wholly or
    // partly silent. A packet's position comes from its first audible frame.
    std::vector<int64_t> carried;
    size_t silentPackets = 0;
    for (const auto& p : packets) {
        const auto audible = std::find_if(p.channel0.begin(), p.channel0.end(), [](int32_t v) { return v != 0; });
        if (audible != p.channel0.begin()) ++silentPackets;
        if (audible != p.channel0.end()) carried.push_back(*audible - (audible - p.channel0.begin()));
    }
    size_t positionJumps = 0;
    for (size_t i = 1; i < carried.size(); ++i) {
        if (carried[i] != carried[i - 1] + kFramesPerPacket) ++positionJumps;
    }
    CHECK(positionJumps == 1, "the carried media positions should jump once, at the restart (got " << positionJumps << ")");
    CHECK(silentPackets <= 3, "at most a few packets around the restart should be silent (got " << silentPackets << ")");
    CHECK(tx.getTransmitStatistics().timelineRestarts == 1, "the transmitter should count the restart");
}

void testLoopbackThroughOurReceiverIsSampleExact() {
    std::cout << "TX to RX loopback on one media clock is sample-exact" << std::endl;
    RxHarness rx;
    TxRouting txRouting;
    PositionWriter writer(rx.clock, txRouting);
    RTPTransmitter tx(txSdp("239.69.99.44", 55078), txMapping(0), TxContext{rx.clock, txRouting});
    SDPSession rxSdp = txSdp("239.69.99.44", 55078);
    rxSdp.direction = "recvonly";
    ChannelMapping rxMapping = txMapping(0);
    RTPReceiver receiver(rxSdp, rxMapping, rx.context());
    CHECK(receiver.start(), "receiver should start");
    PlayoutReader reader(rx, 0);
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    tx.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto heard = heardRegion(reader.stop());
    receiver.stop();
    writer.stop();

    // Every heard sample should be the next media position: decoded L24 is
    // pcm / 2^23, so consecutive positions differ by exactly 2^-23
    size_t breaks = 0;
    for (size_t i = 1; i < heard.size(); ++i) {
        if (heard[i] - heard[i - 1] != 1.0f / 8388608.0f) ++breaks;
    }
    std::cout << "  heard " << heard.size() << " samples, " << breaks << " discontinuities" << std::endl;
    CHECK(heard.size() > 48000, "over a second of audio should be heard (got " << heard.size() << ")");
    CHECK(breaks == 0, "the received audio should be exactly the transmitted positions, in order");
    CHECK(receiver.getPlacementStatistics().reanchors == 0, "the receiver should never need to re-anchor");
}

// With PTP, RTP timestamps are PTP time plus the SDP's mediaclk offset; the
// device's fixed mapping (media position minus PTP time) converts
void testPtpTimestamps() {
    std::cout << "PTP: RTP = media position - PTP offset + mediaclk; continuous again after PTP" << std::endl;
    TxHarness h;
    PositionWriter writer(h.clock, h.routing);
    PacketCapture capture("239.69.99.47", 55084);
    SDPSession sdp = txSdp("239.69.99.47", 55084);
    const uint32_t mediaclk = 4000000000u;
    sdp.mediaClockType = "direct=" + std::to_string(mediaclk);
    RTPTransmitter tx(sdp, txMapping(0), h.context());
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const int64_t k = 7777777;
    h.networkTime.fix(k);  // PTP takes the clock
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    h.networkTime.reset();  // PTP lets go (a timeline restart without it)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    tx.stop();
    writer.stop();
    const auto packets = settled(capture.stop());

    // Before PTP: an RTP timeline of the transmitter's own. From PTP taking
    // over: PTP-stamped, with one jump onto it. When PTP lets go the
    // timestamps carry on unbroken, which leaves them on the same line.
    size_t firstStamped = packets.size();
    size_t stamped = 0;
    size_t jumps = 0;
    size_t sequenceBreaks = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& p = packets[i];
        if (p.timestamp == static_cast<uint32_t>(p.channel0.front() - k) + mediaclk) {
            ++stamped;
            firstStamped = std::min(firstStamped, i);
        }
        if (i == 0) continue;
        if (static_cast<uint16_t>(p.sequence - packets[i - 1].sequence) != 1) ++sequenceBreaks;
        if (p.timestamp - packets[i - 1].timestamp != static_cast<uint32_t>(kFramesPerPacket)) ++jumps;
    }
    std::cout << "  " << stamped << " of " << packets.size() << " packets on PTP time from packet " << firstStamped
              << ", " << jumps << " timestamp jump(s)" << std::endl;
    CHECK(firstStamped > 300 && firstStamped < 420, "PTP stamping starts when PTP takes the clock (" << firstStamped << ")");
    CHECK(stamped == packets.size() - firstStamped, "every packet from then on is on that line");
    CHECK(jumps == 1, "one jump, onto PTP time; none on leaving it (" << jumps << ")");
    CHECK(sequenceBreaks == 0, "sequence numbers never break");
}

// Our TX into our RX, both on PTP's fixed mapping: the receiver places each
// packet by its timestamp at exactly the media position it was sent from
void testPtpLoopbackIsSampleExact() {
    std::cout << "PTP: TX to RX loopback through the fixed mapping lands each sample where it was sent from" << std::endl;
    RxHarness rx;
    const int64_t k = -31415926;
    // Before PTP: another stream anchored the shared mapping somewhere this
    // one does not fit, so it anchors on its own
    rx.networkTime.anchorIfUnset(123456);
    TxRouting txRouting;
    PositionWriter writer(rx.clock, txRouting);
    SDPSession sdp = txSdp("239.69.99.48", 55086);
    sdp.mediaClockType = "direct=2718281828";
    RTPTransmitter tx(sdp, txMapping(0), TxContext{rx.clock, txRouting, &rx.networkTime});
    SDPSession rxSdp = sdp;
    rxSdp.direction = "recvonly";
    ChannelMapping rxMapping = txMapping(0);
    RTPReceiver receiver(rxSdp, rxMapping, rx.context());
    CHECK(receiver.start(), "receiver should start");
    CHECK(tx.start(), "transmitter should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    rx.networkTime.fix(k);  // PTP takes the clock: both sides move onto PTP time
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Read back recent positions straight from the receive buffer: each must
    // hold the sample written at that position on the TX side
    const int64_t end = rx.readPositionNow();
    std::vector<float> frame(128);
    size_t checked = 0;
    size_t wrong = 0;
    size_t missing = 0;  // not (yet) received: a slow machine can deliver a packet late
    for (int64_t position = end - 2000; position < end; ++position) {
        std::fill(frame.begin(), frame.end(), 0.0f);
        rx.routing.read([&](const RxRouting::Route& route) {
            route.buffer->read(position, 1, frame.data(), 128, route.deviceChannelStart);
        });
        ++checked;
        if (frame[0] == 0.0f) {
            ++missing;
        } else if (std::fabs(static_cast<double>(frame[0]) * 8388608.0 - static_cast<double>(position)) > 1.5) {
            ++wrong;
        }
    }
    std::cout << "  " << checked << " positions read back: " << wrong << " wrong, " << missing << " missing" << std::endl;
    tx.stop();
    receiver.stop();
    writer.stop();
    CHECK(checked == 2000 && wrong == 0, wrong << " of " << checked << " positions hold another position's sample");
    CHECK(missing <= 2 * kFramesPerPacket, "and at most a couple of packets not yet arrived (" << missing << ")");
    CHECK(receiver.getPlacementStatistics().reanchors == 0, "no re-anchors: it starts over on PTP's mapping");
    CHECK(rx.networkTime.offset() == k, "PTP's mapping was never moved");
}

// A stream's TTL comes from its SDP: tests use 0 so nothing leaves the host
void testMulticastTtlFollowsTheSdp() {
    std::cout << "TX multicast TTL follows the stream's SDP" << std::endl;
    TxHarness h;
    RTPTransmitter hostOnly(txSdp("239.69.99.45", 55080), txMapping(0), h.context());
    CHECK(hostOnly.start(), "transmitter should start");
    CHECK(hostOnly.multicastTtl() == 0, "a TTL 0 stream should stay on this host (TTL " << int(hostOnly.multicastTtl()) << ")");
    hostOnly.stop();

    SDPSession routed = txSdp("239.69.99.45", 55080);
    routed.ttl = 16;
    RTPTransmitter network(routed, txMapping(2), h.context(), "127.0.0.1");  // loopback interface only
    CHECK(network.start(), "transmitter should start");
    CHECK(network.multicastTtl() == 16, "the socket should use the SDP's TTL (TTL " << int(network.multicastTtl()) << ")");
    network.stop();
}

// Streams bind to an interface by name, joining and sending by its index, so
// a new address (DHCP) cannot break them. Uses lo0, which carries multicast.
void testStreamsBindByInterfaceName() {
    std::cout << "TX and RX bind to an interface by name" << std::endl;
    RxHarness rx;
    TxRouting txRouting;
    PositionWriter writer(rx.clock, txRouting);
    SDPSession rxSdp = txSdp("239.69.99.46", 55082);
    rxSdp.direction = "recvonly";

    {
        RTPReceiver receiver(rxSdp, txMapping(0), rx.context(), "lo0");
        CHECK(receiver.start(), "a receiver on lo0 should start");
        PlayoutReader reader(rx, 0);
        RTPTransmitter tx(txSdp("239.69.99.46", 55082), txMapping(0), TxContext{rx.clock, txRouting}, "lo0");
        CHECK(tx.start(), "a transmitter on lo0 (by name) should start");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        tx.stop();
        const auto heard = heardRegion(reader.stop());
        receiver.stop();
        CHECK(heard.size() > 9600, "a receiver on lo0 hears a transmitter on lo0 (heard " << heard.size() << " samples)");
    }
    {
        RTPReceiver receiver(rxSdp, txMapping(0), rx.context(), "lo0");
        CHECK(receiver.start(), "a receiver on lo0 should start");
        RTPTransmitter tx(txSdp("239.69.99.46", 55082), txMapping(0), TxContext{rx.clock, txRouting});
        CHECK(tx.start(), "a transmitter on the default interface should start");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        tx.stop();
        receiver.stop();
        CHECK(receiver.getStatistics().packetsReceived == 0,
              "a receiver on lo0 hears nothing sent on another interface (got "
                  << receiver.getStatistics().packetsReceived << " packets)");
    }
    writer.stop();
}

// Frames per packet follow the packet time and rate, including AES67's
// fractional packet times (#22): 333 us at 48 kHz is 16 frames
void testFramesPerPacketFollowsPtime() {
    std::cout << "TX frames per packet follow ptime and the sample rate" << std::endl;
    TxHarness h;
    struct Case { double ptime; uint32_t rate; uint32_t frames; };
    const Case cases[] = {{0.125, 48000, 6}, {0.25, 48000, 12}, {0.333, 48000, 16}, {1.0, 48000, 48},
                          {4.0, 48000, 192}, {0.25, 96000, 24}, {1.0, 44100, 44}};
    for (const auto& c : cases) {
        SDPSession sdp = txSdp("239.69.99.47", 55084);
        sdp.ptime = c.ptime;
        sdp.sampleRate = c.rate;
        RTPTransmitter tx(sdp, txMapping(0), h.context());
        CHECK(tx.framesPerPacket() == c.frames,
              c.ptime << " ms at " << c.rate << " Hz should be " << c.frames << " frames (got " << tx.framesPerPacket() << ")");
    }
}

// An app's NaN sample used to leave as positive full scale: the clamp's
// comparisons are false for NaN (#60). It goes out as silence; infinities
// and samples past full scale are clipped.
void testNaNSamplesGoOutAsSilence() {
    std::cout << "#60: NaN samples go out as silence, infinities as full scale" << std::endl;
    TxHarness h;
    PacketCapture capture("239.69.99.150", 55190);
    RTPTransmitter tx(txSdp("239.69.99.150", 55190), txMapping(0), h.context());
    CHECK(tx.start(), "transmitter should start");

    // Keep the stream's buffer written ahead of now: channel 0 alternates NaN
    // and -NaN, channel 1 alternates +infinity and 2.0
    std::vector<float> frames(256 * kChannels);
    for (size_t f = 0; f < 256; ++f) {
        frames[f * kChannels] = (f % 2) ? -std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::quiet_NaN();
        frames[f * kChannels + 1] = (f % 2) ? 2.0f : std::numeric_limits<float>::infinity();
    }
    int64_t next = h.clock.snapshot().sampleAt(hostTimeNow());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        const int64_t target = h.clock.snapshot().sampleAt(hostTimeNow()) + 256;
        while (next < target) {
            h.routing.read([&](const TxRouting::Route& route) {
                route.buffer->write(next, 128, frames.data(), kChannels, 0);
            });
            next += 128;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    tx.stop();
    const auto packets = settled(capture.stop());

    CHECK(packets.size() > 300, "about 380 packets should arrive (got " << packets.size() << ")");
    bool silent = true;
    bool fullScale = true;
    for (const auto& p : packets) {
        for (int32_t sample : p.channel0) silent = silent && sample == 0;
        for (int32_t sample : p.channel1) fullScale = fullScale && sample == 8388607;
    }
    CHECK(silent, "NaN and -NaN should be sent as 0");
    CHECK(fullScale, "+infinity and 2.0 should be sent as positive full scale");
}

} // namespace

int main() {
    testNaNSamplesGoOutAsSilence();
    testPtpTimestamps();
    testPtpLoopbackIsSampleExact();
    testPacketsCarryTheirMediaPositions();
    testSilenceFlowsWithoutTheIOThread();
    testFollowsTheMediaClockRate();
    testTimelineRestartKeepsRtpContinuous();
    testLoopbackThroughOurReceiverIsSampleExact();
    testMulticastTtlFollowsTheSdp();
    testStreamsBindByInterfaceName();
    testFramesPerPacketFollowsPtime();

    std::cout << "\nTX media clock: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
