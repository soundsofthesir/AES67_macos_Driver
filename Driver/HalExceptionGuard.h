/// @file HalExceptionGuard.h
/// @brief Keeps C++ exceptions out of the AudioServerPlugIn C ABI.
///
/// libASPL invokes the plug-in through a C entry point and its bridge does not
/// catch C++ exceptions. An exception that unwinds through that boundary calls
/// std::terminate, taking down coreaudiod and all system audio with it. Every
/// HAL callback that can allocate or start threads must be wrapped here.

#pragma once

#include "DebugLog.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <exception>

namespace AES67 {

/// Log a HAL failure without letting the logger throw again (it may allocate,
/// and these calls often run in a bad_alloc path).
inline void LogHalError(const char* what, const char* detail = nullptr) noexcept {
    try {
        if (detail) {
            AES67_LOGF("HAL: %s: %s", what, detail);
        } else {
            AES67_LOGF("HAL: %s", what);
        }
    } catch (...) {
    }
}

/// Run one HAL callback body, converting any exception into
/// kAudioHardwareUnspecifiedError. The fallback logging is itself guarded:
/// under std::bad_alloc the logger may allocate again.
template <typename Body>
OSStatus RunHalCallback(const char* what, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& e) {
        LogHalError(what, e.what());
    } catch (...) {
        LogHalError(what, "unknown exception");
    }
    return kAudioHardwareUnspecifiedError;
}

} // namespace AES67
