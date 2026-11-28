//
// TestPtpServoScenarios.cpp
// AES67 macOS Driver
// The PTP servo on the networks it will meet: busy switches, lost packets,
// an asymmetric path, a grandmaster that jumps or is replaced. Each scenario
// runs 5 minutes of virtual time with three seeds and measures:
//   - restarts: new timelines the servo starts after the first lock
//   - locked: the share of time the servo reports Locked
//   - wrong while locked: how long it reports Locked while its phase is
//     off by more than the servo's lock threshold (100 us)
//   - final phase and rate error
// A change to the servo that helps one scenario must not break another, so
// every servo change runs all of them.
//
// Every scenario has limits the servo must meet. A scenario with a known gap
// also names its issue and a target: its limits are what the servo does
// today, so a change can't make it worse, and the test says when a change
// meets the target, so the target can become the limits.
//

#include "PtpServoSimulation.h"
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace PtpServoSim;

namespace {

constexpr double kWarmupSeconds = 60.0;
constexpr double kEventSeconds = 120.0;  // when a scenario's event (a jump, a new master) happens
constexpr double kEndSeconds = 360.0;
constexpr double kSampleSeconds = 0.125;
constexpr uint32_t kSeeds[] = {11, 12, 13};
// A jump is confirmed by stepConfirmSamples Syncs, 0.375 s at 8 Sync/s: the
// servo can't know sooner, so it may stay Locked on the old time that long
constexpr double kConfirmSeconds = 0.5;
// A jump down looks like queueing until a Delay_Req sent after it comes
// back: up to one Delay_Req interval (1 s here) plus a Sync
constexpr double kDownJumpSeconds = 1.125;

struct Limits {
    uint32_t minRestarts{0};
    uint32_t maxRestarts{0};
    double minLockedFraction{0.9};
    double maxWrongWhileLockedSeconds{0.0};
    double maxFinalPhaseUs{30.0};
    double maxFinalRatePpm{1.0};
};

struct KnownGap {
    std::string issue;
    Limits target;
};

struct Scenario {
    std::string name;
    double syncHz{8.0};
    Network network;
    std::function<void(Master&)> event;  // applied at kEventSeconds, if set
    Limits limits;
    double expectedPhaseBiasUs{0.0};  // a phase offset PTP cannot see (an asymmetric path)
    std::optional<KnownGap> gap;
};

struct Result {
    uint32_t restarts{0};
    double lockedFraction{0.0};
    double worstWhileLockedUs{0.0};
    double wrongWhileLockedSeconds{0.0};
    double finalPhaseUs{0.0};
    double finalRatePpm{0.0};
};

Result run(const Scenario& s, uint32_t seed) {
    Simulation sim(Master{}, s.network, s.syncHz, seed);
    sim.runUntil(kWarmupSeconds);
    const uint32_t startGeneration = sim.measure().generation;
    const double lockThresholdUs = Servo::Config{}.lockThresholdNs / 1000.0;
    Result r;
    int samples = 0;
    int locked = 0;
    bool eventDone = false;
    for (double t = kWarmupSeconds + kSampleSeconds; t <= kEndSeconds; t += kSampleSeconds) {
        if (s.event && !eventDone && t >= kEventSeconds) {
            s.event(sim.master());
            eventDone = true;
        }
        sim.runUntil(t);
        const Run m = sim.measure();
        ++samples;
        if (m.state == Servo::State::Locked) {
            ++locked;
            const double offUs = std::fabs(m.phaseErrorNs / 1000.0 - s.expectedPhaseBiasUs);
            r.worstWhileLockedUs = std::max(r.worstWhileLockedUs, offUs);
            if (offUs > lockThresholdUs) r.wrongWhileLockedSeconds += kSampleSeconds;
        }
    }
    const Run end = sim.measure();
    r.restarts = end.generation - startGeneration;
    r.lockedFraction = static_cast<double>(locked) / samples;
    r.finalPhaseUs = std::fabs(end.phaseErrorNs / 1000.0 - s.expectedPhaseBiasUs);
    r.finalRatePpm = std::fabs(end.rateErrorPpm);
    return r;
}

std::vector<std::string> failures(const Result& r, const Limits& l) {
    std::vector<std::string> f;
    char buf[160];
    if (r.restarts < l.minRestarts || r.restarts > l.maxRestarts) {
        std::snprintf(buf, sizeof buf, "%u restarts, expected %u to %u", r.restarts, l.minRestarts, l.maxRestarts);
        f.emplace_back(buf);
    }
    if (r.lockedFraction < l.minLockedFraction) {
        std::snprintf(buf, sizeof buf, "locked %.0f%% of the time, needs %.0f%%", 100 * r.lockedFraction,
                      100 * l.minLockedFraction);
        f.emplace_back(buf);
    }
    if (r.wrongWhileLockedSeconds > l.maxWrongWhileLockedSeconds) {
        std::snprintf(buf, sizeof buf, "Locked but over 100 us off for %.3f s (worst %.0f us), limit %.3f s",
                      r.wrongWhileLockedSeconds, r.worstWhileLockedUs, l.maxWrongWhileLockedSeconds);
        f.emplace_back(buf);
    }
    if (r.finalPhaseUs > l.maxFinalPhaseUs) {
        std::snprintf(buf, sizeof buf, "final phase off by %.1f us, limit %.0f us", r.finalPhaseUs, l.maxFinalPhaseUs);
        f.emplace_back(buf);
    }
    if (r.finalRatePpm > l.maxFinalRatePpm) {
        std::snprintf(buf, sizeof buf, "final rate off by %.2f ppm, limit %.1f ppm", r.finalRatePpm,
                      l.maxFinalRatePpm);
        f.emplace_back(buf);
    }
    return f;
}

Network congested(double minMs, double maxMs, double seconds, bool syncOnly = false,
                  double startSeconds = kWarmupSeconds + 10.0) {
    Network n;
    n.congestion.startSeconds = startSeconds;
    n.congestion.everySeconds = 20.0;
    n.congestion.seconds = seconds;
    n.congestion.minNs = minMs * 1e6;
    n.congestion.maxNs = maxMs * 1e6;
    n.congestion.syncOnly = syncOnly;
    return n;
}

// The servo follows a jump with one new timeline, and is Locked on the old
// time only until the jump is confirmed
Limits followsJump() {
    Limits l;
    l.minRestarts = 1;
    l.maxRestarts = 1;
    l.minLockedFraction = 0.8;
    l.maxWrongWhileLockedSeconds = kConfirmSeconds;
    return l;
}

Scenario jump(double stepMs) {
    Scenario s;
    char name[64];
    std::snprintf(name, sizeof name, "master jumps %+g ms", stepMs);
    s.name = name;
    const auto stepNs = static_cast<int64_t>(stepMs * 1e6);
    s.event = [stepNs](Master& m) { m.stepNs = stepNs; };
    s.limits = followsJump();
    return s;
}

// #138, #226: a small jump down is told apart from queueing by the next
// Delay_Req, so it may stay Locked on the old time longer than a jump up
Scenario smallDownJump(double stepMs) {
    Scenario s = jump(stepMs);
    s.limits.maxWrongWhileLockedSeconds = kDownJumpSeconds;
    return s;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> all;

    for (double hz : {1.0, 8.0, 16.0}) {
        Scenario s;
        s.name = "quiet network, " + std::to_string(static_cast<int>(hz)) + " Sync/s";
        s.syncHz = hz;
        all.push_back(s);
    }

    // A busy switch or Wi-Fi link: every packet held up for a few seconds,
    // every 20 s, from after the first lock. Nothing about the master
    // changed, so nothing may restart
    struct Busy { double minMs, maxMs, seconds; bool syncOnly; };
    for (const Busy b : {Busy{0.6, 1.5, 3, false}, Busy{1.2, 2.0, 2, false}, Busy{1.5, 3.0, 2, false},
                         Busy{1.5, 3.0, 5, false}, Busy{3.0, 5.0, 2, false}, Busy{1.5, 3.0, 2, true}}) {
        Scenario s;
        char name[96];
        std::snprintf(name, sizeof name, "%s held %g-%g ms for %g s every 20 s", b.syncOnly ? "Syncs" : "all packets",
                      b.minMs, b.maxMs, b.seconds);
        s.name = name;
        s.network = congested(b.minMs, b.maxMs, b.seconds, b.syncOnly);
        all.push_back(s);
    }

    {
        // #213: the first line is fitted to queued Syncs and the path delay
        // measured against it stays for 64 s. Today: Locked but about one
        // path delay off for about 13 s
        Scenario s;
        s.name = "starts while all packets are held 1.5-3 ms for 5 s every 20 s";
        s.network = congested(1.5, 3.0, 5, false, 0.0);
        s.limits.maxWrongWhileLockedSeconds = 20.0;
        Limits target = s.limits;
        target.maxWrongWhileLockedSeconds = 0.0;
        s.gap = KnownGap{"#213", target};
        all.push_back(s);
    }

    for (double loss : {0.05, 0.3}) {
        Scenario s;
        s.name = std::to_string(static_cast<int>(loss * 100)) + "% of packets lost";
        s.network.lossFraction = loss;
        all.push_back(s);
    }

    {
        // PTP assumes the path is symmetric; half the difference shows as a
        // constant phase offset, which is all the servo may be off by
        Scenario s;
        s.name = "Syncs take 200 us longer than Delay_Reqs";
        s.network.asymmetryNs = 200000.0;
        s.expectedPhaseBiasUs = -100.0;
        all.push_back(s);
    }

    all.push_back(jump(+20));
    all.push_back(jump(-20));
    for (double ms : {+1.0, +3.0}) all.push_back(jump(ms));
    for (double ms : {-1.0, -3.0}) all.push_back(smallDownJump(ms));

    {
        Scenario s;
        s.name = "a different grandmaster takes over";
        s.event = [](Master& m) {
            m.epochNs = kWingEpochNs;
            m.skewPpm = -30.0;
        };
        s.limits = followsJump();
        all.push_back(s);
    }
    return all;
}

}  // namespace

