// Fuzzes RTSPClient::parseResponse with responses from an RTSP server, as the
// client reads them off the socket. The RTSP target is whatever answers on the
// LAN, so every field here is untrusted input.

#include "../../NetworkEngine/Discovery/RTSPClient.h"

#include "FuzzCheck.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string response(reinterpret_cast<const char*>(data), size);
    const auto parsed = AES67::RTSPClient::parseResponse(response);
    if (!parsed) {
        return 0;
    }

    // A status code is three digits (RFC 2326 section 7.1), and a body can
    // never contain more bytes than the response it was parsed from.
    FUZZ_CHECK(parsed->statusCode >= 100 && parsed->statusCode <= 999,
               "status code outside 100-999");
    FUZZ_CHECK(parsed->body.size() <= response.size(), "body larger than the input");
    return 0;
}
