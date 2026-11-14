// Fuzzes the JSON parser behind streams.json. Whatever it accepts must dump
// to text that parses back to the same thing, since the driver rewrites the
// file from what it parsed.

#include "../../NetworkEngine/Json.h"
#include "FuzzCheck.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto parsed = AES67::JsonValue::parse(text);
    if (!parsed) {
        return 0;
    }
    const std::string dumped = parsed->dump();
    const auto reparsed = AES67::JsonValue::parse(dumped);
    FUZZ_CHECK(reparsed.has_value(), "dumped JSON does not parse back");
    FUZZ_CHECK(reparsed->dump() == dumped, "JSON changed in round trip");
    return 0;
}
