/// @file SAPAnnouncer.cpp

#include "SAPAnnouncer.h"
#include "../../Driver/DebugLog.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace AES67 {

namespace {

constexpr uint8_t kVersion1 = 0x20;      // V=1 in the top three bits
constexpr uint8_t kDeletionBit = 0x04;   // T: session deletion
constexpr char kPayloadType[] = "application/sdp";

} // namespace

SAPAnnouncer::SAPAnnouncer(Config config)
    : config_(std::move(config))
    , thread_([this] { run(); })
{
}

SAPAnnouncer::~SAPAnnouncer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : sessions_) {
            send(entry.second, true);
        }
        sessions_.clear();
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    for (const auto& entry : sockets_) {
        ::close(entry.second);
    }
}

uint16_t SAPAnnouncer::messageIdHash(const std::string& sdp) {
    // FNV-1a, folded to 16 bits; 0 is avoided (some receivers treat it as unset)
    uint32_t hash = 2166136261u;
    for (unsigned char c : sdp) {
        hash ^= c;
        hash *= 16777619u;
    }
    const auto folded = static_cast<uint16_t>((hash >> 16) ^ (hash & 0xFFFFu));
    return folded == 0 ? 1 : folded;
}

std::vector<uint8_t> SAPAnnouncer::buildPacket(const std::string& sdp, uint32_t originNetworkOrder, bool deletion) {
    std::vector<uint8_t> packet;
    packet.reserve(8 + sizeof(kPayloadType) + sdp.size());
    packet.push_back(static_cast<uint8_t>(kVersion1 | (deletion ? kDeletionBit : 0)));
    packet.push_back(0);  // no authentication data
    const uint16_t hash = messageIdHash(sdp);
    packet.push_back(static_cast<uint8_t>(hash >> 8));
    packet.push_back(static_cast<uint8_t>(hash & 0xFF));
    const auto* origin = reinterpret_cast<const uint8_t*>(&originNetworkOrder);
    packet.insert(packet.end(), origin, origin + 4);
    packet.insert(packet.end(), kPayloadType, kPayloadType + sizeof(kPayloadType));  // includes the NUL
    packet.insert(packet.end(), sdp.begin(), sdp.end());
    return packet;
}

void SAPAnnouncer::announce(const StreamID& id, const std::string& sdp, const std::string& interfaceAddress,
                            uint8_t ttl) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = sessions_.find(id);
        if (existing != sessions_.end() && existing->second.sdp != sdp) {
            send(existing->second, true);
        }
        Session session{sdp, interfaceAddress, ttl, std::chrono::steady_clock::now() + jitteredInterval()};
        send(session, false);
        sessions_[id] = std::move(session);
        closeUnusedSockets();  // the session's old address, if it moved
    }
    wake_.notify_all();
}

void SAPAnnouncer::withdraw(const StreamID& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }
    send(it->second, true);
    sessions_.erase(it);
    closeUnusedSockets();
}

size_t SAPAnnouncer::sessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void SAPAnnouncer::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (sessions_.empty()) {
            wake_.wait(lock);
            continue;
        }
        auto earliest = sessions_.begin()->second.nextDue;
        for (const auto& entry : sessions_) {
            earliest = std::min(earliest, entry.second.nextDue);
        }
        if (wake_.wait_until(lock, earliest) == std::cv_status::no_timeout) {
            continue;  // sessions or stopping changed: re-plan
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto& entry : sessions_) {
            if (entry.second.nextDue <= now) {
                send(entry.second, false);
                entry.second.nextDue = now + jitteredInterval();
            }
        }
    }
}

std::chrono::steady_clock::duration SAPAnnouncer::jitteredInterval() {
    // RFC 2974: randomise each interval by +/-1/3 so announcers do not synchronise
    std::uniform_real_distribution<double> factor(2.0 / 3.0, 4.0 / 3.0);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(static_cast<double>(config_.interval.count()) * factor(random_)));
}

int SAPAnnouncer::socketFor(const std::string& interfaceAddress, uint8_t ttl) {
    const auto key = std::make_pair(interfaceAddress, ttl);
    auto it = sockets_.find(key);
    if (it != sockets_.end()) {
        return it->second;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        AES67_LOGF("SAPAnnouncer: socket() failed (errno=%d: %s)", errno, std::strerror(errno));
        return -1;
    }
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (!interfaceAddress.empty()) {
        in_addr address{};
        address.s_addr = ::inet_addr(interfaceAddress.c_str());
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, sizeof(address)) < 0) {
            AES67_LOGF("SAPAnnouncer: IP_MULTICAST_IF %s failed (errno=%d: %s)", interfaceAddress.c_str(), errno,
                       std::strerror(errno));
        }
    }
    sockets_[key] = fd;
    return fd;
}

void SAPAnnouncer::closeUnusedSockets() {
    // An interface change gives a session a new address; without this each
    // address it had been on kept a socket open (#72)
    for (auto it = sockets_.begin(); it != sockets_.end();) {
        const bool used = std::any_of(sessions_.begin(), sessions_.end(), [&](const auto& entry) {
            return std::make_pair(entry.second.interfaceAddress, entry.second.ttl) == it->first;
        });
        if (used) {
            ++it;
        } else {
            ::close(it->second);
            it = sockets_.erase(it);
        }
    }
}

void SAPAnnouncer::send(const Session& session, bool deletion) {
    const int fd = socketFor(session.interfaceAddress, session.ttl);
    if (fd < 0) {
        return;
    }
    const uint32_t origin = session.interfaceAddress.empty() ? 0 : ::inet_addr(session.interfaceAddress.c_str());
    const auto packet = buildPacket(session.sdp, origin, deletion);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(config_.port);
    destination.sin_addr.s_addr = ::inet_addr(config_.group.c_str());
    if (::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&destination),
                 sizeof(destination)) < 0) {
        AES67_LOGF("SAPAnnouncer: sendto failed (errno=%d: %s)", errno, std::strerror(errno));
    }
}

} // namespace AES67
