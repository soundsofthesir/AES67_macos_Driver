//
// PortableUtil.h
// AES67 macOS Driver
// Small helpers that avoid undefined behaviour and portability traps (#124)
//

#pragma once

#include <cstdint>
#include <ctime>
#include <random>

namespace aes67::util {

/// A uniformly distributed byte.
///
/// std::uniform_int_distribution<IntType> only permits short, int, long,
/// long long and their unsigned forms. uint8_t is a char type, so
/// uniform_int_distribution<uint8_t> is undefined behaviour. Draw an
/// unsigned over [0, 255] instead and narrow explicitly; conversion to an
/// unsigned type is well-defined and loses nothing here.
template <class Engine>
inline uint8_t randomByte(Engine& engine) {
    std::uniform_int_distribution<unsigned> distribution(0u, 255u);
    return static_cast<uint8_t>(distribution(engine));
}

/// Thread-safe replacement for std::localtime.
///
/// std::localtime returns a pointer to one shared static std::tm, which
/// concurrent callers overwrite. This writes into a buffer the caller owns.
/// Returns false if the conversion fails.
inline bool safeLocalTime(std::time_t time, std::tm& out) {
#if defined(_WIN32)
    return localtime_s(&out, &time) == 0;
#else
    return localtime_r(&time, &out) != nullptr;
#endif
}

} // namespace aes67::util