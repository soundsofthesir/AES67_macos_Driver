/// @file HostTime.h
/// @brief Conversions between mach_absolute_time ticks and nanoseconds.
///
/// Host time is Core Audio's time base: zero timestamps, IO timestamps and
/// SO_TIMESTAMP_MONOTONIC kernel receive timestamps (after
/// hostTimeFromKernelTimestamp) are all mach ticks.
/// Ticks are not nanoseconds (125/3 ns per tick on Apple Silicon).

#pragma once

#include <mach/mach_time.h>
#include <cstdint>
#if defined(__APPLE__) && defined(__x86_64__)
#include <sys/sysctl.h>
#endif

namespace AES67 {

/// Ratio of nanoseconds to host ticks (mach_timebase_info).
struct HostTimebase {
    uint32_t numer{1};
    uint32_t denom{1};

    /// This machine's timebase (cached after the first call).
    static HostTimebase current() {
        static const HostTimebase timebase = [] {
            mach_timebase_info_data_t info{};
            mach_timebase_info(&info);
            return HostTimebase{info.numer, info.denom};
        }();
        return timebase;
    }

    /// Exact (floored) conversion; 128-bit intermediate so large values cannot overflow.
    uint64_t ticksToNanos(uint64_t ticks) const noexcept {
        return static_cast<uint64_t>(static_cast<unsigned __int128>(ticks) * numer / denom);
    }

    uint64_t nanosToTicks(uint64_t nanos) const noexcept {
        return static_cast<uint64_t>(static_cast<unsigned __int128>(nanos) * denom / numer);
    }

    double ticksPerSecond() const noexcept {
        return 1e9 * static_cast<double>(denom) / static_cast<double>(numer);
    }
};

/// Current host time in ticks.
inline uint64_t hostTimeNow() noexcept {
    return mach_absolute_time();
}

/// Converts an SO_TIMESTAMP_MONOTONIC kernel receive timestamp to host ticks.
///
/// The kernel always stamps in the machine's native ticks. An x86_64 process
/// under Rosetta sees a 1/1 timebase and mach_absolute_time() in nanoseconds,
/// so there the native ticks (hw.tbfrequency per second, 24 MHz on Apple
/// Silicon) are scaled to nanoseconds. Everywhere else the two agree.
inline uint64_t hostTimeFromKernelTimestamp(uint64_t kernelTicks) noexcept {
#if defined(__APPLE__) && defined(__x86_64__)
    static const uint64_t nativeTicksPerSecond = [] {
        int translated = 0;
        size_t size = sizeof(translated);
        if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) != 0 || translated != 1) {
            return uint64_t{0};
        }
        uint64_t frequency = 0;
        size = sizeof(frequency);
        if (sysctlbyname("hw.tbfrequency", &frequency, &size, nullptr, 0) != 0) {
            return uint64_t{0};
        }
        return frequency;
    }();
    if (nativeTicksPerSecond != 0) {
        return static_cast<uint64_t>(static_cast<unsigned __int128>(kernelTicks) * 1000000000u / nativeTicksPerSecond);
    }
#endif
    return kernelTicks;
}

} // namespace AES67
