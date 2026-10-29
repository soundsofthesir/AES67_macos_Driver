//
// SapTestSupport.h
// AES67 macOS Driver
// Captures SAP packets for tests. Tests announce on a test port with TTL 0,
// so nothing reaches the network or a SAP listener on the real port 9875.
//

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AES67 {
namespace TestSupport {

// Each test binary that announces gets its own port (AES67_SAP_TEST_PORT, set
// in Tests/CMakeLists.txt): ctest -j runs binaries side by side, and on a
// shared port each one captures the other's announcements and deletions.
#ifndef AES67_SAP_TEST_PORT
#define AES67_SAP_TEST_PORT 19875
#endif
constexpr uint16_t kTestSapPort = AES67_SAP_TEST_PORT;

struct Received {
    std::vector<uint8_t> bytes;
    std::chrono::steady_clock::time_point at;
};

// Joins a SAP group on the given interface and records every packet
class SapCapture {
public:
    SapCapture(const std::string& group, uint16_t port, const std::string& interfaceAddress) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
        mreq.imr_interface.s_addr = inet_addr(interfaceAddress.c_str());
        setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        timeval tv{0, 50000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        thread_ = std::thread([this] {
            uint8_t buffer[4096];
            while (running_) {
                const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    packets_.push_back({std::vector<uint8_t>(buffer, buffer + n), std::chrono::steady_clock::now()});
                }
            }
        });
    }
    ~SapCapture() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        close(fd_);
    }
    std::vector<Received> packets() {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_;
    }

private:
    int fd_{-1};
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Received> packets_;
};

inline bool isDeletion(const Received& r) { return (r.bytes[0] & 0x04) != 0; }

} // namespace TestSupport
} // namespace AES67
