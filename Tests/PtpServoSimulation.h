//
// PtpServoSimulation.h
// AES67 macOS Driver
// A simulated PTP master and network for the servo tests, in virtual time.
// The master's clock is skewed and can jump. The network adds a path delay,
// queueing (which only ever delays a packet), kernel timestamp noise and,
// optionally, congestion bursts, an asymmetric path and lost packets.
// Deterministic for a given seed.
//

#pragma once

#include "../NetworkEngine/PTP/PtpServo.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace PtpServoSim {

using AES67::Ptp::Servo;

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

// Periods in which every packet is held up by minNs to maxNs: a switch port
// or Wi-Fi link that stays busy for a few seconds
struct Congestion {
    double startSeconds{0.0};  // the first period starts here
    double everySeconds{0.0};  // 0: no congestion
    double seconds{0.0};
    double minNs{0.0};
    double maxNs{0.0};
    bool syncOnly{false};  // only the master-to-host direction is congested
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
    Congestion congestion;
    double asymmetryNs{0.0};   // extra delay on the master-to-host path only
    double lossFraction{0.0};  // Syncs and Delay_Reqs that never arrive
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
                if (!lost()) {
                    const uint64_t t1 = master_.at(nextSync);
                    const uint64_t t2 = nextSync + static_cast<uint64_t>(network_.delayNs + network_.asymmetryNs +
                                                                         queueing(nextSync, true) + noise());
                    servo.onSync(t2, t1, 0.0);
                }
                nextSyncNs_ += syncInterval;
            } else {
                // t3 when we send, t4 when the master receives
                if (!lost()) {
                    const uint64_t t3 = nextDelay;
                    const uint64_t t4 =
                        master_.at(t3 + static_cast<uint64_t>(network_.delayNs + queueing(t3, false) + noise()));
                    servo.onDelay(t3, t4, 0.0);
                }
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

    // Draws from the generator only when loss is on, so runs without it
    // see the same packets as before loss existed
    bool lost() {
        if (network_.lossFraction <= 0) return false;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng_) < network_.lossFraction;
    }

    // In a period that starts every `everySeconds` from `startSeconds` and lasts `seconds`
    static bool within(uint64_t atNs, double everySeconds, double seconds, double startSeconds = 0.0) {
        const double t = static_cast<double>(atNs) / 1e9 - startSeconds;
        return everySeconds > 0 && t >= 0 && std::fmod(t, everySeconds) < seconds;
    }

    double queueing(uint64_t atNs, bool towardHost) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const Congestion& c = network_.congestion;
        if ((towardHost || !c.syncOnly) && within(atNs, c.everySeconds, c.seconds, c.startSeconds)) {
            return c.minNs + unit(rng_) * (c.maxNs - c.minNs);
        }
        if (within(atNs, network_.burstEverySeconds, network_.burstSeconds)) {
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

}  // namespace PtpServoSim
