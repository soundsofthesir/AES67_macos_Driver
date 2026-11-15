// A check that stays on in every build type: a property a fuzzer found
// broken prints what broke and aborts, so libFuzzer saves the input.
#pragma once

#include <cstdio>
#include <cstdlib>

#define FUZZ_CHECK(condition, message)                                              \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "FUZZ_CHECK failed: %s (%s:%d)\n", message, __FILE__, \
                         __LINE__);                                                 \
            std::abort();                                                           \
        }                                                                           \
    } while (0)
