//
// TestSDPParser.cpp
// AES67 macOS Driver - Build #4
// Unit tests for SDP Parser
//

#include "../Driver/SDPParser.h"
#include <iostream>
#include <fstream>
#include <cassert>

// These tests check with assert(). Release builds define NDEBUG, which would
// compile every check away and let the suite pass without testing anything.
#ifdef NDEBUG
#error "TestSDPParser needs assert(): build tests without NDEBUG (see Tests/CMakeLists.txt)"
#endif
#include <sstream>
#include <cmath>

namespace AES67 {
namespace Tests {

void testBasicSDPParsing() {
    std::cout << "Test: Basic SDP Parsing... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=Test Stream
i=8 Channel Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.171/32
a=rtpmap:96 L24/48000/8
a=ptime:1
a=framecount:48
)";

    auto session = SDPParser::parseString(sdp);
    assert(session.has_value());
    assert(session->sessionName == "Test Stream");
    assert(session->sessionInfo == "8 Channel Test");
    assert(session->connectionAddress == "239.69.83.171");
    assert(session->port == 5004);
    assert(session->sampleRate == 48000);
    assert(session->numChannels == 8);
    assert(session->encoding == "L24");
    assert(session->ptime == 1);
    assert(session->framecount == 48);

    std::cout << "✓ PASSED\n";
}

void testRiedelCompatibleSDP() {
    std::cout << "Test: Riedel Artist SDP Parsing... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=Riedel Artist IFB
i=Intercom Feed Back 8 Channels
t=0 0
a=clock-domain:PTPv2 0
a=recvonly
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.171/32
a=rtpmap:96 L24/48000/8
a=ptime:1
a=framecount:48
a=source-filter: incl IN IP4 239.69.83.171 192.168.1.100
a=ts-refclk:ptp=IEEE1588-2008:00-1B-21-AC-B5-4F:domain-nmbr=0
a=mediaclk:direct=0
)";

    auto session = SDPParser::parseString(sdp);
    assert(session.has_value());
    assert(session->sessionName == "Riedel Artist IFB");
    assert(session->ptpDomain == 0);
    assert(session->ptpMasterMAC == "00-1B-21-AC-B5-4F");
    assert(session->sourceAddress == "192.168.1.100");

    std::cout << "✓ PASSED\n";
}

void testL16Encoding() {
    std::cout << "Test: L16 Encoding... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=L16 Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L16/48000/2
a=ptime:1
)";

    auto session = SDPParser::parseString(sdp);
    assert(session.has_value());
    assert(session->encoding == "L16");
    assert(session->numChannels == 2);

    std::cout << "✓ PASSED\n";
}

void testHighSampleRates() {
    std::cout << "Test: High Sample Rates (96kHz, 192kHz)... ";

    // Test 96kHz
    std::string sdp96 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=96kHz Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/96000/8
a=ptime:0.5
a=framecount:48
)";

    auto session96 = SDPParser::parseString(sdp96);
    assert(session96.has_value());
    assert(session96->sampleRate == 96000);

    // Test 192kHz
    std::string sdp192 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=192kHz Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/192000/8
a=ptime:0.25
a=framecount:48
)";

    auto session192 = SDPParser::parseString(sdp192);
    assert(session192.has_value());
    assert(session192->sampleRate == 192000);

    std::cout << "✓ PASSED\n";
}

void testMultiChannelConfigurations() {
    std::cout << "Test: Multi-Channel Configurations... ";

    // Test 64 channels
    std::string sdp64 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=64 Channel Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/48000/64
a=ptime:1
)";

    auto session64 = SDPParser::parseString(sdp64);
    assert(session64.has_value());
    assert(session64->numChannels == 64);

    std::cout << "✓ PASSED\n";
}

void testSDPGeneration() {
    std::cout << "Test: SDP Generation... ";

    SDPSession session;
    session.sessionName = "Generated Stream";
    session.sessionInfo = "Test Description";
    session.originAddress = "192.168.1.200";
    session.connectionAddress = "239.69.100.1";
    session.port = 5008;
    session.sampleRate = 48000;
    session.numChannels = 8;
    session.encoding = "L24";
    session.ptime = 1;
    session.framecount = 48;
    session.ptpDomain = 0;

    std::string generated = SDPParser::generate(session);
    assert(!generated.empty());

    // Verify it can be parsed back
    auto reparsed = SDPParser::parseString(generated);
    assert(reparsed.has_value());
    assert(reparsed->sessionName == session.sessionName);
    assert(reparsed->connectionAddress == session.connectionAddress);
    assert(reparsed->port == session.port);

    // RFC 4566 5: every line ends in CRLF (receivers such as Dante may be strict)
    size_t lines = 0;
    for (size_t at = generated.find('\n'); at != std::string::npos; at = generated.find('\n', at + 1)) {
        assert(at > 0 && generated[at - 1] == '\r');
        ++lines;
    }
    assert(lines >= 7 && generated.size() >= 2 && generated.compare(generated.size() - 2, 2, "\r\n") == 0);

    std::cout << "✓ PASSED\n";
}

