//
// TestClockFoundations.cpp
// AES67 macOS Driver
// Step 2, phase 0: host time conversion, the MediaClock model, and the
// timestamped audio buffer. Deterministic: tests use explicit host tick
// values rather than the real clock, except the concurrency stress tests.
//

#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/Clock/MediaClock.h"
#include "../NetworkEngine/Clock/DeviceTimeline.h"
#include "../NetworkEngine/Clock/TimestampedAudioBuffer.h"
#include <mach/mach.h>
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

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

// Apple Silicon: 24 MHz host clock, 125/3 ns per tick
const HostTimebase kAppleSilicon{125, 3};
// 48 kHz at an exact 24 MHz host clock: 0.002 samples per tick
const double kSamplesPerTick48k = MediaClock::samplesPerTick(48000.0, 1.0, kAppleSilicon);

// ---------------------------------------------------------------------------
// HostTimebase
// ---------------------------------------------------------------------------
void testHostTimebase() {
    std::cout << "HostTimebase conversions" << std::endl;

    CHECK(kAppleSilicon.ticksToNanos(3) == 125, "3 ticks should be 125 ns");
    CHECK(kAppleSilicon.nanosToTicks(125) == 3, "125 ns should be 3 ticks");
    CHECK(kAppleSilicon.ticksPerSecond() == 24000000.0, "Apple Silicon host clock should be 24 MHz");

    // A year of uptime in ticks would overflow 64-bit ticks * 125
    const uint64_t yearOfTicks = 24000000ULL * 3600 * 24 * 365;
    CHECK(kAppleSilicon.ticksToNanos(yearOfTicks) == yearOfTicks / 3 * 125,
          "conversion should not overflow for large tick counts");
    const uint64_t hugeNanos = 1000000000000000000ULL;  // ~31.7 years
    CHECK(kAppleSilicon.ticksToNanos(kAppleSilicon.nanosToTicks(hugeNanos)) == hugeNanos,
          "nanoseconds should round-trip through ticks without overflow");

    const HostTimebase identity{1, 1};
    CHECK(identity.ticksToNanos(42) == 42 && identity.nanosToTicks(42) == 42, "1/1 timebase should be identity");

    const HostTimebase current = HostTimebase::current();
    CHECK(current.numer > 0 && current.denom > 0, "current timebase should be valid");
}

// ---------------------------------------------------------------------------
// MediaClock snapshot maths
// ---------------------------------------------------------------------------
void testMediaClockMapping() {
    std::cout << "MediaClock mapping" << std::endl;

    CHECK(std::fabs(kSamplesPerTick48k - 0.002) < 1e-15, "48 kHz at 24 MHz should be 0.002 samples per tick");

    MediaClock clock;
    CHECK(!clock.snapshot().valid(), "a clock that was never reset should be invalid");

    clock.reset(1000, 5000, kSamplesPerTick48k);
    const auto snap = clock.snapshot();
    CHECK(snap.valid() && snap.generation == 1, "reset should start generation 1");

    const MediaPosition atAnchor = snap.positionAt(1000);
    CHECK(atAnchor.sample == 5000 && atAnchor.fraction == 0.0, "position at the anchor should be the anchor sample");

    CHECK(snap.sampleAt(1000 + 24000000) == 5000 + 48000, "one second of ticks should be 48000 samples");
    CHECK(snap.sampleAt(1000 + 499) == 5000, "499 ticks is 0.998 samples, still inside the anchor sample");
    CHECK(snap.sampleAt(1000 + 500) == 5001, "500 ticks is exactly one sample");

    // Host times before the anchor map backwards
    CHECK(snap.sampleAt(1000 - 500) == 4999, "500 ticks before the anchor is one sample earlier");
    CHECK(snap.sampleAt(1000 - 1) == 4999, "one tick before the anchor rounds down to the previous sample");

    // hostAt returns the first tick at or after the sample starts
    for (int64_t s : {int64_t{5000}, int64_t{5001}, int64_t{4999}, int64_t{5000 + 48000}, int64_t{123456}}) {
        CHECK(snap.sampleAt(snap.hostAt(s)) == s, "sampleAt(hostAt(" << s << ")) should round-trip");
        CHECK(snap.sampleAt(snap.hostAt(s) - 1) == s - 1, "the tick before hostAt(" << s << ") belongs to the previous sample");
    }
}

