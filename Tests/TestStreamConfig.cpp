/// @file TestStreamConfig.cpp
/// @brief The JSON reader/writer and streams.json save/load (#65, #66, #67).

#include "../NetworkEngine/Json.h"
#include "../NetworkEngine/StreamConfig.h"
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
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

std::string testDir() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp ? tmp : "/tmp/") + "aes67_streamconfig_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream(path) << contents;
}

PersistedStreamConfig makeConfig(const std::string& name) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = "239.69.99.30";
    sdp.port = 55060;
    sdp.numChannels = 2;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = 97;
    sdp.ptime = 1;
    sdp.framecount = 48;
    sdp.ttl = 0;

    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = name;
    mapping.streamChannelCount = 2;
    mapping.deviceChannelCount = 2;
    return StreamConfigManager::createConfig(sdp, mapping, "test");
}

/// A whole streams.json holding these entries (each a JSON object's text).
std::string fileWithEntries(const std::vector<std::string>& entries) {
    std::string json = "{\"version\": \"1.0\", \"streams\": [";
    for (size_t i = 0; i < entries.size(); ++i) {
        json += (i ? ", " : "") + entries[i];
    }
    return json + "]}";
}

std::string entryText(const PersistedStreamConfig& config) {
    return StreamConfigManager::configToJSON(config).dump();
}

/// `text` with the first occurrence of `from` replaced by `to`.
std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------

void testJsonParsesDocument() {
    std::cout << "JSON: parses objects, arrays and scalars" << std::endl;
    const auto doc = JsonValue::parse(R"( {"a": [1, -2, 3.5e1, true, false, null], "b": {"c": "d"}, "a2": {}} )");
    CHECK(doc && doc->asObject(), "the document should parse to an object");
    if (!doc) return;
    const JsonValue* a = doc->find("a");
    CHECK(a && a->asArray() && a->asArray()->size() == 6, "\"a\" should be a 6-item array");
    if (!a || !a->asArray() || a->asArray()->size() != 6) return;
    const auto& items = *a->asArray();
    CHECK(items[0].asUInt64() == 1u, "1 reads as unsigned");
    CHECK(items[1].asInt64() == -2 && !items[1].asUInt64(), "-2 reads as signed only");
    CHECK(items[2].asDouble() == 35.0 && !items[2].asInt64(), "3.5e1 reads as a double, not an integer");
    CHECK(items[3].asBool() == true && items[4].asBool() == false && items[5].isNull(), "literals");
    const JsonValue* b = doc->find("b");
    CHECK(b && b->find("c") && *b->find("c")->asString() == "d", "nested object member");
    CHECK(doc->find("missing") == nullptr, "a missing key finds nothing");
    CHECK(items[0].asString() == nullptr && !items[3].asUInt64(), "the wrong type reads as nothing");
}

void testJsonRejectsMalformed() {
    std::cout << "JSON: rejects malformed input without throwing" << std::endl;
    const char* bad[] = {
        "", "{", "[1, 2", "[1,]", "{\"a\": 1,}", "{\"a\" 1}", "{a: 1}", "'x'", "\"unterminated",
        "01", "1.", ".5", "-", "1e", "+1", "tru", "nul", "[1] 2", "\"\\x\"", "\"\\u12\"",
        "\"\\ud800\\uZZZZ\"", "{\"ptime\": .}",
    };
    for (const char* text : bad) {
        std::string error;
        CHECK(!JsonValue::parse(text, &error), "should reject: " << text);
        CHECK(!error.empty(), "a rejection should say why: " << text);
    }

    std::string deep(JsonValue::kMaxDepth + 2, '[');
    deep += std::string(JsonValue::kMaxDepth + 2, ']');
    CHECK(!JsonValue::parse(deep), "nesting deeper than kMaxDepth should be refused");
    std::string ok(JsonValue::kMaxDepth, '[');
    ok += std::string(JsonValue::kMaxDepth, ']');
    CHECK(JsonValue::parse(ok).has_value(), "nesting up to kMaxDepth should parse");
}

