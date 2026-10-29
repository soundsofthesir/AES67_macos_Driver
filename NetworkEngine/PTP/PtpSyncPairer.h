/// @file PtpSyncPairer.h
/// @brief Pairs a two-step master's Syncs with their Follow_Ups.

#pragma once

#include <cstdint>
#include <optional>

namespace AES67 {
namespace Ptp {

/// One Sync's timestamps: t2 (our receive time) and t1 (the master's send
/// time, from the Follow_Up), with both messages' corrections added up.
struct SyncSample {
    uint64_t hostReceiveNs;
    uint64_t preciseOriginNs;
    double correctionNs;
};

/// Matches a Sync and its Follow_Up by sequenceId, in either order. Only the
/// latest message waits: anything that is not its partner replaces it, so a
/// message whose partner was lost cannot wait for the 16-bit sequenceId to
/// come round again and pair with an unrelated one. Not thread-safe.
class SyncPairer {
public:
    std::optional<SyncSample> onSync(uint16_t sequenceId, uint64_t hostReceiveNs, double correctionNs) {
        if (waiting_ && !waiting_->isSync && waiting_->sequenceId == sequenceId) {
            // The Follow_Up overtook its Sync
            const SyncSample sample{hostReceiveNs, waiting_->ns, correctionNs + waiting_->correctionNs};
            waiting_.reset();
            return sample;
        }
        waiting_ = Waiting{true, sequenceId, hostReceiveNs, correctionNs};
        return std::nullopt;
    }

    std::optional<SyncSample> onFollowUp(uint16_t sequenceId, uint64_t preciseOriginNs, double correctionNs) {
        if (waiting_ && waiting_->isSync && waiting_->sequenceId == sequenceId) {
            const SyncSample sample{waiting_->ns, preciseOriginNs, waiting_->correctionNs + correctionNs};
            waiting_.reset();
            return sample;
        }
        waiting_ = Waiting{false, sequenceId, preciseOriginNs, correctionNs};
        return std::nullopt;
    }

    void reset() { waiting_.reset(); }

private:
    struct Waiting {
        bool isSync;
        uint16_t sequenceId;
        uint64_t ns;  // t2 for a Sync, t1 for a Follow_Up
        double correctionNs;
    };

    std::optional<Waiting> waiting_;
};

} // namespace Ptp
} // namespace AES67
