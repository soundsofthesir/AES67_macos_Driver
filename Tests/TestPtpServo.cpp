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

#include "../NetworkEngine/PTP/PtpServo.h"
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

constexpr uint64_t kTaiEpochNs = 1790000000ULL * 1000000000ULL;  // PTP time in 2026
constexpr uint64_t kWingEpochNs = 261432ULL * 1000000000ULL;     // a console's power-on time

// A master clock: masterNs(host) = epoch + (1 + skew) * host
struct Master {
    uint64_t epochNs{kTaiEpochNs};
    double skewPpm{50.0};
    int64_t stepNs{0};  // a jump in the master's time (new grandmaster epoch)

    uint64_t at(uint64_t hostNs) const {
        return epochNs + static_cast<uint64_t>(std::llround(static_cast<double>(hostNs) * (1.0 + skewPpm * 1e-6))) +
               static_cast<uint64_t>(stepNs);
    }
};

struct Network {
    double delayNs{300000.0};    // symmetric path delay
    double jitterNs{500000.0};   // extra queueing, uniform 0..jitter, on most packets
    double cleanFraction{0.2};   // packets that see no queueing
    double outlierFraction{0.0}; // packets held up 50 ms
    double timestampNoiseNs{5000.0};  // kernel timestamp noise, either way, on every packet
    // Bursts in which every packet is held up (macOS: the CPU waking up)
    double burstEverySeconds{0.0};
    double burstSeconds{0.0};
    double burstMinNs{150000.0};  // held at least this long, up to jitterNs
};

struct Run {
    double rateErrorPpm{1e9};
    double phaseErrorNs{1e18};
    double pathDelayNs{0};
    Servo::State state{Servo::State::Acquiring};
    uint32_t generation{0};
};

class Simulation {
public:
    Simulation(Master master, Network network, double syncHz, uint32_t seed = 1)
        : master_(master), network_(network), syncHz_(syncHz), rng_(seed) {}

    Servo servo;

    // Advance to `seconds`, exchanging Syncs at syncHz and a Delay_Req each second
    void runUntil(double seconds) {
        const auto end = static_cast<uint64_t>(seconds * 1e9);
        const auto syncInterval = static_cast<uint64_t>(1e9 / syncHz_);
        while (true) {
            const uint64_t nextSync = nextSyncNs_;
            const uint64_t nextDelay = nextDelayNs_;
            const uint64_t next = std::min(nextSync, nextDelay);
            if (next > end) break;
            if (next == nextSync) {
                // t1 when the master sends, t2 when it arrives
                const uint64_t t1 = master_.at(nextSync);
                const uint64_t t2 = nextSync + static_cast<uint64_t>(network_.delayNs + queueing(nextSync) + noise());
                servo.onSync(t2, t1, 0.0);
                nextSyncNs_ += syncInterval;
            } else {
                // t3 when we send, t4 when the master receives
                const uint64_t t3 = nextDelay;
                const uint64_t t4 = master_.at(t3 + static_cast<uint64_t>(network_.delayNs + queueing(t3) + noise()));
                servo.onDelay(t3, t4, 0.0);
                nextDelayNs_ += 1000000000ULL;
            }
            nowNs_ = next;
        }
        nowNs_ = end;
    }

    Run measure() const {
        Run r;
        r.state = servo.state();
        r.generation = servo.generation();
        r.pathDelayNs = servo.pathDelayNs();
        if (const auto e = servo.estimate()) {
            r.rateErrorPpm = (e->rate - (1.0 + master_.skewPpm * 1e-6)) * 1e6;
            r.phaseErrorNs = static_cast<double>(static_cast<int64_t>(e->masterAt(nowNs_) - master_.at(nowNs_)));
        }
        return r;
    }

    Master& master() { return master_; }
    uint64_t nowNs() const { return nowNs_; }
    void skipTo(double seconds) {  // nothing exchanged meanwhile (master lost, Mac asleep)
        nowNs_ = static_cast<uint64_t>(seconds * 1e9);
        nextSyncNs_ = std::max(nextSyncNs_, nowNs_);
        nextDelayNs_ = std::max(nextDelayNs_, nowNs_);
    }

private:
    double noise() {
        std::uniform_real_distribution<double> either(-network_.timestampNoiseNs, network_.timestampNoiseNs);
        return either(rng_);
    }

    double queueing(uint64_t atNs) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        if (network_.burstSeconds > 0 &&
            std::fmod(static_cast<double>(atNs) / 1e9, network_.burstEverySeconds) < network_.burstSeconds) {
            return network_.burstMinNs + unit(rng_) * (network_.jitterNs - network_.burstMinNs);
        }
        const double u = unit(rng_);
        if (u < network_.outlierFraction) return 50e6;
        if (u < network_.outlierFraction + network_.cleanFraction) return 0.0;
        return unit(rng_) * network_.jitterNs;
    }

    Master master_;
    Network network_;
    double syncHz_;
    std::mt19937 rng_;
    uint64_t nowNs_{0};
    uint64_t nextSyncNs_{1000000};
    uint64_t nextDelayNs_{500000000};
};

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
    testHoldover();
    testNoHoldoverBeforeLock();
    testImplausibleRateNeverLocks();
    testSyncTimeout();
    testHoldoverEnds();
    testStaysLockedAndAccurate();

    std::cout << "\nPTP servo: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
