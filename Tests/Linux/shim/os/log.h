// Linux stand-in for <os/log.h>, for the portable test build only. Info
// messages go to stderr so test output still shows what the driver logged.
#pragma once
#include <cstdio>

typedef void* os_log_t;

static inline os_log_t os_log_create(const char*, const char*) { return nullptr; }

#define os_log_info(log, format, message) std::fprintf(stderr, "[log] %s\n", message)
#define os_log_error(log, format, ...) ((void)0)
#define os_log(log, format, ...) ((void)0)
