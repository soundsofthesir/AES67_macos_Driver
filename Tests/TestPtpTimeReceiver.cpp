//
// TestPtpTimeReceiver.cpp
// AES67 macOS Driver
// The PTP time receiver (slave) against scripted masters on lo0. Everything
// runs on the loopback interface with test ports, so nothing reaches the
// network. Each master has its own clock (offset epoch, skewed rate); the
// receiver must follow the right one and know its time.
//

#include "../NetworkEngine/PTP/PtpTimeReceiver.h"
#include "PtpTestMaster.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include <arpa/inet.h>
#include <poll.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <net/if.h>
#include <thread>
#include <vector>

using namespace AES67;
using namespace AES67::Ptp;
using namespace AES67::PtpTest;

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


// Wait up to `seconds` for the receiver to lock, then measure how far its
// idea of the master's time is from the master's actual time
struct Result {
    bool locked{false};
    double phaseErrorNs{1e18};
    double rateErrorPpm{1e9};
    TimeReceiver::Status status;
};

Result waitForLock(TimeReceiver& receiver, const ScriptedMaster& master, double seconds) {
    Result r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        r.status = receiver.status();
        if (r.status.state == Servo::State::Locked && r.status.estimate) {
            std::this_thread::sleep_for(std::chrono::seconds(1));  // settle a little further
            r.status = receiver.status();
            const uint64_t now = hostNanos();
            r.locked = r.status.state == Servo::State::Locked;
            if (r.status.estimate) {
                r.phaseErrorNs = static_cast<double>(static_cast<int64_t>(r.status.estimate->masterAt(now) - master.clockAt(now)));
                r.rateErrorPpm = (r.status.estimate->rate - 1.0) * 1e6 - 100.0;
            }
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return r;
}

void testFollowsATwoStepMaster() {
    std::cout << "Two-step master, multicast Delay_Req, correction fields" << std::endl;
    ScriptedMaster::Settings s;
    s.followUpCorrectionNs = 200000;   // 200 us: a wrong sign shows as 400 us
    s.delayRespCorrectionNs = 150000;  // 150 us: a wrong sign shows as 150 us of phase
    ScriptedMaster master(s);
    TimeReceiver receiver(receiverConfig());
    CHECK(receiver.start(), "the receiver should start");
    const Result r = waitForLock(receiver, master, 20);
    std::cout << "  phase " << r.phaseErrorNs / 1000 << " us, rate " << r.rateErrorPpm << " ppm, path delay "
              << r.status.pathDelayNs / 1000 << " us" << std::endl;
    CHECK(r.locked, "locks to the master");
    CHECK(std::fabs(r.phaseErrorNs) < 100000, "knows the master's time within 100 us (" << r.phaseErrorNs << " ns)");
    CHECK(std::fabs(r.rateErrorPpm) < 20, "and its rate (+100 ppm) within 20 ppm (" << r.rateErrorPpm << ")");
    CHECK(r.status.master && r.status.master->sender == master.identity(), "the master is the one announcing");
    CHECK(r.status.delayResponses > 0 && master.multicastDelayRequests() > 0 && master.unicastDelayRequests() == 0,
          "Delay_Req is sent to the multicast group");
    receiver.stop();
}

void testHybridModeSendsUnicastDelayRequests() {
    std::cout << "Hybrid mode: Delay_Req by unicast to the master" << std::endl;
    ScriptedMaster master(ScriptedMaster::Settings{});
    TimeReceiver receiver(receiverConfig(true));
    CHECK(receiver.start(), "the receiver should start");
    const Result r = waitForLock(receiver, master, 20);
    CHECK(r.locked && std::fabs(r.phaseErrorNs) < 100000, "locks within 100 us (" << r.phaseErrorNs << " ns)");
    CHECK(master.unicastDelayRequests() > 0 && master.multicastDelayRequests() == 0,
          "every Delay_Req goes unicast to the master's address (unicast " << master.unicastDelayRequests()
              << ", multicast " << master.multicastDelayRequests() << ")");
    receiver.stop();
}

void testFollowsAOneStepMaster() {
    std::cout << "One-step master (time in the Sync itself)" << std::endl;
    ScriptedMaster::Settings s;
    s.twoStep = false;
    s.followUpCorrectionNs = 120000;  // carried in the Sync
    ScriptedMaster master(s);
    TimeReceiver receiver(receiverConfig());
    CHECK(receiver.start(), "the receiver should start");
    const Result r = waitForLock(receiver, master, 20);
    CHECK(r.locked && std::fabs(r.phaseErrorNs) < 100000, "locks within 100 us (" << r.phaseErrorNs << " ns)");
    receiver.stop();
}

void testIgnoresAWorseMastersSyncs() {
    std::cout << "A worse master's Syncs (5 s apart in time) are ignored" << std::endl;
    ScriptedMaster::Settings good;
    good.priority1 = 100;
    ScriptedMaster master(good);
    TimeReceiver receiver(receiverConfig());
    CHECK(receiver.start(), "the receiver should start");
    std::this_thread::sleep_for(std::chrono::seconds(1));  // following the good master
    ScriptedMaster::Settings rogue;
    rogue.identityByte = 0x02;
    rogue.priority1 = 200;
    rogue.epochNs = good.epochNs + 5000000000ULL;
    ScriptedMaster other(rogue);
    const Result r = waitForLock(receiver, master, 20);
    CHECK(r.locked && std::fabs(r.phaseErrorNs) < 100000, "follows the better master's time (" << r.phaseErrorNs << " ns)");
    CHECK(r.status.master && r.status.master->sender == master.identity(), "and has selected it");
    CHECK(r.status.generation == 1, "never mixing in the other master's time (generation " << r.status.generation << ")");
    receiver.stop();
}

void testSwitchesToABetterMaster() {
    std::cout << "A better master appears: switch, with a new timeline" << std::endl;
    ScriptedMaster::Settings first;
    first.priority1 = 200;
    ScriptedMaster a(first);
    TimeReceiver receiver(receiverConfig());
    CHECK(receiver.start(), "the receiver should start");
    const Result before = waitForLock(receiver, a, 20);
    CHECK(before.locked, "locks to the first master");

    ScriptedMaster::Settings second;
    second.identityByte = 0x03;
    second.priority1 = 50;
    // 3 ms apart: under the servo's 10 ms step threshold, so only the switch
    // itself can keep the two masters' times from being blended
    second.epochNs = first.epochNs + 3000000ULL;
    second.skewPpm = 100.0;
    ScriptedMaster b(second);
    std::this_thread::sleep_for(std::chrono::seconds(1));  // two announces qualify it
    const Result after = waitForLock(receiver, b, 20);
    CHECK(after.status.master && after.status.master->sender == b.identity(), "switches to the better master");
    CHECK(after.status.generation == before.status.generation + 1, "starting a new timeline");
    CHECK(after.locked && std::fabs(after.phaseErrorNs) < 100000, "and locks to its time (" << after.phaseErrorNs << " ns)");
    receiver.stop();
}

void testHoldoverWhenTheMasterGoes() {
    std::cout << "The master goes: holdover; it returns: same timeline" << std::endl;
    ScriptedMaster::Settings s;
    auto master = std::make_unique<ScriptedMaster>(s);
    TimeReceiver receiver(receiverConfig());
    CHECK(receiver.start(), "the receiver should start");
    const Result before = waitForLock(receiver, *master, 20);
    CHECK(before.locked, "locks");
    master->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // announce timeout is 0.75 s
    const auto quiet = receiver.status();
    CHECK(quiet.state == Servo::State::Holdover && !quiet.master, "holdover with no master (state "
                                                                       << int(quiet.state) << ")");
    master = std::make_unique<ScriptedMaster>(s);
    const Result after = waitForLock(receiver, *master, 20);
    CHECK(after.locked && after.status.generation == before.status.generation,
          "the same master returning resumes the same timeline (generation " << after.status.generation << ")");
    receiver.stop();
}

void testHoldoverWhenSyncsStop() {
    std::cout << "Syncs stop, Announces go on: holdover, then acquiring when holdover runs out" << std::endl;
    ScriptedMaster master(ScriptedMaster::Settings{});
    TimeReceiver::Config config = receiverConfig();
    config.servo.holdoverLimitSeconds = 2.0;
    TimeReceiver receiver(config);
    CHECK(receiver.start(), "the receiver should start");
    const Result before = waitForLock(receiver, master, 20);
    CHECK(before.locked, "locks");
    master.stopSyncs();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // Sync timeout: 1 s at 16 per second
    const auto quiet = receiver.status();
    CHECK(quiet.state == Servo::State::Holdover && quiet.master, "holdover, with the master still announcing (state "
                                                                     << int(quiet.state) << ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    const auto gaveUp = receiver.status();
    CHECK(gaveUp.state == Servo::State::Acquiring && !gaveUp.estimate, "acquiring once holdover runs out (state "
                                                                           << int(gaveUp.state) << ")");
    receiver.stop();
}

} // namespace

int main(int argc, char** argv) {
    // Optional: run only the tests whose names contain argv[1]
    const std::string only = argc > 1 ? argv[1] : "";
    const std::pair<const char*, void (*)()> tests[] = {
        {"twoStep", testFollowsATwoStepMaster},
        {"hybrid", testHybridModeSendsUnicastDelayRequests},
        {"oneStep", testFollowsAOneStepMaster},
        {"worseMaster", testIgnoresAWorseMastersSyncs},
        {"betterMaster", testSwitchesToABetterMaster},
        {"holdover", testHoldoverWhenTheMasterGoes},
        {"syncsStop", testHoldoverWhenSyncsStop},
    };
    for (const auto& [name, test] : tests) {
        if (only.empty() || std::string(name).find(only) != std::string::npos) test();
    }

    std::cout << "\nPTP time receiver: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
