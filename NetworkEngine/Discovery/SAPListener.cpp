#include "SAPListener.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <mutex>

namespace AES67 {

// PIMPL idiom to hide platform-specific implementation details
class SAPListener::Impl {
    friend class SAPListener;  // parseAnnouncement() exposes the packet parser

public:
    Impl() : running_(false), sockFd_(-1) {
    }
    
    ~Impl() {
        stop();
    }
    
    bool initialize() {
        // Create UDP socket
        sockFd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockFd_ < 0) {
            std::cerr << "Failed to create SAP socket" << std::endl;
            return false;
        }
        
        // Enable SO_REUSEADDR to allow reusing the port
        int opt = 1;
        if (setsockopt(sockFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            close(sockFd_);
            return false;
        }
        
        // Bind to SAP multicast address
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9875);  // SAP port
        addr.sin_addr.s_addr = inet_addr("224.2.127.254");  // SAP multicast address
        
        if (bind(sockFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind SAP socket" << std::endl;
            close(sockFd_);
            return false;
        }
        
        // Join multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr("224.2.127.254");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        
        if (setsockopt(sockFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            std::cerr << "Failed to join SAP multicast group" << std::endl;
            close(sockFd_);
            return false;
        }
        
        return true;
    }
    
    bool start() {
        if (running_) {
            return false;
        }
        
        running_ = true;
        listenThread_ = std::thread(&Impl::listenLoop, this);
        
        return true;
    }
    
    void stop() {
        if (!running_) {
            return;
        }
        
        running_ = false;
        
        if (listenThread_.joinable()) {
            listenThread_.join();
        }
        
        if (sockFd_ >= 0) {
            close(sockFd_);
            sockFd_ = -1;
        }
    }
    
    void registerAnnouncementCallback(const SAPAnnouncementCallback& callback) {
        std::lock_guard<std::mutex> lock(callbacksMutex_);
        callbacks_.push_back(callback);
    }
    
    std::vector<SAPAnnouncement> getDiscoveredStreams() const {
        std::lock_guard<std::mutex> lock(discoveredStreamsMutex_);
        return discoveredStreams_;
    }
    
private:
    void listenLoop() {
        char buffer[2048];
        
        while (running_) {
            struct sockaddr_in srcAddr;
            socklen_t addrLen = sizeof(srcAddr);
            
            ssize_t bytesRead = recvfrom(sockFd_, buffer, sizeof(buffer)-1, 0,
                                        (struct sockaddr*)&srcAddr, &addrLen);
            
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                
                // Parse the SAP announcement (inet_ntop: inet_ntoa's buffer is shared across threads)
                char source[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &srcAddr.sin_addr, source, sizeof(source));
                SAPAnnouncement announcement = parseSAPAnnouncement(buffer, bytesRead, source);
                
                if (!announcement.sessionDescription.empty()) {
                    // Store the announcement
                    {
                        std::lock_guard<std::mutex> lock(discoveredStreamsMutex_);
                        
                        // Check if we already have this announcement to avoid duplicates
                        bool found = false;
                        for (const auto& existing : discoveredStreams_) {
                            if (existing.sessionName == announcement.sessionName && 
                                existing.sourceAddress == announcement.sourceAddress) {
                                found = true;
                                break;
                            }
                        }
                        
                        if (!found) {
                            discoveredStreams_.push_back(announcement);
                            
                            // Keep only the most recent announcements (e.g., last 50)
                            if (discoveredStreams_.size() > 50) {
                                discoveredStreams_.erase(discoveredStreams_.begin());
                            }
                        }
                    }
                    
                    // Notify all registered callbacks
                    {
                        std::lock_guard<std::mutex> lock(callbacksMutex_);
                        for (const auto& callback : callbacks_) {
                            callback(announcement);
                        }
                    }
                }
            }
        }
    }
    
    static SAPAnnouncement parseSAPAnnouncement(const char* data, size_t length,
                                                const std::string& sourceAddress) {
        SAPAnnouncement announcement;
        announcement.sourceAddress = sourceAddress;

        // SAP header (RFC 2974) minimum: 4 bytes + 4 bytes originating source
        // Byte 0: V(3) | A(1) | R(1) | T(1) | E(1) | C(1)
        // Byte 1: Auth length
        // Bytes 2-3: Message ID Hash
        // Bytes 4-7: Originating source (IPv4)
        static constexpr size_t kMinSAPHeaderSize = 4;
        static constexpr size_t kMaxSAPPacketSize = 4096; // Reasonable upper bound

        if (length < kMinSAPHeaderSize || length > kMaxSAPPacketSize) {
            return announcement; // Reject undersized or oversized packets
        }

        // Validate SAP version (must be 1, in bits 5-7 of byte 0)
        uint8_t sapHeader = static_cast<uint8_t>(data[0]);
        uint8_t version = (sapHeader >> 5) & 0x07;
        if (version != 1) {
            return announcement; // Unknown SAP version
        }

        // Check type bit (0 = announcement, 1 = deletion)
        uint8_t typeBit = (sapHeader >> 2) & 0x01;
        if (typeBit != 0) {
            return announcement; // Not an announcement (deletion)
        }

        // Check encryption and compression bits — we don't support them
        uint8_t encrypted = (sapHeader >> 1) & 0x01;
        uint8_t compressed = sapHeader & 0x01;
        if (encrypted || compressed) {
            return announcement; // Encrypted/compressed SAP not supported
        }

        // Auth length (number of 32-bit words of authentication data)
        uint8_t authLen = static_cast<uint8_t>(data[1]);

        // Payload offset: 4 (base header) + originating source (4 bytes, or
        // 16 when the A bit marks an IPv6 address) + authLen*4
        const size_t originLength = ((sapHeader >> 4) & 0x01) ? 16 : 4;
        size_t payloadStart = 4 + originLength + (static_cast<size_t>(authLen) * 4);
        if (payloadStart >= length) {
            return announcement; // No room for payload
        }

        // Optional payload type: a null-terminated MIME type before the
        // payload. It is absent when the payload starts directly with "v=0".
        static constexpr char kSdpPrefix[] = "v=0";
        static constexpr char kSdpPayloadType[] = "application/sdp";
        if (length - payloadStart < 3 || std::memcmp(data + payloadStart, kSdpPrefix, 3) != 0) {
            const void* terminator = std::memchr(data + payloadStart, '\0', length - payloadStart);
            if (terminator == nullptr) {
                return announcement; // No payload type terminator
            }
            const size_t typeLength = static_cast<const char*>(terminator) - (data + payloadStart);
            if (std::string(data + payloadStart, typeLength) != kSdpPayloadType) {
                return announcement; // Not an SDP announcement
            }
            payloadStart += typeLength + 1;
        }

        // Payload must contain printable text (SDP). Reject binary garbage.
        size_t payloadLen = length - payloadStart;
        if (payloadLen < 5) { // Minimum valid SDP: "v=0\r\n"
            return announcement;
        }

        // The payload should be an SDP description
        std::string sdpContent(data + payloadStart, payloadLen);

        // The SDP starts here, with its version line
        if (sdpContent.compare(0, 3, kSdpPrefix) != 0) {
            return announcement; // Not valid SDP
        }

        announcement.sessionDescription = sdpContent;

        // Parse basic SDP information
        parseSDPInfo(sdpContent, announcement);

        return announcement;
    }
    
