//
// TestRxTimestamp.cpp
// AES67 macOS Driver
// Step 2, phase 2: placing received packets by RTP timestamp (RtpPlacement,
// NetworkTimeMapping) and the IO thread's lock-free view of the receive
// buffers (RxRouting). Placement tests are deterministic: they feed explicit
// local media positions as arrival times.
//

#include "../NetworkEngine/RTP/RtpPlacement.h"
#include "../NetworkEngine/RTP/RxRouting.h"
#include "../NetworkEngine/Clock/TimestampedAudioBuffer.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace AES67;

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

constexpr uint32_t kFrames = 48;           // 1 ms packets at 48 kHz
constexpr int64_t kLinkOffset = 8 * 48;    // 8 x packet time
// Packets in a run that confirms a timeline change (50 ms at 1 ms packets)
constexpr int kSwitchPackets = static_cast<int>(RtpPlacement::Config{}.sourceSwitchFrames / kFrames);
constexpr uint32_t kSsrcA = 0xAAAA0001;
constexpr uint32_t kSsrcB = 0xBBBB0002;

RtpPlacement::Config config(uint32_t mediaClockOffset = 0) {
    RtpPlacement::Config c;
    c.mediaClockOffset = mediaClockOffset;
    c.linkOffsetFrames = kLinkOffset;
    return c;
}

// A packet's margin: how far its end lands ahead of the read point on arrival
int64_t marginOf(const RtpPlacement::Result& r, int64_t localNow) {
    return r.position + kFrames - (localNow - kLinkOffset);
}

// ---------------------------------------------------------------------------
// Placement basics
// ---------------------------------------------------------------------------
void testFirstPacketAnchors() {
    std::cout << "First packet anchors the shared mapping" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    CHECK(!mapping.anchored(), "mapping should start unanchored");

    const auto r = stream.place(1000, kSsrcA, kFrames, 50000);
    CHECK(r.verdict == RtpPlacement::Verdict::Accepted, "first packet should be accepted");
    CHECK(r.position == 50000 - kFrames, "first packet should end at its arrival position");
    CHECK(mapping.anchored() && mapping.offset() == 50000 - kFrames - 1000, "first packet should anchor the mapping");
}

void testContiguousStreamAndWraparound() {
    std::cout << "Contiguous stream across RTP timestamp wraparound" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    uint32_t ts = 0xFFFFFFFFu - 10 * kFrames;  // wraps after 10 packets
    int64_t now = 100000;
    int64_t expected = 0;
    bool contiguous = true;
    bool accepted = true;
    for (int i = 0; i < 40; ++i, ts += kFrames, now += kFrames) {
        const auto r = stream.place(ts, kSsrcA, kFrames, now);
        accepted = accepted && r.verdict == RtpPlacement::Verdict::Accepted && !r.reanchored;
        if (i > 0) contiguous = contiguous && r.position == expected;
        expected = r.position + kFrames;
    }
    CHECK(accepted, "every in-order packet should be accepted without re-anchoring");
    CHECK(contiguous, "positions should be contiguous across the 32-bit wrap");
}

void testMediaClockOffsetIsRemoved() {
    std::cout << "SDP mediaclk offset is removed before placement" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement a(config(0), mapping);
    RtpPlacement b(config(123456), mapping);
    // Same network time: B's RTP timestamps carry its 123456 mediaclk offset
    a.place(5000, kSsrcA, kFrames, 80000);
    const auto ra = a.place(5000 + kFrames, kSsrcA, kFrames, 80000 + kFrames);
    const auto rb = b.place(5000 + kFrames + 123456, kSsrcB, kFrames, 80000 + kFrames + 7);
    CHECK(rb.verdict == RtpPlacement::Verdict::Accepted, "stream with an offset should be accepted");
    CHECK(rb.position == ra.position, "same network time should land at the same position");
}

