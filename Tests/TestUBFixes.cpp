/// @file TestUBFixes.cpp
/// @brief Regression tests for issue #124 (undefined-behaviour and portability fixes).

#include "../Shared/PortableUtil.h"
#include "../Shared/Types.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

void testRandomByteCoversRange() {
    std::cout << "randomByte: covers 0-255 roughly uniformly" << std::endl;
    std::mt19937 eng(12345);
    std::array<int, 256> histogram{};
    for (int i = 0; i < 256 * 400; ++i) ++histogram[aes67::util::randomByte(eng)];
    for (size_t v = 0; v < histogram.size(); ++v) {
        CHECK(histogram[v] > 250 && histogram[v] < 550,
              "value " << v << " drawn " << histogram[v] << " times (expected about 400)");
    }
}

void testStreamIdGenerateIsValidV4() {
    std::cout << "StreamID::generate: valid, unique UUID v4s" << std::endl;
    std::set<std::string> seen;
    for (int i = 0; i < 4096; ++i) {
        const StreamID id = StreamID::generate();
        const std::string text = id.toString();
        CHECK(!id.isNull(), "a generated ID is never null");
        CHECK(text.size() == 36, "canonical 8-4-4-4-12 form: " << text);
        CHECK(text.size() == 36 && text[14] == '4', "version nibble is 4: " << text);
        CHECK(text.size() == 36 && std::string("89ab").find(text[19]) != std::string::npos,
              "variant nibble is 8, 9, a or b: " << text);
        CHECK(StreamID(text) == id, "toString then parse round-trips: " << text);
        seen.insert(text);
        if (checksFailed) break;
    }
    CHECK(seen.size() == 4096, "4096 generated IDs should all differ, got " << seen.size());
}

void testSafeLocalTimeIsThreadSafe() {
    std::cout << "safeLocalTime: concurrent calls give correct, independent results" << std::endl;
    constexpr int kThreads = 8;
    constexpr int kIterations = 20000;
    std::vector<std::time_t> times(kThreads);
    std::vector<std::tm> expected(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        times[i] = 1700000000 + static_cast<std::time_t>(i) * 40000000;
        CHECK(aes67::util::safeLocalTime(times[i], expected[i]), "conversion should succeed");
    }

    std::atomic<int> mismatches{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kThreads; ++i) {
        pool.emplace_back([&, i] {
            for (int n = 0; n < kIterations; ++n) {
                std::tm got{};
                if (!aes67::util::safeLocalTime(times[i], got) ||
                    got.tm_year != expected[i].tm_year || got.tm_mon != expected[i].tm_mon ||
                    got.tm_mday != expected[i].tm_mday || got.tm_hour != expected[i].tm_hour ||
                    got.tm_min != expected[i].tm_min || got.tm_sec != expected[i].tm_sec) {
                    ++mismatches;
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    CHECK(mismatches.load() == 0, "no thread should ever see another thread's time (" << mismatches.load() << ")");
}

} // namespace

int main() {
    testRandomByteCoversRange();
    testStreamIdGenerateIsValidV4();
    testSafeLocalTimeIsThreadSafe();

    std::cout << "\nUB fixes: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}