void testMediaClockPrecisionAtPTPScale() {
    std::cout << "MediaClock precision at PTP-epoch scale" << std::endl;

    // PTP time in samples since 1970 at 384 kHz is ~6.6e14; use 2^50 (~1.1e15)
    const int64_t anchor = int64_t{1} << 50;
    const double spt = MediaClock::samplesPerTick(384000.0, 1.0, kAppleSilicon);  // 0.016
    MediaClock clock;
    clock.reset(0, anchor, spt);
    const auto snap = clock.snapshot();

    // 12345 ticks * 0.016 = 197.52 samples
    const MediaPosition p = snap.positionAt(12345);
    CHECK(p.sample == anchor + 197, "whole samples should be exact at 2^50");
    CHECK(std::fabs(p.fraction - 0.52) < 1e-9, "fraction should keep sub-sample precision at 2^50 (got " << p.fraction << ")");
    CHECK(snap.sampleAt(snap.hostAt(anchor + 197)) == anchor + 197, "round trip should be exact at 2^50");
}

void testMediaClockUpdates() {
    std::cout << "MediaClock rate changes, steps and resets" << std::endl;

    MediaClock clock;
    clock.reset(0, 0, kSamplesPerTick48k);

    // Rate change is continuous at the switch point and keeps the generation
    const uint64_t switchHost = 24000000;  // 1 s: position 48000
    const MediaPosition before = clock.snapshot().positionAt(switchHost);
    clock.setRate(switchHost, kSamplesPerTick48k * 1.001);  // +1000 ppm
    const auto fast = clock.snapshot();
    const MediaPosition after = fast.positionAt(switchHost);
    CHECK(after.sample == before.sample && std::fabs(after.fraction - before.fraction) < 1e-9,
          "setRate should not move the position at the switch point");
    CHECK(fast.generation == 1, "setRate should keep the generation (no new timeline)");
    CHECK(fast.sampleAt(switchHost + 24000000) == 48000 + 48048, "after the switch a second should be 48048 samples");

    // Step moves the position and starts a new timeline
    clock.step(switchHost, 4800);
    const auto stepped = clock.snapshot();
    CHECK(stepped.sampleAt(switchHost) == 48000 + 4800, "step should move the position by the delta");
    CHECK(stepped.generation == 2, "step should bump the generation (HAL seed)");

    clock.reset(777, 42, kSamplesPerTick48k);
    CHECK(clock.snapshot().generation == 3 && clock.snapshot().sampleAt(777) == 42, "reset should bump the generation and re-anchor");
}

