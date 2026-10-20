//
// Types.cpp
// AES67 macOS Driver - Build #2
// Implementation of common types and utilities
//

#include "Types.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <regex>
#include <chrono>

namespace AES67 {

// ============================================================================
// StreamID Implementation
// ============================================================================

StreamID::StreamID() {
    std::memset(uuid_.data(), 0, 16);
}

StreamID::StreamID(const uint8_t uuid[16]) {
    std::memcpy(uuid_.data(), uuid, 16);
}

StreamID::StreamID(const std::string& uuidString) {
    // Parse UUID string (format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    std::string cleaned = uuidString;
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '-'), cleaned.end());

    std::memset(uuid_.data(), 0, 16);
    if (cleaned.length() != 32) {
        return;
    }

    // Anything that isn't hex leaves the ID null (it comes from streams.json,
    // where a typo must not throw)
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<uint8_t, 16> parsed{};
    for (size_t i = 0; i < 16; i++) {
        const int high = hexValue(cleaned[i * 2]);
        const int low = hexValue(cleaned[i * 2 + 1]);
        if (high < 0 || low < 0) return;
        parsed[i] = static_cast<uint8_t>((high << 4) | low);
    }
    uuid_ = parsed;
}

bool StreamID::operator==(const StreamID& other) const {
    return uuid_ == other.uuid_;
}

bool StreamID::operator!=(const StreamID& other) const {
    return uuid_ != other.uuid_;
}

bool StreamID::operator<(const StreamID& other) const {
    return uuid_ < other.uuid_;
}

std::string StreamID::toString() const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            oss << '-';
        }
        oss << std::setw(2) << static_cast<int>(uuid_[i]);
    }

    return oss.str();
}

bool StreamID::isNull() const {
    return std::all_of(uuid_.begin(), uuid_.end(), [](uint8_t b) { return b == 0; });
}

StreamID StreamID::null() {
    return StreamID();
}

StreamID StreamID::generate() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);

    uint8_t uuid[16];
    for (int i = 0; i < 16; i++) {
        uuid[i] = dis(gen);
    }

    // Set version (4) and variant bits for UUID v4
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    return StreamID(uuid);
}

// ============================================================================
// Statistics Implementation
// ============================================================================

void Statistics::reset() {
    packetsReceived.store(0, std::memory_order_relaxed);
    packetsLost.store(0, std::memory_order_relaxed);
    malformedPackets.store(0, std::memory_order_relaxed);
    outOfOrderPackets.store(0, std::memory_order_relaxed);
    underruns.store(0, std::memory_order_relaxed);
    overruns.store(0, std::memory_order_relaxed);
    jitterNs.store(0, std::memory_order_relaxed);
    latencyNs.store(0, std::memory_order_relaxed);
    bytesReceived.store(0, std::memory_order_relaxed);
    bytesSent.store(0, std::memory_order_relaxed);
    lastPacketTimeNs.store(0, std::memory_order_relaxed);
}

double Statistics::getPacketLossPercent() const {
    uint64_t received = packetsReceived.load(std::memory_order_relaxed);
    if (received == 0) {
        return 0.0;
    }
    uint64_t lost = packetsLost.load(std::memory_order_relaxed);
    return (static_cast<double>(lost) / (received + lost)) * 100.0;
}

int64_t Statistics::timeSinceLastPacketMs() const {
    int64_t lastNs = lastPacketTimeNs.load(std::memory_order_acquire);
    if (lastNs == 0) {
        return -1;  // Never received
    }

    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return (nowNs - lastNs) / 1000000; // ns to ms
}

StatisticsSnapshot Statistics::snapshot() const {
    // Create a consistent snapshot of all atomic statistics
    StatisticsSnapshot snap;
    snap.packetsReceived = packetsReceived.load(std::memory_order_relaxed);
    snap.packetsLost = packetsLost.load(std::memory_order_relaxed);
    snap.malformedPackets = malformedPackets.load(std::memory_order_relaxed);
    snap.outOfOrderPackets = outOfOrderPackets.load(std::memory_order_relaxed);
    snap.underruns = underruns.load(std::memory_order_relaxed);
    snap.overruns = overruns.load(std::memory_order_relaxed);
    snap.jitterNs = jitterNs.load(std::memory_order_relaxed);
    snap.latencyNs = latencyNs.load(std::memory_order_relaxed);
    snap.bytesReceived = bytesReceived.load(std::memory_order_relaxed);
    snap.bytesSent = bytesSent.load(std::memory_order_relaxed);
    snap.lastPacketTimeNs = lastPacketTimeNs.load(std::memory_order_acquire);
    return snap;
}

