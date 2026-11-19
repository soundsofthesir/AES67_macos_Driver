/// @file main.cpp
/// @brief aes67ctl: set up the driver's streams and PTP without editing JSON.
///
///   aes67ctl doctor  [--interface IF] [--group G]...
///   aes67ctl setup   --interface IF [--send G] [--receive G] [options]
///   aes67ctl add     send|receive G --interface IF [options]
///   aes67ctl remove  G
///   aes67ctl list
///   aes67ctl restart
///   aes67ctl status
///
/// The driver reads streams.json and ptp.json only when coreaudiod loads it,
/// so commands that change them restart coreaudiod once at the end
/// (--no-restart batches several changes).

#include "Checks.h"
#include "PacketProbe.h"
#include "System.h"
#include "../../NetworkEngine/StreamSetup.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace AES67;
using namespace AES67::Ctl;

namespace {

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr auto kDeviceTimeout = std::chrono::seconds(15);
constexpr auto kLockTimeout = std::chrono::seconds(30);
constexpr auto kProbeTime = std::chrono::milliseconds(2000);

const char* kUsageText = R"(usage:
  aes67ctl doctor  [--interface IF] [--group G]...
  aes67ctl setup   --interface IF [--send G] [--receive G]
                   [--send-channels N] [--receive-channels N] [--port P] [--ttl T]
                   [--ptp-domain D] [--ptp-hybrid] [--no-ptp]
                   [--no-default-output] [--no-restart]
  aes67ctl add     send|receive G --interface IF [--channels N] [--first-channel N]
                   [--port P] [--ttl T] [--name NAME] [--no-restart]
  aes67ctl remove  G [--no-restart]
  aes67ctl list
  aes67ctl restart
  aes67ctl status

Channels are 1-based in output: a 2-channel send stream from channel 1
carries the Mac's stereo output.
)";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> values;
    std::set<std::string> flags;

    bool has(const std::string& flag) const { return flags.count(flag) > 0; }
    std::string value(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second.back();
    }
    std::vector<std::string> all(const std::string& key) const {
        const auto it = values.find(key);
        return it == values.end() ? std::vector<std::string>{} : it->second;
    }
};

const std::set<std::string> kFlags = {"--ptp-hybrid", "--no-ptp", "--no-default-output", "--no-restart"};

std::optional<Args> parseArgs(int argc, char** argv, std::string* error) {
    Args args;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            args.positional.push_back(arg);
        } else if (kFlags.count(arg)) {
            args.flags.insert(arg);
        } else if (i + 1 < argc) {
            args.values[arg].push_back(argv[++i]);
        } else {
            *error = arg + " needs a value";
            return std::nullopt;
        }
    }
    return args;
}

std::optional<unsigned long> number(const std::string& text, unsigned long max) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 6) return std::nullopt;
    const unsigned long value = std::stoul(text);
    if (value > max) return std::nullopt;
    return value;
}

/// Read a numeric option into `out`; false (with a message) if it isn't a number in range.
template <typename T>
bool readNumber(const Args& args, const std::string& key, unsigned long min, unsigned long max, T& out) {
    const std::string text = args.value(key);
    if (text.empty()) return true;
    const auto value = number(text, max);
    if (!value || *value < min) {
        std::cerr << key << " must be " << min << " to " << max << std::endl;
        return false;
    }
    out = static_cast<T>(*value);
    return true;
}

void printFindings(const std::vector<Finding>& findings) {
    for (const auto& finding : findings) {
        const char* tag = finding.level == Finding::Level::Ok ? "ok  " : finding.level == Finding::Level::Warn ? "warn" : "FAIL";
        std::cout << tag << "  " << finding.text << std::endl;
    }
}

bool needsRoot() {
    if (!usingSystemPaths() || geteuid() == 0) return false;
    std::cerr << "run with sudo: the driver's files are in /Library/Application Support/AES67Driver" << std::endl;
    return true;
}

bool checkInterface(const std::string& name) {
    if (isPlainInterfaceName(name)) return true;
    std::cerr << (name.empty() ? "--interface is required" : "not an interface name: " + name) << std::endl;
    return false;
}

/// The streams file's entries ({} if there is none). Read here rather than
/// with StreamConfigManager::loadConfig: that copies a corrupt file to .bak,
/// and as root in a directory _coreaudiod can write, a planted .bak symlink
/// would aim that copy anywhere. Symlinks are refused.
/// Groups go to `route` as arguments: only plain multicast addresses pass.
bool checkGroups(const std::vector<std::string>& groups) {
    for (const auto& group : groups) {
        if (!Setup::isMulticastIPv4(group)) {
            std::cerr << "not an IPv4 multicast address: " << group << std::endl;
            return false;
        }
    }
    return true;
}