// Writer publishes snapshots whose fields are linked; readers must never see
// a mix of two publications.
void testMediaClockConcurrentReads() {
    std::cout << "MediaClock concurrent reads (seqlock stress)" << std::endl;

    MediaClock clock;
    clock.reset(1, 3, 2.0);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (running.load(std::memory_order_relaxed)) {
                const auto s = clock.snapshot();
                // Invariant maintained by the writer: sample = 3*host, rate = 2*host
                if (s.anchorSample != static_cast<int64_t>(3 * s.anchorHost) ||
                    s.samplesPerTick != 2.0 * static_cast<double>(s.anchorHost)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (uint64_t host = 2; std::chrono::steady_clock::now() < deadline; ++host) {
        clock.reset(host, static_cast<int64_t>(3 * host), 2.0 * static_cast<double>(host));
    }
    running = false;
    for (auto& t : readers) t.join();

    CHECK(reads.load() > 1000, "readers should have run (got " << reads.load() << " reads)");
    CHECK(torn.load() == 0, "no reader should see a torn snapshot (got " << torn.load() << ")");
}

// The IO thread reads the clock; its writers (PTP, stream clock recovery, the
// HAL's control thread) are not real-time. A writer stopped part-way through a
// publish used to keep every reader spinning until it ran again (#61): a
// reader must return at once, with a consistent snapshot, whenever the writer
// stops. The writer is stopped at arbitrary points with thread_suspend.
void testSnapshotWithAStalledWriter() {
    std::cout << "#61: MediaClock reads never wait for a stalled writer" << std::endl;

    MediaClock clock;
    clock.reset(1, 3, 2.0);
    std::atomic<bool> writing{true};
    std::thread writer([&] {
        for (uint64_t host = 2; writing.load(std::memory_order_relaxed); ++host) {
            clock.reset(host, static_cast<int64_t>(3 * host), 2.0 * static_cast<double>(host));
        }
    });
    const thread_act_t writerThread = pthread_mach_thread_np(writer.native_handle());

    // Reads on request on their own thread, so a reader stuck behind the
    // writer is seen as a timeout rather than hanging the test
    std::atomic<int> requests{0};
    std::atomic<int> done{0};
    std::atomic<int> torn{0};
    std::atomic<int> stale{0};
    std::atomic<bool> reading{true};
    std::thread reader([&] {
        int served = 0;
        uint64_t latest = 1;
        while (reading.load()) {
            if (requests.load() == served) {
                std::this_thread::yield();
                continue;
            }
            const auto s = clock.snapshot();
            if (s.anchorSample != static_cast<int64_t>(3 * s.anchorHost) ||
                s.samplesPerTick != 2.0 * static_cast<double>(s.anchorHost)) {
                torn.fetch_add(1);
            }
            // A state the writer published, never one older than already read
            if (!s.valid() || s.anchorHost < latest) {
                stale.fetch_add(1);
            }
            latest = std::max(latest, s.anchorHost);
            done.store(++served);
        }
    });

    std::mt19937 random(61);
    std::uniform_int_distribution<int> gapMicroseconds(0, 50);
    int waited = 0;
    constexpr int kStalls = 2000;
    for (int i = 0; i < kStalls; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(gapMicroseconds(random)));
        thread_suspend(writerThread);
        requests.store(i + 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (done.load() != i + 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (done.load() != i + 1) ++waited;
        thread_resume(writerThread);
        while (done.load() != i + 1) std::this_thread::yield();
    }
    writing = false;
    reading = false;
    writer.join();
    reader.join();

    CHECK(waited == 0, "no read should wait for the stopped writer (" << waited << " of " << kStalls << " did)");
    CHECK(torn.load() == 0, "no read should be torn (got " << torn.load() << ")");
    CHECK(stale.load() == 0, "every read should be a published state, never an older one (got " << stale.load() << ")");
}

// ---------------------------------------------------------------------------
// TimestampedAudioBuffer
// ---------------------------------------------------------------------------
std::vector<float> makeFrames(int64_t firstPosition, size_t frames, size_t channels) {
    std::vector<float> data(frames * channels);
    for (size_t f = 0; f < frames; ++f) {
        for (size_t c = 0; c < channels; ++c) {
            data[f * channels + c] = static_cast<float>((firstPosition + static_cast<int64_t>(f)) % 4096) * 4 + c;
        }
    }
    return data;
}

bool framesMatch(const std::vector<float>& actual, int64_t firstPosition, size_t frames, size_t channels) {
    const auto expected = makeFrames(firstPosition, frames, channels);
    return std::equal(expected.begin(), expected.end(), actual.begin());
}

void testBufferReadWrite() {
    std::cout << "TimestampedAudioBuffer read/write" << std::endl;

    TimestampedAudioBuffer buffer(2, 256);
    CHECK(buffer.channels() == 2 && buffer.capacity() == 256, "buffer should report its shape");

    const auto packet = makeFrames(100, 48, 2);
    buffer.write(100, 48, packet.data(), 2, 0);

    std::vector<float> out(48 * 2, -1.0f);
    CHECK(buffer.read(100, 48, out.data(), 2, 0) == 48, "all written frames should be valid");
    CHECK(framesMatch(out, 100, 48, 2), "read should return exactly what was written");

    std::vector<float> never(16 * 2, -1.0f);
    CHECK(buffer.read(10, 16, never.data(), 2, 0) == 0, "never-written frames should be invalid");
    CHECK(std::all_of(never.begin(), never.end(), [](float v) { return v == 0.0f; }), "invalid frames should read as silence");

    // Partially covered range: [90, 160) over data at [100, 148)
    std::vector<float> partial(70 * 2, -1.0f);
    CHECK(buffer.read(90, 70, partial.data(), 2, 0) == 48, "only the written part should be valid");
    CHECK(partial[0] == 0.0f && partial[9 * 2] == 0.0f, "frames before the packet should be silence");
    CHECK(partial[10 * 2] == packet[0] && partial[57 * 2 + 1] == packet[47 * 2 + 1], "the packet should land at its position");
    CHECK(partial[58 * 2] == 0.0f && partial[69 * 2 + 1] == 0.0f, "frames after the packet should be silence");
}

void testBufferWraparoundAndStaleLaps() {
    std::cout << "TimestampedAudioBuffer wraparound and stale laps" << std::endl;

    TimestampedAudioBuffer buffer(1, 256);

    // A write that crosses the end of the ring
    const auto crossing = makeFrames(240, 50, 1);
    buffer.write(240, 50, crossing.data(), 1, 0);
    std::vector<float> out(50, -1.0f);
    CHECK(buffer.read(240, 50, out.data(), 1, 0) == 50 && framesMatch(out, 240, 50, 1),
          "a write crossing the end of the ring should read back intact");

    // A slot holding the previous lap must not be played as this lap
    const auto old = makeFrames(10, 10, 1);
    buffer.write(10, 10, old.data(), 1, 0);
    std::vector<float> nextLap(10, -1.0f);
    CHECK(buffer.read(10 + 256, 10, nextLap.data(), 1, 0) == 0, "previous-lap data should not count for this lap");
    CHECK(std::all_of(nextLap.begin(), nextLap.end(), [](float v) { return v == 0.0f; }),
          "a missing packet should play as silence even though its slot holds old data");

    // A newer lap replaces the old one
    const auto newer = makeFrames(266, 10, 1);
    buffer.write(266, 10, newer.data(), 1, 0);
    std::vector<float> oldRead(10, -1.0f);
    std::vector<float> newRead(10, -1.0f);
    CHECK(buffer.read(10, 10, oldRead.data(), 1, 0) == 0, "overwritten positions should no longer be valid");
    CHECK(buffer.read(266, 10, newRead.data(), 1, 0) == 10 && framesMatch(newRead, 266, 10, 1), "the newer lap should be readable");

    // Reads are non-destructive: a second reader sees the same data
    std::vector<float> again(10, -1.0f);
    CHECK(buffer.read(266, 10, again.data(), 1, 0) == 10 && again == newRead, "a second read should see identical data");
}

void testBufferStridedAccess() {
    std::cout << "TimestampedAudioBuffer strided access" << std::endl;

    // Source: 8 interleaved device channels; the buffer holds columns 3 and 4
    constexpr size_t kDeviceChannels = 8;
    constexpr size_t kFrames = 4;
    std::vector<float> device(kFrames * kDeviceChannels);
    for (size_t f = 0; f < kFrames; ++f) {
        for (size_t c = 0; c < kDeviceChannels; ++c) device[f * kDeviceChannels + c] = static_cast<float>(f * 10 + c);
    }
    TimestampedAudioBuffer buffer(2, 64);
    buffer.write(1000, kFrames, device.data(), kDeviceChannels, 3);

    // Destination: 6 interleaved channels, buffer lands at columns 1 and 2
    std::vector<float> dest(kFrames * 6, -1.0f);
    CHECK(buffer.read(1000, kFrames, dest.data(), 6, 1) == kFrames, "strided read should find every frame");
    bool placed = true;
    bool untouched = true;
    for (size_t f = 0; f < kFrames; ++f) {
        placed = placed && dest[f * 6 + 1] == f * 10 + 3 && dest[f * 6 + 2] == f * 10 + 4;
        untouched = untouched && dest[f * 6 + 0] == -1.0f && dest[f * 6 + 3] == -1.0f && dest[f * 6 + 5] == -1.0f;
    }
    CHECK(placed, "columns 3 and 4 of the source should land in columns 1 and 2 of the destination");
    CHECK(untouched, "other destination columns should be left alone");
}

void testBufferRejectsBadShape() {
    std::cout << "TimestampedAudioBuffer rejects invalid shapes" << std::endl;

    bool threwCapacity = false;
    bool threwChannels = false;
    try { TimestampedAudioBuffer bad(2, 300); } catch (const std::invalid_argument&) { threwCapacity = true; }
    try { TimestampedAudioBuffer bad(0, 256); } catch (const std::invalid_argument&) { threwChannels = true; }
    CHECK(threwCapacity, "capacity must be a power of two");
    CHECK(threwChannels, "channel count must be non-zero");
}

// One writer streaming packets, several readers reading behind it at random
// distances. Every frame a reader accepts must match its position exactly.
void testBufferConcurrentStress() {
    std::cout << "TimestampedAudioBuffer concurrent stress" << std::endl;

    constexpr size_t kChannels = 4;
    constexpr size_t kCapacity = 1024;
    constexpr size_t kPacket = 48;
    TimestampedAudioBuffer buffer(kChannels, kCapacity);
    std::atomic<int64_t> written{0};
    std::atomic<bool> running{true};
    std::atomic<uint64_t> mismatches{0};
    std::atomic<uint64_t> validFrames{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937 rng(r);
            std::vector<float> out(256 * kChannels);
            while (running.load(std::memory_order_relaxed)) {
                const int64_t head = written.load(std::memory_order_acquire);
                // Up to a full lap behind, so some reads race overwrites
                const int64_t start = head - static_cast<int64_t>(rng() % (kCapacity + 64));
                if (start < 0) continue;
                const size_t frames = 1 + rng() % 256;
                buffer.read(start, frames, out.data(), kChannels, 0);
                for (size_t f = 0; f < frames; ++f) {
                    const float first = out[f * kChannels];
                    if (first == 0.0f && out[f * kChannels + 1] == 0.0f) continue;  // invalid frame
                    validFrames.fetch_add(1, std::memory_order_relaxed);
                    const int64_t position = start + static_cast<int64_t>(f);
                    for (size_t c = 0; c < kChannels; ++c) {
                        const float expected = static_cast<float>(position % 4096) * 4 + c + 1;
                        if (out[f * kChannels + c] != expected) {
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }

    // Values are offset by 1 so that a valid frame is never all zeros
    std::vector<float> packet(kPacket * kChannels);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    int64_t position = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (size_t f = 0; f < kPacket; ++f) {
            for (size_t c = 0; c < kChannels; ++c) {
                packet[f * kChannels + c] = static_cast<float>((position + static_cast<int64_t>(f)) % 4096) * 4 + c + 1;
            }
        }
        buffer.write(position, kPacket, packet.data(), kChannels, 0);
        position += kPacket;
        written.store(position, std::memory_order_release);
    }
    running = false;
    for (auto& t : readers) t.join();

    CHECK(validFrames.load() > 10000, "readers should have accepted many frames (got " << validFrames.load() << ")");
    CHECK(mismatches.load() == 0, "no accepted frame may hold another position's data (got " << mismatches.load() << ")");
}

// ---------------------------------------------------------------------------
// Timeline origin and device zero timestamps (phase 1)
// ---------------------------------------------------------------------------
void testTimelineOrigin() {
    std::cout << "MediaClock timeline origin" << std::endl;

    MediaClock clock;
    clock.reset(1000, 9000, kSamplesPerTick48k);
    CHECK(clock.snapshot().origin == 9000, "reset should set the origin to the start sample");

    clock.setRate(1000 + 24000000, kSamplesPerTick48k * 1.0003);
    CHECK(clock.snapshot().origin == 9000, "setRate should keep the origin (same timeline)");

    clock.step(1000 + 48000000, 4800);
    CHECK(clock.snapshot().origin == 9000, "step should keep the origin so device time jumps by the step");

    clock.reset(5, 777, kSamplesPerTick48k);
    CHECK(clock.snapshot().origin == 777, "a new reset should move the origin");
}

void testZeroTimeStamps() {
    std::cout << "Device zero timestamps" << std::endl;

    constexpr uint32_t kPeriod = 16384;
    MediaClock clock;
    const int64_t origin = int64_t{1} << 46;  // PTP-scale media position
    clock.reset(1000, origin, kSamplesPerTick48k);
    const auto snap = clock.snapshot();

    bool aligned = true;
    bool bracketsNow = true;
    bool hostMatches = true;
    bool monotonic = true;
    ZeroTimeStamp previous{-1.0, 0, 0};
    // Sweep 10 s of host time in steps that are not a multiple of anything
    for (uint64_t host = 1000; host < 1000 + 240000000; host += 1234567) {
        const ZeroTimeStamp zts = zeroTimeStampAt(snap, host, kPeriod);
        const int64_t deviceNow = deviceSampleTimeAt(snap, host);
        aligned = aligned && std::fmod(zts.sampleTime, kPeriod) == 0.0;
        bracketsNow = bracketsNow && zts.sampleTime <= deviceNow && deviceNow < zts.sampleTime + kPeriod;
        hostMatches = hostMatches && snap.sampleAt(zts.hostTime) - snap.origin == static_cast<int64_t>(zts.sampleTime) &&
                      snap.sampleAt(zts.hostTime - 1) - snap.origin == static_cast<int64_t>(zts.sampleTime) - 1;
        monotonic = monotonic && zts.sampleTime >= previous.sampleTime && zts.hostTime >= previous.hostTime;
        previous = zts;
    }
    CHECK(deviceSampleTimeAt(snap, 1000) == 0, "device time should start at 0 at the origin");
    CHECK(aligned, "zero timestamp sample times should be multiples of the period");
    CHECK(bracketsNow, "the zero timestamp should be the latest period boundary at or before now");
    CHECK(hostMatches, "a zero timestamp's host time should be exactly where its sample starts");
    CHECK(monotonic, "zero timestamps should never go backwards");
    CHECK(zeroTimeStampAt(snap, 5000, kPeriod).seed == snap.generation, "the seed should be the clock generation");

    // Before the origin (negative device time) still lands on a period boundary below now
    const ZeroTimeStamp early = zeroTimeStampAt(snap, 1000 - 500 * 5, kPeriod);
    CHECK(early.sampleTime == -static_cast<double>(kPeriod), "negative device time should floor to the previous boundary");

    // A faster clock reaches each boundary sooner in host time
    clock.setRate(1000, kSamplesPerTick48k * 1.0003);
    const auto fast = clock.snapshot();
    const ZeroTimeStamp a = zeroTimeStampAt(fast, 1000 + 10 * 24000000ULL, kPeriod);
    const ZeroTimeStamp b = zeroTimeStampAt(fast, fast.hostAt(fast.origin + static_cast<int64_t>(a.sampleTime) + kPeriod), kPeriod);
    const double ticksPerPeriod = static_cast<double>(b.hostTime - a.hostTime);
    CHECK(std::fabs(ticksPerPeriod - kPeriod / (kSamplesPerTick48k * 1.0003)) <= 1.0,
          "boundary spacing should follow the clock rate (got " << ticksPerPeriod << " ticks)");

    // A step starts a new seed and moves device time by the step
    clock.step(1000 + 24000000ULL, 4800);
    const auto stepped = clock.snapshot();
    CHECK(stepped.generation != snap.generation, "a step should change the seed");
    CHECK(deviceSampleTimeAt(stepped, 1000 + 24000000ULL) == deviceSampleTimeAt(fast, 1000 + 24000000ULL) + 4800,
          "a step should move device time by the step size");
}

} // namespace

int main() {
    testHostTimebase();
    testMediaClockMapping();
    testMediaClockPrecisionAtPTPScale();
    testMediaClockUpdates();
    testMediaClockConcurrentReads();
    testSnapshotWithAStalledWriter();
    testTimelineOrigin();
    testZeroTimeStamps();
    testBufferReadWrite();
    testBufferWraparoundAndStaleLaps();
    testBufferStridedAccess();
    testBufferRejectsBadShape();
    testBufferConcurrentStress();

    std::cout << "\nClock foundations: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
