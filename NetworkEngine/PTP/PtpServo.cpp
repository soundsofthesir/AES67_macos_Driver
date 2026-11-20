/// @file PtpServo.cpp

#include "PtpServo.h"
#include <algorithm>
#include <cmath>

namespace AES67 {
namespace Ptp {

namespace {

double nanosBetween(uint64_t later, uint64_t earlier) {
    return static_cast<double>(static_cast<int64_t>(later - earlier));
}

double medianOf(const std::deque<double>& values) {
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    return sorted.size() % 2 ? sorted[mid] : 0.5 * (sorted[mid - 1] + sorted[mid]);
}

} // namespace

uint64_t Servo::Estimate::masterAt(uint64_t hostNs) const {
    const double elapsed = nanosBetween(hostNs, hostAnchorNs);
    return masterAnchorNs + static_cast<uint64_t>(std::llround(rate * elapsed));
}

Servo::Servo(Config config) : config_(config) {}

void Servo::startTimeline() {
    samples_.clear();
    delays_.clear();
    predictionErrors_.clear();
    recentErrors_.clear();
    stepEvidenceBaselineLine_.reset();
    stepEvidenceBaselineDirection_ = 0;
    rejected_.clear();
    stepCandidates_.clear();
    haveLine_ = false;
    slope_ = 1.0;
    intercept_ = 0.0;
    pathDelayNs_ = 0.0;
    state_ = State::Acquiring;
}

void Servo::reset() {
    startTimeline();
    ++generation_;
}

void Servo::holdover(uint64_t hostNowNs) {
    if (state_ == State::Locked) {
        state_ = State::Holdover;
        holdoverSinceNs_ = hostNowNs;
    }
}

void Servo::tick(uint64_t hostNowNs) {
    if (state_ == State::Locked && samples_.size() >= 2) {
        const double intervalNs =
            nanosBetween(samples_.back().host, samples_.front().host) / static_cast<double>(samples_.size() - 1);
        const double timeoutNs = std::max(static_cast<double>(config_.syncReceiptTimeout) * intervalNs,
                                          config_.minSyncReceiptTimeoutSeconds * 1e9);
        if (nanosBetween(hostNowNs, lastSyncNs_) > timeoutNs) {
            holdover(hostNowNs);
        }
    }
    if (state_ == State::Holdover && nanosBetween(hostNowNs, holdoverSinceNs_) > config_.holdoverLimitSeconds * 1e9) {
        reset();
    }
}

double Servo::envelopeOffsetAt(uint64_t hostNs) const {
    return intercept_ + slope_ * nanosBetween(hostNs, h0_);
}

void Servo::onSync(uint64_t hostReceiveNs, uint64_t masterSendNs, double correctionNs) {
    const Sample sample{hostReceiveNs, masterSendNs + static_cast<uint64_t>(std::llround(correctionNs))};
    lastSyncNs_ = hostReceiveNs;

    if (haveLine_) {
        // How far this Sync is from where the line says it should be: zero
        // for an unqueued packet, negative by its queueing otherwise
        const double error = nanosBetween(sample.master, p0_) - envelopeOffsetAt(sample.host);
        // Capture the fitted line before a run of materially delayed Syncs
        // can tilt it. The threshold is below the bias threshold so this is
        // available when the full median window becomes negative.
        const double negativeRunThreshold = 0.5 * config_.negativeStepBiasNs;
        if (!stepEvidenceBaselineLine_ &&
            (error < -negativeRunThreshold || error > config_.positiveStepThresholdNs)) {
            stepEvidenceBaselineLine_ = LineSnapshot{h0_, p0_, slope_, intercept_};
            stepEvidenceBaselineDirection_ = error < 0 ? -1 : 1;
        } else if (error >= -negativeRunThreshold && error <= config_.positiveStepThresholdNs) {
            stepEvidenceBaselineLine_.reset();
            stepEvidenceBaselineDirection_ = 0;
        }
        recentErrors_.push_back(error);
        if (recentErrors_.size() > config_.stepBiasSamples) recentErrors_.pop_front();
        // Queueing never makes a Sync early, so one well above the line is a
        // step candidate on its own. Queueing does make Syncs late, so a step
        // down shows only as a bias in the recent errors; while that bias
        // lasts, every Sync below the line is a candidate. Both need a settled
        // line: a young one is too rough to tell a step from its own slope
        const bool candidate = std::fabs(error) > config_.stepThresholdNs ||
                               (settled() && ((error > config_.positiveStepThresholdNs && delayStepEvidence(true)) ||
                                              (error < 0 && negativeBias() && delayStepEvidence(false))));
        if (candidate) {
            // A step in the master's time, or a stray: only consecutive Syncs
            // that agree with each other confirm a step
            const bool agrees = stepCandidates_.empty() ||
                std::fabs(nanosBetween(sample.master, stepCandidates_.back().master) -
                          nanosBetween(sample.host, stepCandidates_.back().host)) < 1e6;
            if (!agrees) stepCandidates_.clear();
            stepCandidates_.push_back(sample);
            rejected_.push_back(true);
            if (rejected_.size() > config_.lockHistory) rejected_.pop_front();
            if (stepCandidates_.size() >= config_.stepConfirmSamples) {
                const std::vector<Sample> confirmed = stepCandidates_;
                reset();
                for (const auto& s : confirmed) addSample(s);
            } else if (state_ != State::Holdover) {
                // A rejected Sync says nothing good about the line: holdover
                // ends on an accepted Sync, a confirmed step or the holdover
                // time limit, not here
                judgeLock();
            }
            return;
        }
        stepCandidates_.clear();
        predictionErrors_.push_back(error);
        if (predictionErrors_.size() > std::max(config_.lockHistory, config_.holdHistory)) {
            predictionErrors_.pop_front();
        }
    }
    rejected_.push_back(false);
    if (rejected_.size() > config_.lockHistory) rejected_.pop_front();
    addSample(sample);
}

void Servo::addSample(const Sample& sample) {
    samples_.push_back(sample);
    const auto window = static_cast<uint64_t>(config_.windowSeconds * 1e9);
    while (samples_.size() > 2 && sample.host - samples_.front().host > window) {
        samples_.pop_front();
    }
    refit();
    updatePathDelay();
    judgeLock();
}

void Servo::refit() {
    h0_ = samples_.front().host;
    p0_ = samples_.front().master;
    haveLine_ = true;
    if (samples_.size() == 1) {
        slope_ = 1.0;
        intercept_ = 0.0;
        return;
    }

    // Upper convex hull of (x, y) = (host - h0, master - p0), x increasing
    struct Point { double x, y; };
    std::vector<Point> hull;
    double meanX = 0.0;
    for (const auto& s : samples_) {
        const Point p{nanosBetween(s.host, h0_), nanosBetween(s.master, p0_)};
        meanX += p.x;
        while (hull.size() >= 2) {
            const Point& o = hull[hull.size() - 2];
            const Point& a = hull.back();
            // Drop `a` unless it lies strictly above the segment o -> p
            const long double cross = static_cast<long double>(a.x - o.x) * (p.y - o.y) -
                                      static_cast<long double>(a.y - o.y) * (p.x - o.x);
            if (cross >= 0) hull.pop_back();
            else break;
        }
        hull.push_back(p);
    }
    meanX /= static_cast<double>(samples_.size());

    // The hull edge spanning the mean time is the line closest to the
    // samples on average while above all of them
    size_t edge = 0;
    while (edge + 2 < hull.size() && hull[edge + 1].x <= meanX) ++edge;
    if (hull.size() < 2 || hull[edge + 1].x == hull[edge].x) {
        slope_ = 1.0;
        intercept_ = hull.empty() ? 0.0 : hull.back().y - hull.back().x;
        return;
    }
    slope_ = (hull[edge + 1].y - hull[edge].y) / (hull[edge + 1].x - hull[edge].x);

    // A longer baseline where the hull allows: the chord between the
    // vertices nearest a quarter and three quarters of the way across (the
    // ends of the hull are the oldest and newest Syncs, queued or not)
    const double span = hull.back().x;
    auto nearest = [&hull](double x) {
        size_t best = 0;
        for (size_t i = 1; i < hull.size(); ++i) {
            if (std::fabs(hull[i].x - x) < std::fabs(hull[best].x - x)) best = i;
        }
        return best;
    };
    const size_t a = nearest(span / 4), b = nearest(3 * span / 4);
    if (a + 1 < b && a > 0 && b + 1 < hull.size()) {
        slope_ = (hull[b].y - hull[a].y) / (hull[b].x - hull[a].x);
    }

    // At that slope, the line resting on the hull
    intercept_ = hull.front().y - slope_ * hull.front().x;
    for (const auto& p : hull) intercept_ = std::max(intercept_, p.y - slope_ * p.x);
}

void Servo::onDelay(uint64_t hostSendNs, uint64_t masterReceiveNs, double correctionNs) {
    delays_.push_back(DelayPair{hostSendNs, masterReceiveNs - static_cast<uint64_t>(std::llround(correctionNs)), {}});
    if (delays_.size() > config_.delaySamples) delays_.pop_front();
    updatePathDelay();
}

bool Servo::settled() const {
    return samples_.size() >= config_.minSamples &&
           nanosBetween(samples_.back().host, samples_.front().host) >= config_.minSpanSeconds * 1e9;
}

bool Servo::negativeBias() const {
    // Needs a full window on a settled line
    if (recentErrors_.size() < config_.stepBiasSamples || !settled()) {
        return false;
    }
    return medianOf(recentErrors_) < -config_.negativeStepBiasNs;
}

bool Servo::delayStepEvidence(bool positive) const {
    // Sync queueing pushes Sync errors down, but it pushes the reciprocal
    // Delay_Req residual up. A real master-time step moves both down. Compare
    // recent fixed delay-pair residuals with the preceding history so ordinary
    // path delay and queueing do not look like a step.
    // Sync candidates need independent confirmation. Three Delay_Req pairs
    // also prevent one queued exchange from corroborating a positive step.
    const size_t recentCount = positive ? config_.stepConfirmSamples : 1;
    const size_t baselineCount = config_.stepBiasSamples;
    const int direction = positive ? 1 : -1;
    if (!stepEvidenceBaselineLine_ || stepEvidenceBaselineDirection_ != direction ||
        delays_.size() < recentCount + baselineCount) return false;

    std::vector<double> baseline;
    std::vector<double> recent;
    const auto residual = [this](const DelayPair& d) {
        const auto& line = *stepEvidenceBaselineLine_;
        return nanosBetween(d.masterReceive, line.masterOrigin) -
               (line.intercept + line.slope * nanosBetween(d.hostSend, line.hostOrigin));
    };
    const size_t baselineStart = delays_.size() - recentCount - baselineCount;
    for (size_t i = baselineStart; i < baselineStart + baselineCount; ++i) {
        baseline.push_back(residual(delays_[i]));
    }
    for (size_t i = delays_.size() - recentCount; i < delays_.size(); ++i) {
        recent.push_back(residual(delays_[i]));
    }
    // Compare lower envelopes, not medians. A real clock step shifts every
    // recent exchange; queueing only raises them. The same lower-envelope
    // check in either direction rejects both a negative-bias false positive
    // and the apparent positive jump when a congestion burst drains.
    const double baselineMinimum = *std::min_element(baseline.begin(), baseline.end());
    const double recentMinimum = *std::min_element(recent.begin(), recent.end());
    const double shift = recentMinimum - baselineMinimum;
    return positive ? shift > 0.75 * config_.negativeStepBiasNs
                    : shift < -0.75 * config_.negativeStepBiasNs;
}

void Servo::updatePathDelay() {
    if (!haveLine_ || delays_.empty()) {
        return;
    }
    // t4 = E(t3) + 2 x delay + queueing: the least-queued exchange gives the delay
    const bool fix = settled();
    double best = 0.0;
    bool first = true;
    for (auto& d : delays_) {
        const double twice = d.twiceDelay ? *d.twiceDelay
                                          : nanosBetween(d.masterReceive, p0_) - envelopeOffsetAt(d.hostSend);
        if (fix && !d.twiceDelay) d.twiceDelay = twice;
        if (first || twice < best) {
            best = twice;
            first = false;
        }
    }
    pathDelayNs_ = std::max(0.0, best / 2.0);
}

void Servo::judgeLock() {
    const size_t rejected = static_cast<size_t>(std::count(rejected_.begin(), rejected_.end(), true));
    const bool consistent = !rejected_.empty() &&
        static_cast<double>(rejected) <= config_.maxRejectedFraction * static_cast<double>(rejected_.size());
    const bool enough = settled() && predictionErrors_.size() >= config_.lockHistory;

    bool predicts = false;
    if (enough) {
        // Enough recent Syncs land on the line: unqueued ones on a real
        // network do; scrambled timestamps only touch it now and then
        const bool holding = state_ == State::Locked;
        const double threshold = holding ? 2.0 * config_.lockThresholdNs : config_.lockThresholdNs;
        const size_t history = std::min(predictionErrors_.size(), holding ? config_.holdHistory : config_.lockHistory);
        const auto recent = predictionErrors_.end() - static_cast<std::ptrdiff_t>(history);
        const auto good = std::count_if(recent, predictionErrors_.end(),
                                        [threshold](double error) { return std::fabs(error) <= threshold; });
        const double fraction = holding ? config_.holdGoodFraction : config_.lockGoodFraction;
        predicts = static_cast<double>(good) >= fraction * static_cast<double>(history);
    }
    const bool plausible = std::fabs(slope_ - 1.0) <= config_.maxRateErrorPpm * 1e-6;
    // A negative Sync bias alone may be ordinary receive queueing. Only let it
    // invalidate lock when a Delay_Req confirms the master's clock also moved.
    const bool biased = negativeBias() && delayStepEvidence(false);
    state_ = (enough && predicts && consistent && plausible && !biased) ? State::Locked : State::Acquiring;
}

std::optional<Servo::Estimate> Servo::estimate() const {
    if (!haveLine_ || samples_.empty()) {
        return std::nullopt;
    }
    Estimate e;
    e.hostAnchorNs = samples_.back().host;
    e.masterAnchorNs = p0_ + static_cast<uint64_t>(std::llround(envelopeOffsetAt(e.hostAnchorNs) + pathDelayNs_));
    e.rate = slope_;
    return e;
}

} // namespace Ptp
} // namespace AES67
