//
// StreamConfig.h
// AES67 macOS Driver - Build #10
// Stream configuration persistence with JSON serialization
//

#pragma once

#include "../Shared/Types.h"
#include "../Driver/SDPParser.h"
#include "StreamChannelMapper.h"
#include "Json.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace AES67 {

//
// Persisted Stream Configuration
//
// Complete stream configuration including SDP session and channel mapping
// Used for saving/loading stream configurations across driver restarts
//
struct PersistedStreamConfig {
    SDPSession sdp;
    ChannelMapping mapping;

    // Metadata
    bool enabled{true};              // Whether stream is active
    std::string description;         // User-provided description
    uint64_t createdTimestamp{0};    // When stream was added
    uint64_t modifiedTimestamp{0};   // Last modification

    // Jitter buffer configuration: 0 = default (256 slots).
    // Values are clamped to [32, 4096] and rounded up to next power of 2.
    size_t jitterBufferDepth{0};

    // Network interface for multicast binding (e.g., "en0" or "192.168.1.10").
    // Empty string = bind to INADDR_ANY (all interfaces).
    std::string networkInterface;

    // Validation
    bool isValid() const;
};

/// One entry of streams.json. `raw` is the entry as it was in the file;
/// `config` is empty when it couldn't be read (a field of the wrong type, a
/// number out of range, or an invalid stream). An entry the driver doesn't
/// load is written back from `raw`, so nothing in the file is lost.
struct SavedStreamEntry {
    std::optional<PersistedStreamConfig> config;
    JsonValue raw;
};

struct LoadedStreamConfig {
    enum class Status {
        Missing,  ///< no config file
        Corrupt,  ///< not JSON, or not shaped like streams.json (copied to .bak)
        Loaded,
    };
    Status status{Status::Missing};
    std::vector<SavedStreamEntry> entries;
};

//
// Stream Configuration Manager
//
// Handles saving and loading stream configurations to/from disk
// Configurations are stored in JSON format at:
// /Library/Application Support/AES67Driver/streams.json
//
class StreamConfigManager {
public:
    StreamConfigManager();
    ~StreamConfigManager();

    //
    // Configuration File Management
    //

    /// Get the active configuration file path (first found in search order).
    std::string getConfigPath() const;

    /// Set a custom configuration file path (overrides search).
    void setConfigPath(const std::string& path);

    /// Ensure the configuration directory exists.
    bool ensureConfigDirectoryExists();

    /// Get all config search paths in order of priority.
    /// Search order:
    ///   1. AES67_CONFIG_PATH environment variable (if set, and then only that)
    ///   2. ~/Library/Application Support/AES67Driver/streams.json (user)
    ///   3. /Library/Application Support/AES67Driver/streams.json (system)
    static std::vector<std::string> getConfigSearchPaths();

    /// Find first existing config file from search paths.
    /// @return Path to existing config, or empty string if none found.
    static std::string findExistingConfig();

    //
    // Save/Load Operations
    //

    /// Write all stream configurations, followed by `kept` (entries from the
    /// file that weren't loaded, written back unchanged). The file is
    /// replaced atomically: on any error the old file is left as it was.
    bool saveConfig(const std::vector<PersistedStreamConfig>& configs,
                    const std::vector<JsonValue>& kept = {});

    /// Read the config file. A corrupt file is copied to <path>.bak.
    LoadedStreamConfig loadConfig();

    // Save a single stream configuration (append or update)
    bool saveStream(const PersistedStreamConfig& config);

    // Remove a stream from saved configuration
    bool removeStream(const StreamID& id);

    //
    // JSON Serialization
    //

    // Convert stream configs (and kept entries) to JSON text
    static std::string toJSON(const std::vector<PersistedStreamConfig>& configs,
                              const std::vector<JsonValue>& kept = {});

    /// Parse a whole file. nullopt if it isn't a JSON object with a "streams"
    /// array; a bad entry only makes that entry unreadable.
    static std::optional<std::vector<SavedStreamEntry>> fromJSON(const std::string& json);

    static JsonValue configToJSON(const PersistedStreamConfig& config);

    /// nullopt if a field has the wrong type or doesn't fit, or the result
    /// isn't a valid stream. Missing fields keep their defaults.
    static std::optional<PersistedStreamConfig> configFromJSON(const JsonValue& json);

    //
    // Helper Functions
    //

    // Create a persisted config from SDP and mapping
    static PersistedStreamConfig createConfig(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& description = ""
    );

    // Get current Unix timestamp (seconds since epoch)
    static uint64_t getCurrentTimestamp();

private:
    std::string configPath_;
    std::string defaultConfigFile_{"streams.json"};

    static JsonValue sdpToJSON(const SDPSession& sdp);
    static JsonValue mappingToJSON(const ChannelMapping& mapping);
};

} // namespace AES67
