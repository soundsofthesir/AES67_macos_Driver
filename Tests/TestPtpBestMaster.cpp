//
// TestPtpBestMaster.cpp
// AES67 macOS Driver
// Choosing the PTP master to follow (IEEE 1588-2008 clause 9.3, as a
// slave-only ordinary clock). Time is passed in, in seconds, so every case is
// deterministic.
//

#include "../NetworkEngine/PTP/PtpBestMaster.h"
#include <cmath>
#include <iostream>

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

ClockIdentity clock(uint8_t last) { return ClockIdentity{{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x00, 0x00, last}}; }
PortIdentity port(uint8_t last) { return PortIdentity{clock(last), 1}; }

struct Master {
    PortIdentity sender;
    AnnounceBody body;
    int8_t logInterval{1};  // announce every 2 s
    uint8_t domain{0};
    uint16_t sequence{0};
};

Master master(uint8_t id) {
    Master m;
    m.sender = port(id);
    m.body.grandmasterIdentity = clock(id);
    return m;
}

Message announceFrom(Master& m) {
    const auto bytes = buildAnnounce(m.sender, m.domain, m.sequence++, m.logInterval, m.body);
    return *parse(bytes.data(), bytes.size());
}

// Two announces, one interval apart: enough to qualify (threshold 2)
void qualify(BestMaster& bmc, Master& m, double& now) {
    bmc.onAnnounce(announceFrom(m), now);
    now += 2.0;
    bmc.onAnnounce(announceFrom(m), now);
}

BestMaster::Config config() {
    BestMaster::Config c;
    c.self = clock(0xEE);
    return c;
}

void testComparisonOrder() {
    std::cout << "Masters are compared in the standard's order" << std::endl;
    auto better = [](Master a, Master b) {
        return BestMaster::compare(Candidate{a.sender, a.body, a.logInterval}, Candidate{b.sender, b.body, b.logInterval}) < 0;
    };
    Master a = master(1), b = master(2);
    a.body.grandmasterPriority1 = 100;
    b.body.grandmasterClockClass = 6;  // a far better clock, but priority1 comes first
    CHECK(better(a, b), "priority1 decides before clock class");

    a = master(1); b = master(2);
    b.body.grandmasterClockClass = 6;
    CHECK(better(b, a), "then clock class");
    b = master(2);
    b.body.grandmasterClockAccuracy = 0x21;  // 100 ns
    CHECK(better(b, a), "then clock accuracy");
    b = master(2);
    b.body.grandmasterOffsetScaledLogVariance = 0x4E5D;
    CHECK(better(b, a), "then variance");
    b = master(2);
    b.body.grandmasterPriority2 = 127;
    CHECK(better(b, a), "then priority2");
    CHECK(better(master(1), master(2)), "then the lower grandmaster identity");

    // The same grandmaster reached two ways (through boundary clocks)
    Master direct = master(1), viaBoundary = master(1);
    viaBoundary.sender = port(0);  // a lower sender identity, so only steps removed can decide
    viaBoundary.body.stepsRemoved = 1;
    CHECK(better(direct, viaBoundary), "for the same grandmaster, fewer steps removed wins");
}

void testQualificationIgnoresStrays() {
    std::cout << "A master must announce twice before it is followed" << std::endl;
    BestMaster bmc(config());
    Master a = master(5);
    double now = 0;
    CHECK(!bmc.onAnnounce(announceFrom(a), now) && !bmc.selected(), "one announce is not enough");
    now += 2.0;
    CHECK(bmc.onAnnounce(announceFrom(a), now), "a second, within four intervals, selects it");
    CHECK(bmc.selected() && bmc.selected()->sender == a.sender, "the selected master");

    // A single stray announce from a better clock must not switch master
    Master better = master(1);
    better.body.grandmasterPriority1 = 1;
    now += 0.5;
    CHECK(!bmc.onAnnounce(announceFrom(better), now), "one stray announce does not switch");
    CHECK(bmc.selected()->sender == a.sender, "still on the first master");
    now += 2.0;
    bmc.onAnnounce(announceFrom(a), now);
    CHECK(bmc.onAnnounce(announceFrom(better), now), "once it announces again it qualifies and takes over");
    CHECK(bmc.selected()->sender == better.sender, "the better master is selected");

    // Announces older than four intervals do not count towards qualifying
    BestMaster fresh(config());
    Master slow = master(7);
    double t = 0;
    fresh.onAnnounce(announceFrom(slow), t);
    t += 8.5;  // more than 4 x 2 s later
    CHECK(!fresh.onAnnounce(announceFrom(slow), t) && !fresh.selected(), "announces too far apart do not qualify");
}

