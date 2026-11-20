//
// TestPtpServo.cpp
// AES67 macOS Driver
// The PTP servo: from Sync and Delay_Req timestamps to a model of the
// master's clock in host time. A simulated master with a skewed clock, a
// network delay and positive jitter (queueing only ever delays) checks the
// plan's exit targets: frequency lock within about 30 s and phase settled
// within about 2 minutes at 1 Sync per second. Every timestamp also carries
// +/-5 us of noise either way, as kernel timestamps do. Deterministic (fixed
// seeds).
//

#include "PtpServoSimulation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

using namespace AES67::Ptp;

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

using namespace PtpServoSim;

void testLocksAtOneSyncPerSecond() {
    std::cout << "1 Sync/s, +50 ppm, 0.5 ms jitter: frequency in 30 s, phase in 2 min" << std::endl;
    Simulation sim(Master{}, Network{}, 1.0);
    sim.runUntil(30);
    const Run at30 = sim.measure();
    std::cout << "  30 s: rate error " << at30.rateErrorPpm << " ppm" << std::endl;
    CHECK(std::fabs(at30.rateErrorPpm) < 0.5, "frequency within 0.5 ppm by 30 s (" << at30.rateErrorPpm << ")");
    sim.runUntil(120);
    const Run at120 = sim.measure();
    std::cout << "  120 s: phase error " << at120.phaseErrorNs / 1000 << " us, path delay "
              << at120.pathDelayNs / 1000 << " us" << std::endl;
    CHECK(std::fabs(at120.phaseErrorNs) < 20000, "phase within 20 us by 2 min (" << at120.phaseErrorNs << " ns)");
    CHECK(at120.state == Servo::State::Locked, "locked by 2 min");
    CHECK(std::fabs(at120.pathDelayNs - 300000) < 5000, "path delay within 5 us of 300 us (" << at120.pathDelayNs << ")");
}

void testFasterSyncLocksSooner() {
    std::cout << "8 Sync/s: phase settled within 30 s" << std::endl;
    Simulation sim(Master{}, Network{}, 8.0, 2);
    sim.runUntil(30);
    const Run r = sim.measure();
    CHECK(std::fabs(r.phaseErrorNs) < 20000 && r.state == Servo::State::Locked,
          "locked with phase within 20 us (" << r.phaseErrorNs << " ns)");
}

void testEpochsAndSkews() {
    std::cout << "Any epoch and skew: TAI and a console's power-on time, -100 to +100 ppm" << std::endl;
    for (uint64_t epoch : {kTaiEpochNs, kWingEpochNs}) {
        for (double skew : {-100.0, 0.0, 100.0}) {
            Master m;
            m.epochNs = epoch;
            m.skewPpm = skew;
            Simulation sim(m, Network{}, 4.0, 3);
            sim.runUntil(90);
            const Run r = sim.measure();
            CHECK(std::fabs(r.phaseErrorNs) < 20000 && std::fabs(r.rateErrorPpm) < 0.5,
                  (epoch == kTaiEpochNs ? "TAI" : "power-on") << " epoch, " << skew << " ppm: phase "
                      << r.phaseErrorNs << " ns, rate " << r.rateErrorPpm << " ppm");
        }
    }
}

void testOutliersAreIgnored() {
    std::cout << "Packets held up 50 ms (1 in 50) do not move the clock" << std::endl;
    Network network;
    network.outlierFraction = 0.02;
    Simulation sim(Master{}, network, 4.0, 4);
    sim.runUntil(120);
    const Run r = sim.measure();
    CHECK(std::fabs(r.phaseErrorNs) < 20000 && r.state == Servo::State::Locked,
          "still locked within 20 us (" << r.phaseErrorNs << " ns)");
}

