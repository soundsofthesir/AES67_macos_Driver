//
// TestCodecBitExact.cpp
// AES67 macOS Driver
// L16/L24 decode -> encode must be the identity on every wire value (#136)
//

#include "../NetworkEngine/RTP/SimpleRTP.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace AES67::RTP;

namespace {

int failures = 0;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            std::fprintf(stderr, "FAIL: ");       \
            std::fprintf(stderr, __VA_ARGS__);    \
            std::fprintf(stderr, "\n");           \
            ++failures;                           \
        }                                         \
    } while (0)

void putL16(int32_t v, uint8_t* p) {
    const uint16_t u = static_cast<uint16_t>(v);
    p[0] = static_cast<uint8_t>(u >> 8);
    p[1] = static_cast<uint8_t>(u & 0xFF);
}

void putL24(int32_t v, uint8_t* p) {
    const uint32_t u = static_cast<uint32_t>(v) & 0xFFFFFF;
    p[0] = static_cast<uint8_t>((u >> 16) & 0xFF);
    p[1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>(u & 0xFF);
}

int32_t encodeL16(float x) {
    uint8_t b[2];
    L16Codec::encode(&x, 1, b);
    return static_cast<int16_t>(static_cast<uint16_t>((b[0] << 8) | b[1]));
}

int32_t encodeL24(float x) {
    uint8_t b[3];
    L24Codec::encode(&x, 1, b);
    int32_t v = (b[0] << 16) | (b[1] << 8) | b[2];
    if (v & 0x800000) v -= 0x1000000;
    return v;
}

// Every 16-bit two's complement value survives decode -> encode unchanged
void testL16Exhaustive() {
    constexpr int kCount = 65536;
    std::vector<uint8_t> wire(kCount * 2), back(kCount * 2);
    std::vector<float> decoded(kCount);
    for (int v = -32768; v <= 32767; ++v) putL16(v, &wire[static_cast<size_t>(v + 32768) * 2]);

    L16Codec::decode(wire.data(), wire.size(), decoded.data());
    L16Codec::encode(decoded.data(), decoded.size(), back.data());

    int bad = 0;
    int firstBad = 0;
    for (int v = -32768; v <= 32767; ++v) {
        const size_t i = static_cast<size_t>(v + 32768) * 2;
        if (wire[i] != back[i] || wire[i + 1] != back[i + 1]) {
            if (bad++ == 0) firstBad = v;
        }
    }
    CHECK(bad == 0, "L16 decode->encode changed %d of 65536 values (first: %d)", bad, firstBad);
}

// Every 24-bit two's complement value survives decode -> encode unchanged
void testL24Exhaustive() {
    constexpr int32_t kLo = -8388608, kHi = 8388607;
    constexpr size_t kChunk = 1u << 16;
    std::vector<uint8_t> wire(kChunk * 3), back(kChunk * 3);
    std::vector<float> decoded(kChunk);

    int64_t bad = 0;
    int32_t firstBad = 0;
    for (int64_t base = kLo; base <= kHi; base += static_cast<int64_t>(kChunk)) {
        for (size_t k = 0; k < kChunk; ++k) putL24(static_cast<int32_t>(base + static_cast<int64_t>(k)), &wire[k * 3]);
        L24Codec::decode(wire.data(), wire.size(), decoded.data());
        L24Codec::encode(decoded.data(), decoded.size(), back.data());
        for (size_t k = 0; k < kChunk; ++k) {
            if (wire[k * 3] != back[k * 3] || wire[k * 3 + 1] != back[k * 3 + 1] || wire[k * 3 + 2] != back[k * 3 + 2]) {
                if (bad++ == 0) firstBad = static_cast<int32_t>(base + static_cast<int64_t>(k));
            }
        }
    }
    CHECK(bad == 0, "L24 decode->encode changed %lld of 16777216 values (first: %d)", static_cast<long long>(bad),
          firstBad);
}

void testEdges() {
    // Silence stays silence
    CHECK(encodeL16(0.0f) == 0, "L16: 0.0 should encode to 0");
    CHECK(encodeL24(0.0f) == 0, "L24: 0.0 should encode to 0");

    // Full scale: -1.0 is exactly representable, +1.0 clamps to the largest value
    CHECK(encodeL16(-1.0f) == -32768, "L16: -1.0 should be -32768");
    CHECK(encodeL16(1.0f) == 32767, "L16: +1.0 should clamp to 32767");
    CHECK(encodeL24(-1.0f) == -8388608, "L24: -1.0 should be -8388608");
    CHECK(encodeL24(1.0f) == 8388607, "L24: +1.0 should clamp to 8388607");

    // Out-of-range and non-finite input
    CHECK(encodeL16(2.0f) == 32767 && encodeL16(-2.0f) == -32768, "L16: out-of-range input should clip");
    CHECK(encodeL24(2.0f) == 8388607 && encodeL24(-2.0f) == -8388608, "L24: out-of-range input should clip");
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(encodeL16(inf) == 32767 && encodeL16(-inf) == -32768, "L16: +-inf should clip");
    CHECK(encodeL24(inf) == 8388607 && encodeL24(-inf) == -8388608, "L24: +-inf should clip");
    CHECK(encodeL16(std::nanf("")) == 0 && encodeL24(std::nanf("")) == 0, "NaN should be silence (#60)");

    // One LSB either side of zero (the issue's "-1 becomes 0")
    CHECK(encodeL16(1.0f / 32768.0f) == 1 && encodeL16(-1.0f / 32768.0f) == -1, "L16: +-1 LSB should survive");
    CHECK(encodeL24(1.0f / 8388608.0f) == 1 && encodeL24(-1.0f / 8388608.0f) == -1, "L24: +-1 LSB should survive");
}

// Round to nearest, symmetric about zero: no deadband, no inward bias
void testRounding() {
    const float lsb16 = 1.0f / 32768.0f;
    CHECK(encodeL16(0.4f * lsb16) == 0, "L16: 0.4 LSB rounds to 0");
    CHECK(encodeL16(0.6f * lsb16) == 1, "L16: 0.6 LSB rounds to 1");
    CHECK(encodeL16(-0.6f * lsb16) == -1, "L16: -0.6 LSB rounds to -1");
    CHECK(encodeL16(1.4f * lsb16) == 1 && encodeL16(1.6f * lsb16) == 2, "L16: 1.4/1.6 LSB round to 1/2");
    CHECK(encodeL16(-1.6f * lsb16) == -2, "L16: -1.6 LSB rounds to -2");

    const float lsb24 = 1.0f / 8388608.0f;
    CHECK(encodeL24(0.4f * lsb24) == 0, "L24: 0.4 LSB rounds to 0");
    CHECK(encodeL24(0.6f * lsb24) == 1, "L24: 0.6 LSB rounds to 1");
    CHECK(encodeL24(-0.6f * lsb24) == -1, "L24: -0.6 LSB rounds to -1");

    // Exact ties go to the even neighbour (lrintf, default rounding mode)
    CHECK(encodeL16(0.5f * lsb16) == 0 && encodeL16(1.5f * lsb16) == 2 && encodeL16(2.5f * lsb16) == 2,
          "L16: ties should round to even");
    CHECK(encodeL24(0.5f * lsb24) == 0 && encodeL24(1.5f * lsb24) == 2 && encodeL24(2.5f * lsb24) == 2,
          "L24: ties should round to even");
}

} // namespace

int main() {
    std::printf("Test: L16/L24 decode -> encode is bit-exact (#136)... \n");
    testL16Exhaustive();
    testL24Exhaustive();
    testEdges();
    testRounding();
    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}