void testJsonStrings() {
    std::cout << "JSON: string escapes and round trips" << std::endl;
    const auto doc = JsonValue::parse(R"(["q\"b\\s\/n\nt\tu\u00e9", "\ud83d\ude00"])");
    CHECK(doc.has_value(), "escaped strings should parse");
    if (!doc) return;
    CHECK(*(*doc->asArray())[0].asString() == "q\"b\\s/n\nt\tu\xC3\xA9", "escapes decode, \\u00e9 to UTF-8");
    CHECK(*(*doc->asArray())[1].asString() == "\xF0\x9F\x98\x80", "a surrogate pair decodes to one 4-byte character");

    std::string everyByte;
    for (int c = 0; c < 256; ++c) everyByte += static_cast<char>(c);
    const std::string text = JsonValue::string(everyByte).dump();
    const auto back = JsonValue::parse(text);
    CHECK(back && back->asString() && *back->asString() == everyByte, "every byte value survives dump and parse");
    for (unsigned char c : text) {
        CHECK(c >= 0x20, "dump escapes control characters (found byte " << int(c) << ")");
        if (c < 0x20) break;
    }

    // Half a surrogate pair is one bad character, not a bad document
    const std::string fffd = "\xEF\xBF\xBD";
    const struct { const char* json; std::string expected; } lone[] = {
        {R"("a\ud800")", "a" + fffd},
        {R"("\udc00b")", fffd + "b"},
        {R"("\ud800A")", fffd + "A"},
        {R"("\ud800𐀀")", fffd + "\xF0\x90\x80\x80"},
    };
    for (const auto& c : lone) {
        const auto value = JsonValue::parse(c.json);
        CHECK(value && value->asString() && *value->asString() == c.expected,
              c.json << " should read with U+FFFD for the lone surrogate");
    }

    // Older files were written with raw control characters in strings
    const auto raw = JsonValue::parse("\"a\x01" "b\"");
    CHECK(raw && *raw->asString() == "a\x01" "b", "raw control characters are accepted");
}

void testJsonNumbers() {
    std::cout << "JSON: number ranges and formatting" << std::endl;
    CHECK(JsonValue::parse("18446744073709551615")->asUInt64() == UINT64_MAX, "UINT64_MAX is exact");
    CHECK(!JsonValue::parse("18446744073709551616")->asUInt64(), "UINT64_MAX + 1 doesn't fit");
    CHECK(JsonValue::parse("-9223372036854775808")->asInt64() == INT64_MIN, "INT64_MIN is exact");
    CHECK(!JsonValue::parse("1e999")->asDouble(), "an infinite double reads as nothing");
    CHECK(JsonValue::parse("0.125")->asDouble() == 0.125, "0.125");

    CHECK(JsonValue::number(0.125).dump() == "0.125", "0.125 writes as 0.125");
    CHECK(JsonValue::number(48000.0).dump() == "48000", "48000.0 writes as 48000");
    const double third = 1.0 / 3.0;
    CHECK(JsonValue::parse(JsonValue::number(third).dump())->asDouble() == third, "a double round-trips exactly");
    CHECK(JsonValue::number(std::nan("")).isNull(), "NaN has no JSON form");
    CHECK(JsonValue::number(UINT64_MAX).dump() == "18446744073709551615", "uint64 writes exactly");

    // The host process's locale must not change how numbers read or write
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        CHECK(JsonValue::number(0.125).dump() == "0.125", "a German locale still writes 0.125");
        CHECK(JsonValue::parse("0.125")->asDouble() == 0.125, "a German locale still reads 0.125");
        std::setlocale(LC_NUMERIC, previous.c_str());
    }
}

void testJsonDumpLayout() {
    std::cout << "JSON: dump layout" << std::endl;
    const JsonValue doc = JsonValue::object()
        .add("list", JsonValue::array().push(JsonValue::number(1)).push(JsonValue::number(2)))
        .add("empty", JsonValue::array())
        .add("obj", JsonValue::object().add("k", JsonValue::string("v")));
    CHECK(doc.dump() == "{\n  \"list\": [1, 2],\n  \"empty\": [],\n  \"obj\": {\n    \"k\": \"v\"\n  }\n}",
          "pretty print: flat arrays on one line\n" << doc.dump());
    const auto back = JsonValue::parse(doc.dump());
    CHECK(back && back->dump() == doc.dump(), "dump of a parse of a dump is stable");
}