int main() {
    int passed = 0;
    int failed = 0;
    int gapsOpen = 0;
    for (const Scenario& s : scenarios()) {
        std::printf("%s%s\n", s.name.c_str(), s.gap ? (" (known gap " + s.gap->issue + ")").c_str() : "");
        bool meetsLimits = true;
        bool meetsTarget = true;
        for (uint32_t seed : kSeeds) {
            const Result r = run(s, seed);
            std::printf("  seed %u: %u restarts, locked %.0f%%, wrong while locked %.3f s (worst %.1f us), "
                        "final %.1f us, %.3f ppm\n",
                        seed, r.restarts, 100 * r.lockedFraction, r.wrongWhileLockedSeconds, r.worstWhileLockedUs,
                        r.finalPhaseUs, r.finalRatePpm);
            for (const std::string& f : failures(r, s.limits)) {
                meetsLimits = false;
                std::fprintf(stderr, "  FAIL: %s, seed %u: %s\n", s.name.c_str(), seed, f.c_str());
            }
            if (s.gap && !failures(r, s.gap->target).empty()) meetsTarget = false;
        }
        ++(meetsLimits ? passed : failed);
        if (s.gap) {
            if (meetsTarget) {
                std::printf("  NOTE: meets the target for %s; make the target this scenario's limits\n",
                            s.gap->issue.c_str());
            } else {
                ++gapsOpen;
            }
        }
    }
    std::printf("\nPTP servo scenarios: %d passed, %d failed, %d known gaps open\n", passed, failed, gapsOpen);
    return failed == 0 ? 0 : 1;
}
