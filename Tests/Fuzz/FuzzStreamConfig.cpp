// Fuzzes the streams.json loader. A file can be edited by hand, so anything
// may be in it; every stream it loads must save and load back as a stream.

#include "../../NetworkEngine/StreamConfig.h"
#include "FuzzCheck.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto entries = AES67::StreamConfigManager::fromJSON(text);
    if (!entries) {
        return 0;
    }

    std::vector<AES67::PersistedStreamConfig> configs;
    for (const auto& entry : *entries) {
        if (entry.config) {
            configs.push_back(*entry.config);
        }
    }
    const auto saved = AES67::StreamConfigManager::fromJSON(AES67::StreamConfigManager::toJSON(configs));
    FUZZ_CHECK(saved.has_value(), "saved streams.json does not load back");
    FUZZ_CHECK(saved->size() == configs.size(), "stream count changed in round trip");
    for (const auto& entry : *saved) {
        FUZZ_CHECK(entry.config.has_value(), "a saved stream does not load back");
    }
    return 0;
}
