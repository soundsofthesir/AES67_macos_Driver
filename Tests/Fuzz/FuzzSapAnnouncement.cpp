// Fuzzes SAPListener::parseAnnouncement with SAP packets from the network,
// and feeds any SDP it extracts on to the SDP parser, as the listener would.

#include "../../Driver/SDPParser.h"
#include "../../NetworkEngine/Discovery/SAPListener.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const auto announcement =
        AES67::SAPListener::parseAnnouncement(reinterpret_cast<const char*>(data), size, "192.0.2.1");
    if (!announcement.sessionDescription.empty()) {
        (void)AES67::SDPParser::parseString(announcement.sessionDescription);
    }
    return 0;
}