std::optional<std::vector<SavedStreamEntry>> loadStreams() {
    const std::string path = streamsPath();
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return std::vector<SavedStreamEntry>{};
        std::cerr << "can't read " << path << ": " << std::strerror(errno) << std::endl;
        return std::nullopt;
    }
    std::string text;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) text.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    auto entries = StreamConfigManager::fromJSON(text);
    if (!entries) {
        std::cerr << path << " is not a streams file; fix or remove it" << std::endl;
        return std::nullopt;
    }
    return entries;
}

bool saveStreams(const std::vector<SavedStreamEntry>& entries) {
    std::string error;
    if (!prepareConfigDirectory(&error) || !writeFile(streamsPath(), Setup::toFileText(entries), &error)) {
        std::cerr << error << std::endl;
        return false;
    }
    return true;
}

std::string channelRange(uint16_t first, uint16_t count) {
    return count == 1 ? std::to_string(first + 1) : std::to_string(first + 1) + "-" + std::to_string(first + count);
}

/// Restart coreaudiod and wait for the device. False with a message on failure.
bool restartAndWait() {
    std::string error;
    std::cout << "restarting coreaudiod" << std::endl;
    if (!restartCoreAudio(&error)) {
        std::cerr << error << std::endl;
        return false;
    }
    if (!waitForDevice(kDeviceTimeout)) {
        std::cerr << "AES67 Device did not appear within " << kDeviceTimeout.count()
                  << " s; see: log show --last 2m --info --predicate 'subsystem == \"com.aes67driver\"'" << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

std::string describeProbe(const PersistedStreamConfig& stream, const ProbeResult& result) {
    const bool send = Setup::directionOf({stream, {}}) == Setup::Direction::Send;
    if (!result.joined) return "can't listen on the group";
    if (result.packets == 0) {
        return send ? "no packets (driver not loaded, or this terminal lacks Local Network permission)"
                    : "no packets (the driver joins only while an app uses the device; check the sender)";
    }
    const double perSecond = result.packets * 1000.0 / static_cast<double>(kProbeTime.count());
    char text[96];
    if (stream.sdp.encoding == "L24" && result.peakDbfs) {
        std::snprintf(text, sizeof(text), "%.0f packets/s, peak %.1f dBFS", perSecond, *result.peakDbfs);
    } else if (stream.sdp.encoding == "L24") {
        std::snprintf(text, sizeof(text), "%.0f packets/s, silent", perSecond);
    } else {
        std::snprintf(text, sizeof(text), "%.0f packets/s", perSecond);
    }
    return text;
}

int status() {
    const std::string ptp = lastPtpLockChange(coreAudioStartTime());
    std::cout << "ptp      " << (ptp.empty() ? "no lock since coreaudiod started" : ptp) << std::endl;

    const auto entries = loadStreams();
    if (!entries) return kFailed;
    std::vector<const PersistedStreamConfig*> streams;
    for (const auto& entry : *entries) {
        if (entry.config && entry.config->enabled) streams.push_back(&*entry.config);
    }
    std::vector<ProbeResult> results(streams.size());
    std::vector<std::thread> probes;
    for (size_t i = 0; i < streams.size(); ++i) {
        probes.emplace_back([&, i] {
            const PersistedStreamConfig& s = *streams[i];
            const std::string address = interfaceInfo(s.networkInterface).ipv4;
            if (!address.empty() && Setup::isMulticastIPv4(s.sdp.connectionAddress)) {
                results[i] = probe(s.sdp.connectionAddress, s.sdp.port, address, kProbeTime);
            }
        });
    }
    for (auto& t : probes) t.join();

    bool allSending = true;
    for (size_t i = 0; i < streams.size(); ++i) {
        const PersistedStreamConfig& s = *streams[i];
        const bool send = Setup::directionOf({s, {}}) == Setup::Direction::Send;
        const bool probed = !s.networkInterface.empty();
        allSending = allSending && (!send || !probed || results[i].packets > 0);
        std::cout << (send ? "send     " : "receive  ") << s.sdp.connectionAddress << "  " << s.sdp.numChannels
                  << " ch  " << (send ? "outputs " : "inputs ")
                  << channelRange(s.mapping.deviceChannelStart, s.sdp.numChannels) << "  "
                  << (probed ? s.networkInterface + "  " + describeProbe(s, results[i])
                             : std::string("default interface, not probed"))
                  << std::endl;
    }
    if (streams.empty()) std::cout << "no streams in " << streamsPath() << std::endl;
    std::cout << "output   " << (isDefaultOutput() ? "AES67 Device is the default output" : "AES67 Device is not the default output")
              << std::endl;
    return allSending ? kOk : kFailed;
}

/// After a restart: wait for PTP to lock (if it is on), then report.
int statusAfterRestart(bool ptpOn) {
    if (ptpOn) {
        std::cout << "waiting for PTP lock" << std::endl;
        const auto deadline = std::chrono::steady_clock::now() + kLockTimeout;
        const std::string started = coreAudioStartTime();
        while (std::chrono::steady_clock::now() < deadline && lastPtpLockChange(started).rfind("locked", 0) != 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    return status();
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

int doctor(const Args& args) {
    const std::string name = args.value("--interface");
    if (!name.empty() && !checkInterface(name)) return kUsage;
    std::vector<std::string> groups = args.all("--group");
    if (!checkGroups(groups)) return kUsage;
    if (groups.empty()) {
        if (const auto entries = loadStreams()) {
            for (const auto& entry : *entries) {
                if (entry.config && Setup::isMulticastIPv4(entry.config->sdp.connectionAddress)) {
                    groups.push_back(entry.config->sdp.connectionAddress);
                }
            }
        }
    }
    const auto findings = diagnose(gatherFacts(name, groups));
    printFindings(findings);
    return hasFailure(findings) ? kFailed : kOk;
}

Setup::StreamRequest baseRequest(Setup::Direction direction, const std::string& group, const std::string& interfaceName) {
    Setup::StreamRequest request;
    request.direction = direction;
    request.group = group;
    request.networkInterface = interfaceName;
    request.name = shortHostName() + (direction == Setup::Direction::Send ? " out" : " in");
    return request;
}

bool readCommonOptions(const Args& args, Setup::StreamRequest& request) {
    return readNumber(args, "--port", 1, 65535, request.port) && readNumber(args, "--ttl", 0, 255, request.ttl);
}

/// Add `request` to `entries`; false with a message if it can't be built or clashes.
bool addTo(std::vector<SavedStreamEntry>& entries, const Setup::StreamRequest& request) {
    std::string error;
    const auto stream = Setup::buildStream(request, interfaceInfo(request.networkInterface).ipv4, &error);
    const auto added = stream ? Setup::addStream(entries, *stream, &error) : std::nullopt;
    if (!added) {
        std::cerr << request.group << ": " << error << std::endl;
        return false;
    }
    entries = *added;
    std::cout << (request.direction == Setup::Direction::Send ? "send     " : "receive  ") << request.group << "  "
              << request.channels << " ch  "
              << (request.direction == Setup::Direction::Send ? "outputs " : "inputs ")
              << channelRange(request.firstDeviceChannel, request.channels) << "  " << request.networkInterface
              << std::endl;
    return true;
}

int setup(const Args& args) {
    const std::string name = args.value("--interface");
    const std::string sendGroup = args.value("--send");
    const std::string receiveGroup = args.value("--receive");
    if (!checkInterface(name)) return kUsage;
    if (sendGroup.empty() && receiveGroup.empty()) {
        std::cerr << "give --send, --receive or both" << std::endl;
        return kUsage;
    }
    std::vector<std::string> groups;
    for (const auto& group : {sendGroup, receiveGroup}) {
        if (!group.empty()) groups.push_back(group);
    }
    if (!checkGroups(groups)) return kUsage;

    Setup::StreamRequest send = baseRequest(Setup::Direction::Send, sendGroup, name);
    Setup::StreamRequest receive = baseRequest(Setup::Direction::Receive, receiveGroup, name);
    uint8_t ptpDomain = 0;
    if (!readNumber(args, "--send-channels", 1, Setup::kMaxStreamChannels, send.channels) ||
        !readNumber(args, "--receive-channels", 1, Setup::kMaxStreamChannels, receive.channels) ||
        !readCommonOptions(args, send) || !readCommonOptions(args, receive) ||
        !readNumber(args, "--ptp-domain", 0, 127, ptpDomain)) {
        return kUsage;
    }
    if (needsRoot()) return kFailed;

    // Fix the directory's owner first, so doctor doesn't warn about what setup fixes
    std::string error;
    if (!prepareConfigDirectory(&error)) {
        std::cerr << error << std::endl;
        return kFailed;
    }
    const auto findings = diagnose(gatherFacts(name, groups));
    printFindings(findings);
    if (hasFailure(findings)) return kFailed;

    auto entries = loadStreams();
    if (!entries) return kFailed;
    if ((!sendGroup.empty() && !addTo(*entries, send)) || (!receiveGroup.empty() && !addTo(*entries, receive))) {
        return kFailed;
    }
    // ptp.json first: if streams.json then fails, nothing that changes what
    // the driver sends has been written
    const bool ptpOn = !args.has("--no-ptp");
    if (ptpOn) {
        if (!writeFile(ptpPath(), Setup::ptpFileText(name, ptpDomain, args.has("--ptp-hybrid")), &error)) {
            std::cerr << error << std::endl;
            return kFailed;
        }
    }
    if (!saveStreams(*entries)) {
        if (ptpOn) std::cerr << "ptp.json was updated; streams.json was not" << std::endl;
        return kFailed;
    }
    if (ptpOn) std::cout << "ptp      " << name << ", domain " << static_cast<int>(ptpDomain) << std::endl;

    if (args.has("--no-restart")) return kOk;
    if (!restartAndWait()) return kFailed;
    if (!args.has("--no-default-output")) {
        if (!setDefaultOutput(&error)) {
            std::cerr << error << std::endl;
            return kFailed;
        }
    }
    return statusAfterRestart(ptpOn);
}

int add(const Args& args) {
    if (args.positional.size() != 2 || (args.positional[0] != "send" && args.positional[0] != "receive")) {
        std::cerr << "usage: aes67ctl add send|receive GROUP --interface IF" << std::endl;
        return kUsage;
    }
    const std::string name = args.value("--interface");
    if (!checkInterface(name) || !checkGroups({args.positional[1]})) return kUsage;
    const auto direction = args.positional[0] == "send" ? Setup::Direction::Send : Setup::Direction::Receive;
    Setup::StreamRequest request = baseRequest(direction, args.positional[1], name);
    unsigned long firstChannel = 1;
    if (!readNumber(args, "--channels", 1, Setup::kMaxStreamChannels, request.channels) ||
        !readNumber(args, "--first-channel", 1, StreamChannelMapper::kMaxDeviceChannels, firstChannel) ||
        !readCommonOptions(args, request)) {
        return kUsage;
    }
    request.firstDeviceChannel = static_cast<uint16_t>(firstChannel - 1);
    request.name = args.value("--name", request.name);
    if (needsRoot()) return kFailed;

    auto entries = loadStreams();
    if (!entries || !addTo(*entries, request) || !saveStreams(*entries)) return kFailed;
    return args.has("--no-restart") || restartAndWait() ? kOk : kFailed;
}

int remove(const Args& args) {
    if (args.positional.size() != 1) {
        std::cerr << "usage: aes67ctl remove GROUP" << std::endl;
        return kUsage;
    }
    if (!checkGroups(args.positional)) return kUsage;
    if (needsRoot()) return kFailed;
    auto entries = loadStreams();
    if (!entries) return kFailed;
    size_t removed = 0;
    const auto left = Setup::removeStreams(*entries, args.positional[0], &removed);
    if (removed == 0) {
        std::cerr << "no stream on " << args.positional[0] << std::endl;
        return kFailed;
    }
    if (!saveStreams(left)) return kFailed;
    std::cout << "removed  " << removed << (removed == 1 ? " stream" : " streams") << std::endl;
    return args.has("--no-restart") || restartAndWait() ? kOk : kFailed;
}

int list() {
    const auto entries = loadStreams();
    if (!entries) return kFailed;
    for (const auto& entry : *entries) {
        if (!entry.config) {
            std::cout << "unreadable entry (kept as written)" << std::endl;
            continue;
        }
        const PersistedStreamConfig& s = *entry.config;
        const bool send = Setup::directionOf(entry) == Setup::Direction::Send;
        std::cout << (send ? "send     " : "receive  ") << s.sdp.connectionAddress << ":" << s.sdp.port << "  "
                  << s.sdp.numChannels << " ch  " << (send ? "outputs " : "inputs ")
                  << channelRange(s.mapping.deviceChannelStart, s.sdp.numChannels) << "  "
                  << (s.networkInterface.empty() ? "default interface" : s.networkInterface) << "  "
                  << s.sdp.sessionName << (s.enabled ? "" : "  (disabled)") << std::endl;
    }
    if (entries->empty()) std::cout << "no streams in " << streamsPath() << std::endl;
    return kOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help") {
        std::cout << kUsageText;
        return argc < 2 ? kUsage : kOk;
    }
    const std::string command = argv[1];
    std::string error;
    const auto args = parseArgs(argc, argv, &error);
    if (!args) {
        std::cerr << error << std::endl;
        return kUsage;
    }
    if (command == "doctor") return doctor(*args);
    if (command == "setup") return setup(*args);
    if (command == "add") return add(*args);
    if (command == "remove") return remove(*args);
    if (command == "list") return list();
    if (command == "status") return status();
    if (command == "restart") {
        if (geteuid() != 0) {
            std::cerr << "run with sudo" << std::endl;
            return kFailed;
        }
        return restartAndWait() ? kOk : kFailed;
    }
    std::cerr << "unknown command: " << command << "\n" << kUsageText;
    return kUsage;
}
