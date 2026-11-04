// Fuzzes the ptp.json parser. Malformed settings must leave PTP off with a
// reason, never crash the driver host.

#include "../../NetworkEngine/PTP/PtpSettings.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    std::string error;
    (void)AES67::Ptp::Settings::parse(text, &error);
    return 0;
}
