//
// TestClockRecovery.cpp
// AES67 macOS Driver
// Step 2, phase 3: the device clock follows a received stream's clock.
// ClockServo steers the media clock rate so the reference stream's margin
// (how far ahead of playout its packets land) stays constant; the
// RecoveredClockSource chooses the reference stream and publishes the rate.
// Simulations run in virtual time and are deterministic (fixed RNG seeds).
//

#include "../NetworkEngine/Clock/ClockServo.h"
#include "../NetworkEngine/Clock/RecoveredClockSource.h"
#include "../NetworkEngine/RTP/RtpPlacement.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "RxTestSupport.h"
#include "TestWait.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
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

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kFrames = 48;
constexpr int64_t kLinkOffset = 384;

// ---------------------------------------------------------------------------
// End-to-end simulation: a sender whose clock runs at (1 + ppm) relative to
// the Mac's host clock sends 1 ms packets with positive network jitter. The
// local media clock advances at ratio * nominal, steered (or not) by the
// servo. Every packet goes through the real RtpPlacement.
// ---------------------------------------------------------------------------
struct SimResult {
    uint64_t packets{0};
    uint64_t accepted{0};
    uint64_t reanchors{0};
    uint64_t drops{0};
    double maxLatencyErrorAfterSettle{0.0};  // |true latency - initial latency|, samples
    double peakLatencyError{0.0};
    double finalRatio{1.0};
    double maxRatioStepAfterSettle{0.0};
};

SimResult simulate(double senderPpm, double hours, bool servoEnabled, double maxJitterSeconds = 0.0005,
                   uint32_t seed = 1) {
    ClockServo::Config servoConfig;
    servoConfig.sampleRate = kSampleRate;
    ClockServo servo(servoConfig);

    NetworkTimeMapping mapping;
    RtpPlacement::Config placementConfig;
    placementConfig.linkOffsetFrames = kLinkOffset;
    RtpPlacement placement(placementConfig, mapping);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0.0, maxJitterSeconds);
    std::bernoulli_distribution undelayed(0.2);  // some packets see no queuing at all

    const double senderRate = kSampleRate * (1.0 + senderPpm * 1e-6);
    const double baseDelay = 0.0002;
    const double settleSeconds = 300.0;
    const auto packets = static_cast<uint64_t>(hours * 3600.0 * 1000.0);

    SimResult r;
    double ratio = 1.0;
    double localPosition = 1e6;  // local media position (samples)
    double lastTime = 0.0;
    double referenceLatency = 0.0;
    bool haveReference = false;

    for (uint64_t k = 0; k < packets; ++k) {
        const double sendTime = static_cast<double>(k * kFrames) / senderRate;
        const double arrival = sendTime + baseDelay + (undelayed(rng) ? 0.0 : jitter(rng));
        if (arrival > lastTime) {
            localPosition += (arrival - lastTime) * kSampleRate * ratio;
            lastTime = arrival;
        }
        const auto localArrival = static_cast<int64_t>(std::floor(localPosition));
        const auto result = placement.place(static_cast<uint32_t>(k * kFrames), 0x5EED, kFrames, localArrival);
        ++r.packets;
        if (result.reanchored) ++r.reanchors;
        if (result.verdict != RtpPlacement::Verdict::Accepted) {
            ++r.drops;
            continue;
        }
        ++r.accepted;

        const double margin = static_cast<double>(result.position + kFrames) - (localPosition - kLinkOffset);
        if (servoEnabled) {
            if (auto next = servo.observe(arrival, margin)) {
                if (arrival > settleSeconds) r.maxRatioStepAfterSettle = std::max(r.maxRatioStepAfterSettle, std::fabs(*next - ratio));
                ratio = *next;
            }
        }

        // Independent measurement of the latency the listener experiences:
        // the margin this packet would have had with no jitter. Measuring the
        // jittered margins directly would report jitter as latency change.
        const double undelayedArrival = sendTime + baseDelay;
        const double undelayedLocal = localPosition - (lastTime - undelayedArrival) * kSampleRate * ratio;
        const double latency = static_cast<double>(result.position + kFrames) - (undelayedLocal - kLinkOffset);
        if (!haveReference) {
            referenceLatency = latency;
            haveReference = true;
        }
        const double error = std::fabs(latency - referenceLatency);
        r.peakLatencyError = std::max(r.peakLatencyError, error);
        if (arrival > settleSeconds) r.maxLatencyErrorAfterSettle = std::max(r.maxLatencyErrorAfterSettle, error);
    }
    r.finalRatio = ratio;
    return r;
}

