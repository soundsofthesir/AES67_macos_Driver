//
// RxTestSupport.h
// AES67 macOS Driver
// Shared helpers for tests of the timestamp-driven receive path: the device
// context a receiver runs against, a reader that plays the IO thread's part,
// and a host-only RTP sender.
//

#pragma once

#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/Clock/MediaClock.h"
#include "../NetworkEngine/RTP/RtpPlacement.h"
#include "../NetworkEngine/RTP/RxContext.h"
#include "../NetworkEngine/RTP/RxRouting.h"
#include "../Driver/AudioThreadPriority.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace AES67::TestSupport {

constexpr uint32_t kFramesPerPacket = 48;
constexpr int64_t kTestLinkOffset = 8 * 48;  // 8 ms at 48 kHz
constexpr uint8_t kPayloadTypeL24 = 97;

/// What the device provides to its receivers: a running host-clock media
/// clock, the shared network time mapping, IO routing and the link offset.
struct RxHarness {
    MediaClock clock;
    NetworkTimeMapping networkTime;
    RxRouting routing;
    std::atomic<int64_t> linkOffsetFrames{kTestLinkOffset};

    RxHarness() {
        clock.reset(hostTimeNow(), 0, MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current()));
    }

    RxContext context() { return RxContext{clock, networkTime, routing, linkOffsetFrames}; }

    /// Media position the IO thread would be reading now.
    int64_t readPositionNow() const {
        return clock.snapshot().sampleAt(hostTimeNow()) - linkOffsetFrames.load();
    }
};

/// Plays the IO thread: reads device channels through the routing as the
/// read point advances in real time, collecting everything it hears.
class PlayoutReader {
public:
    PlayoutReader(RxHarness& harness, std::vector<uint32_t> deviceChannels)
        : harness_(harness), channels_(std::move(deviceChannels)), samples_(channels_.size()),
          next_(harness.readPositionNow()) {
        thread_ = std::thread([this] {
            // Real-time priority, like Core Audio's IO thread: a starved reader
            // falls more than a buffer behind and sees overwritten audio
            AudioThreadPriority::configureForRealTime();
            std::vector<float> frames(4096 * 128);
            while (running_) {
                pump(frames);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            pump(frames);
        });
    }

    PlayoutReader(RxHarness& harness, uint32_t deviceChannel)
        : PlayoutReader(harness, std::vector<uint32_t>{deviceChannel}) {}

    ~PlayoutReader() {
        if (thread_.joinable()) stopAll();
    }

    /// Stop and return what the first channel heard.
    std::vector<float> stop() { return stopAll().front(); }

    /// Stop and return what each channel heard, in constructor order.
    std::vector<std::vector<float>> stopAll() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return samples_;
    }

private:
    void pump(std::vector<float>& frames) {
        const int64_t target = harness_.readPositionNow();
        while (next_ < target) {
            const auto count = static_cast<size_t>(std::min<int64_t>(target - next_, 4096));
            std::fill(frames.begin(), frames.begin() + count * 128, 0.0f);
            harness_.routing.read([&](const RxRouting::Route& route) {
                route.buffer->read(next_, count, frames.data(), 128, route.deviceChannelStart);
            });
            for (size_t c = 0; c < channels_.size(); ++c) {
                for (size_t f = 0; f < count; ++f) samples_[c].push_back(frames[f * 128 + channels_[c]]);
            }
            next_ += static_cast<int64_t>(count);
        }
    }

    RxHarness& harness_;
    const std::vector<uint32_t> channels_;
    std::vector<std::vector<float>> samples_;
    int64_t next_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

/// Number of non-silent samples.
inline size_t countNonSilent(const std::vector<float>& samples) {
    return static_cast<size_t>(std::count_if(samples.begin(), samples.end(), [](float s) { return s != 0.0f; }));
}

/// The stretch from the first to the last non-silent sample: the stream as
/// heard, including any silence inside it.
inline std::vector<float> heardRegion(const std::vector<float>& samples) {
    const auto first = std::find_if(samples.begin(), samples.end(), [](float s) { return s != 0.0f; });
    if (first == samples.end()) return {};
    const auto last = std::find_if(samples.rbegin(), samples.rend(), [](float s) { return s != 0.0f; }).base();
    return std::vector<float>(first, last);
}

inline size_t countNear(const std::vector<float>& samples, float value) {
    return static_cast<size_t>(std::count_if(samples.begin(), samples.end(),
                                             [value](float s) { return std::fabs(s - value) < 1e-3f; }));
}

/// Multicast sender that only delivers to this host (TTL 0).
class LoopbackSender {
public:
    LoopbackSender(const char* group, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        unsigned char ttl = 0;
        unsigned char loop = 1;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(port);
        dest_.sin_addr.s_addr = inet_addr(group);
    }
    ~LoopbackSender() { if (fd_ >= 0) close(fd_); }

    /// One L24 packet of `frames` frames where every sample of every channel
    /// equals `value`, followed by `extraBytes` zero bytes.
    void sendL24(uint16_t seq, uint32_t timestamp, uint16_t channels, float value, uint32_t ssrc = 0x12345678,
                 size_t frames = kFramesPerPacket, size_t extraBytes = 0) {
        std::vector<uint8_t> pkt(12 + frames * channels * 3 + extraBytes);
        pkt[0] = 0x80;
        pkt[1] = kPayloadTypeL24;
        pkt[2] = seq >> 8;
        pkt[3] = seq & 0xFF;
        for (int i = 0; i < 4; ++i) pkt[4 + i] = (timestamp >> (24 - 8 * i)) & 0xFF;
        for (int i = 0; i < 4; ++i) pkt[8 + i] = (ssrc >> (24 - 8 * i)) & 0xFF;
        const int32_t pcm = static_cast<int32_t>(value * 8388607.0f);
        for (size_t s = 0; s < frames * channels; ++s) {
            pkt[12 + s * 3 + 0] = (pcm >> 16) & 0xFF;
            pkt[12 + s * 3 + 1] = (pcm >> 8) & 0xFF;
            pkt[12 + s * 3 + 2] = pcm & 0xFF;
        }
        sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
    }

private:
    int fd_{-1};
    sockaddr_in dest_{};
};

} // namespace AES67::TestSupport
