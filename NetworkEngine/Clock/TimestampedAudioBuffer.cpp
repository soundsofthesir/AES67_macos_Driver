/// @file TimestampedAudioBuffer.cpp

#include "TimestampedAudioBuffer.h"
#include <stdexcept>

namespace AES67 {

TimestampedAudioBuffer::TimestampedAudioBuffer(size_t channels, size_t capacityFrames)
    : channels_(channels)
    , capacity_(capacityFrames)
    , mask_(static_cast<uint64_t>(capacityFrames) - 1)
{
    if (channels == 0) {
        throw std::invalid_argument("TimestampedAudioBuffer: channel count must be non-zero");
    }
    if (capacityFrames == 0 || (capacityFrames & (capacityFrames - 1)) != 0) {
        throw std::invalid_argument("TimestampedAudioBuffer: capacity must be a power of two");
    }

    samples_ = std::make_unique<std::atomic<float>[]>(capacity_ * channels_);
    tags_ = std::make_unique<std::atomic<int64_t>[]>(capacity_);
    for (size_t i = 0; i < capacity_ * channels_; ++i) {
        samples_[i].store(0.0f, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < capacity_; ++i) {
        tags_[i].store(kEmpty, std::memory_order_relaxed);
    }
}

void TimestampedAudioBuffer::write(int64_t position, size_t frames, const float* src, size_t srcStride,
                                   size_t srcFirstColumn) noexcept {
    for (size_t f = 0; f < frames; ++f) {
        const int64_t framePosition = position + static_cast<int64_t>(f);
        const size_t slot = slotOf(framePosition);
        std::atomic<float>* out = &samples_[slot * channels_];
        const float* in = src + f * srcStride + srcFirstColumn;

        // Seqlock write: invalidate the tag, then the data, then publish the
        // new tag. A reader that saw any new data also sees the invalidation.
        tags_[slot].store(kEmpty, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t c = 0; c < channels_; ++c) {
            out[c].store(in[c], std::memory_order_relaxed);
        }
        tags_[slot].store(framePosition, std::memory_order_release);
    }
}

size_t TimestampedAudioBuffer::read(int64_t position, size_t frames, float* dst, size_t dstStride,
                                    size_t dstFirstColumn) const noexcept {
    // Published before copying: a packet written from here on that ends at or
    // before `end` may already have been passed by this read
    const int64_t end = position + static_cast<int64_t>(frames);
    if (end > readEnd_.load(std::memory_order_relaxed)) {
        readEnd_.store(end, std::memory_order_relaxed);
    }

    size_t valid = 0;
    for (size_t f = 0; f < frames; ++f) {
        const int64_t framePosition = position + static_cast<int64_t>(f);
        const size_t slot = slotOf(framePosition);
        const std::atomic<float>* in = &samples_[slot * channels_];
        float* out = dst + f * dstStride + dstFirstColumn;

        if (tags_[slot].load(std::memory_order_acquire) == framePosition) {
            for (size_t c = 0; c < channels_; ++c) {
                out[c] = in[c].load(std::memory_order_relaxed);
            }
            // Seqlock read: if the tag still holds this position, nothing we
            // copied came from a concurrent overwrite
            std::atomic_thread_fence(std::memory_order_acquire);
            if (tags_[slot].load(std::memory_order_relaxed) == framePosition) {
                ++valid;
                continue;
            }
        }

        for (size_t c = 0; c < channels_; ++c) {
            out[c] = 0.0f;
        }
    }
    return valid;
}

} // namespace AES67
