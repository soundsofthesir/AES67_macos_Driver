/// @file TestUBFixes.cpp
/// @brief Regression tests for issue #124 (undefined-behaviour and portability fixes).
///
/// Run this under ThreadSanitizer as well (-fsanitize=thread): the data race in
/// std::localtime can pass every value check, and only the sanitizer reports it.

#include "../Shared/NonBlockingLogger.h"
#include "../Shared/PortableUtil.h"
#include "../Shared/RingBuffer.hpp"
#include "../Shared/Types.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
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

// The ring buffer holds std::atomic indices, so it can be neither copied nor
// moved; the move operations are deleted explicitly rather than "defaulted"
static_assert(!std::is_copy_constructible<SPSCRingBuffer<float>>::value, "ring buffer must not be copyable");
static_assert(!std::is_copy_assignable<SPSCRingBuffer<float>>::value, "ring buffer must not be copy-assignable");
static_assert(!std::is_move_constructible<SPSCRingBuffer<float>>::value, "ring buffer must not be movable");
static_assert(!std::is_move_assignable<SPSCRingBuffer<float>>::value, "ring buffer must not be move-assignable");

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

void testLogMacrosAreSingleStatements() {
    std::cout << "LOG_* macros: expand to one statement (no dangling else)" << std::endl;
    CHECK(AES67::g_logger == nullptr, "these checks need the global logger unset");

    // With the old "if (g_logger) ..." macros the else below bound to the
    // macro's own if, so it ran when the logger was null and the outer
    // condition was true, and was skipped when the outer condition was false
    bool elseTaken = false;
    const bool conditionFalse = false;
    if (conditionFalse) LOG_INFO("never logged"); else elseTaken = true;
    CHECK(elseTaken, "else must run when the outer condition is false");

    elseTaken = false;
    const bool conditionTrue = true;
    if (conditionTrue) LOG_ERROR("no logger is set"); else elseTaken = true;
    CHECK(!elseTaken, "else must not run when the outer condition is true");
}

void testLoggerWritesWellFormedTimestampsUnderConcurrency() {
    std::cout << "NonBlockingLogger: concurrent logging writes well-formed timestamps" << std::endl;
    const std::string path = (std::filesystem::temp_directory_path() / "aes67_ubfixes_logger.log").string();
    std::filesystem::remove(path);

    constexpr int kThreads = 4;
    constexpr int kMessages = 500;
    {
        NonBlockingLogger logger(path);
        std::vector<std::thread> pool;
        for (int t = 0; t < kThreads; ++t) {
            pool.emplace_back([&logger, t] {
                for (int n = 0; n < kMessages; ++n) {
                    logger.info("thread " + std::to_string(t) + " message " + std::to_string(n));
                }
            });
        }
        for (auto& thread : pool) thread.join();
    }  // the destructor stops the writer thread and flushes

    std::ifstream file(path);
    std::string line;
    int lines = 0;
    int malformed = 0;
    while (std::getline(file, line)) {
        ++lines;
        // [YYYY-MM-DD HH:MM:SS.mmm] [INFO] ...
        const bool ok = line.size() > 26 && line[0] == '[' && line[5] == '-' && line[8] == '-' &&
                        line[11] == ' ' && line[14] == ':' && line[17] == ':' && line[20] == '.' &&
                        line[24] == ']';
        if (!ok) ++malformed;
    }
    CHECK(lines == kThreads * kMessages, "every message should be written (found " << lines << ")");
    CHECK(malformed == 0, "every line should start with a full timestamp (" << malformed << " malformed)");
    std::filesystem::remove(path);
}

} // namespace

int main() {
    testRandomByteCoversRange();
    testStreamIdGenerateIsValidV4();
    testSafeLocalTimeIsThreadSafe();
    testLogMacrosAreSingleStatements();
    testLoggerWritesWellFormedTimestampsUnderConcurrency();

    std::cout << "\nUB fixes: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}