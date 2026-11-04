// Linux stand-in for <os/log.h>, for the portable test build only. Info
// messages go to stderr so test output still shows what the driver logged;
// AES67_QUIET_LOG=1 (set by Tests/Fuzz/run-fuzzers.sh) drops them.
#pragma once
#include <cstdio>
#include <cstdlib>

typedef void* os_log_t;

static inline os_log_t os_log_create(const char*, const char*) { return nullptr; }

static inline void aes67_shim_log(const char* message) {
    static const bool quiet = std::getenv("AES67_QUIET_LOG") != nullptr;
    if (!quiet) {
        std::fprintf(stderr, "[log] %s\n", message);
    }
}

#define os_log_info(log, format, message) aes67_shim_log(message)
#define os_log_error(log, format, ...) ((void)0)
#define os_log(log, format, ...) ((void)0)
