#ifndef AUDIO_THREAD_PRIORITY_H
#define AUDIO_THREAD_PRIORITY_H

#include <pthread.h>
#include <chrono>

namespace AES67 {

/**
 * Real-time scheduling for the threads that send and receive audio packets.
 *
 * Only ever changes the calling thread: never the process, which inside
 * coreaudiod is the whole audio server.
 */
class AudioThreadPriority {
public:
    /// How often a thread wakes when its caller gives no period.
    static constexpr std::chrono::nanoseconds kDefaultPeriod = std::chrono::milliseconds(1);

    /**
     * Give the current thread the time-constraint (real-time) policy for a
     * thread that wakes every `period`, using up to half of it. Timeshare
     * threads, even at the highest precedence, wake about 115 us after their
     * deadline (#62); time-constraint threads wake within a few.
     *
     * If the kernel refuses it, the thread falls back to leaving timeshare at
     * the highest precedence.
     * @return true if the time-constraint policy was applied
     */
    static bool configureForRealTime(std::chrono::nanoseconds period = kDefaultPeriod);

    /**
     * Configure a specific thread; see configureForRealTime().
     */
    static bool configureThreadForRealTime(pthread_t thread, std::chrono::nanoseconds period = kDefaultPeriod);

    /**
     * Return the current thread to the standard (timeshare) policy.
     */
    static void restoreNormalPriority();
};

} // namespace AES67

#endif // AUDIO_THREAD_PRIORITY_H
