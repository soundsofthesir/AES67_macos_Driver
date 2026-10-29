#include "AudioThreadPriority.h"
#include "DebugLog.h"
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <algorithm>
#include <cstdint>

namespace AES67 {

namespace {

// The kernel refuses a computation below its minimum real-time quantum
// (about 50 us) or above 50 ms
constexpr std::chrono::nanoseconds kMinComputation = std::chrono::microseconds(50);
constexpr std::chrono::nanoseconds kMaxComputation = std::chrono::milliseconds(50);

uint32_t toAbsoluteTime(std::chrono::nanoseconds duration) {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const auto ns = static_cast<uint64_t>(std::max<int64_t>(duration.count(), 0));
    return static_cast<uint32_t>(std::min<uint64_t>(ns * timebase.denom / timebase.numer, UINT32_MAX));
}

kern_return_t applyTimeConstraint(thread_act_t thread, std::chrono::nanoseconds period) {
    const auto computation = std::clamp(period / 2, kMinComputation, kMaxComputation);
    thread_time_constraint_policy_data_t policy{};
    policy.period = toAbsoluteTime(period);
    policy.computation = toAbsoluteTime(computation);
    policy.constraint = toAbsoluteTime(std::max(period, computation));
    policy.preemptible = TRUE;
    return thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

// Out of timeshare at the highest precedence: what these threads used before
// time-constraint, still far better than timeshare under load
void applyFallback(thread_act_t thread) {
    thread_extended_policy_data_t extended{};
    extended.timeshare = FALSE;
    thread_policy_set(thread, THREAD_EXTENDED_POLICY, reinterpret_cast<thread_policy_t>(&extended),
                      THREAD_EXTENDED_POLICY_COUNT);
    thread_precedence_policy_data_t precedence{};
    precedence.importance = 63;
    thread_policy_set(thread, THREAD_PRECEDENCE_POLICY, reinterpret_cast<thread_policy_t>(&precedence),
                      THREAD_PRECEDENCE_POLICY_COUNT);
}

} // namespace

bool AudioThreadPriority::configureForRealTime(std::chrono::nanoseconds period) {
    return configureThreadForRealTime(pthread_self(), period);
}

bool AudioThreadPriority::configureThreadForRealTime(pthread_t thread, std::chrono::nanoseconds period) {
    const thread_act_t machThread = pthread_mach_thread_np(thread);
    const kern_return_t result = applyTimeConstraint(machThread, period);
    if (result == KERN_SUCCESS) {
        return true;
    }
    AES67_LOGF("AudioThreadPriority: time-constraint policy refused (kern_return=%d: %s); using high precedence",
               result, mach_error_string(result));
    applyFallback(machThread);
    return false;
}

void AudioThreadPriority::restoreNormalPriority() {
    thread_standard_policy_data_t standard{};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY,
                      reinterpret_cast<thread_policy_t>(&standard), THREAD_STANDARD_POLICY_COUNT);
}

} // namespace AES67
