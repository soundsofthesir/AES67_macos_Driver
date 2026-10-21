//
// DebugLog.h
// AES67 macOS Driver
// Debug logging to the unified log (os_log).
//
// The driver runs sandboxed inside the Core Audio driver host as _coreaudiod.
// It can write to /tmp (spike S4), but a log file there could already exist
// under another user, so logs go to the unified log instead:
//
//   log show --last 5m --info --predicate 'subsystem == "com.aes67driver"'
//   log stream --info --predicate 'subsystem == "com.aes67driver"'
//
// Not real-time safe: never log from the IO thread.
//

#pragma once

#include <os/log.h>
#include <cstdarg>
#include <cstdio>

namespace AES67 {
namespace Debug {

inline os_log_t Logger() {
    static os_log_t log = os_log_create("com.aes67driver", "driver");
    return log;
}

// Write a log message
inline void Log(const char* message) {
    os_log_info(Logger(), "%{public}s", message);
}

// Log with formatted string
inline void LogF(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

} // namespace Debug
} // namespace AES67

// Convenience macros
#define AES67_LOG(msg) AES67::Debug::Log(msg)
#define AES67_LOGF(fmt, ...) AES67::Debug::LogF(fmt, __VA_ARGS__)