// ---------------------------------------------------------------------------
// streams.json
// ---------------------------------------------------------------------------

void testAwkwardNamesRoundTrip() {
    std::cout << "#67: brackets, braces, quotes and backslashes in names survive save and load" << std::endl;
    const std::string path = testDir() + "/streams.json";
    StreamConfigManager manager;
    manager.setConfigPath(path);

    const std::vector<std::string> names = {"Studio [A", "Studio }A", "My \"A\" \\ B", "Back\\slash", "tab\tand\x01" "ctl"};
    std::vector<PersistedStreamConfig> configs;
    for (const auto& name : names) configs.push_back(makeConfig(name));

    for (int round = 1; round <= 3; ++round) {
        CHECK(manager.saveConfig(configs), "round " << round << " should save");
        const LoadedStreamConfig loaded = manager.loadConfig();
        CHECK(loaded.status == LoadedStreamConfig::Status::Loaded, "round " << round << " should load");
        CHECK(loaded.entries.size() == names.size(), "round " << round << ": every entry should come back");
        configs.clear();
        for (size_t i = 0; i < loaded.entries.size() && i < names.size(); ++i) {
            const auto& config = loaded.entries[i].config;
            CHECK(config && config->sdp.sessionName == names[i],
                  "round " << round << ": name " << i << " should be unchanged, got <"
                           << (config ? config->sdp.sessionName : "unreadable") << ">");
            if (config) configs.push_back(*config);
        }
    }
    CHECK(std::filesystem::exists(path) && !std::filesystem::exists(path + ".bak"), "nothing should be treated as corrupt");
}

void testOldFormatStillLoads() {
    std::cout << "Files written by the old writer still load" << std::endl;
    const std::string path = testDir() + "/streams.json";
    writeFile(path, R"({
  "version": "1.0",
  "streams": [
    {
      "enabled": true,
      "description": "",
      "createdTimestamp": 1759400000,
      "modifiedTimestamp": 1759400000,
      "jitterBufferDepth": 0,
      "networkInterface": "en0",
      "sdp": {
        "sessionName": "Riedel 1",
        "sessionInfo": "",
        "sessionID": 1311768467294899695,
        "sessionVersion": 1,
        "originUsername": "-",
        "originAddress": "192.168.1.20",
        "connectionAddress": "239.69.1.1",
        "ttl": 32,
        "port": 5004,
        "payloadType": 98,
        "encoding": "L24",
        "sampleRate": 48000,
        "numChannels": 8,
        "ptime": 0.125,
        "framecount": 6,
        "sourceAddress": "192.168.1.20",
        "ptpDomain": 0,
        "ptpMasterMAC": "",
        "mediaClockType": "direct",
        "direction": "recvonly"
      },
      "mapping": {
        "streamID": "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0",
        "streamName": "Riedel 1",
        "streamChannelCount": 8,
        "streamChannelOffset": 0,
        "deviceChannelStart": 16,
        "deviceChannelCount": 8,
        "channelMap": []
      }
    }
  ]
})");
    StreamConfigManager manager;
    manager.setConfigPath(path);
    const LoadedStreamConfig loaded = manager.loadConfig();
    CHECK(loaded.status == LoadedStreamConfig::Status::Loaded && loaded.entries.size() == 1, "the file should load");
    if (loaded.entries.empty() || !loaded.entries[0].config) {
        CHECK(false, "the entry should be readable");
        return;
    }
    const auto& config = *loaded.entries[0].config;
    CHECK(config.sdp.sessionID == 1311768467294899695ull, "a 64-bit session ID is exact");
    CHECK(config.sdp.ptime == 0.125 && config.sdp.framecount == 6, "packet time");
    CHECK(config.mapping.deviceChannelStart == 16 && config.networkInterface == "en0", "mapping and interface");
    CHECK(config.mapping.streamID.toString() == StreamID("0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0").toString(),
          "stream ID");
}