void testLockIsJudgedIndependently() {
    std::cout << "Lock is judged by prediction, so garbage never looks locked" << std::endl;
    Simulation early(Master{}, Network{}, 1.0, 5);
    early.runUntil(3);
    CHECK(early.measure().state == Servo::State::Acquiring, "not locked on a few samples");

    // Two masters on one domain, interleaved: their times disagree by seconds
    Servo servo;
    Master a, b;
    b.epochNs += 7000000000ULL;
    bool everLocked = false;
    for (int i = 1; i <= 200; ++i) {
        const uint64_t host = static_cast<uint64_t>(i) * 250000000ULL;
        const Master& m = (i % 2) ? a : b;
        servo.onSync(host + 300000, m.at(host), 0.0);
        everLocked = everLocked || servo.state() == Servo::State::Locked;
    }
    CHECK(!everLocked, "interleaved masters must never be reported as locked");

    // Timestamps that are wrong by a few milliseconds (Syncs paired with the
    // wrong Follow_Up, or two closely matched masters): never rejected as
    // steps, but never predictable either
    Servo scrambled;
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> wrongBy(-3e6, 3e6);
    Master m;
    bool lockedOnScrambled = false;
    for (int i = 1; i <= 480; ++i) {
        const uint64_t host = static_cast<uint64_t>(i) * 250000000ULL;
        scrambled.onSync(host + 300000, m.at(host) + static_cast<uint64_t>(std::llround(wrongBy(rng))), 0.0);
        lockedOnScrambled = lockedOnScrambled || scrambled.state() == Servo::State::Locked;
    }
    CHECK(!lockedOnScrambled, "timestamps wrong by milliseconds must never be reported as locked");
}

void testGrandmasterTimeStep() {
    std::cout << "A jump in the master's time is detected and followed" << std::endl;
    Simulation sim(Master{}, Network{}, 4.0, 6);
    sim.runUntil(60);
    const uint32_t before = sim.measure().generation;
    sim.master().stepNs = 1000000000;  // the grandmaster's time jumps 1 s
    sim.runUntil(180);
    const Run r = sim.measure();
    CHECK(r.generation == before + 1, "the jump starts one new timeline (generation " << r.generation << ")");
    CHECK(std::fabs(r.phaseErrorNs) < 20000 && r.state == Servo::State::Locked,
          "and locks to the new time (" << r.phaseErrorNs << " ns)");
}

// #138: a step of a few ms is below the 10 ms step threshold; it must be
// confirmed as a step, not fitted as ordinary Syncs that tilt the line (or,
// negative, left on the old line while the Syncs fall below it)
void testSmallGrandmasterStepIsFollowed() {
    std::cout << "A jump of 1, 3 or 5 ms, either way, in the master's time is detected and followed" << std::endl;
    for (const int64_t stepNs : {1000000LL, -1000000LL, 3000000LL, -3000000LL, 5000000LL, -5000000LL}) {
        Simulation sim(Master{}, Network{}, 8.0, 8);
        sim.runUntil(60);
        const uint32_t before = sim.measure().generation;
        sim.master().stepNs = stepNs;
        // Never Locked while the phase is more than lockThreshold off, once the
        // step has been in the recent Syncs long enough to be judged (a step
        // down is only judged on the median of stepBiasSamples, 1 s at 8 Sync/s)
        int lockedWhileWrong = 0;
        for (double t = 60.125; t <= 120.0; t += 0.125) {
            sim.runUntil(t);
            const Run s = sim.measure();
            if (t > 61.0 && std::fabs(s.phaseErrorNs) > Servo::Config{}.lockThresholdNs &&
                s.state == Servo::State::Locked) {
                ++lockedWhileWrong;
            }
        }
        const Run r = sim.measure();
        CHECK(r.generation == before + 1,
              "a " << stepNs / 1000000 << " ms step starts one new timeline (generation " << r.generation << ")");
        CHECK(std::fabs(r.rateErrorPpm) < 5.0,
              "a " << stepNs / 1000000 << " ms step leaves the rate within 5 ppm (" << r.rateErrorPpm << " ppm)");
        CHECK(std::fabs(r.phaseErrorNs) < 50000 && r.state == Servo::State::Locked,
              "a " << stepNs / 1000000 << " ms step is locked to within 50 us (" << r.phaseErrorNs << " ns)");
        CHECK(std::fabs(r.pathDelayNs - 300000) < 5000,
              "a " << stepNs / 1000000 << " ms step: path delay within 5 us of 300 us (" << r.pathDelayNs << ")");
        CHECK(lockedWhileWrong == 0,
              "a " << stepNs / 1000000 << " ms step: locked while the phase was off by over 100 us at "
                   << lockedWhileWrong << " points after the first second");
    }
}

