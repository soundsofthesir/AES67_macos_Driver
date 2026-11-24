// RtpSequenceTracker.h
// Cumulative RTP packet loss that reordering and duplicates cannot inflate (#155)

#pragma once

#include <cstdint>

namespace AES67 {

/// Tracks the highest sequence number received and which of the 63 before it
/// have arrived. A sequence number counts as lost when the highest one passes
/// it, and comes back off the loss count if it arrives later within the
/// window, so cumulative loss is the number of packets never received
/// (RFC 3550 A.3). A packet older than the window, or one already received,
/// changes nothing. No allocation; receive thread only.
class RtpSequenceTracker {
public:
    struct Update {
        int64_t lostDelta{0};     // change to the cumulative loss count
        bool outOfOrder{false};   // at or behind the highest sequence number
    };

    Update onPacket(uint16_t sequence) noexcept {
        if (!started_) {
            started_ = true;
            highest_ = sequence;
            // Nothing before the first packet was expected, so none of it is lost
            received_ = ~uint64_t{0};
            return {};
        }

        const auto ahead = static_cast<int16_t>(sequence - highest_);
        if (ahead > 0) {
            const auto step = static_cast<uint32_t>(ahead);
            received_ = step < kWindow ? (received_ << step) | 1 : 1;
            highest_ = sequence;
            return {static_cast<int64_t>(step) - 1, false};
        }

        const auto behind = static_cast<uint32_t>(-static_cast<int32_t>(ahead));
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

    bool started_{false};
    uint16_t highest_{0};
    uint64_t received_{0};  // bit n: highest_ - n has arrived
};

} // namespace AES67
