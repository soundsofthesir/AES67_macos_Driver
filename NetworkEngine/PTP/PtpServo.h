/// @file PtpServo.h
/// @brief From PTP timestamps to a model of the master's clock in host time.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace AES67 {
namespace Ptp {

/// Estimates the master's clock as a line in host time, from Sync (t1, t2)
/// and Delay_Req (t3, t4) exchanges. Host times are kernel receive and send
/// times in nanoseconds; master times are PTP nanoseconds plus corrections.
///
/// Network queueing only ever delays a packet, so each Sync lies on or below
/// the true line (minus the path delay). The estimate rests on the upper
/// convex hull of recent Syncs: unqueued packets lie on it, so a few of them
/// fix frequency and phase however much the rest are delayed. The slope is
/// the chord between the hull's vertices nearest a quarter and three
/// quarters of the way across the window (a long baseline, so timestamp
/// noise barely tilts it; the hull edge over the mean time when the hull is
/// too sparse), and the line at that slope rests on the hull. The path delay
/// comes from the least-queued Delay_Req over a long history (it is stable),
/// each measured against the line when it arrived, so a later change of
/// slope cannot rewrite it.
///
/// Lock is judged independently of the estimate: by how well it predicts
/// each new Sync before that Sync is used, and by how few Syncs had to be
/// rejected. A real network delivers a good share of Syncs unqueued, so many
/// predictions are close; garbage leaves only the odd one on the line, so
/// "how many are good" is the test, not "is the best one good". Holding lock
/// is judged more leniently than acquiring it. A jump in the master's time
/// (a new grandmaster epoch) is confirmed by consecutive agreeing Syncs, then
/// starts a new timeline (generation). A line further from the host clock's
/// rate than any real clock could be never locks.
///
/// Holdover keeps a locked line when the master goes (its Announces time
/// out) or its Syncs stop; an unlocked line is never held over. Holdover
/// ends when Syncs return, or after holdoverLimitSeconds, when the line is
/// dropped and a new timeline begins. Deterministic and not thread-safe.
class Servo {
public:
    struct Config {
        double windowSeconds{64.0};      // Syncs used for the estimate
        size_t minSamples{8};            // before lock can be declared
        double minSpanSeconds{8.0};
        size_t delaySamples{64};         // Delay_Req exchanges kept for the path delay: it is
                                         // stable, and a long history all but ensures an unqueued one
        size_t lockHistory{16};          // predictions judged to acquire lock
        double lockThresholdNs{100000};  // a good prediction is within 100 us
        double lockGoodFraction{0.25};   // of recent predictions that must be good
        // Once locked, judged over a longer history at twice the threshold:
        // receive delays on macOS come in bursts (CPU wake-ups), which can
        // spoil a second's worth of Syncs while the estimate stays good
        size_t holdHistory{64};
        double holdGoodFraction{0.125};
        double stepThresholdNs{10e6};    // a Sync this far off is a step candidate
        size_t stepConfirmSamples{3};    // consecutive agreeing candidates confirm a step
        double maxRejectedFraction{0.25};
        double maxRateErrorPpm{500.0};     // a line further from the host clock is not a real clock
        size_t syncReceiptTimeout{3};      // Sync intervals without a Sync before Locked drops to Holdover
        double minSyncReceiptTimeoutSeconds{1.0};
        double holdoverLimitSeconds{60.0};  // then the line is dropped and a new timeline begins
    };

    enum class State { Acquiring, Locked, Holdover };

    /// The master's clock as a line through an anchor in host time.
    struct Estimate {
        uint64_t hostAnchorNs{0};
        uint64_t masterAnchorNs{0};
        double rate{1.0};  // master nanoseconds per host nanosecond

        uint64_t masterAt(uint64_t hostNs) const;
    };

    Servo() : Servo(Config{}) {}
    explicit Servo(Config config);

    /// A Sync: received at host time t2, sent at master time t1.
    /// @param correctionNs Sync plus Follow_Up correction fields.
    void onSync(uint64_t hostReceiveNs, uint64_t masterSendNs, double correctionNs);

    /// A Delay_Req: sent at host time t3, received at master time t4.
    /// @param correctionNs The Delay_Resp correction field (subtracted from t4).
    void onDelay(uint64_t hostSendNs, uint64_t masterReceiveNs, double correctionNs);

    /// The master has gone: keep a locked line until it (or another) returns.
    void holdover(uint64_t hostNowNs);

    /// Apply the Sync receipt timeout and the holdover limit. Call regularly.
    void tick(uint64_t hostNowNs);

    /// Forget everything (a different master); starts a new generation.
    void reset();

    State state() const { return state_; }
    uint32_t generation() const { return generation_; }
    double pathDelayNs() const { return pathDelayNs_; }
    std::optional<Estimate> estimate() const;

private:
    struct Sample {
        uint64_t host;
        uint64_t master;
    };
    struct DelayPair {
        uint64_t hostSend;
        uint64_t masterReceive;
        // t4 - E(t3), fixed against the line as it stood when the pair
        // arrived (accurate near its recent end); until the line has
        // settled, re-measured against each new line
        std::optional<double> twiceDelay;
    };

    void startTimeline();
    void addSample(const Sample& sample);
    void refit();
    double envelopeOffsetAt(uint64_t hostNs) const;  // E(h) - p0, in ns
    void updatePathDelay();
    bool settled() const;  // enough Syncs, over a long enough span, to trust the line
    void judgeLock();

    const Config config_;

    std::deque<Sample> samples_;
    std::deque<DelayPair> delays_;
    std::deque<double> predictionErrors_;  // t1 - E(t2) before each accepted Sync, newest last
    std::deque<bool> rejected_;            // per Sync received
    std::vector<Sample> stepCandidates_;

    // The line E(h) = p0 + intercept + slope * (h - h0): the master's time at
    // host h, less the path delay
    bool haveLine_{false};
    uint64_t h0_{0};
    uint64_t p0_{0};
    double slope_{1.0};
    double intercept_{0.0};
    double pathDelayNs_{0.0};

    uint64_t lastSyncNs_{0};       // host receive time of the latest Sync
    uint64_t holdoverSinceNs_{0};

    State state_{State::Acquiring};
    uint32_t generation_{1};
};

} // namespace Ptp
} // namespace AES67
