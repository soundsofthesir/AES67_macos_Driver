//
// TestPtpClockSource.cpp
// AES67 macOS Driver
// PTP as the clock source, end to end on lo0: a scripted master, the real
// time receiver, and the real MediaClock steered by the source. The media
// clock must run at the master's rate and in phase with its time (through
// the source's offset), without ever starting a new timeline.
//

#include "../NetworkEngine/Clock/PtpClockSource.h"
#include "PtpTestMaster.h"
#include "TestWait.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

using namespace AES67;
using namespace AES67::PtpTest;

namespace {

// 5 samples (~100 us) on a quiet machine, 25 on a loaded CI runner
const double kPhaseLimitSamples = AES67::TestWait::timingLimit(5.0, 25.0);

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

// The device's side: a media clock, written under a lock, and only on the
// timeline a rate was computed for
struct Device {
    MediaClock clock;
    std::mutex writeMutex;
    std::atomic<double> sampleRate{48000.0};
    std::atomic<int> rateWrites{0};
    std::atomic<int> staleWrites{0};
    std::atomic<int> changes{0};
    std::atomic<int64_t> fixedOffset{0};
    std::atomic<uint32_t> fixedGeneration{0};

    Device() { restart(7000000); }

    void restart(int64_t at) {
        std::lock_guard<std::mutex> lock(writeMutex);
        clock.reset(hostTimeNow(), at, MediaClock::samplesPerTick(sampleRate.load(), 1.0, HostTimebase::current()));
    }

    std::unique_ptr<PtpClockSource> makeSource() {
        PtpClockSource::Config config;
        config.receiver = receiverConfig();
        return std::make_unique<PtpClockSource>(
            config, clock, sampleRate,
            [this](uint32_t generation, double samplesPerTick) {
                std::lock_guard<std::mutex> lock(writeMutex);
                if (clock.snapshot().generation != generation) {
                    ++staleWrites;
                    return;
                }
                clock.setRate(hostTimeNow(), samplesPerTick);
                ++rateWrites;
            },
            [this] { ++changes; },
            [this](uint32_t generation, int64_t offset) {
                fixedGeneration = generation;
                fixedOffset = offset;
            });
    }
};

bool waitFor(const std::function<bool()>& condition, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return condition();
}

// The master's true time in samples plus the offset, minus the media position
double phaseErrorSamples(const Device& device, const PtpClockSource& source, const ScriptedMaster& master) {
    const uint64_t now = hostTimeNow();
    const MediaPosition ptp = ptpSamples(master.clockAt(HostTimebase::current().ticksToNanos(now)), 48000.0);
    const MediaPosition media = device.clock.snapshot().positionAt(now);
    return static_cast<double>(ptp.sample + *source.status().offset - media.sample) + (ptp.fraction - media.fraction);
}

double rateErrorPpm(const Device& device, double skewPpm) {
    const double nominal = MediaClock::samplesPerTick(48000.0, 1.0 + skewPpm * 1e-6, HostTimebase::current());
    return (device.clock.snapshot().samplesPerTick / nominal - 1.0) * 1e6;
}

void testLeavesTheClockAloneWithoutAMaster() {
    std::cout << "No master: the clock is left alone" << std::endl;
    Device device;
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(!source->active() && !source->locked() && source->clockDomain() == 0, "not active, not locked, no domain");
    CHECK(device.rateWrites == 0 && device.changes == 0, "no rate written, nothing to report");
    source->stop();
}

void testKeepsTryingWithoutItsInterface() {
    std::cout << "Its interface missing: keeps trying, touches nothing" << std::endl;
    Device device;
    PtpClockSource::Config config;
    config.receiver = receiverConfig();
    config.receiver.networkInterface = "aes67test9";  // no such interface
    config.retryPeriod = std::chrono::milliseconds(100);
    PtpClockSource source(config, device.clock, device.sampleRate, [&](uint32_t, double) { ++device.rateWrites; });
    source.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!source.receiving() && !source.active() && device.rateWrites == 0, "not receiving, no rate written");
    source.stop();
}

