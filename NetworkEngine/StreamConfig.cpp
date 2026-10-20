//
// StreamConfig.cpp
// AES67 macOS Driver - Build #10
// Stream configuration persistence implementation
//

#include "StreamConfig.h"
#include "NetworkUtils.h"
#include "../Driver/DebugLog.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <pwd.h>
#include <unistd.h>

namespace AES67 {

// ============================================================================
// PersistedStreamConfig Implementation
// ============================================================================

bool PersistedStreamConfig::isValid() const {
    bool sdpValid = sdp.isValid();
    bool mappingValid = mapping.isValid();

    if (!sdpValid) {
        AES67_LOG("PersistedStreamConfig: SDP is invalid");
    }
    if (!mappingValid) {
        AES67_LOG("PersistedStreamConfig: Mapping is invalid");
    }

    return sdpValid && mappingValid;
}

// ============================================================================
// StreamConfigManager Implementation
// ============================================================================

StreamConfigManager::StreamConfigManager() {
    // An explicit AES67_CONFIG_PATH is used even before it exists: falling
    // through to another file would load (and then save over) a config the
    // caller didn't ask for
    const char* envPath = std::getenv("AES67_CONFIG_PATH");
    std::string existingConfig = findExistingConfig();
    if (envPath && envPath[0] != '\0') {
        configPath_ = envPath;
        AES67_LOGF("StreamConfigManager: Using AES67_CONFIG_PATH: %s", configPath_.c_str());
    } else if (!existingConfig.empty()) {
        configPath_ = existingConfig;
        AES67_LOGF("StreamConfigManager: Found config at: %s", configPath_.c_str());
    } else {
        // Default to system-wide location for new configs
        configPath_ = "/Library/Application Support/AES67Driver/" + defaultConfigFile_;
        AES67_LOGF("StreamConfigManager: No existing config found, will use: %s", configPath_.c_str());
    }

    // Log available interfaces for debugging
    auto interfaces = NetworkUtils::getActiveInterfacesWithIPs();
    AES67_LOGF("StreamConfigManager: Available network interfaces (%zu):", interfaces.size());
    for (const auto& iface : interfaces) {
        AES67_LOGF("  - %s: %s", iface.first.c_str(), iface.second.c_str());
    }
}

std::vector<std::string> StreamConfigManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    // 1. Environment variable override (highest priority)
    const char* envPath = std::getenv("AES67_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') {
        paths.push_back(envPath);
        AES67_LOGF("StreamConfigManager: AES67_CONFIG_PATH set to: %s", envPath);
        return paths;
    }

    // 2. User-level config (~/.config/AES67Driver or ~/Library/Application Support)
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        std::string userPath = std::string(home) + "/Library/Application Support/AES67Driver/streams.json";
        paths.push_back(userPath);
    }

    // 3. System-wide config (default)
    paths.push_back("/Library/Application Support/AES67Driver/streams.json");

    return paths;
}

std::string StreamConfigManager::findExistingConfig() {
    auto searchPaths = getConfigSearchPaths();

    for (const auto& path : searchPaths) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            AES67_LOGF("StreamConfigManager: Found config file at: %s", path.c_str());
            return path;
        }
    }

    return "";
}

StreamConfigManager::~StreamConfigManager() = default;

std::string StreamConfigManager::getConfigPath() const {
    return configPath_;
}

void StreamConfigManager::setConfigPath(const std::string& path) {
    configPath_ = path;
}

bool StreamConfigManager::ensureConfigDirectoryExists() {
    // Extract directory from config path
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) {
        return false;
    }

    std::string dir = configPath_.substr(0, lastSlash);

    // Check if directory already exists
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Directory doesn't exist — create parent first, then target.
    // For /Library/Application Support/AES67Driver/ the parent should already exist,
    // but handle the case where it doesn't gracefully.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("StreamConfigManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }

    // Set directory permissions to 755 (rwxr-xr-x)
    chmod(dir.c_str(), 0755);

    return true;
}