void testDriftWithoutServoReanchors() {
    std::cout << "Control: without the servo, drift re-anchors (phase 2 behaviour)" << std::endl;
    const SimResult r = simulate(+100.0, 1.0, false);
    CHECK(r.reanchors > 10, "a 100 ppm sender should re-anchor repeatedly without clock recovery (got " << r.reanchors << ")");
}

void runEightHours(double ppm) {
    const SimResult r = simulate(ppm, 8.0, true);
    CHECK(r.reanchors == 0, ppm << " ppm, 8 h: no re-anchors (got " << r.reanchors << ")");
    CHECK(r.drops == 0, ppm << " ppm, 8 h: no dropped packets (got " << r.drops << ")");
    CHECK(r.accepted == r.packets, ppm << " ppm, 8 h: every packet plays");
    CHECK(r.maxLatencyErrorAfterSettle <= 1.0,
          ppm << " ppm, 8 h: latency constant to +/-1 sample once settled (worst " << r.maxLatencyErrorAfterSettle << ")");
    CHECK(std::fabs((r.finalRatio - 1.0) * 1e6 - ppm) < 0.5,
          ppm << " ppm, 8 h: clock locked to the sender (ratio " << (r.finalRatio - 1.0) * 1e6 << " ppm)");
    CHECK(r.maxRatioStepAfterSettle < 5e-6,
          ppm << " ppm, 8 h: rate changes stay smooth (largest step " << r.maxRatioStepAfterSettle * 1e6 << " ppm)");
}

void testEightHoursAtPlusMinus100ppm() {
    std::cout << "Success criterion 1: 8 h at +/-100 ppm, no re-anchors, latency +/-1 sample" << std::endl;
    runEightHours(+100.0);
    runEightHours(-100.0);
}

void testTransientsStayInsideTheWindow() {
    std::cout << "Acquisition transients stay well inside the playout window" << std::endl;
    const SimResult small = simulate(+100.0, 0.25, true);
    CHECK(small.peakLatencyError < 40.0, "100 ppm: peak latency error should be small (got " << small.peakLatencyError << " samples)");
    const SimResult large = simulate(+1000.0, 0.25, true);
    CHECK(large.reanchors == 0, "1000 ppm: even a large offset must not re-anchor (got " << large.reanchors << ")");
    CHECK(large.peakLatencyError < kLinkOffset * 0.75,
          "1000 ppm: peak latency error should stay inside the window (got " << large.peakLatencyError << " samples)");

    // 3 ms of jitter leaves some 100 ms windows with no undelayed packet;
    // those must not kick the rate
    const SimResult jittery = simulate(+50.0, 0.25, true, 0.003, 7);
    CHECK(jittery.reanchors == 0 && jittery.maxLatencyErrorAfterSettle <= 1.0,
          "3 ms jitter: still locked (re-anchors " << jittery.reanchors << ", error " << jittery.maxLatencyErrorAfterSettle << ")");
    CHECK(jittery.maxRatioStepAfterSettle < 5e-6,
          "3 ms jitter: starved windows must not kick the rate (largest step " << jittery.maxRatioStepAfterSettle * 1e6 << " ppm)");
}

// ---------------------------------------------------------------------------
// RecoveredClockSource: reference selection and publishing
// ---------------------------------------------------------------------------
struct WriterLog {
    std::vector<double> ratios;
    std::vector<uint64_t> hosts;
};

void testReferenceSelection() {
    std::cout << "Recovered clock follows one reference stream" << std::endl;

    const HostTimebase timebase{1, 1};  // 1 tick = 1 ns
    WriterLog log;
    ClockServo::Config config;
    RecoveredClockSource source(config, [&](uint64_t host, double ratio) {
        log.hosts.push_back(host);
        log.ratios.push_back(ratio);
    }, timebase);

    int streamA = 0;
    int streamB = 0;
    // A arrives first and becomes the reference; B's (wildly different)
    // margins must never reach the servo
    for (int i = 0; i < 3000; ++i) {
        const uint64_t host = static_cast<uint64_t>(i) * 1000000;  // 1 ms
        source.observe(&streamA, host, 384.0 - i * 0.0048);        // A drifting slowly
        source.observe(&streamB, host + 1, 100000.0 + i);          // B nonsense
    }
    CHECK(source.reference() == &streamA, "the first stream to report should be the reference");
    CHECK(!log.ratios.empty(), "the servo should publish rates");
    bool sane = true;
    for (double ratio : log.ratios) sane = sane && std::fabs(ratio - 1.0) < 500e-6;
    CHECK(sane, "rates should come from A alone, not B's nonsense margins");

    // A goes silent: after the takeover delay B becomes the reference,
    // keeping the current rate (no step)
    const double before = source.ratio();
    const uint64_t later = 3000ULL * 1000000 + static_cast<uint64_t>(RecoveredClockSource::kTakeoverSilenceSeconds * 1.5e9);
    source.observe(&streamB, later, 384.0);
    CHECK(source.reference() == &streamB, "a silent reference should be replaced");
    CHECK(source.ratio() == before, "takeover must keep the current rate");

    // A timeline restart returns to nominal rate with no reference
    source.reset();
    CHECK(source.reference() == nullptr && source.ratio() == 1.0, "reset should clear the reference and the rate");
}