void testInvalidSDP() {
    std::cout << "Test: Invalid SDP Handling... ";

    // Empty SDP
    auto empty = SDPParser::parseString("");
    assert(!empty.has_value());

    // Missing required fields
    std::string incomplete = R"(v=0
s=Incomplete
)";
    auto inc = SDPParser::parseString(incomplete);
    assert(!inc.has_value());

    std::cout << "✓ PASSED\n";
}

void testFileOperations() {
    std::cout << "Test: File Operations... ";

    // Create test SDP file
    std::string testPath = "/tmp/test_aes67.sdp";
    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=File Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/48000/8
)";

    // Write test file
    std::ofstream out(testPath);
    out << sdp;
    out.close();

    // Parse from file
    auto session = SDPParser::parseFile(testPath);
    assert(session.has_value());
    assert(session->sessionName == "File Test");

    // Cleanup
    std::remove(testPath.c_str());

    std::cout << "✓ PASSED\n";
}

// The o= line is "<user> <id> <version> <nettype> <addrtype> <address>", with
// nettype IN and addrtype IP4 (RFC 4566). A session built from defaults, as
// runtime-created TX streams are, must not swap them.
void testOriginLineFromDefaults() {
    std::cout << "Test: o= line from default session fields... ";

    SDPSession session;
    session.sessionName = "Defaults";
    session.originAddress = "10.0.0.5";
    session.connectionAddress = "239.69.100.2";
    session.port = 5004;

    const std::string generated = SDPParser::generate(session);
    const size_t origin = generated.find("o=");
    assert(origin != std::string::npos);
    const std::string line = generated.substr(origin, generated.find("\r\n", origin) - origin);
    const std::string expectedEnd = " IN IP4 10.0.0.5";
    assert(line.size() > expectedEnd.size() &&
           line.compare(line.size() - expectedEnd.size(), expectedEnd.size(), expectedEnd) == 0);

    auto reparsed = SDPParser::parseString(generated);
    assert(reparsed.has_value());
    assert(reparsed->originNetworkType == "IN");
    assert(reparsed->originAddressType == "IP4");

    std::cout << "PASSED\n";
}

std::string sdpWithRefclk(const std::string& refclk) {
    // The form a Behringer WING (Dante) announces; its real ts-refclk line
    // writes the PTP domain as a bare trailing number
    return "v=0\n"
           "o=- 1 1 IN IP4 10.46.70.20\n"
           "s=WING\n"
           "c=IN IP4 239.69.218.192/32\n"
           "t=0 0\n"
           "a=recvonly\n"
           "m=audio 5004 RTP/AVP 103\n"
           "a=rtpmap:103 L24/48000/8\n"
           "a=ptime:1\n" +
           refclk +
           "a=mediaclk:direct=0\n";
}