// ---------------------------------------------------------------------------
// Late packets, jitter and second sources
// ---------------------------------------------------------------------------
void testLateAndJitteredPackets() {
    std::cout << "Late and jittered packets" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    uint32_t ts = 7000;
    int64_t now = 20000;
    for (int i = 0; i < 20; ++i, ts += kFrames, now += kFrames) stream.place(ts, kSsrcA, kFrames, now);

    const auto jittered = stream.place(ts, kSsrcA, kFrames, now + kLinkOffset / 2);
    CHECK(jittered.verdict == RtpPlacement::Verdict::Accepted, "a packet delayed by half the link offset should still play");

    const auto tooLate = stream.place(ts + kFrames, kSsrcA, kFrames, now + kFrames + kLinkOffset + kFrames);
    CHECK(tooLate.verdict == RtpPlacement::Verdict::Late, "a packet delayed beyond the link offset should be late");

    const auto ancient = stream.place(ts - 400 * kFrames, kSsrcA, kFrames, now + 2 * kFrames);
    CHECK(ancient.verdict == RtpPlacement::Verdict::Late && !ancient.reanchored, "a very old packet should be dropped as late");

    const auto next = stream.place(ts + 3 * kFrames, kSsrcA, kFrames, now + 3 * kFrames);
    CHECK(next.verdict == RtpPlacement::Verdict::Accepted && next.position == jittered.position + 3 * kFrames,
          "the stream should continue unchanged after late packets");
    CHECK(stream.lateDrops() == 2 && stream.reanchors() == 0, "two late drops, no re-anchor");
}

void testInterleavedSecondSource() {
    std::cout << "Interleaved second source is ignored" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    uint32_t tsA = 5000;
    uint32_t tsB = 2000000000u;
    int64_t now = 30000;
    size_t acceptedA = 0;
    size_t foreignB = 0;
    for (int i = 0; i < 100; ++i, tsA += kFrames, tsB += kFrames, now += kFrames) {
        acceptedA += stream.place(tsA, kSsrcA, kFrames, now).verdict == RtpPlacement::Verdict::Accepted;
        foreignB += stream.place(tsB, kSsrcB, kFrames, now + 3).verdict == RtpPlacement::Verdict::Foreign;
    }
    CHECK(acceptedA == 100, "every packet from the first source should play (got " << acceptedA << ")");
    CHECK(foreignB == 100, "every packet from the second source should be dropped (got " << foreignB << ")");
    CHECK(stream.reanchors() == 0, "interleaving must not trigger a re-anchor");
}

// ---------------------------------------------------------------------------
// Restarts
// ---------------------------------------------------------------------------
void testRestartWithNewTimestampsReanchors() {
    std::cout << "Restart with unrelated timestamps re-anchors after a sustained run" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    uint32_t ts = 9000;
    int64_t now = 40000;
    for (int i = 0; i < 20; ++i, ts += kFrames, now += kFrames) stream.place(ts, kSsrcA, kFrames, now);

    // Same SSRC, timestamps jump far away (e.g. sender rebooted)
    uint32_t restartTs = 3000000000u;
    RtpPlacement::Result r{};
    int dropped = 0;
    for (int i = 0; i < kSwitchPackets; ++i, restartTs += kFrames, now += kFrames) {
        r = stream.place(restartTs, kSsrcA, kFrames, now);
        if (r.verdict != RtpPlacement::Verdict::Accepted) ++dropped;
    }
    CHECK(dropped == kSwitchPackets - 1,
          "the new run should be dropped until it has lasted 50 ms (dropped " << dropped << ")");
    CHECK(r.verdict == RtpPlacement::Verdict::Accepted && r.reanchored, "the packet completing 50 ms should re-anchor and play");
    CHECK(r.position == now - kFrames - kFrames, "the re-anchored packet should end at its arrival");
    const auto next = stream.place(restartTs, kSsrcA, kFrames, now);
    CHECK(next.verdict == RtpPlacement::Verdict::Accepted && next.position == r.position + kFrames,
          "the restarted stream should continue contiguously");
}

void testNewSourceOnSameTimelineKeepsPlacement() {
    std::cout << "New SSRC on the same timeline switches without moving" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    uint32_t ts = 11000;
    int64_t now = 60000;
    RtpPlacement::Result last{};
    for (int i = 0; i < 20; ++i, ts += kFrames, now += kFrames) last = stream.place(ts, kSsrcA, kFrames, now);

    // A PTP-locked sender restarts with a new SSRC but its timestamps still
    // follow network time, so placement must not move
    RtpPlacement::Result r{};
    for (int i = 0; i < kSwitchPackets; ++i, ts += kFrames, now += kFrames) r = stream.place(ts, kSsrcB, kFrames, now);
    CHECK(r.verdict == RtpPlacement::Verdict::Accepted && !r.reanchored, "the new source should be adopted without re-anchoring");
    CHECK(r.position == last.position + kSwitchPackets * kFrames, "positions should continue on the same timeline");
    CHECK(stream.place(ts, kSsrcA, kFrames, now).verdict == RtpPlacement::Verdict::Foreign, "the old SSRC is now foreign");
}