namespace {

/// Write `contents` to a temporary file next to `path`, flush it to disk and
/// rename it over `path`. rename() is atomic, so a reader sees either the old
/// file or the whole new one; on any failure the old file is untouched.
bool replaceFileAtomically(const std::string& path, const std::string& contents) {
    std::string tempPath = path + ".XXXXXX";
    const int fd = mkostemp(tempPath.data(), O_CLOEXEC);
    if (fd < 0) {
        AES67_LOGF("StreamConfigManager: Failed to create temp file for %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    const auto abandon = [&](const char* step) {
        AES67_LOGF("StreamConfigManager: Failed to %s %s: %s", step, tempPath.c_str(), strerror(errno));
        close(fd);
        unlink(tempPath.c_str());
        return false;
    };

    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t n = write(fd, contents.data() + written, contents.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return abandon("write");
        written += static_cast<size_t>(n);
    }
    // mkstemp makes the file 0600: keep the old file's mode, or 0644 for a
    // new one (the manager app reads it as another user)
    struct stat old;
    const mode_t mode = stat(path.c_str(), &old) == 0 ? (old.st_mode & 07777) : 0644;
    if (fchmod(fd, mode) != 0) return abandon("set permissions on");
    // fsync alone leaves the data in the drive's cache on macOS
    if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) return abandon("flush");
    if (close(fd) != 0) {
        AES67_LOGF("StreamConfigManager: Failed to close %s: %s", tempPath.c_str(), strerror(errno));
        unlink(tempPath.c_str());
        return false;
    }
    if (rename(tempPath.c_str(), path.c_str()) != 0) {
        AES67_LOGF("StreamConfigManager: Failed to replace %s: %s", path.c_str(), strerror(errno));
        unlink(tempPath.c_str());
        return false;
    }
    return true;
}

} // namespace

bool StreamConfigManager::saveConfig(const std::vector<PersistedStreamConfig>& configs,
                                     const std::vector<JsonValue>& kept) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("StreamConfigManager: Failed to create config directory");
        return false;
    }

    // rename() would replace a symlink with a plain file: write to its target
    std::error_code ec;
    std::string target = configPath_;
    if (std::filesystem::is_symlink(configPath_, ec)) {
        const auto resolved = std::filesystem::canonical(configPath_, ec);
        if (ec) {
            AES67_LOGF("StreamConfigManager: %s is a broken symlink: %s", configPath_.c_str(), ec.message().c_str());
            return false;
        }
        target = resolved.string();
    }

    if (!replaceFileAtomically(target, toJSON(configs, kept) + "\n")) {
        return false;
    }

    AES67_LOGF("StreamConfigManager: Saved %zu stream configurations (+%zu not loaded) to %s",
               configs.size(), kept.size(), configPath_.c_str());
    return true;
}