void testBadValuesOnlyLoseTheirEntry() {
    std::cout << "#66: a bad value makes only its own entry unreadable, and nothing throws" << std::endl;
    const std::string dir = testDir();
    const std::string good = entryText(makeConfig("Good"));
    const std::string base = entryText(makeConfig("Bad"));

    struct Case { const char* what; std::string entry; };
    const std::vector<Case> cases = {
        {"sessionID 25 digits", replaced(base, "\"sessionID\": 0", "\"sessionID\": 1234567890123456789012345")},
        {"channelMap [99999999999]", replaced(base, "\"channelMap\": []", "\"channelMap\": [99999999999]")},
        {"ptpDomain 99999999999", replaced(base, "\"ptpDomain\": 0", "\"ptpDomain\": 99999999999")},
        {"port 70000", replaced(base, "\"port\": 55060", "\"port\": 70000")},
        {"ttl 256", replaced(base, "\"ttl\": 0", "\"ttl\": 256")},
        {"negative port", replaced(base, "\"port\": 55060", "\"port\": -1")},
        {"fractional channel count", replaced(base, "\"numChannels\": 2", "\"numChannels\": 2.5")},
        {"port as a string", replaced(base, "\"port\": 55060", "\"port\": \"55060\"")},
        {"sdp as an array", replaced(base, "\"sdp\": {", "\"sdp\": [], \"x\": {")},
        {"non-hex stream ID", replaced(base, "\"streamID\": \"", "\"streamID\": \"zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz\", \"x\": \"")},
        {"an entry that isn't an object", "42"},
        {"ptime 1e300 (#75)", replaced(base, "\"ptime\": 1,", "\"ptime\": 1e300,")},
        {"ptime 0 (#75)", replaced(base, "\"ptime\": 1,", "\"ptime\": 0,")},
        {"framecount 4000000000 (#75)", replaced(base, "\"framecount\": 48", "\"framecount\": 4000000000")},
    };

    for (const auto& c : cases) {
        CHECK(c.entry != base, c.what << ": the test should have changed the entry");
        const std::string path = dir + "/bad.json";
        writeFile(path, fileWithEntries({good, c.entry}));
        StreamConfigManager manager;
        manager.setConfigPath(path);
        LoadedStreamConfig loaded;
        try {
            loaded = manager.loadConfig();
        } catch (const std::exception& e) {
            CHECK(false, c.what << ": threw " << e.what());
            continue;
        }
        CHECK(loaded.status == LoadedStreamConfig::Status::Loaded && loaded.entries.size() == 2,
              c.what << ": the file should still load with both entries");
        if (loaded.entries.size() != 2) continue;
        CHECK(loaded.entries[0].config && loaded.entries[0].config->sdp.sessionName == "Good",
              c.what << ": the good entry should be readable");
        CHECK(!loaded.entries[1].config, c.what << ": the bad entry should be unreadable");
        CHECK(loaded.entries[1].raw.dump() == JsonValue::parse(c.entry)->dump(),
              c.what << ": the bad entry should be kept exactly");
    }
}

void testCorruptFileIsBackedUpAndKept() {
    std::cout << "A corrupt file is copied to .bak and left in place" << std::endl;
    const std::string path = testDir() + "/streams.json";
    const std::string text = R"({"version": "1.0", "streams": [{"sdp": {"ptime": .}}]})";
    writeFile(path, text);
    StreamConfigManager manager;
    manager.setConfigPath(path);
    const LoadedStreamConfig loaded = manager.loadConfig();
    CHECK(loaded.status == LoadedStreamConfig::Status::Corrupt, "\"ptime\": . is not JSON");
    CHECK(readFile(path) == text, "the original file should be left alone");
    CHECK(readFile(path + ".bak") == text, "a copy should be in .bak");

    writeFile(path, R"({"version": "1.0"})");
    CHECK(manager.loadConfig().status == LoadedStreamConfig::Status::Corrupt, "no streams array is corrupt");
    std::filesystem::remove(path);
    CHECK(manager.loadConfig().status == LoadedStreamConfig::Status::Missing, "no file is missing, not corrupt");
}

