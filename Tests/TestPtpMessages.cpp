//
// TestPtpMessages.cpp
// AES67 macOS Driver
// PTP (IEEE 1588-2008) message parsing and building. The golden vectors are
// written byte by byte from the standard's layout, not produced by our own
// builder, so a mistake in the code cannot be mirrored in the test.
//

#include "../NetworkEngine/PTP/PtpMessages.h"
#include <cstdint>
#include <iostream>
#include <vector>

using namespace AES67::Ptp;

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

// A Behringer WING's grandmaster identity; its PTP time counts from power-on,
// so seconds are small (about 261432 s, 0x03FD38) rather than TAI since 1970
const std::vector<uint8_t> kWingClock = {0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0xD1, 0x7B, 0xF3};

std::vector<uint8_t> header(uint8_t type, uint16_t length, uint8_t flags0, uint8_t flags1,
                            std::vector<uint8_t> correction, uint16_t sequence, uint8_t control, uint8_t logInterval) {
    std::vector<uint8_t> h = {type, 0x02, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length & 0xFF),
                              0x00, 0x00, flags0, flags1};
    h.insert(h.end(), correction.begin(), correction.end());              // 8..15
    h.insert(h.end(), {0x00, 0x00, 0x00, 0x00});                          // 16..19 reserved
    h.insert(h.end(), kWingClock.begin(), kWingClock.end());              // 20..27 clockIdentity
    h.insert(h.end(), {0x00, 0x01});                                      // 28..29 portNumber 1
    h.insert(h.end(), {static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence & 0xFF)});
    h.push_back(control);
    h.push_back(logInterval);
    return h;
}

const std::vector<uint8_t> kZeroCorrection(8, 0x00);

// seconds 261432 (0x00 00 00 03 FD 38), nanoseconds as given
std::vector<uint8_t> wingTimestamp(uint32_t nanoseconds) {
    return {0x00, 0x00, 0x00, 0x03, 0xFD, 0x38, static_cast<uint8_t>(nanoseconds >> 24),
            static_cast<uint8_t>(nanoseconds >> 16), static_cast<uint8_t>(nanoseconds >> 8),
            static_cast<uint8_t>(nanoseconds)};
}

std::vector<uint8_t> concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void testSyncTwoStep() {
    std::cout << "Sync (two-step)" << std::endl;
    // logMessageInterval -3: 8 Sync per second
    const auto bytes = concat(header(0x00, 44, 0x02, 0x00, kZeroCorrection, 0x1234, 0x00, 0xFD), wingTimestamp(500000000));
    const auto m = parse(bytes.data(), bytes.size());
    CHECK(m.has_value(), "a Sync should parse");
    if (!m) return;
    CHECK(m->header.type == MessageType::Sync, "type Sync");
    CHECK(m->header.version == 2 && m->header.length == 44 && m->header.domain == 0, "version, length, domain");
    CHECK(m->header.twoStep() && !m->header.unicast(), "the two-step flag is read");
    CHECK(m->header.source.clock.toString() == "00-1D-C1-FF-FE-D1-7B-F3" && m->header.source.port == 1,
          "the sender's port identity");
    CHECK(m->header.sequenceId == 0x1234, "sequence ID");
    CHECK(m->header.logMessageInterval == -3, "log interval is signed (-3 = 8 per second)");
    CHECK(m->timestamp.seconds == 261432 && m->timestamp.nanoseconds == 500000000, "origin timestamp");
    CHECK(m->timestamp.totalNanoseconds() == 261432500000000ULL, "timestamp in nanoseconds");
}

void testFollowUpWithCorrection() {
    std::cout << "Follow_Up with a correction field" << std::endl;
    // correction 0x18000 = 98304 / 2^16 = 1.5 ns
    const std::vector<uint8_t> correction = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00};
    const auto bytes = concat(header(0x08, 44, 0x00, 0x00, correction, 0x1234, 0x02, 0xFD), wingTimestamp(500000123));
    const auto m = parse(bytes.data(), bytes.size());
    CHECK(m && m->header.type == MessageType::FollowUp, "a Follow_Up should parse");
    if (!m) return;
    CHECK(m->header.correction == 98304 && m->header.correctionNanoseconds() == 1.5, "correction in scaled and real ns");
    CHECK(m->timestamp.nanoseconds == 500000123, "precise origin timestamp");
}

