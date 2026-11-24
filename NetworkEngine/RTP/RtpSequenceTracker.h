/// @file RtpSequenceTracker.h
/// @brief Cumulative RTP packet loss that reordering, duplicates and sender
/// restarts cannot inflate (#155).

#pragma once

#include <cstdint>

namespace AES67 {

/// Tracks the highest sequence number received and which of the 63 before it
/// have arrived. A sequence number counts as lost when the highest one passes
/// it, and comes back off the loss count if it arrives later within the
/// window, so cumulative loss is the number of packets never received
/// (RFC 3550 A.3). A packet older than the window, or one already received,
/// changes nothing.
///
/// A jump of kMaxDropout or more forward, or more than kMaxMisorder back, is
/// not loss: two packets in sequence after it (a restarted sender) restart
/// tracking there, as RFC 3550 A.1 does. No allocation; receive thread only.
class RtpSequenceTracker {
public:
    struct Update {
        int64_t lostDelta{0};     // change to the cumulative loss count
        bool outOfOrder{false};   // at or behind the highest sequence number
    };

    Update onPacket(uint16_t sequence) noexcept {
        if (!started_ || (resyncPending_ && sequence == resyncSequence_)) {
            restartAt(sequence);
            return {};
        }

        const auto ahead = static_cast<int16_t>(sequence - highest_);
        if (ahead > 0 && ahead < kMaxDropout) {
            resyncPending_ = false;
            const auto step = static_cast<uint32_t>(ahead);
            received_ = step < kWindow ? (received_ << step) | 1 : 1;
            highest_ = sequence;
            return {static_cast<int64_t>(step) - 1, false};
        }

        const auto behind = static_cast<uint32_t>(-static_cast<int32_t>(ahead));
        if (ahead > 0 || behind > kMaxMisorder) {
            // Too far to be loss or reordering: wait for the next in sequence
            resyncPending_ = true;
            resyncSequence_ = static_cast<uint16_t>(sequence + 1);
            return {};
        }
        if (behind >= kWindow) {
            return {0, true};
        }
        const uint64_t bit = uint64_t{1} << behind;
        if ((received_ & bit) != 0) {
            return {0, true};  // a duplicate (behind == 0 is always one)
        }
        received_ |= bit;
        return {-1, true};
    }

    void reset() noexcept { started_ = false; }

private:
    static constexpr uint32_t kWindow = 64;
    static constexpr int16_t kMaxDropout = 3000;   // RFC 3550 A.1 MAX_DROPOUT
    static constexpr uint32_t kMaxMisorder = 100;  // RFC 3550 A.1 MAX_MISORDER

    void restartAt(uint16_t sequence) noexcept {
        started_ = true;
        resyncPending_ = false;
        highest_ = sequence;
        // Nothing before the first packet was expected, so none of it is lost
        received_ = ~uint64_t{0};
    }

    bool started_{false};
    bool resyncPending_{false};
    uint16_t resyncSequence_{0};
    uint16_t highest_{0};
    uint64_t received_{0};  // bit n: highest_ - n has arrived
};

} // namespace AES67
