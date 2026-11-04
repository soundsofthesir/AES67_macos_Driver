// Fuzzes Ptp::parse with PTP datagrams from the network. Whatever it accepts
// must be usable as-is by the servo: timestamps within the range that
// converts to nanoseconds without overflow.

#include "../../NetworkEngine/PTP/PtpMessages.h"
#include "FuzzCheck.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const auto message = AES67::Ptp::parse(data, size);
    if (!message) {
        return 0;
    }
    FUZZ_CHECK(message->timestamp.nanoseconds < 1000000000u, "nanoseconds out of range");
    FUZZ_CHECK(message->timestamp.seconds <= (1ULL << 34), "seconds past the overflow bound");
    (void)message->timestamp.totalNanoseconds();
    return 0;
}
