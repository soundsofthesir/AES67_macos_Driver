//
// SDPParser.cpp
// AES67 macOS Driver - Build #2
// SDP parser implementation with full Riedel Artist compatibility
//

#include "SDPParser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <optional>

namespace AES67 {

namespace {

/// `text` as a decimal number from 0 to `max`, or nullopt. Digits only (no
/// sign, space or suffix), so a value outside its field's range is refused
/// rather than wrapped into it (#73).
std::optional<uint64_t> decimal(const std::string& text, uint64_t max) {
    if (text.empty()) return std::nullopt;
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<uint64_t>(c - '0');
        if (value > (max - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

/// The first number of SDP's "<port>/<count>" and "<ttl>/<count>" forms.
std::string beforeSlash(const std::string& text) {
    return text.substr(0, text.find('/'));
}

} // namespace

// ============================================================================
// Validation
// ============================================================================

bool SDPSession::isValid() const {
    return getValidationErrors().empty();
}

std::vector<std::string> SDPSession::getValidationErrors() const {
    std::vector<std::string> errors;

    if (sessionName.empty()) {
        errors.push_back("Session name (s=) is required");
    }

    if (connectionAddress.empty()) {
        errors.push_back("Connection address (c=) is required");
    }

    if (port == 0) {
        errors.push_back("Port must be non-zero");
    }

    if (encoding != "L16" && encoding != "L24" && encoding != "AM824") {
        errors.push_back("Invalid encoding: " + encoding);
    }

    if (sampleRate == 0) {
        errors.push_back("Sample rate must be non-zero");
    }

    if (numChannels == 0) {
        errors.push_back("Channel count must be non-zero");
    }

    // Checked here, not only in the a=ptime parser: sessions also come from
    // streams.json, and packet buffers are sized from these
    if (!std::isfinite(ptime) || ptime <= 0.0) {
        errors.push_back("Packet time (ptime) must be a positive number of milliseconds");
    } else if (sampleRate > 0) {
        const double frames = static_cast<double>(sampleRate) * ptime / 1000.0;
        if (frames < 1.0 || frames >= kMaxFramesPerPacket + 0.5) {
            errors.push_back("Packet time (ptime) of " + std::to_string(ptime) + " ms is " +
                             std::to_string(frames) + " frames per packet; must be 1 to " +
                             std::to_string(kMaxFramesPerPacket));
        }
    }

    if (framecount > kMaxFramesPerPacket) {
        errors.push_back("Frame count must be at most " + std::to_string(kMaxFramesPerPacket));
    }

    return errors;
}

// ============================================================================
// Parsing
// ============================================================================

std::optional<SDPSession> SDPParser::parseFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseString(buffer.str());
}

std::optional<SDPSession> SDPParser::parseString(const std::string& sdp) {
    SDPSession session;
    auto lines = splitLines(sdp);

    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;  // Skip empty lines and comments
        }

        if (line.size() < 2 || line[1] != '=') {
            continue;  // Invalid line format
        }

        char type = line[0];
        std::string value = line.substr(2);

        switch (type) {
            case 'v':  // Version (should be 0)
                break;

            case 'o':  // Origin
                if (!parseOriginLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 's':  // Session name
                session.sessionName = trim(value);
                break;

            case 'i':  // Session information
                session.sessionInfo = trim(value);
                break;

            case 'c':  // Connection
                if (!parseConnectionLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 't':  // Timing
                if (!parseTimingLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 'm':  // Media
                if (!parseMediaLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 'a':  // Attribute
                if (!parseAttributeLine(value, session)) {
                    return std::nullopt;
                }
                break;

            default:
                // Unknown line type, ignore
                break;
        }
    }

    // Validate parsed session
    if (!session.isValid()) {
        return std::nullopt;
    }

    return session;
}

bool SDPParser::parseOriginLine(const std::string& line, SDPSession& session) {
    // Format: o=<username> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>
    auto parts = splitString(line, ' ');
    if (parts.size() < 6) {
        return false;
    }

    session.originUsername = parts[0];

    const auto id = decimal(parts[1], UINT64_MAX);
    const auto version = decimal(parts[2], UINT64_MAX);
    if (!id || !version) {
        return false;
    }
    session.sessionID = *id;
    session.sessionVersion = *version;

    session.originNetworkType = parts[3];
    session.originAddressType = parts[4];
    session.originAddress = parts[5];

    return true;
}

bool SDPParser::parseConnectionLine(const std::string& line, SDPSession& session) {
    // Format: c=<nettype> <addrtype> <connection-address>
    // Example: c=IN IP4 239.69.83.171/32
    auto parts = splitString(line, ' ');
    if (parts.size() < 3) {
        return false;
    }

    session.connectionType = parts[0];
    session.connectionNetwork = parts[1];

    std::string addrPart = parts[2];

    // Handle multicast with TTL: address/ttl, or address/ttl/count
    auto slashPos = addrPart.find('/');
    if (slashPos != std::string::npos) {
        session.connectionAddress = addrPart.substr(0, slashPos);
        const auto ttl = decimal(beforeSlash(addrPart.substr(slashPos + 1)), UINT8_MAX);
        if (!ttl) {
            return false;
        }
        session.ttl = static_cast<uint8_t>(*ttl);
    } else {
        session.connectionAddress = addrPart;
    }

    return true;
}

bool SDPParser::parseTimingLine(const std::string& line, SDPSession& session) {
    // Format: t=<start-time> <stop-time>
    auto parts = splitString(line, ' ');
    if (parts.size() < 2) {
        return false;
    }

    const auto start = decimal(parts[0], UINT64_MAX);
    const auto stop = decimal(parts[1], UINT64_MAX);
    if (!start || !stop) {
        return false;
    }
    session.timeStart = *start;
    session.timeStop = *stop;

    return true;
}

bool SDPParser::parseMediaLine(const std::string& line, SDPSession& session) {
    // Format: m=<media> <port> <proto> <fmt> ...
    // Example: m=audio 5004 RTP/AVP 96
    auto parts = splitString(line, ' ');
    if (parts.size() < 4) {
        return false;
    }

    session.mediaType = parts[0];

    // <port> or <port>/<count>
    const auto port = decimal(beforeSlash(parts[1]), UINT16_MAX);
    if (!port) {
        return false;
    }
    session.port = static_cast<uint16_t>(*port);

    session.transport = parts[2];

    // RTP payload types are 7 bits
    const auto payloadType = decimal(parts[3], 127);
    if (!payloadType) {
        return false;
    }
    session.payloadType = static_cast<uint8_t>(*payloadType);

    return true;
}

bool SDPParser::parseAttributeLine(const std::string& line, SDPSession& session) {
    // Format: a=<attribute> or a=<attribute>:<value>
    auto colonPos = line.find(':');
    std::string attribute, value;

    if (colonPos != std::string::npos) {
        attribute = line.substr(0, colonPos);
        value = line.substr(colonPos + 1);
    } else {
        attribute = line;
    }

    // Parse specific attributes
    if (attribute == "rtpmap") {
        return parseRTPMapAttribute(value, session);
    } else if (attribute == "ptime") {
        return parsePTimeAttribute(value, session);
    } else if (attribute == "framecount") {
        return parseFrameCountAttribute(value, session);
    } else if (attribute == "source-filter") {
        return parseSourceFilterAttribute(value, session);
    } else if (attribute == "ts-refclk") {
        return parsePTPRefClockAttribute(value, session);
    } else if (attribute == "mediaclk") {
        return parseMediaClockAttribute(value, session);
    } else if (attribute == "recvonly" || attribute == "sendonly" ||
               attribute == "sendrecv" || attribute == "inactive") {
        session.direction = attribute;
        return true;
    } else {
        // Store unknown attributes
        session.customAttributes[attribute] = value;
        return true;
    }
}

bool SDPParser::parseRTPMapAttribute(const std::string& value, SDPSession& session) {
    // Format: <payload type> <encoding name>/<clock rate>/<encoding params>
    // Example: 96 L24/48000/2
    auto parts = splitString(value, ' ');
    if (parts.size() < 2) {
        return false;
    }

    auto formatParts = splitString(parts[1], '/');
    if (formatParts.size() < 2) {
        return false;
    }

    session.encoding = formatParts[0];

    const auto sampleRate = decimal(formatParts[1], UINT32_MAX);
    // RFC 3551: no channel count means one channel
    const auto channels = formatParts.size() >= 3 ? decimal(formatParts[2], UINT16_MAX) : std::optional<uint64_t>(1);
    if (!sampleRate || !channels) {
        return false;
    }
    session.sampleRate = static_cast<uint32_t>(*sampleRate);
    session.numChannels = static_cast<uint16_t>(*channels);

    return true;
}

bool SDPParser::parsePTimeAttribute(const std::string& value, SDPSession& session) {
    try {
        // Milliseconds, fractional allowed; anything else is an error, not 0
        char* end = nullptr;
        const double ms = std::strtod(value.c_str(), &end);
        while (end && (*end == ' ' || *end == '\t' || *end == '\r')) ++end;
        if (end == value.c_str() || (end && *end != '\0') || !std::isfinite(ms) || ms <= 0.0) {
            return false;
        }
        session.ptime = ms;
        return true;
    } catch (...) {
        return false;
    }
}

bool SDPParser::parseFrameCountAttribute(const std::string& value, SDPSession& session) {
    const auto framecount = decimal(trim(value), UINT32_MAX);
    if (!framecount) {
        return false;
    }
    session.framecount = static_cast<uint32_t>(*framecount);
    return true;
}

bool SDPParser::parseSourceFilterAttribute(const std::string& value, SDPSession& session) {
    // Format: incl IN IP4 <dst-address> <src-list>
    // Example: incl IN IP4 239.69.83.171 192.168.1.100
    // Note: SDP source-filter values often have a leading space after the colon
    // (e.g., "a=source-filter: incl ..."), so trim before splitting to avoid
    // an empty first token that shifts all indices.
    auto parts = splitString(trim(value), ' ');
    if (parts.size() >= 5) {
        session.sourceAddress = parts[4];
        return true;
    }
    return false;
}

bool SDPParser::parsePTPRefClockAttribute(const std::string& value, SDPSession& session) {
    // RFC 7273: ptp=IEEE1588-2008:<EUI-64 grandmaster>[:domain-nmbr=<n>].
    // Riedel writes "domain-nmbr=0"; Dante devices (e.g. Behringer WING)
    // write a bare ":0"; some senders omit the domain (it is then 0).
    //   ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:domain-nmbr=0
    //   ptp=IEEE1588-2008:00-1D-C1-FF-FE-D1-7B-F3:0
    // The identity is a dash-separated EUI-64 (some senders write a 6-byte
    // MAC), so ':' only ever starts the domain.
    static const std::regex ptpRegex(
        R"(ptp=IEEE1588-2008:((?:[0-9A-Fa-f]{2}-){5,7}[0-9A-Fa-f]{2})(?::(?:domain-nmbr=)?(\d{1,3}))?\s*$)");
    std::smatch match;

    if (std::regex_search(value, match, ptpRegex)) {
        // A PTP domain number is one byte
        const auto domain = match[2].matched ? decimal(match[2], UINT8_MAX) : std::optional<uint64_t>(0);
        if (!domain) {
            return false;
        }
        session.ptpMasterMAC = match[1];
        session.ptpDomain = static_cast<int>(*domain);
        return true;
    }

    // Any other reference (localmac=, ptp=...:traceable, vendor forms) leaves
    // the stream receivable: unless a PTP reference was already found (RFC
    // 7273 allows several), record that there is none rather than rejecting
    // the whole description over one attribute
    if (session.ptpMasterMAC.empty()) {
        session.ptpDomain = -1;
    }
    session.customAttributes["ts-refclk"] = value;
    return true;
}

bool SDPParser::parseMediaClockAttribute(const std::string& value, SDPSession& session) {
    session.mediaClockType = value;
    return true;
}

// ============================================================================
// Generation
// ============================================================================

std::string SDPParser::generate(const SDPSession& session) {
    // RFC 4566 5: lines end in CRLF (parsers should accept LF; not all do)
    static constexpr const char* kLineEnd = "\r\n";
    std::ostringstream sdp;

    // Version
    sdp << "v=0" << kLineEnd;

    // Origin
    sdp << generateOriginLine(session) << kLineEnd;

    // Session name and info
    sdp << "s=" << session.sessionName << kLineEnd;
    if (!session.sessionInfo.empty()) {
        sdp << "i=" << session.sessionInfo << kLineEnd;
    }

    // Connection
    sdp << generateConnectionLine(session) << kLineEnd;

    // Timing
    sdp << "t=" << session.timeStart << " " << session.timeStop << kLineEnd;

    // Media
    sdp << generateMediaLine(session) << kLineEnd;

    // Attributes
    auto attributes = generateAttributes(session);
    for (const auto& attr : attributes) {
        sdp << attr << kLineEnd;
    }

    return sdp.str();
}

std::string SDPParser::generateOriginLine(const SDPSession& session) {
    std::ostringstream line;

    uint64_t sessId = session.sessionID;
    if (sessId == 0) {
        sessId = static_cast<uint64_t>(std::time(nullptr));
    }

    line << "o=" << session.originUsername
         << " " << sessId
         << " " << session.sessionVersion
         << " " << session.originNetworkType
         << " " << session.originAddressType
         << " " << session.originAddress;

    return line.str();
}

std::string SDPParser::generateConnectionLine(const SDPSession& session) {
    std::ostringstream line;
    line << "c=" << session.connectionType
         << " " << session.connectionNetwork
         << " " << session.connectionAddress;

    if (session.ttl != 0) {
        line << "/" << static_cast<int>(session.ttl);
    }

    return line.str();
}

std::string SDPParser::generateMediaLine(const SDPSession& session) {
    std::ostringstream line;
    line << "m=" << session.mediaType
         << " " << session.port
         << " " << session.transport
         << " " << static_cast<int>(session.payloadType);
    return line.str();
}

std::vector<std::string> SDPParser::generateAttributes(const SDPSession& session) {
    std::vector<std::string> attributes;

    // rtpmap
    std::ostringstream rtpmap;
    rtpmap << "a=rtpmap:" << static_cast<int>(session.payloadType)
           << " " << session.encoding
           << "/" << session.sampleRate
           << "/" << session.numChannels;
    attributes.push_back(rtpmap.str());

    // ptime
    // Shortest exact form: 1, 0.25, 0.125, 0.333
    char ptime[32];
    std::snprintf(ptime, sizeof(ptime), "%.3f", session.ptime);
    std::string text(ptime);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
    attributes.push_back("a=ptime:" + text);

    // framecount
    attributes.push_back("a=framecount:" + std::to_string(session.framecount));

    // direction
    attributes.push_back("a=" + session.direction);

    // source-filter
    if (!session.sourceAddress.empty()) {
        std::ostringstream sourceFilter;
        sourceFilter << "a=source-filter: incl IN IP4 "
                    << session.connectionAddress << " "
                    << session.sourceAddress;
        attributes.push_back(sourceFilter.str());
    }

    // PTP reference clock
    if (session.ptpDomain >= 0 && !session.ptpMasterMAC.empty()) {
        std::ostringstream ptpRefclk;
        // AES67's form (as Dante and RAVENNA write it): the domain number
        // after the grandmaster, not RFC 7273's domain-nmbr=
        ptpRefclk << "a=ts-refclk:ptp=IEEE1588-2008:"
                  << session.ptpMasterMAC
                  << ":" << session.ptpDomain;
        attributes.push_back(ptpRefclk.str());
    }

    // Media clock
    if (!session.mediaClockType.empty()) {
        attributes.push_back("a=mediaclk:" + session.mediaClockType);
    }

    // Custom attributes
    for (const auto& [key, value] : session.customAttributes) {
        if (value.empty()) {
            attributes.push_back("a=" + key);
        } else {
            attributes.push_back("a=" + key + ":" + value);
        }
    }

    return attributes;
}

bool SDPParser::writeFile(const SDPSession& session, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << generate(session);
    return true;
}

// ============================================================================
// Validation
// ============================================================================

bool SDPParser::validate(const SDPSession& session, std::vector<std::string>* errors) {
    auto validationErrors = session.getValidationErrors();

    if (errors) {
        *errors = validationErrors;
    }

    return validationErrors.empty();
}

// ============================================================================
// Convenience Functions
// ============================================================================

SDPSession SDPParser::createDefaultTxSession(
    const std::string& name,
    const std::string& sourceIP,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    uint32_t sampleRate,
    const std::string& encoding
) {
    SDPSession session;

    session.sessionName = name;
    session.sessionInfo = "AES67 Stream";
    session.sessionID = static_cast<uint64_t>(std::time(nullptr));
    session.sessionVersion = 0;

    session.originUsername = "-";
    session.originAddress = sourceIP;
    session.originAddressType = "IP4";
    session.originNetworkType = "IN";

    session.connectionAddress = multicastIP;
    session.connectionType = "IN";
    session.connectionNetwork = "IP4";
    session.ttl = 32;

    session.timeStart = 0;
    session.timeStop = 0;

    session.mediaType = "audio";
    session.port = port;
    session.transport = "RTP/AVP";
    session.payloadType = 96;

    session.encoding = encoding;
    session.sampleRate = sampleRate;
    session.numChannels = numChannels;

    // Calculate ptime and framecount
    session.ptime = 1;  // 1ms packets
    session.framecount = sampleRate / 1000;  // Samples per 1ms

    session.sourceAddress = sourceIP;
    session.direction = "sendonly";

    session.ptpDomain = 0;
    session.mediaClockType = "direct=0";

    return session;
}

StreamInfo SDPParser::toStreamInfo(const SDPSession& session) {
    StreamInfo info;

    info.id = StreamID::generate();
    info.name = session.sessionName;
    info.description = session.sessionInfo;

    // Network
    info.source.ip = session.sourceAddress;
    info.source.port = 0;  // Not in SDP

    info.multicast.ip = session.connectionAddress;
    info.multicast.port = session.port;
    info.multicast.ttl = session.ttl;

    // Audio format
    if (session.encoding == "L16") {
        info.encoding = AudioEncoding::L16;
    } else if (session.encoding == "L24") {
        info.encoding = AudioEncoding::L24;
    } else {
        info.encoding = AudioEncoding::Unknown;
    }

    info.sampleRate = session.sampleRate;
    info.numChannels = session.numChannels;
    info.payloadType = session.payloadType;

    // Timing
    info.ptime = static_cast<uint32_t>(std::llround(session.ptime * 1000.0));  // microseconds
    info.framecount = session.framecount;

    // PTP
    info.ptp.domain = session.ptpDomain;
    info.ptp.masterMAC = session.ptpMasterMAC;
    info.ptp.enabled = (session.ptpDomain >= 0);

    return info;
}

SDPSession SDPParser::fromStreamInfo(const StreamInfo& info) {
    SDPSession session;

    session.sessionName = info.name;
    session.sessionInfo = info.description;
    session.sessionID = static_cast<uint64_t>(std::time(nullptr));

    session.originAddress = info.source.ip;
    session.connectionAddress = info.multicast.ip;
    session.port = info.multicast.port;
    session.ttl = info.multicast.ttl;

    switch (info.encoding) {
        case AudioEncoding::L16:
            session.encoding = "L16";
            break;
        case AudioEncoding::L24:
            session.encoding = "L24";
            break;
        default:
            session.encoding = "L24";
    }

    session.sampleRate = info.sampleRate;
    session.numChannels = info.numChannels;
    session.payloadType = info.payloadType;

    session.ptime = info.ptime / 1000.0;  // StreamInfo holds microseconds
    session.framecount = info.framecount;

    session.sourceAddress = info.source.ip;

    session.ptpDomain = info.ptp.domain;
    session.ptpMasterMAC = info.ptp.masterMAC;

    return session;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::vector<std::string> SDPParser::splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        // Remove \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return lines;
}

std::vector<std::string> SDPParser::splitString(const std::string& str, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream stream(str);
    std::string part;

    while (std::getline(stream, part, delimiter)) {
        parts.push_back(trim(part));
    }

    return parts;
}

std::string SDPParser::trim(const std::string& str) {
    const char* whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}

bool SDPParser::startsWith(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

bool SDPParser::isValidIPv4(const std::string& ip) {
    std::regex ipv4Regex(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch match;

    if (!std::regex_match(ip, match, ipv4Regex)) {
        return false;
    }

    for (int i = 1; i <= 4; i++) {
        int octet = std::stoi(match[i]);
        if (octet < 0 || octet > 255) {
            return false;
        }
    }

    return true;
}

bool SDPParser::isValidPort(uint16_t port) {
    return port > 0;
}

bool SDPParser::isValidSampleRate(uint32_t sampleRate) {
    static const std::vector<uint32_t> validRates = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
    };
    return std::find(validRates.begin(), validRates.end(), sampleRate) != validRates.end();
}

bool SDPParser::isValidEncoding(const std::string& encoding) {
    return encoding == "L16" || encoding == "L24" || encoding == "AM824";
}

} // namespace AES67
