//
// SimpleRTP.h
// AES67 macOS Driver - Build #8
// Minimal RTP implementation for AES67 (RFC 3550)
//

#pragma once

#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace AES67 {
namespace RTP {

//
// RTP Header (RFC 3550 Section 5.1)
//
// 0                   1                   2                   3
// 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//
#pragma pack(push, 1)
struct RTPHeader {
    // Byte 0
    uint8_t cc:4;          // CSRC count
    uint8_t extension:1;   // Extension bit
    uint8_t padding:1;     // Padding bit
    uint8_t version:2;     // Version (always 2)

    // Byte 1
    uint8_t payloadType:7; // Payload type
    uint8_t marker:1;      // Marker bit

    // Bytes 2-3
    uint16_t sequenceNumber;

    // Bytes 4-7
    uint32_t timestamp;

    // Bytes 8-11
    uint32_t ssrc;

    // Convert to network byte order
    void toNetworkOrder() {
        sequenceNumber = htons(sequenceNumber);
        timestamp = htonl(timestamp);
        ssrc = htonl(ssrc);
    }

    // Convert from network byte order
    void toHostOrder() {
        sequenceNumber = ntohs(sequenceNumber);
        timestamp = ntohl(timestamp);
        ssrc = ntohl(ssrc);
    }
};
#pragma pack(pop)

static_assert(sizeof(RTPHeader) == 12, "RTP header must be 12 bytes");

//
// RTP Payload Types (RFC 3551)
//
constexpr uint8_t PT_PCMU = 0;      // G.711 μ-law
constexpr uint8_t PT_GSM = 3;       // GSM
constexpr uint8_t PT_G723 = 4;      // G.723
constexpr uint8_t PT_PCMA = 8;      // G.711 A-law
constexpr uint8_t PT_L16_2CH = 10;  // L16 stereo
constexpr uint8_t PT_L16_1CH = 11;  // L16 mono
constexpr uint8_t PT_DYNAMIC = 96;  // Dynamic payload types start here

//
// AES67 uses dynamic payload types (96-127) for L16/L24
//
/// Signed distance from one 16-bit RTP sequence number to another, accounting
/// for wraparound: positive if `to` is after `from`, in [-32768, 32767].
inline int32_t sequenceDistance(uint16_t from, uint16_t to) {
    return static_cast<int16_t>(static_cast<uint16_t>(to - from));
}

constexpr uint8_t PT_AES67_L16 = 96;
constexpr uint8_t PT_AES67_L24 = 97;

//
// RTP Packet
//
struct RTPPacket {
    RTPHeader header;
    uint8_t* payload;
    size_t payloadSize;

    RTPPacket() : payload(nullptr), payloadSize(0) {
        header.version = 2;
        header.padding = 0;
        header.extension = 0;
        header.cc = 0;
        header.marker = 0;
        header.payloadType = PT_AES67_L16;
        header.sequenceNumber = 0;
        header.timestamp = 0;
        header.ssrc = 0;
    }
};

/// Parses one received RTP packet (RFC 3550 5.1). The payload starts after
/// any CSRCs and header extension and excludes padding. Returns false if the
/// packet is shorter than its header says.
bool parsePacket(uint8_t* data, size_t length, RTPPacket& packet);

//
// RTP Socket - Simple UDP multicast wrapper
//
class RTPSocket {
public:
    RTPSocket();
    ~RTPSocket();

    // Receiver setup
    /// @param interfaceIndex Interface to join on (if_nametoindex); 0 lets the kernel choose.
    bool openReceiver(const char* multicastIP, uint16_t port, unsigned interfaceIndex = 0);

    // Transmitter setup
    /// @param interfaceIndex Interface to send on (if_nametoindex); 0 lets the routing table choose.
    /// @param ttl Multicast TTL. 0 keeps packets on this host (loopback only).
    bool openTransmitter(const char* multicastIP, uint16_t port, unsigned interfaceIndex = 0, uint8_t ttl = 32);

    /// The socket's multicast TTL as the kernel reports it (0 if not open).
    uint8_t multicastTTL() const;

    // Send RTP packet
    ssize_t send(const RTPPacket& packet);

    // Receive RTP packet
    /// @param kernelHostTime If non-null, receives the kernel's arrival time in
    ///        host ticks (SO_TIMESTAMP_MONOTONIC), or 0 if unavailable.
    ssize_t receive(RTPPacket& packet, uint8_t* buffer, size_t bufferSize, uint64_t* kernelHostTime = nullptr);

    // Close socket
    void close();

    // Socket status checks
    bool isOpen() const { return sockfd_ >= 0; }
    bool isValid() const;

    // Get socket file descriptor for select/poll
    int getFd() const { return sockfd_; }

private:
    int sockfd_;
    struct sockaddr_in multicastAddr_;
    bool isReceiver_;
    unsigned joinedInterfaceIndex_{0};  // interface the group was joined on (for the leave)
};

//
// L16 Audio Encoder/Decoder (16-bit PCM, network byte order)
//
class L16Codec {
public:
    // Encode float samples to L16 (big-endian 16-bit PCM)
    static void encode(const float* samples, size_t numSamples, uint8_t* output);

    // Decode L16 to float samples
    static void decode(const uint8_t* input, size_t numBytes, float* samples);
};

//
// L24 Audio Encoder/Decoder (24-bit PCM, network byte order)
//
class L24Codec {
public:
    // Encode float samples to L24 (big-endian 24-bit PCM)
    static void encode(const float* samples, size_t numSamples, uint8_t* output);

    // Decode L24 to float samples
    static void decode(const uint8_t* input, size_t numBytes, float* samples);
};

} // namespace RTP
} // namespace AES67