// #138: a single Sync a little above the line (not a step) must not start a
// new timeline or drop lock: queueing never makes a Sync early, so only a
// sustained bias counts
void testIsolatedPositiveExcursionIsNotAStep() {
    std::cout << "Isolated Syncs 1.5 ms above the line do not start a new timeline or drop lock" << std::endl;
    Servo servo;
    Master m;
    for (int i = 1; i <= 800; ++i) {
        const uint64_t host = static_cast<uint64_t>(i) * 125000000ULL;
        uint64_t master = m.at(host);
        if (i > 200 && i % 25 == 0) master += 1500000;  // one Sync in 25, 1.5 ms late on the master side
        servo.onSync(host + 300000, master, 0.0);
    }
    CHECK(servo.generation() == 1, "isolated excursions start no new timeline (generation " << servo.generation() << ")");
    CHECK(servo.state() == Servo::State::Locked, "still locked after them (state " << int(servo.state()) << ")");
}

// #211: sustained receive queueing creates a negative Sync bias, but cannot
// move the reciprocal Delay_Req measurements down like a master-time step.
void testSustainedQueueBurstsAreNotSteps() {
    std::cout << "Sustained millisecond queue bursts do not start a new timeline" << std::endl;
    struct Burst { double minNs; double maxNs; double durationSeconds; };
    for (const Burst burst : {Burst{1200000.0, 2000000.0, 2.0},
                              Burst{1500000.0, 3000000.0, 2.0},
                              Burst{1500000.0, 3000000.0, 5.0},
                              Burst{1000000.0, 3000000.0, 3.0}}) {
        Network network;
        network.burstEverySeconds = 20.0;
        network.burstSeconds = burst.durationSeconds;
        network.burstMinNs = burst.minNs;
        network.jitterNs = burst.maxNs;
        Simulation sim(Master{}, network, 8.0, 3);
        double changedAt = 0.0;
        for (double t = 0.125; t <= 300.0; t += 0.125) {
            sim.runUntil(t);
            if (changedAt == 0.0 && sim.measure().generation != 1) changedAt = t;
        }
        CHECK(sim.measure().generation == 1,
              "queueing " << burst.minNs / 1e6 << "-" << burst.maxNs / 1e6 << " ms for "
                          << burst.durationSeconds << " s does not change generation ("
                          << sim.measure().generation << ", first change at " << changedAt << " s)");
    }
}

void testHoldover() {
    std::cout << "Holdover keeps time when the master goes quiet, then resumes" << std::endl;
    Simulation sim(Master{}, Network{}, 4.0, 7);
    sim.runUntil(120);
    const uint32_t generation = sim.measure().generation;
    sim.servo.holdover(sim.nowNs());
    sim.skipTo(150);  // 30 s without the master
    sim.servo.tick(sim.nowNs());
    const Run quiet = sim.measure();
    CHECK(quiet.state == Servo::State::Holdover, "in holdover");
    CHECK(std::fabs(quiet.phaseErrorNs) < 50000, "30 s of holdover drifts under 50 us (" << quiet.phaseErrorNs << " ns)");
    sim.runUntil(200);
    const Run back = sim.measure();
    CHECK(back.state == Servo::State::Locked && back.generation == generation,
          "the master's return resumes the same timeline, without a step");
    CHECK(std::fabs(back.phaseErrorNs) < 20000, "within 20 us again (" << back.phaseErrorNs << " ns)");
}

// #109: a master that returns on a new epoch must not report Locked while
// its first Syncs are only step candidates
void testHoldoverStepIsNotLocked() {
    std::cout << "A new epoch after holdover stays in holdover until the step is confirmed" << std::endl;
    Simulation sim(Master{}, Network{}, 4.0, 7);
    sim.runUntil(120);
    const uint32_t generation = sim.measure().generation;
    CHECK(sim.measure().state == Servo::State::Locked, "locked before the master goes");
    sim.servo.holdover(sim.nowNs());
    sim.master().stepNs = 1000000000;  // the master reboots 1 s off
    const size_t confirm = Servo::Config{}.stepConfirmSamples;
    for (size_t i = 1; i < confirm; ++i) {
        sim.runUntil(120.0 + 0.25 * static_cast<double>(i));  // one Sync
        CHECK(sim.measure().state == Servo::State::Holdover,
              "rejected Sync " << i << ": still in holdover (state " << int(sim.measure().state) << ")");
    }
    sim.runUntil(120.0 + 0.25 * static_cast<double>(confirm));
    const Run stepped = sim.measure();
    CHECK(stepped.state == Servo::State::Acquiring && stepped.generation == generation + 1,
          "the confirmed step starts a new timeline, acquiring (state " << int(stepped.state) << ", generation "
                                                                        << stepped.generation << ")");
}