void testTimeoutUsesTheMastersInterval() {
    std::cout << "A silent master is dropped after three of its own announce intervals" << std::endl;
    BestMaster bmc(config());
    Master a = master(5);  // every 2 s: timeout 6 s
    double now = 0;
    qualify(bmc, a, now);
    CHECK(!bmc.tick(now + 5.9) && bmc.selected(), "still followed just inside 6 s");
    CHECK(bmc.tick(now + 6.1) && !bmc.selected(), "dropped just after 6 s");

    // A burst then silence: still qualified (three announces within the
    // window), so only the timeout, from its own interval, can drop it
    BestMaster fast(config());
    Master f = master(6);
    f.logInterval = -2;  // every 0.25 s: timeout 0.75 s
    for (double t : {0.0, 0.05, 0.1}) fast.onAnnounce(announceFrom(f), t);
    CHECK(fast.selected().has_value(), "a fast master qualifies");
    CHECK(!fast.tick(0.8) && fast.selected(), "still followed 0.7 s after its last announce");
    CHECK(fast.tick(0.9) && !fast.selected(), "dropped 0.8 s after it, not after a fixed timeout");
}

void testFallsBackToTheNextMaster() {
    std::cout << "When the master goes quiet, the next qualified master takes over" << std::endl;
    BestMaster bmc(config());
    Master best = master(1), backup = master(2);
    best.body.grandmasterPriority1 = 10;
    double now = 0;
    qualify(bmc, best, now);
    now = 0;
    qualify(bmc, backup, now);
    CHECK(bmc.selected()->sender == best.sender, "the better master is followed");
    // The backup keeps announcing; the best stops
    for (int i = 0; i < 4; ++i) {
        now += 2.0;
        bmc.onAnnounce(announceFrom(backup), now);
        bmc.tick(now);
    }
    CHECK(bmc.selected() && bmc.selected()->sender == backup.sender, "the backup takes over");
}

void testIgnoredAnnounces() {
    std::cout << "Announces that must not count are ignored" << std::endl;
    BestMaster bmc(config());
    double now = 0;

    Master otherDomain = master(3);
    otherDomain.domain = 1;
    qualify(bmc, otherDomain, now);
    CHECK(!bmc.selected(), "another PTP domain");

    Master self = master(0xEE);  // our own clock identity, looped back
    qualify(bmc, self, now);
    CHECK(!bmc.selected(), "our own announces");

    Master tooFar = master(4);
    tooFar.body.stepsRemoved = 255;
    qualify(bmc, tooFar, now);
    CHECK(!bmc.selected(), "stepsRemoved of 255 or more");

    // An interval of 127 would keep a silent master for 1.7e38 s, and -128
    // would never qualify one
    for (int8_t logInterval : {int8_t(127), int8_t(5), int8_t(-4), int8_t(-128)}) {
        BestMaster outside(config());
        Master m = master(7);
        m.logInterval = logInterval;
        double t = 0;
        for (int i = 0; i < 4; ++i, t += 0.1) outside.onAnnounce(announceFrom(m), t);
        CHECK(!outside.selected(), "log announce interval " << int(logInterval));
    }
}

void testAnnounceIntervalLimits() {
    std::cout << "Announce intervals from 1/8 s to 16 s are followed" << std::endl;
    for (int8_t logInterval : {int8_t(-3), int8_t(4)}) {
        BestMaster bmc(config());
        Master m = master(8);
        m.logInterval = logInterval;
        const double interval = std::ldexp(1.0, logInterval);
        bmc.onAnnounce(announceFrom(m), 0);
        bmc.onAnnounce(announceFrom(m), interval);
        CHECK(bmc.selected().has_value(), "log interval " << int(logInterval) << " qualifies");
        CHECK(!bmc.tick(interval * 3.9) && bmc.selected(), "log interval " << int(logInterval) << " kept inside its timeout");
        CHECK(bmc.tick(interval * 4.1) && !bmc.selected(), "log interval " << int(logInterval) << " dropped after it");
    }
}

} // namespace

int main() {
    testComparisonOrder();
    testQualificationIgnoresStrays();
    testTimeoutUsesTheMastersInterval();
    testFallsBackToTheNextMaster();
    testIgnoredAnnounces();
    testAnnounceIntervalLimits();

    std::cout << "\nPTP best master: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
