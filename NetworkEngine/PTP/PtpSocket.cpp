/// @file PtpSocket.cpp

#include "PtpSocket.h"
#include "../Clock/HostTime.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace AES67 {
namespace Ptp {

Socket::~Socket() { close(); }

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);  // also leaves any multicast group
        fd_ = -1;
    }
}

bool Socket::open(const Options& options) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }
    const int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    ::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &on, sizeof(on));
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &options.ttl, sizeof(options.ttl));
    const unsigned char loop = 1;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    if (options.interfaceIndex != 0 &&
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IFINDEX, &options.interfaceIndex, sizeof(options.interfaceIndex)) < 0) {
        close();
        return false;
    }

    if (options.port != 0) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(options.port);
        local.sin_addr.s_addr = options.bindAddress.empty() ? htonl(INADDR_ANY) : ::inet_addr(options.bindAddress.c_str());
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            close();
            return false;
        }
    }

    if (!options.group.empty()) {
        // By interface index when one is given, so a new address on the
        // interface leaves the membership intact; index 0 needs the
        // address-based join (MCAST_JOIN_GROUP requires a real index)
        int joined = -1;
        if (options.interfaceIndex != 0) {
            group_req join{};
            join.gr_interface = options.interfaceIndex;
            auto* group = reinterpret_cast<sockaddr_in*>(&join.gr_group);
            group->sin_family = AF_INET;
            group->sin_len = sizeof(sockaddr_in);
            group->sin_addr.s_addr = ::inet_addr(options.group.c_str());
            joined = ::setsockopt(fd_, IPPROTO_IP, MCAST_JOIN_GROUP, &join, sizeof(join));
        } else {
            ip_mreq membership{};
            membership.imr_multiaddr.s_addr = ::inet_addr(options.group.c_str());
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
            joined = ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
        }
        if (joined < 0) {
            close();
            return false;
        }
    }

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

long Socket::receive(uint8_t* buffer, size_t size, uint64_t& hostTicks, uint32_t& sourceAddress) {
    hostTicks = 0;
    sourceAddress = 0;
    if (fd_ < 0) {
        return -1;
    }
    sockaddr_in source{};
    iovec iov{buffer, size};
    char control[CMSG_SPACE(sizeof(uint64_t)) + 64];
    msghdr message{};
    message.msg_name = &source;
    message.msg_namelen = sizeof(source);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = ::recvmsg(fd_, &message, 0);
    if (received < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP_MONOTONIC) {
            uint64_t kernelTicks = 0;
            std::memcpy(&kernelTicks, CMSG_DATA(c), sizeof(kernelTicks));
            hostTicks = hostTimeFromKernelTimestamp(kernelTicks);
        }
    }
    sourceAddress = source.sin_addr.s_addr;
    return static_cast<long>(received);
}

bool Socket::sendTo(uint32_t address, uint16_t port, const uint8_t* data, size_t size) {
    if (fd_ < 0) {
        return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = address;
    return ::sendto(fd_, data, size, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) ==
           static_cast<ssize_t>(size);
}

bool Socket::sendTo(const std::string& address, uint16_t port, const uint8_t* data, size_t size) {
    return sendTo(::inet_addr(address.c_str()), port, data, size);
}

} // namespace Ptp
} // namespace AES67