// RFC 7273 spells the domain "domain-nmbr=0" (Riedel does); Dante devices
// write a bare ":0", and some senders give no domain or a non-PTP reference.
// None of these may reject an otherwise valid stream.
void testTsRefclkForms() {
    std::cout << "Test: ts-refclk forms seen in the wild... ";

    auto wing = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:0\n"));
    assert(wing.has_value());
    assert(wing->encoding == "L24" && wing->sampleRate == 48000 && wing->numChannels == 8);
    assert(wing->connectionAddress == "239.69.218.192" && wing->port == 5004 && wing->payloadType == 103);
    assert(wing->ptpMasterMAC == "00-1D-C1-FF-FE-D1-7B-F3");
    assert(wing->ptpDomain == 0);

    auto bareDomain = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:5\n"));
    assert(bareDomain.has_value() && bareDomain->ptpDomain == 5);

    auto rfc = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:domain-nmbr=7\n"));
    assert(rfc.has_value() && rfc->ptpDomain == 7 && rfc->ptpMasterMAC == "00-1D-C1-FF-FE-D1-7B-F3");

    auto noDomain = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3\n"));
    assert(noDomain.has_value() && noDomain->ptpDomain == 0 && noDomain->ptpMasterMAC == "00-1D-C1-FF-FE-D1-7B-F3");

    auto localMac = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:localmac=00-1D-C1-D1-7B-F3\n"));
    assert(localMac.has_value() && localMac->ptpDomain == -1 && localMac->ptpMasterMAC.empty());

    auto traceable = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:traceable\n"));
    assert(traceable.has_value() && traceable->ptpDomain == -1);

    // RFC 7273 allows several references; an unrecognised one must not erase
    // a PTP reference already found
    auto both = SDPParser::parseString(sdpWithRefclk("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:0\n"
                                                     "a=ts-refclk:localmac=00-1D-C1-D1-7B-F3\n"));
    assert(both.has_value() && both->ptpDomain == 0 && both->ptpMasterMAC == "00-1D-C1-FF-FE-D1-7B-F3");

    // We write AES67's form (as Dante and RAVENNA do), which reads back the same
    SDPSession ours = *wing;
    ours.ptpDomain = 3;
    const std::string generated = SDPParser::generate(ours);
    assert(generated.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:3\r\n") != std::string::npos);
    assert(generated.find("domain-nmbr") == std::string::npos);
    auto reread = SDPParser::parseString(generated);
    assert(reread.has_value() && reread->ptpDomain == 3 && reread->ptpMasterMAC == "00-1D-C1-FF-FE-D1-7B-F3");

    std::cout << "PASSED\n";
}

// AES67 packet times include 125, 250 and 333 microseconds (#22)
void testFractionalPtime() {
    std::cout << "Test: fractional a=ptime... ";

    const double values[] = {0.125, 0.25, 0.333, 1.0, 4.0};
    for (double ptime : values) {
        std::ostringstream text;
        text << ptime;
        auto session = SDPParser::parseString(sdpWithRefclk("a=ptime:" + text.str() + "\n"));
        assert(session.has_value());
        assert(std::abs(session->ptime - ptime) < 1e-9);

        // And it survives generating and parsing again
        const std::string generated = SDPParser::generate(*session);
        assert(generated.find("a=ptime:" + text.str() + "\r\n") != std::string::npos);
        auto reparsed = SDPParser::parseString(generated);
        assert(reparsed.has_value() && std::abs(reparsed->ptime - ptime) < 1e-9);
    }

    // Nonsense is rejected rather than read as 0
    assert(!SDPParser::parseString(sdpWithRefclk("a=ptime:fast\n")).has_value());
    assert(!SDPParser::parseString(sdpWithRefclk("a=ptime:0\n")).has_value());

    std::cout << "PASSED\n";
}

// RFC 3551: an rtpmap without a channel count means one channel (#24)
void testRtpmapWithoutChannelCount() {
    std::cout << "Test: rtpmap without a channel count... ";

    auto mono = SDPParser::parseString(R"(v=0
o=- 1 1 IN IP4 10.0.0.5
s=Mono
c=IN IP4 239.69.1.1/32
t=0 0
m=audio 5004 RTP/AVP 96
a=rtpmap:96 L24/48000
)");
    assert(mono.has_value());
    assert(mono->numChannels == 1);

    std::cout << "PASSED\n";
}

// Packet timing is validated however a session was made (#75): a session read
// from streams.json never went through the a=ptime parser
void testPacketTimingValidation() {
    std::cout << "Test: packet timing validation... ";

    auto session = [](double ptime, uint32_t sampleRate, uint32_t framecount) {
        SDPSession s;
        s.sessionName = "Timing";
        s.connectionAddress = "239.69.1.1";
        s.encoding = "L24";
        s.sampleRate = sampleRate;
        s.ptime = ptime;
        s.framecount = framecount;
        return s;
    };

    // The AES67 packet times, at the rates we run
    assert(session(0.125, 48000, 6).isValid());
    assert(session(0.333, 48000, 16).isValid());
    assert(session(1.0, 96000, 96).isValid());
    assert(session(4.0, 48000, 192).isValid());
    assert(session(1.0, 192000, 192).isValid());
    assert(session(1.0, 48000, 0).isValid());  // no a=framecount
    assert(session(10.0, 48000, 480).isValid());

    // Not a packet time
    assert(!session(0.0, 48000, 0).isValid());
    assert(!session(-1.0, 48000, 0).isValid());
    assert(!session(std::nan(""), 48000, 0).isValid());
    assert(!session(INFINITY, 48000, 0).isValid());
    assert(!session(1e300, 48000, 0).isValid());

    // More frames per packet than the receiver takes
    assert(!session(20.0, 48000, 0).isValid());       // 960 frames
    assert(!session(4.0, 192000, 0).isValid());       // 768 frames
    assert(!session(1.0, 48000, 513).isValid());
    assert(!session(1.0, 48000, 4000000000u).isValid());

    const auto errors = session(0.0, 48000, 0).getValidationErrors();
    assert(errors.size() == 1 && errors[0].find("ptime") != std::string::npos);

    std::cout << "PASSED\n";
}

void runAllTests() {
    std::cout << "\n=== AES67 SDP Parser Test Suite ===\n\n";

    testBasicSDPParsing();
    testRiedelCompatibleSDP();
    testL16Encoding();
    testHighSampleRates();
    testMultiChannelConfigurations();
    testSDPGeneration();
    testOriginLineFromDefaults();
    testTsRefclkForms();
    testFractionalPtime();
    testRtpmapWithoutChannelCount();
    testPacketTimingValidation();
    testInvalidSDP();
    testFileOperations();

    std::cout << "\n✅ All SDP Parser tests passed!\n\n";
}

} // namespace Tests
} // namespace AES67

int main() {
    try {
        AES67::Tests::runAllTests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