void testDelayRespWithNegativeCorrection() {
    std::cout << "Delay_Resp" << std::endl;
    // correction -65536 = -1 ns; requesting port AA-BB-CC-FF-FE-DD-EE-FF port 1
    const std::vector<uint8_t> correction = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    auto bytes = concat(header(0x09, 54, 0x00, 0x00, correction, 0x0042, 0x03, 0x00), wingTimestamp(501000000));
    bytes = concat(bytes, {0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF, 0x00, 0x01});
    const auto m = parse(bytes.data(), bytes.size());
    CHECK(m && m->header.type == MessageType::DelayResp, "a Delay_Resp should parse");
    if (!m) return;
    CHECK(m->header.correction == -65536 && m->header.correctionNanoseconds() == -1.0, "a negative correction");
    CHECK(m->timestamp.nanoseconds == 501000000, "receive timestamp");
    CHECK(m->requestingPort.has_value() && m->requestingPort->clock.toString() == "AA-BB-CC-FF-FE-DD-EE-FF" &&
              m->requestingPort->port == 1,
          "requesting port identity");
}

void testAnnounce() {
    std::cout << "Announce" << std::endl;
    // log interval 1 (every 2 s); UTC offset 37; priority1 128; class 248,
    // accuracy 0xFE (unknown), variance 0xFFFF; priority2 128; grandmaster
    // = the WING; stepsRemoved 0; timeSource 0xA0 (internal oscillator)
    auto bytes = concat(header(0x0B, 64, 0x00, 0x08, kZeroCorrection, 0x0007, 0x05, 0x01), std::vector<uint8_t>(10, 0x00));
    bytes = concat(bytes, {0x00, 0x25, 0x00, 0x80, 0xF8, 0xFE, 0xFF, 0xFF, 0x80});
    bytes = concat(bytes, kWingClock);
    bytes = concat(bytes, {0x00, 0x00, 0xA0});
    const auto m = parse(bytes.data(), bytes.size());
    CHECK(m && m->header.type == MessageType::Announce && m->announce.has_value(), "an Announce should parse");
    if (!m || !m->announce) return;
    const AnnounceBody& a = *m->announce;
    CHECK(m->header.logMessageInterval == 1, "announce interval 2 s");
    CHECK(m->header.ptpTimescale(), "the PTP timescale flag");
    CHECK(a.currentUtcOffset == 37, "UTC offset");
    CHECK(a.grandmasterPriority1 == 128 && a.grandmasterPriority2 == 128, "priorities");
    CHECK(a.grandmasterClockClass == 248 && a.grandmasterClockAccuracy == 0xFE &&
              a.grandmasterOffsetScaledLogVariance == 0xFFFF,
          "clock quality");
    CHECK(a.grandmasterIdentity.toString() == "00-1D-C1-FF-FE-D1-7B-F3", "grandmaster identity");
    CHECK(a.stepsRemoved == 0 && a.timeSource == 0xA0, "steps removed and time source");
}

void testRejectsBadPackets() {
    std::cout << "Malformed packets are rejected" << std::endl;
    const auto sync = concat(header(0x00, 44, 0x02, 0x00, kZeroCorrection, 1, 0x00, 0xFD), wingTimestamp(0));
    CHECK(!parse(sync.data(), 33).has_value(), "shorter than a header");
    CHECK(!parse(sync.data(), 40).has_value(), "a Sync cut short");
    auto v1 = sync;
    v1[1] = 0x01;
    CHECK(!parse(v1.data(), v1.size()).has_value(), "PTP version 1");
    auto overlong = sync;
    overlong[3] = 60;  // messageLength 60 in a 44-byte datagram
    CHECK(!parse(overlong.data(), overlong.size()).has_value(), "a messageLength longer than the datagram");
    const auto announce = concat(header(0x0B, 44, 0, 0, kZeroCorrection, 1, 0x05, 0x01), std::vector<uint8_t>(10, 0));
    CHECK(!parse(announce.data(), announce.size()).has_value(), "an Announce without its body");

    // nanoseconds counts within one second, so 1e9 or more is not a timestamp
    const auto maxNanoseconds = concat(header(0x00, 44, 0x02, 0x00, kZeroCorrection, 1, 0x00, 0xFD), wingTimestamp(999999999));
    CHECK(parse(maxNanoseconds.data(), maxNanoseconds.size()).has_value(), "nanoseconds 999999999");
    for (uint32_t nanoseconds : {1000000000u, 0xFFFFFFFFu}) {
        const auto sync = concat(header(0x00, 44, 0x02, 0x00, kZeroCorrection, 1, 0x00, 0xFD), wingTimestamp(nanoseconds));
        CHECK(!parse(sync.data(), sync.size()).has_value(), "a Sync with nanoseconds " << nanoseconds);
        const auto followUp = concat(header(0x08, 44, 0, 0, kZeroCorrection, 1, 0x02, 0xFD), wingTimestamp(nanoseconds));
        CHECK(!parse(followUp.data(), followUp.size()).has_value(), "a Follow_Up with nanoseconds " << nanoseconds);
        auto delayResp = concat(header(0x09, 54, 0, 0, kZeroCorrection, 1, 0x03, 0x00), wingTimestamp(nanoseconds));
        delayResp.insert(delayResp.end(), 10, 0x00);
        CHECK(!parse(delayResp.data(), delayResp.size()).has_value(), "a Delay_Resp with nanoseconds " << nanoseconds);
        auto announce = concat(header(0x0B, 64, 0, 0, kZeroCorrection, 1, 0x05, 0x01), wingTimestamp(nanoseconds));
        announce.insert(announce.end(), 20, 0x00);
        CHECK(!parse(announce.data(), announce.size()).has_value(), "an Announce with nanoseconds " << nanoseconds);
    }

    // Message types we do not decode still parse their header (so a slave
    // can ignore them by type rather than see garbage)
    const auto signaling = concat(header(0x0C, 44, 0, 0, kZeroCorrection, 1, 0x05, 0x7F), std::vector<uint8_t>(10, 0));
    const auto m = parse(signaling.data(), signaling.size());
    CHECK(m && m->header.type == MessageType::Signaling && !m->announce, "a Signaling message parses its header only");
}