void testKeptEntriesAreWrittenBack() {
    std::cout << "#64: saveConfig writes kept entries back unchanged" << std::endl;
    const std::string path = testDir() + "/streams.json";
    StreamConfigManager manager;
    manager.setConfigPath(path);
    PersistedStreamConfig disabled = makeConfig("Disabled");
    disabled.enabled = false;
    const JsonValue kept[] = {
        StreamConfigManager::configToJSON(disabled),
        *JsonValue::parse(R"({"someFutureField": [1, {"x": "y"}], "enabled": false})"),
    };
    CHECK(manager.saveConfig({makeConfig("Running")}, {kept[0], kept[1]}), "should save");
    const LoadedStreamConfig loaded = manager.loadConfig();
    CHECK(loaded.entries.size() == 3, "all three entries should be in the file");
    if (loaded.entries.size() != 3) return;
    CHECK(loaded.entries[0].config && loaded.entries[0].config->sdp.sessionName == "Running", "the running stream first");
    CHECK(loaded.entries[1].config && !loaded.entries[1].config->enabled, "the disabled stream stays disabled");
    CHECK(loaded.entries[2].raw.dump() == kept[1].dump(), "an entry we can't read is kept exactly");
}

void testSaveIsAtomic() {
    std::cout << "#65: a failed save leaves the old file intact" << std::endl;
    const std::string dir = testDir();
    const std::string path = dir + "/streams.json";
    StreamConfigManager manager;
    manager.setConfigPath(path);
    std::vector<PersistedStreamConfig> configs = {makeConfig("A"), makeConfig("B")};
    CHECK(manager.saveConfig(configs), "the first save should work");
    const std::string before = readFile(path);
    CHECK(before.size() > 200, "the file should be bigger than the write limit below");

    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "the file should be readable by everyone (0644)");

    // A full disk, simulated: writes past 200 bytes fail (SIGXFSZ ignored)
    configs.push_back(makeConfig("C"));
    const auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
    rlimit previousLimit{};
    getrlimit(RLIMIT_FSIZE, &previousLimit);
    rlimit limit = previousLimit;
    limit.rlim_cur = 200;
    setrlimit(RLIMIT_FSIZE, &limit);
    const bool saved = manager.saveConfig(configs);
    setrlimit(RLIMIT_FSIZE, &previousLimit);
    std::signal(SIGXFSZ, previousHandler);

    CHECK(!saved, "saveConfig should report the failed write");
    CHECK(readFile(path) == before, "the old file should be unchanged");
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++files;
    }
    CHECK(files == 1, "no temporary file should be left behind (found " << files << " files)");
    CHECK(manager.loadConfig().entries.size() == 2, "the old file should still load");
}

void testSaveAfterCorruptLoadKeepsTheBackup() {
    std::cout << "Saving over a corrupt file leaves the copy in .bak" << std::endl;
    const std::string path = testDir() + "/streams.json";
    const std::string corrupt = "{\"streams\": [{\"enabled\": tru";
    writeFile(path, corrupt);
    StreamConfigManager manager;
    manager.setConfigPath(path);
    CHECK(manager.loadConfig().status == LoadedStreamConfig::Status::Corrupt, "the file is corrupt");
    CHECK(manager.saveConfig({makeConfig("Fresh")}), "a save should replace it");
    CHECK(manager.loadConfig().entries.size() == 1, "the new file should load");
    CHECK(readFile(path + ".bak") == corrupt, "the corrupt original should still be in .bak");
}

