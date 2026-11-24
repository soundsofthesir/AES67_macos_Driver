/// @file RingBuffer.hpp
/// @brief Lock-free SPSC ring buffer. RT-safe: no allocation, no locks, no blocking.

#pragma once

#include <atomic>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace AES67 {

/// Lock-free single-producer, single-consumer ring buffer.
///
/// Thread safety via memory-ordered atomics (no locks). Cache-line aligned
/// indices prevent false sharing. T must be trivially copyable.
///
/// @tparam T Element type (must be trivially copyable)
template<typename T>
class SPSCRingBuffer {
public:
    /// Construct with given capacity (usable elements; internal size is capacity+1).
    /// @param capacity Maximum number of elements the buffer can hold.
    explicit SPSCRingBuffer(size_t capacity)
        : buffer_(capacity + 1),  // +1 for full/empty distinction
          capacity_(capacity + 1)
    {
        static_assert(std::is_trivially_copyable<T>::value,
                     "T must be trivially copyable for memcpy safety");
    }

    // Prevent copy (would break single-producer/consumer guarantee)
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // Prevent move too. The indices are std::atomic, which cannot be moved, so
    // the compiler-generated move operations were always defined as deleted
    // (clang: -Wdefaulted-function-deleted); a buffer is shared between its
    // producer and consumer threads and must stay where it was constructed.
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    /// Write elements into the buffer (producer thread only).
    /// @param data Source data pointer.
    /// @param count Number of elements to write.
    /// @return Number of elements actually written (may be less if buffer is full).
    size_t write(const T* data, size_t count) noexcept {
        // Load indices with appropriate memory ordering
        const size_t writeIdx = writeIndex_.load(std::memory_order_relaxed);
        const size_t readIdx = readIndex_.load(std::memory_order_acquire);

        // Calculate available space
        const size_t available = getAvailableWrite(writeIdx, readIdx);
        const size_t toWrite = std::min(count, available);

        if (toWrite == 0) {
            return 0;  // Buffer full
        }

        // Write in one or two chunks (handling wrap-around)
        const size_t firstChunk = std::min(toWrite, capacity_ - writeIdx);
        std::memcpy(&buffer_[writeIdx], data, firstChunk * sizeof(T));

        if (firstChunk < toWrite) {
            // Wrap around to beginning
            const size_t secondChunk = toWrite - firstChunk;
            std::memcpy(&buffer_[0], data + firstChunk, secondChunk * sizeof(T));
        }

        // Update write index with release semantics
        // Ensures all writes complete before index update is visible
        const size_t newWriteIdx = (writeIdx + toWrite) % capacity_;
        writeIndex_.store(newWriteIdx, std::memory_order_release);

        return toWrite;
    }

    /// Read elements from the buffer (consumer thread only). RT-safe.
    /// @param data Destination data pointer.
    /// @param count Number of elements to read.
    /// @return Number of elements actually read (may be less if buffer is empty).
    size_t read(T* data, size_t count) noexcept {
        // Load indices with appropriate memory ordering
        const size_t readIdx = readIndex_.load(std::memory_order_relaxed);
        const size_t writeIdx = writeIndex_.load(std::memory_order_acquire);

        // Calculate available data
        const size_t available = getAvailableRead(readIdx, writeIdx);
        const size_t toRead = std::min(count, available);

        if (toRead == 0) {
            return 0;  // Buffer empty
        }

        // Read in one or two chunks (handling wrap-around)
        const size_t firstChunk = std::min(toRead, capacity_ - readIdx);
        std::memcpy(data, &buffer_[readIdx], firstChunk * sizeof(T));

        if (firstChunk < toRead) {
            // Wrap around to beginning
            const size_t secondChunk = toRead - firstChunk;
            std::memcpy(data + firstChunk, &buffer_[0], secondChunk * sizeof(T));
        }

        // Update read index with release semantics
        // Ensures all reads complete before index update is visible
        const size_t newReadIdx = (readIdx + toRead) % capacity_;
        readIndex_.store(newReadIdx, std::memory_order_release);

        return toRead;
    }

    /// Number of elements available for reading. Thread-safe, RT-safe.
    size_t available() const noexcept {
        const size_t writeIdx = writeIndex_.load(std::memory_order_acquire);
        const size_t readIdx = readIndex_.load(std::memory_order_relaxed);
        return getAvailableRead(readIdx, writeIdx);
    }

    /// Number of free elements available for writing. Thread-safe, RT-safe.
    size_t availableWrite() const noexcept {
        const size_t writeIdx = writeIndex_.load(std::memory_order_relaxed);
        const size_t readIdx = readIndex_.load(std::memory_order_acquire);
        return getAvailableWrite(writeIdx, readIdx);
    }

    /// Reset buffer to empty. WARNING: not thread-safe -- call only when idle.
    void reset() noexcept {
        writeIndex_.store(0, std::memory_order_release);
        readIndex_.store(0, std::memory_order_release);
    }

    /// Usable capacity (excludes the internal sentinel element).
    size_t capacity() const noexcept {
        return capacity_ - 1;
    }

    /// Check if buffer is empty. RT-safe.
    bool isEmpty() const noexcept {
        return available() == 0;
    }

    /// Check if buffer is full. RT-safe.
    bool isFull() const noexcept {
        return availableWrite() == 0;
    }

private:
    // Calculate available elements for reading
    size_t getAvailableRead(size_t readIdx, size_t writeIdx) const noexcept {
        if (writeIdx >= readIdx) {
            return writeIdx - readIdx;
        } else {
            return capacity_ - readIdx + writeIdx;
        }
    }

    // Calculate available space for writing
    // Reserve one element to distinguish full from empty
    size_t getAvailableWrite(size_t writeIdx, size_t readIdx) const noexcept {
        if (readIdx > writeIdx) {
            return readIdx - writeIdx - 1;
        } else {
            return capacity_ - writeIdx + readIdx - 1;
        }
    }

    // Data storage
    std::vector<T> buffer_;
    size_t capacity_;

    // Cache-line aligned atomic indices to prevent false sharing
    // On most systems, cache lines are 64 bytes
    alignas(64) std::atomic<size_t> writeIndex_{0};
    alignas(64) std::atomic<size_t> readIndex_{0};
};

} // namespace AES67