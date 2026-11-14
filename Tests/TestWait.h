//
// TestWait.h
// AES67 macOS Driver
// Waiting on a condition instead of a fixed sleep. A slow or loaded machine
// (a CI runner, a sanitizer build) gets more time; a fast one returns at once.
//

#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace AES67::TestWait {

/// Generous upper bound for something that normally takes milliseconds.
constexpr std::chrono::milliseconds kDefaultTimeout{5000};

/// Polls every millisecond until condition() is true or timeout passes.
/// Returns whether the condition became true.
template <typename Condition>
bool waitUntil(Condition&& condition, std::chrono::milliseconds timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return condition();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// True when AES67_TEST_RELAXED_TIMING=1 is set (CI runners). Only limits on
/// how accurate timing is may be widened by it, never checks of correctness.
inline bool relaxedTiming() {
    const char* value = std::getenv("AES67_TEST_RELAXED_TIMING");
    return value && std::strcmp(value, "1") == 0;
}

/// A limit on timing accuracy: strict normally, wider with relaxedTiming().
template <typename T>
T timingLimit(T strict, T relaxed) {
    return relaxedTiming() ? relaxed : strict;
}

} // namespace AES67::TestWait
