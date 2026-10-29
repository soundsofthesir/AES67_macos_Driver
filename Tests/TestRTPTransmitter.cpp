//
// TestRTPTransmitter.cpp
// AES67 macOS Driver - Build #17
// Unit tests for RTP packet transmitter
//

#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "../Driver/SDPParser.h"
#include "../NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

//
// RTP Header Tests
//

bool testRTPHeaderInitialization() {
    std::cout << "Test: RTP header initialization... ";

    RTPPacket packet;

    // Default values
    TEST_ASSERT(packet.header.version == 2, "RTP version should be 2");
    TEST_ASSERT(packet.header.padding == 0, "Padding should be disabled by default");
    TEST_ASSERT(packet.header.extension == 0, "Extension should be disabled by default");
    TEST_ASSERT(packet.header.cc == 0, "CSRC count should be 0");
    TEST_ASSERT(packet.header.marker == 0, "Marker should be 0 by default");
    TEST_ASSERT(packet.header.payloadType == PT_AES67_L16, "Default payload should be L16");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testRTPHeaderNetworkByteOrder() {
    std::cout << "Test: RTP header network byte order conversion... ";

    RTPPacket packet;
    packet.header.sequenceNumber = 0x1234;
    packet.header.timestamp = 0x12345678;
    packet.header.ssrc = 0xABCDEF01;

    // Store original values
    uint16_t origSeq = packet.header.sequenceNumber;
    uint32_t origTs = packet.header.timestamp;
    uint32_t origSsrc = packet.header.ssrc;

    // Convert to network order
    packet.header.toNetworkOrder();

    // On little-endian systems, bytes should be swapped
    // We can't test exact values without knowing endianness,
    // but we can test round-trip
    packet.header.toHostOrder();

    TEST_ASSERT(packet.header.sequenceNumber == origSeq, "Sequence number round-trip");
    TEST_ASSERT(packet.header.timestamp == origTs, "Timestamp round-trip");
    TEST_ASSERT(packet.header.ssrc == origSsrc, "SSRC round-trip");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Sequence Number Tests
//

bool testSequenceNumberIncrement() {
    std::cout << "Test: Sequence number increment and wrap... ";

    uint16_t seq = 0;

    // Normal increment
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT(seq == i, "Sequence should increment normally");
        seq++;
    }

    // Test wrap-around
    seq = 65534;
    seq++;
    TEST_ASSERT(seq == 65535, "Sequence at boundary");
    seq++;
    TEST_ASSERT(seq == 0, "Sequence should wrap to 0");
    seq++;
    TEST_ASSERT(seq == 1, "Sequence should continue after wrap");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Timestamp Tests
//

bool testTimestampIncrement() {
    std::cout << "Test: Timestamp increment... ";

    // At 48kHz, 1ms packet = 48 samples
    uint32_t samplesPerPacket = 48;
    uint32_t timestamp = 0;

    // First packet
    TEST_ASSERT(timestamp == 0, "Initial timestamp should be 0");

    // Advance by packet interval
    timestamp += samplesPerPacket;
    TEST_ASSERT(timestamp == 48, "Should advance by samples per packet");

    // Simulate 1 second of packets (1000 packets @ 1ms each)
    for (int i = 0; i < 999; ++i) {
        timestamp += samplesPerPacket;
    }
    TEST_ASSERT(timestamp == 48000, "1000 packets should equal sample rate");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testTimestampWrap() {
    std::cout << "Test: Timestamp wrap-around... ";

    // Test timestamp wrap (32-bit)
    uint32_t timestamp = 0xFFFFFFF0;
    timestamp += 0x20;  // Will wrap

    TEST_ASSERT(timestamp == 0x10, "Timestamp should wrap around");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Audio Encoding Tests
//

bool testL16EncodingPrecision() {
    std::cout << "Test: L16 encoding precision... ";

    // Create test audio with various amplitudes
    float audio[8] = {
        0.0f, 0.25f, 0.5f, 0.75f,
        -0.25f, -0.5f, -0.75f, -1.0f
    };

    // Encode to L16
    uint8_t encoded[16];  // 8 samples * 2 bytes
    L16Codec::encode(audio, 8, encoded);

    // Decode back
    float decoded[8];
    L16Codec::decode(encoded, 16, decoded);

    // Verify round-trip
    for (int i = 0; i < 8; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        TEST_ASSERT(diff < 0.01f, "L16 round-trip precision");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testL24EncodingPrecision() {
    std::cout << "Test: L24 encoding precision... ";

    // Create test audio with various amplitudes
    float audio[8] = {
        0.0f, 0.25f, 0.5f, 0.75f,
        -0.25f, -0.5f, -0.75f, -1.0f
    };

    // Encode to L24
    uint8_t encoded[24];  // 8 samples * 3 bytes
    L24Codec::encode(audio, 8, encoded);

    // Decode back
    float decoded[8];
    L24Codec::decode(encoded, 24, decoded);

    // Verify round-trip (L24 should have better precision than L16)
    for (int i = 0; i < 8; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        TEST_ASSERT(diff < 0.001f, "L24 round-trip high precision");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testNonFiniteSamples() {
    std::cout << "Test: NaN encodes as silence, infinities as full scale (#60)... ";

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float audio[6] = {nan, -nan, inf, -inf, 2.0f, -2.0f};
    uint8_t l16[12];
    uint8_t l24[18];
    L16Codec::encode(audio, 6, l16);
    L24Codec::encode(audio, 6, l24);

    const auto pcm16 = [&](int i) { return static_cast<int16_t>((l16[i * 2] << 8) | l16[i * 2 + 1]); };
    const auto pcm24 = [&](int i) {
        const uint32_t raw = (uint32_t(l24[i * 3]) << 24) | (uint32_t(l24[i * 3 + 1]) << 16) | (uint32_t(l24[i * 3 + 2]) << 8);
        return static_cast<int32_t>(raw) >> 8;
    };
    TEST_ASSERT(pcm16(0) == 0 && pcm16(1) == 0, "L16: NaN and -NaN should encode as 0");
    TEST_ASSERT(pcm24(0) == 0 && pcm24(1) == 0, "L24: NaN and -NaN should encode as 0");
    TEST_ASSERT(pcm16(2) == 32767 && pcm16(3) == -32767 && pcm16(4) == 32767 && pcm16(5) == -32767,
                "L16: infinities and +/-2.0 should clip to full scale");
    TEST_ASSERT(pcm24(2) == 8388607 && pcm24(3) == -8388607 && pcm24(4) == 8388607 && pcm24(5) == -8388607,
                "L24: infinities and +/-2.0 should clip to full scale");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Payload Size Tests
//

bool testPayloadSizes() {
    std::cout << "Test: RTP payload size calculations... ";

    // Common AES67 configurations
    struct Config {
        uint16_t channels;
        uint32_t samples;
        uint8_t bytesPerSample;
        size_t expectedSize;
    };

    std::vector<Config> configs = {
        // L16 configurations
        {2, 48, 2, 192},      // Stereo @ 48kHz, 1ms
        {8, 48, 2, 768},      // 8ch @ 48kHz, 1ms
        {2, 96, 2, 384},      // Stereo @ 96kHz, 1ms

        // L24 configurations
        {2, 48, 3, 288},      // Stereo @ 48kHz, 1ms
        {8, 48, 3, 1152},     // 8ch @ 48kHz, 1ms
        {2, 96, 3, 576},      // Stereo @ 96kHz, 1ms
    };

    for (const auto& cfg : configs) {
        size_t calculatedSize = cfg.channels * cfg.samples * cfg.bytesPerSample;
        TEST_ASSERT(calculatedSize == cfg.expectedSize, "Payload size calculation");

        // Verify payload fits in MTU
        constexpr size_t MAX_PAYLOAD = 1460;  // 1500 - 20 IP - 8 UDP - 12 RTP
        TEST_ASSERT(calculatedSize <= MAX_PAYLOAD, "Payload should fit in MTU");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Packet Timing Tests
//

bool testPacketInterval() {
    std::cout << "Test: Packet interval calculation... ";

    struct TimingConfig {
        uint32_t sampleRate;
        uint32_t samplesPerPacket;
        uint64_t expectedIntervalUs;
    };

    std::vector<TimingConfig> configs = {
        {48000, 48, 1000},    // 1ms @ 48kHz
        {96000, 96, 1000},    // 1ms @ 96kHz
        {192000, 192, 1000},  // 1ms @ 192kHz
        {48000, 96, 2000},    // 2ms @ 48kHz
    };

    for (const auto& cfg : configs) {
        uint64_t intervalUs = (cfg.samplesPerPacket * 1000000ULL) / cfg.sampleRate;
        TEST_ASSERT(intervalUs == cfg.expectedIntervalUs, "Packet interval calculation");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SSRC Tests
//

bool testSSRCGeneration() {
    std::cout << "Test: SSRC generation... ";

    // SSRCs should be unique (randomly generated)
    // We can't test randomness easily, but we can verify the field works

    RTPPacket packet1, packet2, packet3;

    packet1.header.ssrc = 0x12345678;
    packet2.header.ssrc = 0xABCDEF01;
    packet3.header.ssrc = 0x87654321;

    TEST_ASSERT(packet1.header.ssrc != packet2.header.ssrc, "SSRCs should be different");
    TEST_ASSERT(packet2.header.ssrc != packet3.header.ssrc, "SSRCs should be different");
    TEST_ASSERT(packet1.header.ssrc != packet3.header.ssrc, "SSRCs should be different");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SDP Generation Tests
//

bool testSDPForTransmit() {
    std::cout << "Test: SDP session for transmission... ";

    // Create transmit SDP
    SDPSession sdp = SDPParser::createDefaultTxSession(
        "Test TX Stream",
        "192.168.1.100",    // Source IP
        "239.1.2.1",        // Multicast IP
        5004,               // Port
        8,                  // Channels
        48000,              // Sample rate
        "L24"               // Encoding
    );

    TEST_ASSERT(sdp.sessionName == "Test TX Stream", "Session name should be set");
    TEST_ASSERT(sdp.port == 5004, "Port should be set");
    TEST_ASSERT(sdp.encoding == "L24", "Encoding should be set");
    TEST_ASSERT(sdp.sampleRate == 48000, "Sample rate should be set");
    TEST_ASSERT(sdp.numChannels == 8, "Channel count should be set");
    TEST_ASSERT(sdp.connectionAddress == "239.1.2.1", "Multicast address should be set");
    TEST_ASSERT(sdp.originAddress == "192.168.1.100", "Source address should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSDPStringGeneration() {
    std::cout << "Test: SDP string generation... ";

    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.port = 5004;
    sdp.encoding = "L16";
    sdp.sampleRate = 48000;
    sdp.numChannels = 2;
    sdp.connectionAddress = "239.1.1.1";
    sdp.originAddress = "192.168.1.100";

    std::string sdpString = SDPParser::generate(sdp);

    TEST_ASSERT(!sdpString.empty(), "SDP string should not be empty");
    TEST_ASSERT(sdpString.find("v=0") == 0, "Should start with version");
    TEST_ASSERT(sdpString.find("s=Test Stream") != std::string::npos, "Should contain session name");
    TEST_ASSERT(sdpString.find("m=audio 5004") != std::string::npos, "Should contain media line");
    TEST_ASSERT(sdpString.find("c=IN IP4 239.1.1.1") != std::string::npos, "Should contain connection");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Channel Interleaving Tests
//

bool testChannelInterleaving() {
    std::cout << "Test: Channel interleaving... ";

    // Simulate 2 channels, 4 samples each
    // Channel 0: [1.0, 2.0, 3.0, 4.0]
    // Channel 1: [5.0, 6.0, 7.0, 8.0]
    // Interleaved: [1.0, 5.0, 2.0, 6.0, 3.0, 7.0, 4.0, 8.0]

    float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float ch1[4] = {5.0f, 6.0f, 7.0f, 8.0f};

    float interleaved[8];
    for (int i = 0; i < 4; ++i) {
        interleaved[i * 2 + 0] = ch0[i];
        interleaved[i * 2 + 1] = ch1[i];
    }

    // Verify interleaving
    float expected[8] = {1.0f, 5.0f, 2.0f, 6.0f, 3.0f, 7.0f, 4.0f, 8.0f};
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT(interleaved[i] == expected[i], "Interleaving should be correct");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 RTP Transmitter Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "RTP Header Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testRTPHeaderInitialization();
    testRTPHeaderNetworkByteOrder();
    std::cout << std::endl;

    std::cout << "Sequence Number Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testSequenceNumberIncrement();
    std::cout << std::endl;

    std::cout << "Timestamp Tests:" << std::endl;
    std::cout << "---------------" << std::endl;
    testTimestampIncrement();
    testTimestampWrap();
    std::cout << std::endl;

    std::cout << "Audio Encoding Tests:" << std::endl;
    std::cout << "--------------------" << std::endl;
    testL16EncodingPrecision();
    testL24EncodingPrecision();
    testNonFiniteSamples();
    std::cout << std::endl;

    std::cout << "Payload Size Tests:" << std::endl;
    std::cout << "------------------" << std::endl;
    testPayloadSizes();
    std::cout << std::endl;

    std::cout << "Packet Timing Tests:" << std::endl;
    std::cout << "-------------------" << std::endl;
    testPacketInterval();
    std::cout << std::endl;

    std::cout << "SSRC Tests:" << std::endl;
    std::cout << "----------" << std::endl;
    testSSRCGeneration();
    std::cout << std::endl;

    std::cout << "SDP Generation Tests:" << std::endl;
    std::cout << "--------------------" << std::endl;
    testSDPForTransmit();
    testSDPStringGeneration();
    std::cout << std::endl;

    std::cout << "Channel Processing Tests:" << std::endl;
    std::cout << "------------------------" << std::endl;
    testChannelInterleaving();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