void testSaveWritesThroughSymlink() {
    std::cout << "Saving through a symlinked streams.json keeps the link" << std::endl;
    const std::string dir = testDir();
    const std::string realPath = dir + "/real.json";
    const std::string linkPath = dir + "/streams.json";
    writeFile(realPath, fileWithEntries({}));
    std::filesystem::create_symlink(realPath, linkPath);

    StreamConfigManager manager;
    manager.setConfigPath(linkPath);
    CHECK(manager.saveConfig({makeConfig("Linked")}), "the save should work");
    CHECK(std::filesystem::is_symlink(linkPath), "streams.json should still be a symlink");
    CHECK(readFile(realPath).find("Linked") != std::string::npos, "the link's target should hold the save");

    std::filesystem::remove(realPath);
    CHECK(!manager.saveConfig({makeConfig("Dangling")}), "a broken symlink should fail the save, not replace the link");
    CHECK(std::filesystem::is_symlink(linkPath) && !std::filesystem::exists(realPath), "the broken link should be left alone");
}

void testByteOrderMarkIsIgnored() {
    std::cout << "A UTF-8 byte order mark at the start of the file is ignored" << std::endl;
    const std::string path = testDir() + "/streams.json";
    writeFile(path, "\xEF\xBB\xBF" + fileWithEntries({entryText(makeConfig("BOM"))}));
    StreamConfigManager manager;
    manager.setConfigPath(path);
    const LoadedStreamConfig loaded = manager.loadConfig();
    CHECK(loaded.status == LoadedStreamConfig::Status::Loaded && loaded.entries.size() == 1 && loaded.entries[0].config,
          "the file should load");
}

void testUnreadableFileIsNotMissing() {
    std::cout << "A config file that exists but can't be read is not reported missing" << std::endl;
    const std::string path = testDir() + "/streams.json";
    writeFile(path, fileWithEntries({entryText(makeConfig("Locked"))}));
    chmod(path.c_str(), 0000);
    StreamConfigManager manager;
    manager.setConfigPath(path);
    const auto status = manager.loadConfig().status;
    chmod(path.c_str(), 0644);
    if (geteuid() == 0) {
        std::cout << "  (skipped: root can read any file)" << std::endl;
        return;
    }
    CHECK(status == LoadedStreamConfig::Status::Corrupt, "an unreadable file should not count as missing");
}

void testSaveKeepsFilePermissions() {
    std::cout << "Saving keeps the file's permissions; a new file gets 0644" << std::endl;
    const std::string path = testDir() + "/streams.json";
    StreamConfigManager manager;
    manager.setConfigPath(path);
    CHECK(manager.saveConfig({makeConfig("New")}), "the first save should work");
    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "a new file should be 0644");
    chmod(path.c_str(), 0640);
    CHECK(manager.saveConfig({makeConfig("Again")}), "the second save should work");
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640, "a 0640 file should stay 0640");
}

void testConfigPathFromEnvironmentIsAuthoritative() {
    std::cout << "AES67_CONFIG_PATH is used even before the file exists" << std::endl;
    const std::string path = testDir() + "/not-yet.json";
    setenv("AES67_CONFIG_PATH", path.c_str(), 1);
    StreamConfigManager manager;
    CHECK(manager.getConfigPath() == path, "the manager should use " << path << ", not " << manager.getConfigPath());
    CHECK(StreamConfigManager::getConfigSearchPaths().size() == 1, "nothing else should be searched");
    CHECK(manager.loadConfig().status == LoadedStreamConfig::Status::Missing, "the file doesn't exist yet");
    unsetenv("AES67_CONFIG_PATH");
}

} // namespace

int main() {
    testJsonParsesDocument();
    testJsonRejectsMalformed();
    testJsonStrings();
    testJsonNumbers();
    testJsonDumpLayout();
    testAwkwardNamesRoundTrip();
    testOldFormatStillLoads();
    testBadValuesOnlyLoseTheirEntry();
    testCorruptFileIsBackedUpAndKept();
    testKeptEntriesAreWrittenBack();
    testSaveIsAtomic();
    testSaveAfterCorruptLoadKeepsTheBackup();
    testSaveWritesThroughSymlink();
    testByteOrderMarkIsIgnored();
    testUnreadableFileIsNotMissing();
    testSaveKeepsFilePermissions();
    testConfigPathFromEnvironmentIsAuthoritative();

    std::filesystem::remove_all(testDir());
    std::cout << "\nStream config: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