// ---------------------------------------------------------------------------
// Shared mapping across streams
// ---------------------------------------------------------------------------
void testStreamsShareAlignment() {
    std::cout << "Streams on one network timeline are sample-aligned" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement a(config(), mapping);
    RtpPlacement b(config(), mapping);
    uint32_t ts = 15000;
    int64_t now = 90000;
    bool aligned = true;
    for (int i = 0; i < 50; ++i, ts += kFrames, now += kFrames) {
        const auto ra = a.place(ts, kSsrcA, kFrames, now);
        const auto rb = b.place(ts, kSsrcB, kFrames, now + 13 + (i % 7) * 5);  // B arrives with its own jitter
        aligned = aligned && ra.position == rb.position;
    }
    CHECK(aligned, "the same RTP timestamp should land at the same position on both streams despite arrival jitter");
}

void testUnrelatedClockGetsOwnAnchor() {
    std::cout << "A stream on an unrelated timeline gets its own anchor" << std::endl;

    NetworkTimeMapping mapping;
    RtpPlacement a(config(), mapping);
    RtpPlacement b(config(), mapping);
    int64_t now = 120000;
    a.place(1000, kSsrcA, kFrames, now);
    const int64_t shared = mapping.offset();

    const auto first = b.place(1500000000u, kSsrcB, kFrames, now + kFrames);
    CHECK(first.verdict == RtpPlacement::Verdict::Accepted && first.position == now + kFrames - kFrames,
          "an unrelated stream's first packet should play, anchored at its arrival");
    CHECK(mapping.offset() == shared, "the shared mapping must not move for an unrelated stream");
    const auto second = b.place(1500000000u + kFrames, kSsrcB, kFrames, now + 2 * kFrames);
    CHECK(second.verdict == RtpPlacement::Verdict::Accepted && second.position == first.position + kFrames,
          "the unrelated stream should continue contiguously");
}

// ---------------------------------------------------------------------------
// Drift (host clock source): re-anchors keep latency bounded
// ---------------------------------------------------------------------------
void simulateDrift(double senderPpm) {
    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    const double localPerNetwork = 1.0 / (1.0 + senderPpm * 1e-6);  // local samples per sender sample
    uint32_t ts = 1234;
    bool marginsInWindow = true;
    size_t accepted = 0;
    const int packets = 3600 * 1000;  // one hour
    for (int i = 0; i < packets; ++i, ts += kFrames) {
        const auto now = static_cast<int64_t>(1000000 + i * kFrames * localPerNetwork);
        const auto r = stream.place(ts, kSsrcA, kFrames, now);
        if (r.verdict == RtpPlacement::Verdict::Accepted) {
            ++accepted;
            const int64_t m = marginOf(r, now);
            marginsInWindow = marginsInWindow && m > 0 && m <= 2 * kLinkOffset;
        }
    }
    // Drift of senderPpm moves the margin by 48000 * ppm * 1e-6 samples per second;
    // a re-anchor is needed each time it crosses a link offset
    const double expected = 3600.0 * 48000.0 * std::fabs(senderPpm) * 1e-6 / kLinkOffset;
    CHECK(marginsInWindow, senderPpm << " ppm: every accepted packet should land inside the playout window");
    CHECK(stream.reanchors() >= 0.8 * expected && stream.reanchors() <= 1.25 * expected + 1,
          senderPpm << " ppm: re-anchors should match the drift (got " << stream.reanchors() << ", expected ~" << expected << ")");
    const uint64_t confirming = static_cast<uint64_t>(kSwitchPackets - 1) * stream.reanchors();
    CHECK(stream.lateDrops() + stream.earlyDrops() <= confirming,
          senderPpm << " ppm: only the packets confirming each re-anchor may be dropped");
    CHECK(accepted >= static_cast<size_t>(packets) - confirming, senderPpm << " ppm: everything else should play");
}

void testDriftIsBounded() {
    std::cout << "Drift on the host clock re-anchors with bounded latency" << std::endl;
    simulateDrift(+50.0);
    simulateDrift(-50.0);
    simulateDrift(0.0);
}