void testReanchorReacquires() {
    std::cout << "A re-anchored reference keeps its rate and re-learns its margin" << std::endl;

    const HostTimebase timebase{1, 1};
    double lastRatio = 1.0;
    ClockServo::Config config;
    RecoveredClockSource source(config, [&](uint64_t, double ratio) { lastRatio = ratio; }, timebase);
    int stream = 0;
    // Settle at a slowly shrinking margin (Mac clock 100 ppm fast)
    for (int i = 0; i < 20000; ++i) source.observe(&stream, static_cast<uint64_t>(i) * 1000000, 384.0 - i * 0.0048);
    const double settled = source.ratio();
    CHECK(settled < 1.0, "a shrinking margin should slow the local clock (ratio " << settled << ")");

    // The placement re-anchored: margins jump back up. The servo must not
    // read that jump as error.
    source.streamReanchored(&stream);
    for (int i = 20000; i < 20300; ++i) source.observe(&stream, static_cast<uint64_t>(i) * 1000000, 384.0);
    CHECK(std::fabs(source.ratio() - settled) < 2e-6, "a re-anchor must not kick the rate (moved "
                                                          << (source.ratio() - settled) * 1e6 << " ppm)");
}

void testOutageDoesNotKickTheRate() {
    std::cout << "An outage in the reference stream does not kick the rate" << std::endl;

    // Twin servos two seconds into acquisition, while the error is still large
    ClockServo::Config config;
    ClockServo interrupted(config);
    ClockServo continuous(config);
    double lastSeconds = 0.0;
    for (int i = 0; i < 2000; ++i) {
        lastSeconds = i * 0.001;
        interrupted.observe(lastSeconds, 384.0 - i * 0.0048);
        continuous.observe(lastSeconds, 384.0 - i * 0.0048);
    }
    // The same packet closes the open window: on time for one, after a 30 s
    // outage for the other. A gap carries no new information, so both must
    // make the same update; weighting the error by the gap's length did not.
    const double resumeMargin = 384.0 - 2000 * 0.0048;
    continuous.observe(lastSeconds + config.windowSeconds, resumeMargin);
    interrupted.observe(lastSeconds + 30.0, resumeMargin);
    CHECK(std::fabs(interrupted.ratio() - continuous.ratio()) < 1e-9,
          "an outage must update the rate like any other window (differs by "
              << (interrupted.ratio() - continuous.ratio()) * 1e6 << " ppm)");
}

void testConcurrentObservers() {
    std::cout << "Concurrent observers (run under ThreadSanitizer)" << std::endl;

    std::atomic<uint64_t> writes{0};
    ClockServo::Config config;
    RecoveredClockSource source(config, [&](uint64_t, double) { writes.fetch_add(1); }, HostTimebase{1, 1});
    std::vector<int> streams(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i) source.observe(&streams[t], static_cast<uint64_t>(i) * 1000000, 384.0);
        });
    }
    for (auto& th : threads) th.join();
    CHECK(source.reference() != nullptr, "one stream should hold the reference");
    CHECK(writes.load() > 0, "rates should have been published");
}

// ---------------------------------------------------------------------------
// End to end through a real RTPReceiver on multicast loopback: a sender whose
// clock runs 1500 ppm fast. The servo is faster than the device's (1 rad/s,
// settles in seconds) so the test stays short.
// ---------------------------------------------------------------------------
struct LoopbackRun {
    RTPReceiver::PlacementStatistics stats;
    double meanRatio{1.0};  // rate the clock actually ran at over the last 3 s
    size_t heard{0};
    size_t gaps{0};
};