void testFollowsAMaster() {
    std::cout << "Follows a +100 ppm master: its rate, in phase, no new timeline" << std::endl;
    Device device;
    const uint32_t generation = device.clock.snapshot().generation;
    ScriptedMaster master(ScriptedMaster::Settings{});
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    CHECK(waitFor([&] { return source->locked(); }, 20), "locks");
    std::this_thread::sleep_for(std::chrono::seconds(3));  // slew out the takeover's half sample and settle
    const double phase = phaseErrorSamples(device, *source, master);
    const double rate = rateErrorPpm(device, 100.0);
    std::cout << "  phase " << phase << " samples, rate " << rate << " ppm" << std::endl;
    CHECK(source->active(), "PTP has the clock");
    CHECK(std::fabs(phase) < kPhaseLimitSamples, "within " << kPhaseLimitSamples << " samples of the master's time (" << phase << ")");
    // Seconds after lock PTP may still be pulling in a few samples of phase,
    // which moves the rate by tens of ppm (100 ppm at most, by design)
    CHECK(std::fabs(rate) < 50, "at its rate within 50 ppm (" << rate << ")");
    CHECK(device.clock.snapshot().generation == generation, "the device timeline never restarted");
    CHECK(source->clockDomain() == PtpClockSource::clockDomainFor(master.identity().clock, 0) &&
              source->clockDomain() != 0,
          "the clock domain is the grandmaster's");
    CHECK(device.changes >= 1, "the device was told lock and domain changed");
    CHECK(device.fixedOffset == *source->status().offset && device.fixedGeneration == generation,
          "and given the offset, for its timeline");
    source->stop();
    CHECK(!source->locked() && source->clockDomain() == 0, "stopped: neither locked nor in a domain");
}

void testADeviceTimelineRestart() {
    std::cout << "The device timeline restarts (IO start): PTP takes it over again" << std::endl;
    Device device;
    ScriptedMaster master(ScriptedMaster::Settings{});
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    CHECK(waitFor([&] { return source->locked(); }, 20), "locks");
    const int64_t before = *source->status().offset;
    device.restart(device.clock.snapshot().sampleAt(hostTimeNow()) + (1 << 20));
    CHECK(waitFor([&] { return source->status().offset && *source->status().offset != before; }, 2),
          "a new offset for the new timeline");
    CHECK(waitFor([&] { return device.fixedGeneration == device.clock.snapshot().generation; }, 1) &&
              device.fixedOffset == *source->status().offset,
          "given to the device for the new timeline");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const double phase = phaseErrorSamples(device, *source, master);
    CHECK(source->active() && std::fabs(phase) < kPhaseLimitSamples, "in phase on it (" << phase << ")");
    source->stop();
}

// The interface changes under it (cable replugged, new address, link down
// and up): the receiver starts again on the interface as it is now, its
// timeline numbers keep rising, and it locks again
void testRejoinsWhenItsInterfaceChanges() {
    std::cout << "Its interface changes or goes: the receiver starts again, and locks again" << std::endl;
    std::mutex stateMutex;
    std::optional<NetworkInterfaceDetection::InterfaceState> state = NetworkInterfaceDetection::currentState("lo0");
    CHECK(state.has_value(), "lo0 has a state");

    Device device;
    ScriptedMaster master(ScriptedMaster::Settings{});
    PtpClockSource::Config config;
    config.receiver = receiverConfig();
    config.retryPeriod = std::chrono::milliseconds(100);
    config.monitor.interval = std::chrono::milliseconds(100);
    config.monitor.provider = [&](const std::string&) {
        std::lock_guard<std::mutex> lock(stateMutex);
        return state;
    };
    std::atomic<int> changes{0};
    PtpClockSource source(
        config, device.clock, device.sampleRate,
        [&](uint32_t generation, double samplesPerTick) {
            std::lock_guard<std::mutex> lock(device.writeMutex);
            if (device.clock.snapshot().generation == generation) device.clock.setRate(hostTimeNow(), samplesPerTick);
        },
        [&] { ++changes; });
    source.start();
    CHECK(waitFor([&] { return source.locked(); }, 20), "locks");
    const uint32_t generation = source.status().receiver.generation;

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state->ipv4 = "127.0.0.99";  // a new address, as after DHCP or a replug
    }
    CHECK(waitFor([&] { return source.receiverRestarts() == 1; }, 2), "the receiver starts again");
    CHECK(waitFor([&] { return !source.locked(); }, 1), "not locked meanwhile (Core Audio is told)");
    CHECK(waitFor([&] { return source.locked(); }, 20), "locks again");
    CHECK(source.status().receiver.generation > generation,
          "on a new timeline number (" << source.status().receiver.generation << " after " << generation << ")");
    CHECK(source.active(), "PTP kept the clock throughout");

    std::optional<NetworkInterfaceDetection::InterfaceState> saved;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        saved = state;
        state.reset();  // the interface is gone
    }
    CHECK(waitFor([&] { return !source.receiving(); }, 2), "gone: not receiving");
    CHECK(!source.locked() && source.clockDomain() == 0, "and not reported locked while it cannot hear the master");
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state = saved;
    }
    CHECK(waitFor([&] { return source.receiving(); }, 2), "back: receiving again");
    source.stop();
}

} // namespace

int main() {
    testLeavesTheClockAloneWithoutAMaster();
    testKeepsTryingWithoutItsInterface();
    testFollowsAMaster();
    testADeviceTimelineRestart();
    testRejoinsWhenItsInterfaceChanges();

    std::cout << "\nPTP clock source: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
