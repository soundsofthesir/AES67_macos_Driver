//
// CriticalWait.h
// AES67 macOS Driver
// Sleeps until a host time on a kqueue timer that macOS doesn't coalesce.
//
// While no client is running IO, Core Audio lowers the driver's host process
// to the background band, and the kernel then delays ordinary timers
// (mach_wait_until, nanosleep) by tens of ms to batch wakeups. The transmit
// thread's 1 ms packets came out in bursts with gaps of up to 100 ms (#201).
// A NOTE_CRITICAL timer is exempt from that coalescing: measured in the
// background band, about 1 in 100 wakes ran more than 2 ms late, against 19
// in 20 with mach_wait_until.
//

#pragma once

#include <mach/mach_time.h>
#include <sys/event.h>
#include <unistd.h>
#include <cstdint>

namespace AES67 {

class CriticalWait {
public:
    CriticalWait() : kq_(kqueue()) {}
    ~CriticalWait() {
        if (kq_ >= 0) close(kq_);
    }
    CriticalWait(const CriticalWait&) = delete;
    CriticalWait& operator=(const CriticalWait&) = delete;

    /// Sleeps until mach_absolute_time() reaches `deadline`. Falls back to
    /// mach_wait_until if the kqueue couldn't be made or the timer fails.
    void until(uint64_t deadline) noexcept {
        if (kq_ >= 0) {
            struct kevent event;
            EV_SET(&event, kTimerId, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                   NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_CRITICAL, static_cast<intptr_t>(deadline), nullptr);
            struct kevent fired;
            if (kevent(kq_, &event, 1, &fired, 1, nullptr) == 1 && !(fired.flags & EV_ERROR)) {
                return;
            }
        }
        mach_wait_until(deadline);
    }

    bool usesKqueue() const noexcept { return kq_ >= 0; }

private:
    static constexpr uintptr_t kTimerId = 1;
    int kq_;
};

} // namespace AES67