// #53: an Announce timeout before lock must not turn an unchecked line
// into one the device clock follows
void testNoHoldoverBeforeLock() {
    std::cout << "A master lost before lock leaves the servo acquiring, not in holdover" << std::endl;
    // The issue's case: two Syncs 125 ms apart, the first queued 1 ms
    Servo servo;
    const uint64_t t1 = kTaiEpochNs;
    servo.onSync(1000000 + 1000000, t1, 0.0);
    servo.onSync(126000000, t1 + 125000000, 0.0);
    CHECK(servo.estimate().has_value(), "a line from two Syncs");
    servo.holdover(200000000);
    CHECK(servo.state() == Servo::State::Acquiring, "two Syncs: still acquiring (state " << int(servo.state()) << ")");

    Simulation sim(Master{}, Network{}, 4.0, 8);
    sim.runUntil(5);  // too short a span to lock
    CHECK(sim.measure().state == Servo::State::Acquiring, "5 s of Syncs: acquiring");
    sim.servo.holdover(sim.nowNs());
    CHECK(sim.measure().state == Servo::State::Acquiring, "5 s of Syncs, then the master goes: still acquiring");
}

// #53: a line can only be followed at a rate a real clock could have
void testImplausibleRateNeverLocks() {
    std::cout << "A line more than 500 ppm from the host clock never locks" << std::endl;
    for (double skew : {-2000.0, 1000.0, 8000.0}) {
        Master m;
        m.skewPpm = skew;
        Simulation sim(m, Network{}, 4.0, 9);
        bool everLocked = false;
        for (int t = 10; t <= 120; t += 2) {
            sim.runUntil(t);
            everLocked = everLocked || sim.measure().state == Servo::State::Locked;
        }
        CHECK(!everLocked, skew << " ppm never locks");
    }
    Master m;
    m.skewPpm = 400.0;
    Simulation sim(m, Network{}, 4.0, 9);
    sim.runUntil(120);
    CHECK(sim.measure().state == Servo::State::Locked, "400 ppm locks");
}

// #54: Syncs stopping while the master still announces
void testSyncTimeout() {
    std::cout << "Locked without Syncs for 3 intervals (at least 1 s): holdover" << std::endl;
    // 1 Sync/s: the last at about 119.0 s, timeout 3 s
    Simulation slow(Master{}, Network{}, 1.0, 10);
    slow.runUntil(120);
    CHECK(slow.measure().state == Servo::State::Locked, "1 Sync/s: locked");
    slow.skipTo(121.5);
    slow.servo.tick(slow.nowNs());
    CHECK(slow.measure().state == Servo::State::Locked, "2.5 s after the last Sync: still locked");
    slow.skipTo(122.5);
    slow.servo.tick(slow.nowNs());
    CHECK(slow.measure().state == Servo::State::Holdover, "3.5 s after it: holdover");

    // 4 Sync/s: 3 intervals is 0.75 s, so the 1 s floor applies
    Simulation fast(Master{}, Network{}, 4.0, 10);
    fast.runUntil(120);
    const uint32_t generation = fast.measure().generation;
    fast.skipTo(120.6);
    fast.servo.tick(fast.nowNs());
    CHECK(fast.measure().state == Servo::State::Locked, "4 Sync/s, 0.85 s after the last Sync: still locked");
    fast.skipTo(121.5);
    fast.servo.tick(fast.nowNs());
    CHECK(fast.measure().state == Servo::State::Holdover, "1.75 s after it: holdover");
    fast.runUntil(170);
    const Run back = fast.measure();
    CHECK(back.state == Servo::State::Locked && back.generation == generation,
          "Syncs returning resume the same timeline");
}