LoadedStreamConfig StreamConfigManager::loadConfig() {
    LoadedStreamConfig result;

    std::ifstream file(configPath_);
    if (!file.is_open()) {
        struct stat st;
        if (stat(configPath_.c_str(), &st) != 0) {
            AES67_LOGF("StreamConfigManager: Config file not found: %s", configPath_.c_str());
            return result;
        }
        // There but unreadable (permissions, sandbox): still the user's
        // config, which the test stream must not be saved over
        AES67_LOGF("WARNING: StreamConfigManager: Cannot read config file %s: %s",
                   configPath_.c_str(), strerror(errno));
        result.status = LoadedStreamConfig::Status::Corrupt;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();
    file.close();

    auto entries = fromJSON(json);
    if (entries) {
        result.status = LoadedStreamConfig::Status::Loaded;
        result.entries = std::move(*entries);
        AES67_LOGF("StreamConfigManager: Read %zu stream entries from %s", result.entries.size(), configPath_.c_str());
        return result;
    }

    // Corrupt: keep a copy. The file itself stays until something is saved
    // over it, so nothing more is lost if the copy fails.
    result.status = LoadedStreamConfig::Status::Corrupt;
    AES67_LOGF("WARNING: StreamConfigManager: Config file is corrupt, backing up to %s.bak",
               configPath_.c_str());

    std::error_code ec;
    std::filesystem::copy_file(configPath_, configPath_ + ".bak",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        AES67_LOGF("StreamConfigManager: Failed to create backup: %s", ec.message().c_str());
    }
    return result;
}

// ============================================================================
// JSON Serialization
// ============================================================================

std::string StreamConfigManager::toJSON(const std::vector<PersistedStreamConfig>& configs,
                                        const std::vector<JsonValue>& kept) {
    JsonValue streams = JsonValue::array();
    for (const auto& config : configs) {
        streams.push(configToJSON(config));
    }
    for (const auto& entry : kept) {
        streams.push(entry);
    }

    return JsonValue::object()
        .add("version", JsonValue::string("1.0"))
        .add("streams", std::move(streams))
        .dump();
}

JsonValue StreamConfigManager::configToJSON(const PersistedStreamConfig& config) {
    return JsonValue::object()
        .add("enabled", JsonValue::boolean(config.enabled))
        .add("description", JsonValue::string(config.description))
        .add("createdTimestamp", JsonValue::number(config.createdTimestamp))
        .add("modifiedTimestamp", JsonValue::number(config.modifiedTimestamp))
        .add("jitterBufferDepth", JsonValue::number(static_cast<uint64_t>(config.jitterBufferDepth)))
        .add("networkInterface", JsonValue::string(config.networkInterface))
        .add("sdp", sdpToJSON(config.sdp))
        .add("mapping", mappingToJSON(config.mapping));
}

JsonValue StreamConfigManager::sdpToJSON(const SDPSession& sdp) {
    return JsonValue::object()
        .add("sessionName", JsonValue::string(sdp.sessionName))
        .add("sessionInfo", JsonValue::string(sdp.sessionInfo))
        .add("sessionID", JsonValue::number(static_cast<uint64_t>(sdp.sessionID)))
        .add("sessionVersion", JsonValue::number(static_cast<uint64_t>(sdp.sessionVersion)))
        .add("originUsername", JsonValue::string(sdp.originUsername))
        .add("originAddress", JsonValue::string(sdp.originAddress))
        .add("connectionAddress", JsonValue::string(sdp.connectionAddress))
        .add("ttl", JsonValue::number(static_cast<uint64_t>(sdp.ttl)))
        .add("port", JsonValue::number(static_cast<uint64_t>(sdp.port)))
        .add("payloadType", JsonValue::number(static_cast<uint64_t>(sdp.payloadType)))
        .add("encoding", JsonValue::string(sdp.encoding))
        .add("sampleRate", JsonValue::number(static_cast<uint64_t>(sdp.sampleRate)))
        .add("numChannels", JsonValue::number(static_cast<uint64_t>(sdp.numChannels)))
        .add("ptime", JsonValue::number(sdp.ptime))
        .add("framecount", JsonValue::number(static_cast<uint64_t>(sdp.framecount)))
        .add("sourceAddress", JsonValue::string(sdp.sourceAddress))
        .add("ptpDomain", JsonValue::number(static_cast<int64_t>(sdp.ptpDomain)))
        .add("ptpMasterMAC", JsonValue::string(sdp.ptpMasterMAC))
        .add("mediaClockType", JsonValue::string(sdp.mediaClockType))
        .add("direction", JsonValue::string(sdp.direction));
}

JsonValue StreamConfigManager::mappingToJSON(const ChannelMapping& mapping) {
    JsonValue channelMap = JsonValue::array();
    for (int channel : mapping.channelMap) {
        channelMap.push(JsonValue::number(channel));
    }

    return JsonValue::object()
        .add("streamID", JsonValue::string(mapping.streamID.toString()))
        .add("streamName", JsonValue::string(mapping.streamName))
        .add("streamChannelCount", JsonValue::number(static_cast<uint64_t>(mapping.streamChannelCount)))
        .add("streamChannelOffset", JsonValue::number(static_cast<uint64_t>(mapping.streamChannelOffset)))
        .add("deviceChannelStart", JsonValue::number(static_cast<uint64_t>(mapping.deviceChannelStart)))
        .add("deviceChannelCount", JsonValue::number(static_cast<uint64_t>(mapping.deviceChannelCount)))
        .add("channelMap", std::move(channelMap));
}

// ============================================================================
// JSON Deserialization
// ============================================================================

namespace {

/// Reads optional fields of one JSON object. A missing field leaves its
/// target as it was; a field of the wrong type or out of range marks the
/// whole object unreadable, and the first such field is logged.
class FieldReader {
public:
    FieldReader(const JsonValue& object, const char* what) : object_(object), what_(what) {}

    bool ok() const { return ok_; }

    template <typename T>
    void unsignedInt(const char* key, T& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const auto n = value->asUInt64();
        if (!n || *n > std::numeric_limits<T>::max()) return fail(key);
        out = static_cast<T>(*n);
    }

    void signedInt(const char* key, int& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const auto n = value->asInt64();
        if (!n || *n < std::numeric_limits<int>::min() || *n > std::numeric_limits<int>::max()) return fail(key);
        out = static_cast<int>(*n);
    }

    void number(const char* key, double& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const auto n = value->asDouble();
        if (!n) return fail(key);
        out = *n;
    }

    void text(const char* key, std::string& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const std::string* s = value->asString();
        if (!s) return fail(key);
        out = *s;
    }

    void flag(const char* key, bool& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const auto b = value->asBool();
        if (!b) return fail(key);
        out = *b;
    }

    void intArray(const char* key, std::vector<int>& out) {
        const JsonValue* value = object_.find(key);
        if (!value) return;
        const JsonValue::Array* items = value->asArray();
        if (!items) return fail(key);
        std::vector<int> result;
        for (const auto& item : *items) {
            const auto n = item.asInt64();
            if (!n || *n < std::numeric_limits<int>::min() || *n > std::numeric_limits<int>::max()) return fail(key);
            result.push_back(static_cast<int>(*n));
        }
        out = std::move(result);
    }

    /// A nested object, or nullptr (and unreadable) if it is something else.
    const JsonValue* object(const char* key) {
        const JsonValue* value = object_.find(key);
        if (!value) return nullptr;
        if (!value->asObject()) fail(key);
        return value->asObject() ? value : nullptr;
    }

private:
    void fail(const char* key) {
        if (ok_) AES67_LOGF("StreamConfigManager: Unreadable %s field \"%s\"", what_, key);
        ok_ = false;
    }

    const JsonValue& object_;
    const char* what_;
    bool ok_{true};
};

bool readSdp(const JsonValue& json, SDPSession& sdp) {
    FieldReader r(json, "sdp");
    r.text("sessionName", sdp.sessionName);
    r.text("sessionInfo", sdp.sessionInfo);
    r.unsignedInt("sessionID", sdp.sessionID);
    r.unsignedInt("sessionVersion", sdp.sessionVersion);
    r.text("originUsername", sdp.originUsername);
    r.text("originAddress", sdp.originAddress);
    r.text("connectionAddress", sdp.connectionAddress);
    r.unsignedInt("ttl", sdp.ttl);
    r.unsignedInt("port", sdp.port);
    r.unsignedInt("payloadType", sdp.payloadType);
    r.text("encoding", sdp.encoding);
    r.unsignedInt("sampleRate", sdp.sampleRate);
    r.unsignedInt("numChannels", sdp.numChannels);
    r.number("ptime", sdp.ptime);  // fractional ms allowed
    r.unsignedInt("framecount", sdp.framecount);
    r.text("sourceAddress", sdp.sourceAddress);
    r.signedInt("ptpDomain", sdp.ptpDomain);
    r.text("ptpMasterMAC", sdp.ptpMasterMAC);
    r.text("mediaClockType", sdp.mediaClockType);
    r.text("direction", sdp.direction);
    return r.ok();
}

bool readMapping(const JsonValue& json, ChannelMapping& mapping) {
    FieldReader r(json, "mapping");
    std::string streamID;
    r.text("streamID", streamID);
    mapping.streamID = StreamID(streamID);  // malformed reads as null, which isValid() refuses
    r.text("streamName", mapping.streamName);
    r.unsignedInt("streamChannelCount", mapping.streamChannelCount);
    r.unsignedInt("streamChannelOffset", mapping.streamChannelOffset);
    r.unsignedInt("deviceChannelStart", mapping.deviceChannelStart);
    r.unsignedInt("deviceChannelCount", mapping.deviceChannelCount);
    r.intArray("channelMap", mapping.channelMap);
    return r.ok();
}

} // namespace

std::optional<std::vector<SavedStreamEntry>> StreamConfigManager::fromJSON(const std::string& json) {
    // Some editors start a UTF-8 file with a byte order mark
    std::string_view text = json;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);

    std::string error;
    auto document = JsonValue::parse(text, &error);
    if (!document) {
        AES67_LOGF("StreamConfigManager: Config is not valid JSON (%s)", error.c_str());
        return std::nullopt;
    }

    const JsonValue* streams = document->find("streams");
    if (!streams || !streams->asArray()) {
        AES67_LOG("StreamConfigManager: Config has no \"streams\" array");
        return std::nullopt;
    }

    std::vector<SavedStreamEntry> entries;
    for (const auto& item : *streams->asArray()) {
        entries.push_back(SavedStreamEntry{configFromJSON(item), item});
    }
    return entries;
}