    static void parseSDPInfo(const std::string& sdp, SAPAnnouncement& announcement) {
        // Parse the SDP content to extract stream information
        size_t lastPos = 0;
        size_t pos = 0;

        while ((pos = sdp.find('\n', lastPos)) != std::string::npos) {
            std::string line = sdp.substr(lastPos, pos - lastPos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back(); // Remove carriage return if present
            }

            // Parse session name
            if (line.length() >= 2 && line.substr(0, 2) == "s=") {
                announcement.sessionName = line.substr(2);
            }
            // Parse connection information
            else if (line.length() >= 2 && line.substr(0, 2) == "c=") {
                // Format: c=IN IP4 <address>
                size_t addrStart = line.rfind(' ');
                if (addrStart != std::string::npos) {
                    announcement.multicastAddress = line.substr(addrStart + 1);
                }
            }
            // Parse media information
            else if (line.length() >= 2 && line.substr(0, 2) == "m=") {
                // Format: m=audio <port> RTP/AVP <payload_type>
                size_t portStart = line.find(' ', 2);
                if (portStart != std::string::npos) {
                    portStart++; // Skip the space
                    size_t portEnd = line.find(' ', portStart);
                    if (portEnd != std::string::npos) {
                        std::string portStr = line.substr(portStart, portEnd - portStart);
                        try {
                            announcement.port = std::stoi(portStr);
                        } catch (...) {
                            announcement.port = 0;
                        }
                    }
                }
            }
            // Parse RTP attribute (a=rtpmap)
            else if (line.length() >= 9 && line.substr(0, 9) == "a=rtpmap:") {
                // Format: a=rtpmap:<payload_type> <encoding_name>/<clock_rate>[/<channels>]
                // This can be used for additional stream information if needed
            }

            lastPos = pos + 1;
        }

        // Handle the final line without newline
        if (lastPos < sdp.length()) {
            std::string line = sdp.substr(lastPos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // Parse session name
            if (line.length() >= 2 && line.substr(0, 2) == "s=") {
                announcement.sessionName = line.substr(2);
            }
            // Parse connection information
            else if (line.length() >= 2 && line.substr(0, 2) == "c=") {
                size_t addrStart = line.rfind(' ');
                if (addrStart != std::string::npos) {
                    announcement.multicastAddress = line.substr(addrStart + 1);
                }
            }
            // Parse media information
            else if (line.length() >= 2 && line.substr(0, 2) == "m=") {
                size_t portStart = line.find(' ', 2);
                if (portStart != std::string::npos) {
                    portStart++; // Skip the space
                    size_t portEnd = line.find(' ', portStart);
                    if (portEnd != std::string::npos) {
                        std::string portStr = line.substr(portStart, portEnd - portStart);
                        try {
                            announcement.port = std::stoi(portStr);
                        } catch (...) {
                            announcement.port = 0;
                        }
                    }
                }
            }
        }
    }
    
    std::atomic<bool> running_;
    int sockFd_;
    std::thread listenThread_;
    
    mutable std::mutex callbacksMutex_;
    std::vector<SAPAnnouncementCallback> callbacks_;
    
    mutable std::mutex discoveredStreamsMutex_;
    std::vector<SAPAnnouncement> discoveredStreams_;
};

SAPListener::SAPListener() : pimpl_(std::make_unique<Impl>()) {
}

SAPListener::~SAPListener() = default;

bool SAPListener::initialize() {
    return pimpl_->initialize();
}

bool SAPListener::start() {
    return pimpl_->start();
}

void SAPListener::stop() {
    pimpl_->stop();
}

void SAPListener::registerAnnouncementCallback(const SAPAnnouncementCallback& callback) {
    pimpl_->registerAnnouncementCallback(callback);
}

std::vector<SAPAnnouncement> SAPListener::getDiscoveredStreams() const {
    return pimpl_->getDiscoveredStreams();
}

SAPAnnouncement SAPListener::parseAnnouncement(const char* data, size_t length, const std::string& sourceAddress) {
    return Impl::parseSAPAnnouncement(data, length, sourceAddress);
}

} // namespace AES67