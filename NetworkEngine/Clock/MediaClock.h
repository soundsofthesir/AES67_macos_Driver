/// @file MediaClock.h
/// @brief The device's media clock: an affine map between host time and media sample position.
///
/// With PTP the media position is PTP time in samples since the PTP epoch, so
/// every device on the grandmaster agrees on it. The active clock source keeps
/// the map up to date; Core Audio's zero timestamps, RX placement and TX RTP
/// timestamps all read it. See Docs/Step2-Clocking-Plan.md.

#pragma once

#include "HostTime.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace AES67 {

/// A media sample position with sub-sample precision. Kept as a whole sample
/// plus a fraction because a double alone loses sub-sample precision at
/// PTP-epoch magnitudes (~1e15 samples).
struct MediaPosition {
    int64_t sample{0};
    double fraction{0.0};  // [0, 1)
};

/// Lock-free media clock. One writer at a time (the active clock source)
/// publishes; any number of readers, including the real-time IO thread, take
/// snapshots without blocking.
///
/// A seqcount latch: two copies of the state, which the writer updates one
/// after the other while readers read the other one. A writer stopped
/// part-way never holds readers up, however low its priority (#61); a reader
/// only retries if the writer moved on while it was reading.
class MediaClock {
public:
    /// One published state of the clock. Pure value: safe to use after the
    /// clock has moved on.
    struct Snapshot {
        uint64_t anchorHost{0};
        int64_t anchorSample{0};
        double anchorFraction{0.0};
        double samplesPerTick{0.0};
        int64_t origin{0};       // media position where this timeline started (set by reset)
        uint32_t generation{0};  // 0 = never set; changes on every discontinuity (HAL seed)

        bool valid() const noexcept { return generation != 0; }

        MediaPosition positionAt(uint64_t host) const noexcept {
            // Signed tick difference so host times before the anchor work too
            const auto ticks = static_cast<int64_t>(host - anchorHost);
            const double offset = anchorFraction + static_cast<double>(ticks) * samplesPerTick;
            const double whole = std::floor(offset);
            return {anchorSample + static_cast<int64_t>(whole), offset - whole};
        }

        int64_t sampleAt(uint64_t host) const noexcept {
            return positionAt(host).sample;
        }

        /// First host tick at or after the start of `sample`, so
        /// sampleAt(hostAt(s)) == s (requires less than one sample per tick).
        uint64_t hostAt(int64_t sample) const noexcept {
            const double offset = static_cast<double>(sample - anchorSample) - anchorFraction;
            uint64_t host = anchorHost + static_cast<uint64_t>(static_cast<int64_t>(std::ceil(offset / samplesPerTick)));
            // The division can land a tick either side of the exact boundary;
            // settle on it using the same arithmetic as sampleAt()
            while (sampleAt(host - 1) >= sample) --host;
            while (sampleAt(host) < sample) ++host;
            return host;
        }
    };

    /// Samples per host tick for a sample rate running at `rateRatio` times nominal.
    static double samplesPerTick(double sampleRate, double rateRatio, const HostTimebase& timebase) noexcept {
        return sampleRate * rateRatio / timebase.ticksPerSecond();
    }

    /// Real-time safe. Retries only if a publish completed half while reading.
    Snapshot snapshot() const noexcept {
        for (;;) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            const Snapshot s = slots_[before & 1u].load();
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                return s;
            }
        }
    }

    // Writer side: call from one thread only.

    /// Start a new timeline at `sample`, which becomes its origin (new generation).
    void reset(uint64_t host, int64_t sample, double samplesPerTick) noexcept {
        Snapshot next{host, sample, 0.0, samplesPerTick, sample, nextGeneration()};
        publish(next);
    }

    /// Change rate from `host` onwards, keeping the position and origin continuous.
    void setRate(uint64_t host, double samplesPerTick) noexcept {
        Snapshot next = rebasedAt(host);
        next.samplesPerTick = samplesPerTick;
        publish(next);
    }

    /// Jump the position by `deltaSamples` at `host` (new generation). The
    /// origin is kept, so device time jumps by the same amount.
    void step(uint64_t host, int64_t deltaSamples) noexcept {
        Snapshot next = rebasedAt(host);
        next.anchorSample += deltaSamples;
        next.generation = nextGeneration();
        publish(next);
    }

private:
    Snapshot rebasedAt(uint64_t host) const noexcept {
        Snapshot current = snapshot();
        const MediaPosition position = current.positionAt(host);
        current.anchorHost = host;
        current.anchorSample = position.sample;
        current.anchorFraction = position.fraction;
        return current;
    }

    uint32_t nextGeneration() const noexcept {
        const uint32_t next = snapshot().generation + 1;
        return next == 0 ? 1 : next;  // 0 is reserved for "never set"
    }

    void publish(const Snapshot& s) noexcept {
        // Between publishes the sequence is even and readers use slot 0
        const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
        sequence_.store(sequence + 1, std::memory_order_release);  // readers move to slot 1
        std::atomic_thread_fence(std::memory_order_release);
        slots_[0].store(s);
        sequence_.store(sequence + 2, std::memory_order_release);  // and back to slot 0
        std::atomic_thread_fence(std::memory_order_release);
        slots_[1].store(s);
    }

    /// One copy of the state, field by field (readers may race the writer on it)
    struct Slot {
        std::atomic<uint64_t> anchorHost{0};
        std::atomic<int64_t> anchorSample{0};
        std::atomic<double> anchorFraction{0.0};
        std::atomic<double> samplesPerTick{0.0};
        std::atomic<int64_t> origin{0};
        std::atomic<uint32_t> generation{0};

        Snapshot load() const noexcept {
            Snapshot s;
            s.anchorHost = anchorHost.load(std::memory_order_relaxed);
            s.anchorSample = anchorSample.load(std::memory_order_relaxed);
            s.anchorFraction = anchorFraction.load(std::memory_order_relaxed);
            s.samplesPerTick = samplesPerTick.load(std::memory_order_relaxed);
            s.origin = origin.load(std::memory_order_relaxed);
            s.generation = generation.load(std::memory_order_relaxed);
            return s;
        }

        void store(const Snapshot& s) noexcept {
            anchorHost.store(s.anchorHost, std::memory_order_relaxed);
            anchorSample.store(s.anchorSample, std::memory_order_relaxed);
            anchorFraction.store(s.anchorFraction, std::memory_order_relaxed);
            samplesPerTick.store(s.samplesPerTick, std::memory_order_relaxed);
            origin.store(s.origin, std::memory_order_relaxed);
            generation.store(s.generation, std::memory_order_relaxed);
        }
    };

    std::atomic<uint32_t> sequence_{0};
    Slot slots_[2];
};

} // namespace AES67
