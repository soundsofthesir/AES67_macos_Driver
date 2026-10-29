/// @file PtpBestMaster.cpp

#include "PtpBestMaster.h"
#include <cmath>
#include <utility>

namespace AES67 {
namespace Ptp {

namespace {

// Announce intervals we follow: 1/8 s (SMPTE ST 2059-2's fastest) to 16 s
// (the slowest in the default and AES67 media profiles). Outside this, the
// receipt timeout and qualification window are meaningless.
constexpr int8_t kMinLogAnnounceInterval = -3;
constexpr int8_t kMaxLogAnnounceInterval = 4;

} // namespace

BestMaster::BestMaster(Config config) : config_(std::move(config)) {}

double BestMaster::intervalOf(const Candidate& candidate) {
    return std::ldexp(1.0, candidate.logAnnounceInterval);  // 2^logMessageInterval seconds
}

bool BestMaster::qualified(const Record& record, double now) const {
    const double window = config_.foreignMasterWindow * intervalOf(record.latest);
    int recent = 0;
    for (double arrival : record.arrivals) {
        if (now - arrival <= window) ++recent;
    }
    return recent >= config_.foreignMasterThreshold;
}

bool BestMaster::alive(const Record& record, double now) const {
    return now - record.lastSeen <= config_.announceReceiptTimeout * intervalOf(record.latest);
}

int BestMaster::compare(const Candidate& a, const Candidate& b) {
    const AnnounceBody& x = a.announce;
    const AnnounceBody& y = b.announce;
    auto order = [](auto p, auto q) { return p < q ? -1 : (q < p ? 1 : 0); };

    if (x.grandmasterIdentity != y.grandmasterIdentity) {
        // Different grandmasters: the better clock, in the standard's order
        if (int c = order(x.grandmasterPriority1, y.grandmasterPriority1)) return c;
        if (int c = order(x.grandmasterClockClass, y.grandmasterClockClass)) return c;
        if (int c = order(x.grandmasterClockAccuracy, y.grandmasterClockAccuracy)) return c;
        if (int c = order(x.grandmasterOffsetScaledLogVariance, y.grandmasterOffsetScaledLogVariance)) return c;
        if (int c = order(x.grandmasterPriority2, y.grandmasterPriority2)) return c;
        return order(x.grandmasterIdentity, y.grandmasterIdentity);
    }
    // The same grandmaster by different paths: the shorter path, then the
    // lower sender identity
    if (int c = order(x.stepsRemoved, y.stepsRemoved)) return c;
    return order(a.sender, b.sender);
}

bool BestMaster::onAnnounce(const Message& message, double now) {
    if (message.header.type != MessageType::Announce || !message.announce) {
        return false;
    }
    if (message.header.domain != config_.domain || message.header.source.clock == config_.self ||
        message.announce->stepsRemoved >= 255 || message.header.logMessageInterval < kMinLogAnnounceInterval ||
        message.header.logMessageInterval > kMaxLogAnnounceInterval) {
        return false;
    }

    Record& record = masters_[message.header.source];
    record.latest = Candidate{message.header.source, *message.announce, message.header.logMessageInterval};
    record.lastSeen = now;
    record.arrivals.push_back(now);
    const double window = config_.foreignMasterWindow * intervalOf(record.latest);
    while (!record.arrivals.empty() && now - record.arrivals.front() > window) {
        record.arrivals.pop_front();
    }
    return reselect(now);
}

bool BestMaster::tick(double now) {
    for (auto it = masters_.begin(); it != masters_.end();) {
        it = alive(it->second, now) ? std::next(it) : masters_.erase(it);
    }
    return reselect(now);
}

bool BestMaster::reselect(double now) {
    const Record* best = nullptr;
    for (const auto& entry : masters_) {
        const Record& record = entry.second;
        if (!alive(record, now) || !qualified(record, now)) {
            continue;
        }
        if (best == nullptr || compare(record.latest, best->latest) < 0) {
            best = &record;
        }
    }
    std::optional<PortIdentity> choice;
    if (best != nullptr) {
        choice = best->latest.sender;
    }
    if (choice == selected_) {
        return false;
    }
    selected_ = choice;
    return true;
}

std::optional<Candidate> BestMaster::selected() const {
    if (!selected_) {
        return std::nullopt;
    }
    auto it = masters_.find(*selected_);
    return it == masters_.end() ? std::nullopt : std::optional<Candidate>(it->second.latest);
}

} // namespace Ptp
} // namespace AES67
