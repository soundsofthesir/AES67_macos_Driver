/// @file TimestampedAudioBuffer.h
/// @brief Multi-channel audio stored by media sample position.
///
/// Step 2 replaces FIFO ring buffers with storage indexed by media position:
/// the receive thread writes each packet at the position its RTP timestamp
/// gives, and the IO thread reads the positions Core Audio is due to play.
/// The same class serves TX in the other direction. See
/// Docs/Step2-Clocking-Plan.md.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace AES67 {

/// Ring of frames addressed by absolute media position (slot = position mod
/// capacity). Each slot is tagged with the position last written there, so a
/// read returns audio only for frames written for exactly that position;
/// anything else (never written, a previous lap, a lost packet) reads as
/// silence. Reads do not consume, so any number of readers see the same data.
///
/// One writer thread; any number of reader threads, including the real-time
/// IO thread. Lock-free and allocation-free after construction. Each frame is
/// protected by its tag as a seqlock, so a read racing an overwrite is
/// detected and returned as silence rather than mixed data.
class TimestampedAudioBuffer {
public:
    /// @param channels Channels per frame (> 0).
    /// @param capacityFrames Ring size in frames; must be a power of two.
    /// @throws std::invalid_argument on an invalid shape.
    TimestampedAudioBuffer(size_t channels, size_t capacityFrames);

    TimestampedAudioBuffer(const TimestampedAudioBuffer&) = delete;
    TimestampedAudioBuffer& operator=(const TimestampedAudioBuffer&) = delete;

    size_t channels() const noexcept { return channels_; }
    size_t capacity() const noexcept { return capacity_; }

    /// Writer only. Stores `frames` frames starting at media `position`,
    /// taking channels() values per frame from
    /// src[frame * srcStride + srcFirstColumn + channel].
    void write(int64_t position, size_t frames, const float* src, size_t srcStride,
               size_t srcFirstColumn) noexcept;

    /// Reads `frames` frames starting at media `position` into
    /// dst[frame * dstStride + dstFirstColumn + channel]. Frames not written for
    /// exactly that position are filled with silence.
    /// @return Number of valid (non-silence-filled) frames.
    size_t read(int64_t position, size_t frames, float* dst, size_t dstStride,
                size_t dstFirstColumn) const noexcept;

    /// readEnd() before any read.
    static constexpr int64_t kNothingRead = INT64_MIN;

    /// One past the furthest position any read has covered, or kNothingRead.
    /// A writer can compare a packet's end against it to learn that playout
    /// has already passed that audio (#185). Relaxed: a diagnostic, exact
    /// except while a read and a write of the same positions overlap.
    int64_t readEnd() const noexcept { return readEnd_.load(std::memory_order_relaxed); }

private:
    static constexpr int64_t kEmpty = INT64_MIN;

    size_t slotOf(int64_t position) const noexcept {
        return static_cast<size_t>(static_cast<uint64_t>(position) & mask_);
    }

    const size_t channels_;
    const size_t capacity_;
    const uint64_t mask_;
    std::unique_ptr<std::atomic<float>[]> samples_;  // capacity_ * channels_, slot-major
    std::unique_ptr<std::atomic<int64_t>[]> tags_;   // position held by each slot

    // Written by readers, read by the writer. On its own cache line so a
    // reader's store does not invalidate the fields above for the writer.
    alignas(64) mutable std::atomic<int64_t> readEnd_{kNothingRead};
};

} // namespace AES67