void testBuildDelayReq() {
    std::cout << "Building a Delay_Req" << std::endl;
    PortIdentity self{ClockIdentity{{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF}}, 1};
    const auto multicast = buildDelayReq(self, 0, 0x0042, false);
    std::vector<uint8_t> expected = {0x01, 0x02, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00};  // type, version, 44, domain 0, flags
    expected.insert(expected.end(), 12, 0x00);                                         // correction, reserved
    expected.insert(expected.end(), {0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF, 0x00, 0x01});
    expected.insert(expected.end(), {0x00, 0x42, 0x01, 0x7F});  // sequence, control Delay_Req, log interval 0x7F
    expected.insert(expected.end(), 10, 0x00);                  // origin timestamp (t3 is measured locally)
    CHECK(multicast == expected, "a multicast Delay_Req matches the standard byte for byte");

    const auto unicast = buildDelayReq(self, 3, 0x0043, true);
    CHECK(unicast.size() == 44 && unicast[4] == 3 && (unicast[6] & 0x04) != 0,
          "a unicast Delay_Req (hybrid mode) sets the domain and the unicast flag");
}

void testRoundTrip() {
    std::cout << "Built messages parse back" << std::endl;
    PortIdentity master{ClockIdentity{{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0xD1, 0x7B, 0xF3}}, 1};
    PortIdentity slave{ClockIdentity{{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF}}, 1};
    const Timestamp t{261432, 500000123};

    const auto sync = parse(buildSync(master, 0, 7, -3, true, Timestamp{}).data(), 44);
    CHECK(sync && sync->header.twoStep() && sync->header.sequenceId == 7 && sync->header.logMessageInterval == -3,
          "Sync");
    const auto follow = buildFollowUp(master, 0, 7, -3, t, 98304);
    const auto followUp = parse(follow.data(), follow.size());
    CHECK(followUp && followUp->timestamp == t && followUp->header.correction == 98304, "Follow_Up");
    const auto resp = buildDelayResp(master, 0, 9, 0, t, slave, -65536);
    const auto delayResp = parse(resp.data(), resp.size());
    CHECK(delayResp && delayResp->requestingPort == slave && delayResp->header.correction == -65536, "Delay_Resp");
    AnnounceBody body;
    body.grandmasterIdentity = master.clock;
    body.grandmasterPriority1 = 100;
    body.grandmasterClockClass = 6;
    const auto ann = buildAnnounce(master, 0, 3, 1, body);
    const auto announce = parse(ann.data(), ann.size());
    CHECK(announce && announce->announce && announce->announce->grandmasterPriority1 == 100 &&
              announce->announce->grandmasterClockClass == 6 && announce->announce->grandmasterIdentity == master.clock,
          "Announce");
}

} // namespace

int main() {
    testSyncTwoStep();
    testFollowUpWithCorrection();
    testDelayRespWithNegativeCorrection();
    testAnnounce();
    testRejectsBadPackets();
    testBuildDelayReq();
    testRoundTrip();

    std::cout << "\nPTP messages: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