// #54: holdover does not last for ever
void testHoldoverEnds() {
    std::cout << "Holdover gives up after 60 s: acquiring, on a new timeline" << std::endl;
    Simulation sim(Master{}, Network{}, 4.0, 12);
    sim.runUntil(120);
    const uint32_t generation = sim.measure().generation;
    sim.servo.holdover(sim.nowNs());
    sim.skipTo(179);
    sim.servo.tick(sim.nowNs());
    CHECK(sim.measure().state == Servo::State::Holdover, "59 s: still in holdover");
    sim.skipTo(181);
    sim.servo.tick(sim.nowNs());
    const Run after = sim.measure();
    CHECK(after.state == Servo::State::Acquiring && !sim.servo.estimate() && after.generation == generation + 1,
          "61 s: acquiring, no estimate, a new timeline (state " << int(after.state) << ", generation "
                                                                    << after.generation << ")");
    sim.runUntil(300);
    CHECK(sim.measure().state == Servo::State::Locked, "and locks again when Syncs return");
}

// Over many networks, not one lucky run: once settled, the servo stays
// locked almost all the time, and whenever it says it is locked, its phase
// is close. A deliberately harsh network: 80% of packets queued up to 0.5 ms.
void testStaysLockedAndAccurate() {
    std::cout << "20 networks at 1 and 8 Sync/s: locked >= 95% of the time, within 50 us when locked" << std::endl;
    double worstLocked = 0;
    double leastLocked = 100;
    for (double hz : {1.0, 8.0}) {
        for (uint32_t seed = 11; seed <= 20; ++seed) {
            Simulation sim(Master{}, Network{}, hz, seed);
            int locked = 0;
            int total = 0;
            for (int t = 121; t <= 600; ++t) {
                sim.runUntil(t);
                const Run r = sim.measure();
                ++total;
                if (r.state == Servo::State::Locked) {
                    ++locked;
                    worstLocked = std::max(worstLocked, std::fabs(r.phaseErrorNs));
                }
            }
            leastLocked = std::min(leastLocked, 100.0 * locked / total);
        }
    }
    std::cout << "  least locked run " << leastLocked << "%, worst phase while locked " << worstLocked / 1000 << " us"
              << std::endl;
    CHECK(leastLocked >= 95.0, "every run should be locked at least 95% of the time (" << leastLocked << "%)");
    CHECK(worstLocked < 50000, "phase within 50 us whenever locked (" << worstLocked << " ns)");
}

// Delays on macOS come in bursts: whole seconds in which every packet is
// late. Lock must ride through them, as the estimate does
void testStaysLockedThroughBursts() {
    std::cout << "16 Sync/s, every packet late for 1 s in 4: stays locked, within 50 us" << std::endl;
    Network bursty;
    bursty.burstEverySeconds = 4.0;
    bursty.burstSeconds = 1.0;
    double leastLocked = 100;
    double worstLocked = 0;
    for (uint32_t seed = 31; seed <= 40; ++seed) {
        Simulation sim(Master{}, bursty, 16.0, seed);
        int locked = 0;
        int total = 0;
        for (double t = 30; t <= 300; t += 0.25) {
            sim.runUntil(t);
            const Run r = sim.measure();
            ++total;
            if (r.state == Servo::State::Locked) {
                ++locked;
                worstLocked = std::max(worstLocked, std::fabs(r.phaseErrorNs));
            }
        }
        leastLocked = std::min(leastLocked, 100.0 * locked / total);
    }
    std::cout << "  least locked run " << leastLocked << "%, worst phase while locked " << worstLocked / 1000 << " us"
              << std::endl;
    CHECK(leastLocked >= 99.0, "locked at least 99% of the time (" << leastLocked << "%)");
    CHECK(worstLocked < 50000, "phase within 50 us whenever locked (" << worstLocked << " ns)");
}

} // namespace

int main() {
    testLocksAtOneSyncPerSecond();
    testFasterSyncLocksSooner();
    testEpochsAndSkews();
    testOutliersAreIgnored();
    testLockIsJudgedIndependently();
    testStaysLockedThroughBursts();
    testGrandmasterTimeStep();
    testSmallGrandmasterStepIsFollowed();
    testIsolatedPositiveExcursionIsNotAStep();
    testSustainedQueueBurstsAreNotSteps();
    testHoldover();
    testHoldoverStepIsNotLocked();
    testNoHoldoverBeforeLock();
    testImplausibleRateNeverLocks();
    testSyncTimeout();
    testHoldoverEnds();
    testStaysLockedAndAccurate();

    std::cout << "\nPTP servo: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