std::optional<PersistedStreamConfig> StreamConfigManager::configFromJSON(const JsonValue& json) {
    if (!json.asObject()) {
        AES67_LOG("StreamConfigManager: Stream entry is not an object");
        return std::nullopt;
    }

    PersistedStreamConfig config;
    FieldReader r(json, "stream");
    r.flag("enabled", config.enabled);
    r.text("description", config.description);
    r.unsignedInt("createdTimestamp", config.createdTimestamp);
    r.unsignedInt("modifiedTimestamp", config.modifiedTimestamp);
    r.unsignedInt("jitterBufferDepth", config.jitterBufferDepth);
    // Kept as written ("en0", an IPv4 address, or "auto"/""): streams
    // resolve it each time they start. Resolving it here froze the
    // address of the moment, and saving then made it permanent.
    // No interface specified: left empty, which means the primary interface
    // as it is when the stream starts
    r.text("networkInterface", config.networkInterface);

    const JsonValue* sdp = r.object("sdp");
    const JsonValue* mapping = r.object("mapping");
    if (!r.ok()) return std::nullopt;

    if (sdp && !readSdp(*sdp, config.sdp)) {
        AES67_LOG("StreamConfigManager: Failed to parse SDP from JSON");
        return std::nullopt;
    }
    if (mapping && !readMapping(*mapping, config.mapping)) {
        AES67_LOG("StreamConfigManager: Failed to parse mapping from JSON");
        return std::nullopt;
    }

    if (!config.isValid()) {
        AES67_LOG("StreamConfigManager: Parsed config is invalid");
        return std::nullopt;
    }

    return config;
}

// ============================================================================
// Helper Functions
// ============================================================================

PersistedStreamConfig StreamConfigManager::createConfig(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    const std::string& description
) {
    PersistedStreamConfig config;
    config.sdp = sdp;
    config.mapping = mapping;
    config.description = description;
    config.enabled = true;
    config.createdTimestamp = getCurrentTimestamp();
    config.modifiedTimestamp = config.createdTimestamp;

    return config;
}

uint64_t StreamConfigManager::getCurrentTimestamp() {
    return static_cast<uint64_t>(std::time(nullptr));
}

} // namespace AES67