LoopbackRun receiveDriftingSender(const char* group, uint16_t port, double senderPpm, double seconds, bool recover) {
    RxHarness harness;
    ClockServo::Config fast;
    fast.naturalFrequency = 1.0;
    RecoveredClockSource recovery(fast, [&](uint64_t, double ratio) {
        harness.clock.setRate(hostTimeNow(), MediaClock::samplesPerTick(kSampleRate, ratio, HostTimebase::current()));
    });
    RxContext context = harness.context();
    if (recover) context.clockRecovery = &recovery;

    SDPSession sdp;
    sdp.sessionName = "Drifting sender";
    sdp.connectionAddress = group;
    sdp.port = port;
    sdp.numChannels = 2;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = kPayloadTypeL24;
    sdp.ptime = 1;
    sdp.framecount = kFramesPerPacket;
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Drifting sender";
    mapping.streamChannelCount = 2;
    mapping.deviceChannelStart = 0;
    mapping.deviceChannelCount = 2;

    RTPReceiver receiver(sdp, mapping, context);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PlayoutReader reader(harness, 0);
    LoopbackSender sender(group, port);
    AudioThreadPriority::configureForRealTime();
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e6 / (1.0 + senderPpm * 1e-6)));
    const auto packets = static_cast<uint32_t>(seconds * 1000.0);
    const uint32_t measureFrom = packets > 3000 ? packets - 3000 : 0;
    uint64_t measureHost = 0;
    double measurePosition = 0.0;
    const auto positionNow = [&](uint64_t host) {
        const MediaPosition p = harness.clock.snapshot().positionAt(host);
        return static_cast<double>(p.sample) + p.fraction;
    };
    auto next = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < packets; ++i) {
        if (i == measureFrom) {
            measureHost = hostTimeNow();
            measurePosition = positionNow(measureHost);
        }
        sender.sendL24(static_cast<uint16_t>(i), i * kFramesPerPacket, 2, 0.5f);
        std::this_thread::sleep_until(next += period);
    }
    const uint64_t endHost = hostTimeNow();
    const double elapsedSeconds = static_cast<double>(endHost - measureHost) / HostTimebase::current().ticksPerSecond();
    AudioThreadPriority::restoreNormalPriority();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto region = heardRegion(reader.stop());
    LoopbackRun run;
    run.stats = receiver.getPlacementStatistics();
    receiver.stop();
    run.meanRatio = (positionNow(endHost) - measurePosition) / (kSampleRate * elapsedSeconds);
    run.heard = region.size();
    run.gaps = region.size() - countNonSilent(region);
    CHECK(recovery.reference() == nullptr, "a stopped receiver should release the reference");
    return run;
}

void testLoopbackDriftingSender() {
    std::cout << "Loopback: a 1500 ppm fast sender through a real receiver" << std::endl;
    constexpr double kPpm = 1500.0;

    const LoopbackRun control = receiveDriftingSender("239.69.99.30", 55060, kPpm, 8.0, false);
    std::cout << "  control: " << control.stats.reanchors << " re-anchors; recovered: ";
    CHECK(control.stats.reanchors >= 1,
          "control: without recovery the drift should force a re-anchor (got " << control.stats.reanchors << ")");

    const LoopbackRun recovered = receiveDriftingSender("239.69.99.31", 55062, kPpm, 8.0, true);
    std::cout << recovered.stats.reanchors << " re-anchors, " << recovered.gaps << " silent samples, mean rate "
              << (recovered.meanRatio - 1.0) * 1e6 << " ppm" << std::endl;
    CHECK(recovered.stats.reanchors == 0, "recovered: no re-anchors (got " << recovered.stats.reanchors << ")");
    // Scheduling on a loaded CI runner delays some packets past the window
    const uint64_t outsideLimit = TestWait::timingLimit<uint64_t>(2, 20);
    const size_t gapLimit = TestWait::timingLimit<size_t>(2, 10) * kFramesPerPacket;
    CHECK(recovered.stats.lateDrops + recovered.stats.earlyDrops <= outsideLimit,
          "recovered: no packets outside the window (late " << recovered.stats.lateDrops << ", early "
                                                            << recovered.stats.earlyDrops << ")");
    CHECK(recovered.gaps <= gapLimit && recovered.heard > 6 * 48000,
          "recovered: continuous audio (" << recovered.gaps << " silent samples in " << recovered.heard << ")");
    CHECK(std::fabs((recovered.meanRatio - 1.0) * 1e6 - kPpm) < 100.0,
          "recovered: device clock runs at the sender's rate (" << (recovered.meanRatio - 1.0) * 1e6 << " ppm)");
}

} // namespace

int main() {
    testDriftWithoutServoReanchors();
    testEightHoursAtPlusMinus100ppm();
    testTransientsStayInsideTheWindow();
    testReferenceSelection();
    testReanchorReacquires();
    testOutageDoesNotKickTheRate();
    testConcurrentObservers();
    testLoopbackDriftingSender();

    std::cout << "\nClock recovery: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