// ============================================================================
// NetworkAddress Implementation
// ============================================================================

bool NetworkAddress::isValid() const {
    return !ip.empty() && port > 0;
}

bool NetworkAddress::isMulticast() const {
    return Utils::isMulticastIP(ip);
}

bool NetworkAddress::isAES67Multicast() const {
    return Utils::isAES67MulticastIP(ip);
}

std::string NetworkAddress::toString() const {
    return ip + ":" + std::to_string(port);
}

// ============================================================================
// PTPConfig Implementation
// ============================================================================

bool PTPConfig::isValid() const {
    return domain >= 0 && domain <= 127;
}

// ============================================================================
// StreamInfo Implementation
// ============================================================================

bool StreamInfo::isValid() const {
    return !id.isNull() &&
           !name.empty() &&
           multicast.isValid() &&
           encoding != AudioEncoding::Unknown &&
           sampleRate > 0 &&
           numChannels > 0;
}

// ============================================================================
// DeviceConfig Implementation
// ============================================================================

bool DeviceConfig::isValid() const {
    return sampleRate > 0 &&
           bufferSize > 0 &&
           ringBufferSize > 0 &&
           !deviceName.empty() &&
           !deviceUID.empty();
}

// ============================================================================
// Error Implementation
// ============================================================================

std::string Error::toString() const {
    std::ostringstream oss;
    oss << "Error " << static_cast<int>(code) << ": " << message;
    if (!context.empty()) {
        oss << " (" << context << ")";
    }
    return oss.str();
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace Utils {

uint32_t sampleRateToHz(SampleRate sr) {
    return static_cast<uint32_t>(sr);
}

SampleRate hzToSampleRate(uint32_t hz) {
    switch (hz) {
        case 44100: return SampleRate::SR_44100;
        case 48000: return SampleRate::SR_48000;
        case 88200: return SampleRate::SR_88200;
        case 96000: return SampleRate::SR_96000;
        case 176400: return SampleRate::SR_176400;
        case 192000: return SampleRate::SR_192000;
        case 352800: return SampleRate::SR_352800;
        case 384000: return SampleRate::SR_384000;
        default: return SampleRate::SR_48000;
    }
}

bool isValidIPv4(const std::string& ip) {
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

bool isMulticastIP(const std::string& ip) {
    if (!isValidIPv4(ip)) {
        return false;
    }

    size_t dotPos = ip.find('.');
    if (dotPos == std::string::npos) {
        return false;
    }

    int firstOctet = std::stoi(ip.substr(0, dotPos));
    return firstOctet >= 224 && firstOctet <= 239;
}

bool isAES67MulticastIP(const std::string& ip) {
    if (!isValidIPv4(ip)) {
        return false;
    }

    size_t dotPos = ip.find('.');
    if (dotPos == std::string::npos) {
        return false;
    }

    int firstOctet = std::stoi(ip.substr(0, dotPos));
    return firstOctet == 239;  // AES67 recommends 239.x.x.x range
}

uint64_t getNanoseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

uint64_t getMicroseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

uint64_t getMilliseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string formatBytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

std::string formatDuration(std::chrono::milliseconds ms) {
    auto hours = std::chrono::duration_cast<std::chrono::hours>(ms);
    ms -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(ms);
    ms -= minutes;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ms);

    std::ostringstream oss;
    if (hours.count() > 0) {
        oss << hours.count() << "h ";
    }
    if (minutes.count() > 0 || hours.count() > 0) {
        oss << minutes.count() << "m ";
    }
    oss << seconds.count() << "s";

    return oss.str();
}

} // namespace Utils

} // namespace AES67
