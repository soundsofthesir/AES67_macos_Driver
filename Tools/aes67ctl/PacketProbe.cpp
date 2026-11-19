/// @file PacketProbe.cpp
/// @brief Listens to a stream's group for a few seconds (receive only).

#include "PacketProbe.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cmath>

namespace AES67 {
namespace Ctl {

namespace {

constexpr size_t kRtpHeader = 12;
constexpr double kFullScale = 8388608.0;  // 2^23

}  // namespace

std::pair<size_t, size_t> rtpPayload(const std::vector<uint8_t>& packet) {
    if (packet.size() < kRtpHeader || (packet[0] >> 6) != 2) return {0, 0};
    size_t start = kRtpHeader + 4u * (packet[0] & 0x0F);
    if (packet[0] & 0x10) {  // header extension
        if (packet.size() < start + 4) return {0, 0};
        start += 4 + 4u * ((packet[start + 2] << 8) | packet[start + 3]);
    }
    size_t end = packet.size();
    if (packet[0] & 0x20) end -= packet.back();  // padding
    if (start > end || end > packet.size()) return {0, 0};
    return {start, end};
}

uint32_t peakL24(const uint8_t* payload, size_t size) {
    uint32_t peak = 0;
    for (size_t i = 0; i + 3 <= size; i += 3) {
        int32_t sample = (payload[i] << 16) | (payload[i + 1] << 8) | payload[i + 2];
        if (sample & 0x800000) sample -= 0x1000000;
        const uint32_t magnitude = static_cast<uint32_t>(sample < 0 ? -sample : sample);
        if (magnitude > peak) peak = magnitude;
    }
    return peak;
}

ProbeResult probe(const std::string& group, uint16_t port, const std::string& interfaceAddress,
                  std::chrono::milliseconds duration) {
    ProbeResult result;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return result;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(group.c_str());  // only this group's packets
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
    mreq.imr_interface.s_addr = inet_addr(interfaceAddress.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        close(fd);
        return result;
    }
    result.joined = true;
    timeval tv{0, 100000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t peak = 0;
    std::vector<uint8_t> buffer(2048);
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) continue;
        const std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + n);
        const auto [start, end] = rtpPayload(packet);
        if (end == 0) continue;
        ++result.packets;
        const uint32_t packetPeak = peakL24(packet.data() + start, end - start);
        if (packetPeak > peak) peak = packetPeak;
    }
    close(fd);
    if (peak > 0) result.peakDbfs = 20.0 * std::log10(peak / kFullScale);
    return result;
}

}  // namespace Ctl
}  // namespace AES67
