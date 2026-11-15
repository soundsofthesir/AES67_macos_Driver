// Fuzzes SDPParser::parseString with SDP as it arrives from SAP, RTSP or a
// file. Anything it accepts must survive a round trip through generate(),
// because that is how the driver writes streams.json and announces streams.

#include "../../Driver/SDPParser.h"
#include "FuzzCheck.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto parsed = AES67::SDPParser::parseString(text);
    if (!parsed) {
        return 0;
    }

    const std::string generated = AES67::SDPParser::generate(*parsed);
    const auto reparsed = AES67::SDPParser::parseString(generated);
    FUZZ_CHECK(reparsed.has_value(), "generated SDP does not parse back");
    FUZZ_CHECK(reparsed->connectionAddress == parsed->connectionAddress, "address changed in round trip");
    FUZZ_CHECK(reparsed->port == parsed->port, "port changed in round trip");
    FUZZ_CHECK(reparsed->payloadType == parsed->payloadType, "payload type changed in round trip");
    FUZZ_CHECK(reparsed->encoding == parsed->encoding, "encoding changed in round trip");
    FUZZ_CHECK(reparsed->sampleRate == parsed->sampleRate, "sample rate changed in round trip");
    FUZZ_CHECK(reparsed->numChannels == parsed->numChannels, "channel count changed in round trip");
    return 0;
}