// ---------------------------------------------------------------------------
// RxRouting
// ---------------------------------------------------------------------------
void testRoutingPublishAndRead() {
    std::cout << "RxRouting publish, read and unpublish" << std::endl;

    RxRouting routing;
    TimestampedAudioBuffer a(2, 256);
    TimestampedAudioBuffer b(8, 256);
    CHECK(routing.publish(&a, 4), "publishing a route should succeed");
    CHECK(routing.publish(&b, 10), "publishing a second route should succeed");
    CHECK(!routing.publish(&b, 125), "a route past the last device channel should be rejected");

    size_t visits = 0;
    bool startsCorrect = true;
    routing.read([&](const RxRouting::Route& route) {
        ++visits;
        startsCorrect = startsCorrect && ((route.buffer == &a && route.deviceChannelStart == 4) ||
                                          (route.buffer == &b && route.deviceChannelStart == 10));
    });
    CHECK(visits == 2 && startsCorrect, "both routes should be visited with their channel starts");

    routing.unpublish(&a);
    visits = 0;
    routing.read([&](const RxRouting::Route& route) { ++visits; CHECK(route.buffer == &b, "only b should remain"); });
    CHECK(visits == 1, "an unpublished route should no longer be visited");
}

// Routes are published, unpublished and their buffers destroyed while an IO
// thread reads continuously. Unpublish must not return while a read could
// still touch the buffer (run under AddressSanitizer to catch a violation).
void testRoutingReclamationUnderLoad() {
    std::cout << "RxRouting reclamation under concurrent reads" << std::endl;

    RxRouting routing;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> badReads{0};

    std::thread io([&] {
        std::vector<float> out(64 * 128);
        while (running.load(std::memory_order_relaxed)) {
            routing.read([&](const RxRouting::Route& route) {
                // Every published buffer holds the value 1.0 at positions 0..63
                if (route.buffer->read(0, 64, out.data(), 128, route.deviceChannelStart) != 64 ||
                    out[route.deviceChannelStart] != 1.0f) {
                    badReads.fetch_add(1, std::memory_order_relaxed);
                }
            });
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // A fixed amount of work on both threads (with a generous time cap), so a
    // loaded machine slows the test down rather than failing it: the cycles
    // start once the reader runs, and go on until it has read enough
    const std::vector<float> ones(64 * 2, 1.0f);
    size_t cycles = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reads.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    while ((cycles < 500 || reads.load() <= 1000) && std::chrono::steady_clock::now() < deadline) {
        auto buffer = std::make_unique<TimestampedAudioBuffer>(2, 256);
        buffer->write(0, 64, ones.data(), 2, 0);
        routing.publish(buffer.get(), static_cast<uint32_t>(cycles % 100));
        routing.unpublish(buffer.get());
        buffer.reset();  // freed immediately after unpublish returns
        ++cycles;
    }
    running = false;
    io.join();

    CHECK(cycles > 100 && reads.load() > 1000, "both threads should have run (" << cycles << " cycles, " << reads.load() << " reads)");
    CHECK(badReads.load() == 0, "no read may see a freed or partially published buffer (got " << badReads.load() << ")");
}

// ---------------------------------------------------------------------------
// A sender or network stall followed by a catch-up burst: the late packets
// are dropped, but the burst must not re-anchor the timeline (which would add
// the stall to the latency, then jump back as the burst turns early).
// ---------------------------------------------------------------------------
void runStallAndBurst(uint32_t frames, int stallPackets, const char* label) {
    NetworkTimeMapping mapping;
    RtpPlacement::Config c = config();
    RtpPlacement stream(c, mapping);
    uint32_t ts = 4000;
    int64_t now = 200000;
    RtpPlacement::Result before{};
    for (int i = 0; i < 100; ++i, ts += frames, now += frames) before = stream.place(ts, kSsrcA, frames, now);

    // Nothing arrives for the stall, then the backlog arrives at once
    now += static_cast<int64_t>(stallPackets) * frames;
    for (int i = 0; i < stallPackets; ++i, ts += frames) stream.place(ts, kSsrcA, frames, now);

    // Back on schedule
    RtpPlacement::Result after{};
    bool allAccepted = true;
    for (int i = 0; i < 50; ++i, ts += frames, now += frames) {
        after = stream.place(ts, kSsrcA, frames, now);
        allAccepted = allAccepted && after.verdict == RtpPlacement::Verdict::Accepted;
    }
    CHECK(stream.reanchors() == 0, label << ": a stall and burst must not re-anchor (got " << stream.reanchors() << ")");
    CHECK(allAccepted, label << ": packets after the burst should all play");
    CHECK(after.position == before.position + static_cast<int64_t>(stallPackets + 50) * frames,
          label << ": the timeline should be unchanged, so latency is unchanged");
    CHECK(stream.lateDrops() > 0, label << ": the burst's late packets should be dropped");
}

void testStallAndBurstDoesNotReanchor() {
    std::cout << "A stall and catch-up burst does not re-anchor" << std::endl;
    runStallAndBurst(48, 30, "1 ms packets, 30 ms stall");
    runStallAndBurst(6, 240, "125 us packets, 30 ms stall");
    runStallAndBurst(48, 12, "1 ms packets, 12 ms stall");
}

// A stalled sender need not burst at once: in the real HAL a sender stalled
// 15 ms, then sent its next 4 packets at normal pace (all 15 ms late, so their
// margins agreed) before bursting the rest. Those 4 packets looked exactly like
// a timeline change and re-anchored, and the catch-up then re-anchored back.
// A real timeline change persists; a stall does not.
void runStallWithSteadyLateRun(int stallPackets, int steadyPackets, const char* label) {
    NetworkTimeMapping mapping;
    RtpPlacement stream(config(), mapping);
    const uint32_t frames = kFrames;
    const int64_t stall = static_cast<int64_t>(stallPackets) * frames;
    uint32_t ts = 4000;
    int64_t now = 200000;  // when the next packet is due
    RtpPlacement::Result before{};
    for (int i = 0; i < 100; ++i, ts += frames, now += frames) before = stream.place(ts, kSsrcA, frames, now);

    // The next packets arrive at the normal pace, each `stall` late
    for (int i = 0; i < steadyPackets; ++i, ts += frames, now += frames) stream.place(ts, kSsrcA, frames, now + stall);
    // The rest of the backlog arrives at once, then back on schedule
    const int64_t burstArrival = now + stall;
    int packets = 100 + steadyPackets;
    for (; now < burstArrival; ts += frames, now += frames, ++packets) stream.place(ts, kSsrcA, frames, burstArrival);
    RtpPlacement::Result after{};
    bool allAccepted = true;
    for (int i = 0; i < 50; ++i, ts += frames, now += frames, ++packets) {
        after = stream.place(ts, kSsrcA, frames, now);
        allAccepted = allAccepted && after.verdict == RtpPlacement::Verdict::Accepted;
    }
    CHECK(stream.reanchors() == 0, label << ": a stall must not re-anchor (got " << stream.reanchors() << ")");
    CHECK(allAccepted, label << ": packets after the stall should all play");
    CHECK(after.position == before.position + static_cast<int64_t>(packets - 100) * frames,
          label << ": the timeline should be unchanged");
}

void testStallWithSteadyLateRunDoesNotReanchor() {
    std::cout << "A stall whose first late packets keep pace does not re-anchor" << std::endl;
    runStallWithSteadyLateRun(15, 4, "15 ms stall, 4 steady late packets (seen in the HAL)");
    runStallWithSteadyLateRun(22, 10, "22 ms stall, 10 steady late packets");
}

} // namespace

// ---------------------------------------------------------------------------
// PTP: the mapping is PTP's offset (media position minus PTP time), fixed
// ---------------------------------------------------------------------------
void testPtpMappingPlacesByTimestamp() {
    std::cout << "PTP mapping: packets land at PTP time plus the offset, whenever they arrive" << std::endl;
    NetworkTimeMapping mapping;
    const int64_t k = 123456789;
    mapping.fix(k);
    CHECK(mapping.fixed() && mapping.anchored() && mapping.offset() == k, "fixed at PTP's offset");

    const uint32_t mediaclk = 3000000000u;  // the sender's SDP a=mediaclk:direct=
    RtpPlacement stream(config(mediaclk), mapping);
    const int64_t ptp = 4000000;  // PTP time in samples of the first packet
    for (int i = 0; i < 20; ++i) {
        const int64_t p = ptp + i * kFrames;
        const int64_t arrival = p + kFrames + k + 37 + (i % 5) * 11;  // network delay and jitter
        const auto r = stream.place(static_cast<uint32_t>(p) + mediaclk, kSsrcA, kFrames, arrival);
        CHECK(r.verdict == RtpPlacement::Verdict::Accepted && r.position == p + k,
              "packet " << i << " at PTP time + offset (" << r.position - (p + k) << " off)");
    }
    CHECK(mapping.offset() == k, "the first packet did not re-anchor it");
}

void testPtpMappingIsNeverMoved() {
    std::cout << "PTP mapping: a stream off PTP time anchors on its own; the mapping stays" << std::endl;
    NetworkTimeMapping mapping;
    const int64_t k = 5000000;
    mapping.fix(k);
    RtpPlacement onPtp(config(), mapping);
    RtpPlacement offPtp(config(), mapping);
    int64_t p = 800000;
    // The second sender's timestamps are 1 s away from PTP time (another clock)
    RtpPlacement::Result off{};
    for (int i = 0; i < 20 + kSwitchPackets; ++i, p += kFrames) {
        const int64_t arrival = p + kFrames + k + 50;
        const auto r = onPtp.place(static_cast<uint32_t>(p), kSsrcA, kFrames, arrival);
        CHECK(r.position == p + k, "the PTP stream stays at PTP time");
        off = offPtp.place(static_cast<uint32_t>(p + 48000), kSsrcB, kFrames, arrival + 3);
    }
    CHECK(off.verdict == RtpPlacement::Verdict::Accepted, "the other stream plays, on its own anchor");
    CHECK(mapping.offset() == k && mapping.fixed(), "and never moves PTP's mapping");

    // Same for a PTP stream that drifts out of the window (the sender left PTP)
    RtpPlacement drifting(config(), mapping);
    drifting.place(static_cast<uint32_t>(p), kSsrcA, kFrames, p + kFrames + k + 50);
    RtpPlacement::Result r{};
    for (int i = 0; i < kSwitchPackets + 2; ++i) {
        p += kFrames;
        r = drifting.place(static_cast<uint32_t>(p), kSsrcA, kFrames, p + kFrames + k + 50 + 3 * kLinkOffset);
    }
    CHECK(r.verdict == RtpPlacement::Verdict::Accepted && drifting.reanchors() == 1, "it re-anchors, on its own");
    CHECK(mapping.offset() == k, "PTP's mapping is not moved (" << mapping.offset() - k << ")");
}

void testPtpMappingIgnoresStragglers() {
    std::cout << "PTP mapping: a first packet off PTP time is not trusted; a sustained run is" << std::endl;
    NetworkTimeMapping mapping;
    const int64_t k = 2000000;
    mapping.fix(k);
    RtpPlacement stream(config(), mapping);
    int64_t p = 600000;
    // Two stragglers stamped before the sender moved onto PTP time
    const auto first = stream.place(static_cast<uint32_t>(p + 9999999), kSsrcA, kFrames, p + kFrames + k + 50);
    CHECK(first.verdict != RtpPlacement::Verdict::Accepted, "a first packet that does not fit is dropped");
    p += kFrames;
    stream.place(static_cast<uint32_t>(p + 9999999), kSsrcA, kFrames, p + kFrames + k + 50);
    p += kFrames;
    // Then the sender on PTP time
    const auto onPtp = stream.place(static_cast<uint32_t>(p), kSsrcA, kFrames, p + kFrames + k + 50);
    CHECK(onPtp.verdict == RtpPlacement::Verdict::Accepted && onPtp.position == p + k,
          "the next packet on PTP time plays at PTP time");
    CHECK(stream.reanchors() == 0, "no anchor of its own was taken");
}

void testFixReplacesAnArrivalAnchor() {
    std::cout << "PTP fixing the mapping replaces an arrival anchor; a timeline restart clears it" << std::endl;
    NetworkTimeMapping mapping;
    CHECK(mapping.anchorIfUnset(777) == 777 && !mapping.fixed(), "anchored by a packet's arrival");
    mapping.fix(999);
    CHECK(mapping.offset() == 999 && mapping.fixed(), "PTP's offset replaces it");
    CHECK(mapping.anchorIfUnset(1) == 999, "a first packet does not move it");
    mapping.reset();
    CHECK(!mapping.anchored() && !mapping.fixed(), "a restart clears it until PTP fixes it again");
}

int main() {
    testPtpMappingPlacesByTimestamp();
    testPtpMappingIsNeverMoved();
    testPtpMappingIgnoresStragglers();
    testFixReplacesAnArrivalAnchor();
    testFirstPacketAnchors();
    testContiguousStreamAndWraparound();
    testMediaClockOffsetIsRemoved();
    testLateAndJitteredPackets();
    testInterleavedSecondSource();
    testRestartWithNewTimestampsReanchors();
    testNewSourceOnSameTimelineKeepsPlacement();
    testStreamsShareAlignment();
    testUnrelatedClockGetsOwnAnchor();
    testDriftIsBounded();
    testStallAndBurstDoesNotReanchor();
    testStallWithSteadyLateRunDoesNotReanchor();
    testRoutingPublishAndRead();
    testRoutingReclamationUnderLoad();

    std::cout << "\nRX timestamp placement: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
