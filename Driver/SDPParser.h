//
// SDPParser.h
// AES67 macOS Driver - Build #1
// SDP (Session Description Protocol) parser with Riedel Artist compatibility
//

#pragma once

#include "../Shared/Types.h"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace AES67 {

//
// SDP Session
//
// Represents a complete SDP session description for an AES67 audio stream
// Compatible with Riedel Artist SDP format
//
struct SDPSession {
    // Session-level information
    std::string sessionName;        // s=
    std::string sessionInfo;        // i=
    uint64_t sessionID{0};          // o= (session ID)
    uint64_t sessionVersion{0};     // o= (version)

    // Origin
    std::string originUsername{"-"};       // o= (username, usually "-")
    std::string originAddress;             // o= (unicast address)
    std::string originAddressType{"IP4"};  // o= addrtype (RFC 4566: "IP4")
    std::string originNetworkType{"IN"};   // o= nettype (RFC 4566: "IN")

    // Connection (c=)
    std::string connectionAddress;   // Usually multicast IP
    std::string connectionType{"IN"};
    std::string connectionNetwork{"IP4"};
    uint8_t ttl{32};                // TTL for multicast

    // Timing (t=)
    uint64_t timeStart{0};          // Usually 0 (permanent session)
    uint64_t timeStop{0};           // Usually 0 (permanent session)

    // Media (m=)
    std::string mediaType{"audio"};
    uint16_t port{5004};
    std::string transport{"RTP/AVP"};
    uint8_t payloadType{96};        // Dynamic payload type

    // Audio format (a=rtpmap)
    std::string encoding{"L24"};    // L16, L24, or AM824
    uint32_t sampleRate{48000};
    uint16_t numChannels{2};

    // Packet timing (a=ptime, a=framecount)
    double ptime{1.0};              // Packet time in ms (AES67: 0.125, 0.25, 0.333, 1, 4)
    uint32_t framecount{48};        // Samples per packet

    // Source filter (a=source-filter)
    std::string sourceAddress;      // Unicast source IP

    // PTP timing (a=ts-refclk, a=mediaclk)
    int ptpDomain{0};               // -1 = no PTP
    std::string ptpMasterMAC;       // PTP grandmaster MAC address
    std::string mediaClockType{"direct=0"};  // Media clock reference

    // Additional attributes
    std::string direction{"recvonly"};  // sendonly, recvonly, sendrecv, inactive
    std::map<std::string, std::string> customAttributes;

    // Validation
    bool isValid() const;
    std::vector<std::string> getValidationErrors() const;
};

/// The most frames a packet may carry: what the receiver decodes (4 ms at
/// 96 kHz, 2 ms at 192 kHz). Sessions asking for more are invalid.
constexpr uint32_t kMaxFramesPerPacket = 512;

/// Frames in one packet: sample rate x packet time, rounded (333 us at 48 kHz
/// is 16 frames). Falls back to a=framecount if the packet time is unusable.
inline uint32_t framesPerPacket(const SDPSession& session) {
    const double frames = static_cast<double>(session.sampleRate) * session.ptime / 1000.0;
    if (frames >= 1.0) {
        return static_cast<uint32_t>(frames + 0.5);
    }
    return session.framecount > 0 ? session.framecount : 1;
}

//
// SDP Parser
//
// Parses and generates SDP files according to RFC 4566 with AES67 extensions
// Fully compatible with Riedel Artist SDP format
//
class SDPParser {
public:
    //
    // Parse SDP from file
    //
    static std::optional<SDPSession> parseFile(const std::string& filepath);

    //
    // Parse SDP from string
    //
    static std::optional<SDPSession> parseString(const std::string& sdp);

    //
    // Generate SDP string from session
    //
    static std::string generate(const SDPSession& session);

    //
    // Write SDP session to file
    //
    static bool writeFile(const SDPSession& session, const std::string& filepath);

    //
    // Validate SDP session
    //
    static bool validate(const SDPSession& session, std::vector<std::string>* errors = nullptr);

    //
    // Create default SDP session for transmit
    //
    static SDPSession createDefaultTxSession(
        const std::string& name,
        const std::string& sourceIP,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        uint32_t sampleRate,
        const std::string& encoding = "L24"
    );

    //
    // Extract stream info from SDP session
    //
    static StreamInfo toStreamInfo(const SDPSession& session);

    //
    // Create SDP session from stream info
    //
    static SDPSession fromStreamInfo(const StreamInfo& info);

private:
    // Parsing helper functions
    static bool parseSessionLine(const std::string& line, SDPSession& session);
    static bool parseOriginLine(const std::string& line, SDPSession& session);
    static bool parseConnectionLine(const std::string& line, SDPSession& session);
    static bool parseTimingLine(const std::string& line, SDPSession& session);
    static bool parseMediaLine(const std::string& line, SDPSession& session);
    static bool parseAttributeLine(const std::string& line, SDPSession& session);

    // Attribute parsers (a=)
    static bool parseRTPMapAttribute(const std::string& value, SDPSession& session);
    static bool parsePTimeAttribute(const std::string& value, SDPSession& session);
    static bool parseFrameCountAttribute(const std::string& value, SDPSession& session);
    static bool parseSourceFilterAttribute(const std::string& value, SDPSession& session);
    static bool parsePTPRefClockAttribute(const std::string& value, SDPSession& session);
    static bool parseMediaClockAttribute(const std::string& value, SDPSession& session);
    static bool parseDirectionAttribute(const std::string& value, SDPSession& session);

    // Generation helper functions
    static std::string generateOriginLine(const SDPSession& session);
    static std::string generateConnectionLine(const SDPSession& session);
    static std::string generateMediaLine(const SDPSession& session);
    static std::vector<std::string> generateAttributes(const SDPSession& session);

    // Utility functions
    static std::vector<std::string> splitLines(const std::string& text);
    static std::vector<std::string> splitString(const std::string& str, char delimiter);
    static std::string trim(const std::string& str);
    static bool startsWith(const std::string& str, const std::string& prefix);

    // Validation helpers
    static bool isValidIPv4(const std::string& ip);
    static bool isValidPort(uint16_t port);
    static bool isValidSampleRate(uint32_t sampleRate);
    static bool isValidEncoding(const std::string& encoding);
};

} // namespace AES67
