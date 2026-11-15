// Linux stand-in for <mach/mach_time.h>, for the portable test build only
// (Tests/Linux/run-portable-tests.sh). Host ticks are CLOCK_MONOTONIC
// nanoseconds, so the timebase is 1/1.
#pragma once
#include <stdint.h>
#include <time.h>

typedef struct {
    uint32_t numer;
    uint32_t denom;
} mach_timebase_info_data_t;

static inline int mach_timebase_info(mach_timebase_info_data_t* info) {
    info->numer = 1;
    info->denom = 1;
    return 0;
}

static inline uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
