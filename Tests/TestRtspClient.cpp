//
// TestRtspClient.cpp
// AES67 macOS Driver
// RTSP response parsing (RFC 2326 section 7.1). The response comes from
// whatever answers on the LAN, so a malformed one must fail, not throw or
// allocate what its headers ask for.
//

#include "../NetworkEngine/Discovery/RTSPClient.h"

#include <iostream>
#include <string>

using namespace AES67;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

void testValidResponse() {
    std::cout << "valid DESCRIBE response" << std::endl;
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 1\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "v=0\n");
    CHECK(response.has_value(), "a well-formed response should parse");
    if (!response) {
        return;
    }
    CHECK(response->statusCode == 200, "status code should be 200");
    CHECK(response->statusMessage == "OK", "status message should be OK");
    CHECK(response->headers.at("CSeq") == "1", "CSeq header should be 1");
    CHECK(response->headers.at("Content-Type") == "application/sdp", "Content-Type should survive");
    CHECK(response->body == "v=0\n", "body should be the declared length");
    CHECK(response->isSuccess(), "200 is a success");
}

void testLineEndingsAndTrimming() {
    std::cout << "LF line endings and header whitespace" << std::endl;
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\n"
        "CSeq:\t 42  \n"
        "\n");
    CHECK(response.has_value(), "LF-only responses should parse");
    if (response) {
        CHECK(response->headers.at("CSeq") == "42", "header value should be trimmed");
        CHECK(response->body.empty(), "no body without Content-Length or trailing bytes");
    }
}

void testHeaderWithoutColon() {
    std::cout << "header line without a colon" << std::endl;
    const auto response = RTSPClient::parseResponse(
        "RTSP/1.0 200 OK\r\n"
        "not a header\r\n"
        "Server: test\r\n"
        "\r\n");
    CHECK(response.has_value(), "an unparsable header line should be skipped");
    if (response) {
        CHECK(response->headers.size() == 1, "only the well-formed header should be kept");
        CHECK(response->headers.count("Server") == 1, "Server header should be kept");
    }
}

void testMalformedStatusLines() {
    std::cout << "malformed status lines" << std::endl;
    CHECK(!RTSPClient::parseResponse("").has_value(), "empty input should fail");
    CHECK(!RTSPClient::parseResponse("RTSP/1.0").has_value(), "status line without a code should fail");
    CHECK(!RTSPClient::parseResponse("RTSP/1.0 abc OK\r\n\r\n").has_value(), "non-numeric code should fail");
    CHECK(!RTSPClient::parseResponse("RTSP/1.0 42 OK\r\n\r\n").has_value(), "two-digit code should fail");
    CHECK(!RTSPClient::parseResponse("RTSP/1.0 1000 OK\r\n\r\n").has_value(), "four-digit code should fail");
    CHECK(!RTSPClient::parseResponse("HTTP/1.1 200 OK\r\n\r\n").has_value(), "a non-RTSP protocol should fail");
    CHECK(!RTSPClient::parseResponse("\r\n").has_value(), "a blank first line should fail");
}

void testContentLength() {
    std::cout << "Content-Length from the network" << std::endl;

    // Not a number: std::stoul would throw, so reject it.
    for (const char* value : {"", "abc", "-1", "+4", "1e3", "0x10"}) {
        const std::string text = std::string("RTSP/1.0 200 OK\r\nContent-Length: ") + value + "\r\n\r\nbody";
        CHECK(!RTSPClient::parseResponse(text).has_value(),
              std::string("Content-Length '") + value + "' should be rejected");
    }

    // Optional whitespace around a field value is allowed (RFC 7230 3.2.4).
    const auto padded = RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length:  4 \r\n\r\nbody");
    CHECK(padded.has_value() && padded->body == "body", "whitespace around Content-Length should be trimmed");

    // A number, but not one we are willing to allocate for.
    CHECK(!RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length: 999999999\r\n\r\n").has_value(),
          "an absurd Content-Length should be rejected");
    CHECK(!RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length: 1048577\r\n\r\n").has_value(),
          "a body above 1 MiB should be rejected");

    // Fewer bytes than promised: the body must not be padded out to the header.
    const auto truncated = RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    CHECK(!truncated.has_value(), "a truncated body should fail the response");

    // More bytes than promised: the declared body is what the caller gets.
    const auto extra = RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nbodyEXTRA");
    CHECK(extra.has_value(), "extra bytes after the body should parse");
    if (extra) {
        CHECK(extra->body == "body", "body should stop at Content-Length");
    }

    // Zero length: a body that follows is not part of this response.
    const auto zero = RTSPClient::parseResponse("RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\nignored");
    CHECK(zero.has_value(), "a zero Content-Length should parse");
    if (zero) {
        CHECK(zero->body.empty(), "zero Content-Length should give an empty body");
    }
}

void testBinaryBody() {
    std::cout << "body with NUL bytes" << std::endl;
    std::string text = "RTSP/1.0 200 OK\r\nContent-Length: 3\r\n\r\n";
    text.push_back('\0');
    text.push_back('a');
    text.push_back('b');
    const auto response = RTSPClient::parseResponse(text);
    CHECK(response.has_value(), "a binary body should parse");
    if (response) {
        CHECK(response->body.size() == 3, "binary body should keep its length");
        CHECK(response->body[0] == '\0' && response->body[1] == 'a' && response->body[2] == 'b',
              "binary body should keep its bytes");
    }
}

void testNoBody() {
    std::cout << "response without headers" << std::endl;
    const auto response = RTSPClient::parseResponse("RTSP/1.0 454 Session Not Found\r\n");
    CHECK(response.has_value(), "a response with only a status line should parse");
    if (response) {
        CHECK(response->statusCode == 454, "status code should be 454");
        CHECK(!response->isSuccess(), "454 is not a success");
        CHECK(response->statusMessage == "Session Not Found", "status message should be kept");
    }
}

} // namespace

int main() {
    std::cout << "RTSP response parsing tests" << std::endl;
    std::cout << "===========================" << std::endl;

    testValidResponse();
    testLineEndingsAndTrimming();
    testHeaderWithoutColon();
    testMalformedStatusLines();
    testContentLength();
    testBinaryBody();
    testNoBody();

    std::cout << std::endl << "Checks passed: " << checksPassed
              << ", failed: " << checksFailed << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
