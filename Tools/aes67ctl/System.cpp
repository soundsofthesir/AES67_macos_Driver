/// @file System.cpp
/// @brief The parts of aes67ctl that look at or change this Mac.

#include "System.h"
#include "Version.h"
#include "../../NetworkEngine/StreamConfig.h"
#include <CoreAudio/CoreAudio.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

extern char** environ;

namespace AES67 {
namespace Ctl {

namespace {

constexpr const char* kSystemDir = "/Library/Application Support/AES67Driver";
constexpr const char* kDriverBundle = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver";
constexpr const char* kDeviceUID = "com.aes67.driver.device";

const char* env(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : nullptr;
}

std::string toString(CFStringRef text) {
    if (!text) return "";
    char buffer[256] = {};
    return CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8) ? buffer : "";
}

AudioObjectPropertyAddress defaultOutputAddress() {
    return {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

}  // namespace

std::optional<AudioObjectID> findDevice() {
    CFStringRef uid = CFStringCreateWithCString(nullptr, kDeviceUID, kCFStringEncodingUTF8);
    AudioObjectID device = kAudioObjectUnknown;
    AudioValueTranslation translation{&uid, sizeof(uid), &device, sizeof(device)};
    UInt32 size = sizeof(translation);
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &translation);
    CFRelease(uid);
    if (status != noErr || device == kAudioObjectUnknown) return std::nullopt;
    return device;
}

std::string installedDriverVersion() {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(kDriverBundle), static_cast<CFIndex>(std::strlen(kDriverBundle)), true);
    CFDictionaryRef info = url ? CFBundleCopyInfoDictionaryInDirectory(url) : nullptr;
    if (url) CFRelease(url);
    if (!info) return "";
    const auto value = [&](const char* key) {
        CFStringRef name = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
        const void* found = CFDictionaryGetValue(info, name);
        CFRelease(name);
        return found && CFGetTypeID(found) == CFStringGetTypeID() ? toString(static_cast<CFStringRef>(found)) : "";
    };
    const std::string version = value("CFBundleShortVersionString");
    const std::string build = value("CFBundleVersion");
    CFRelease(info);
    if (version.empty()) return "";
    return build.empty() ? version : version + " build " + build;
}

std::optional<std::string> runningDriverVersion() {
    const auto device = findDevice();
    if (!device) return std::nullopt;
    const AudioObjectPropertyAddress address{kVersionPropertySelector, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    CFStringRef text = nullptr;
    UInt32 size = sizeof(text);
    if (AudioObjectGetPropertyData(*device, &address, 0, nullptr, &size, &text) != noErr || !text) return "";
    const std::string version = toString(text);
    CFRelease(text);
    return version;
}

int run(const std::vector<std::string>& argv, std::string* output) {
    int pipeFds[2];
    if (pipe(pipeFds) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (spawned != 0) {
        close(pipeFds[0]);
        return -1;
    }
    std::string text;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(pipeFds[0], buffer, sizeof(buffer))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) text.append(buffer, static_cast<size_t>(n));
    }
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (output) *output = std::move(text);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

InterfaceInfo interfaceInfo(const std::string& name) {
    InterfaceInfo info;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return info;
    for (ifaddrs* entry = list; entry; entry = entry->ifa_next) {
        if (name != entry->ifa_name) continue;
        info.exists = true;
        info.up = info.up || ((entry->ifa_flags & IFF_UP) && (entry->ifa_flags & IFF_RUNNING));
        if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET && info.ipv4.empty()) {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr, text, sizeof(text));
            info.ipv4 = text;
        }
    }
    freeifaddrs(list);
    return info;
}

Facts gatherFacts(const std::string& interfaceName, const std::vector<std::string>& groups) {
    Facts facts;
    struct stat st {};
    facts.driverInstalled = stat(kDriverBundle, &st) == 0 && S_ISDIR(st.st_mode);
    const std::string dir = std::filesystem::path(streamsPath()).parent_path();
    if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        facts.configDirExists = true;
        const passwd* driverUser = getpwnam("_coreaudiod");
        facts.configDirOwnedByDriver = !usingSystemPaths() || (driverUser && st.st_uid == driverUser->pw_uid);
    }
    facts.internetSharing = run({"/usr/bin/pgrep", "-x", "InternetSharing"}) == 0;

    facts.interfaceName = interfaceName;
    if (interfaceName.empty()) return facts;
    const InterfaceInfo info = interfaceInfo(interfaceName);
    facts.interfaceExists = info.exists;
    facts.interfaceUp = info.up;
    facts.ipv4 = info.ipv4;
    std::string ifconfig;
    if (run({"/sbin/ifconfig", "-a"}, &ifconfig) == 0) facts.bridge = bridgeOf(ifconfig, interfaceName);
    for (const auto& group : groups) {
        std::string route;
        run({"/sbin/route", "-n", "get", group}, &route);
        facts.groupRoutes.emplace_back(group, routeInterface(route));
    }
    return facts;
}

std::string streamsPath() {
    if (const char* path = env("AES67_CONFIG_PATH")) return path;
    return std::string(kSystemDir) + "/streams.json";
}

std::string ptpPath() {
    if (const char* path = env("AES67_PTP_CONFIG_PATH")) return path;
    return std::string(kSystemDir) + "/ptp.json";
}

bool usingSystemPaths() {
    return !env("AES67_CONFIG_PATH") && !env("AES67_PTP_CONFIG_PATH");
}

bool prepareConfigDirectory(std::string* error) {
    const std::filesystem::path streamsDir = std::filesystem::path(streamsPath()).parent_path();
    const std::filesystem::path ptpDir = std::filesystem::path(ptpPath()).parent_path();
    for (const auto& dir : {streamsDir, ptpDir}) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (error) *error = "can't create " + dir.string() + ": " + ec.message();
            return false;
        }
    }
    if (!usingSystemPaths()) return true;
    // The driver saves by renaming a temp file in this directory (#42)
    const passwd* driverUser = getpwnam("_coreaudiod");
    if (!driverUser || chown(kSystemDir, driverUser->pw_uid, 0) != 0 || chmod(kSystemDir, 0755) != 0) {
        if (error) *error = std::string("can't give ") + kSystemDir + " to _coreaudiod: " + strerror(errno);
        return false;
    }
    return true;
}

bool writeFile(const std::string& path, const std::string& contents, std::string* error) {
    if (StreamConfigManager::writeFileAtomically(path, contents)) return true;
    if (error) *error = "can't write " + path;
    return false;
}

bool restartCoreAudio(std::string* error) {
    std::string pidText;
    run({"/usr/bin/pgrep", "-x", "coreaudiod"}, &pidText);
    const pid_t old = static_cast<pid_t>(std::atoi(pidText.c_str()));
    // launchd starts it again. With SIP on, launchctl kickstart can't restart it
    if (run({"/usr/bin/killall", "coreaudiod"}) != 0) {
        if (error) *error = "couldn't stop coreaudiod (killall failed)";
        return false;
    }
    // Talk to Core Audio only once the old daemon is gone, or this process
    // connects to it and loses the device list as it exits
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (old > 0 && kill(old, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool waitForDevice(std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (findDevice()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

bool setDefaultOutput(std::string* error) {
    const auto device = findDevice();
    if (!device) {
        if (error) *error = "AES67 Device not found";
        return false;
    }
    AudioObjectID id = *device;
    const AudioObjectPropertyAddress address = defaultOutputAddress();
    const OSStatus status =
        AudioObjectSetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, sizeof(id), &id);
    if (status != noErr) {
        if (error) *error = "Core Audio refused the default output change (" + std::to_string(status) + ")";
        return false;
    }
    return true;
}

bool isDefaultOutput() {
    const auto device = findDevice();
    if (!device) return false;
    AudioObjectID current = kAudioObjectUnknown;
    UInt32 size = sizeof(current);
    const AudioObjectPropertyAddress address = defaultOutputAddress();
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &current) == noErr &&
           current == *device;
}

std::string coreAudioStartTime() {
    std::string pidText;
    if (run({"/usr/bin/pgrep", "-x", "coreaudiod"}, &pidText) != 0) return "";
    const pid_t pid = static_cast<pid_t>(std::atoi(pidText.c_str()));
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kinfo_proc info{};
    size_t size = sizeof(info);
    if (pid <= 0 || sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return "";
    const time_t started = info.kp_proc.p_starttime.tv_sec;
    tm local{};
    localtime_r(&started, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::string lastPtpLockChange(const std::string& start) {
    if (start.empty()) return "";
    std::string output;
    run({"/usr/bin/log", "show", "--start", start, "--style", "compact", "--predicate",
         "subsystem == \"com.aes67driver\" AND "
         "(composedMessage BEGINSWITH \"PTP: locked\" OR composedMessage BEGINSWITH \"PTP: not locked\")"},
        &output);
    std::string last;
    size_t from = 0;
    while (from < output.size()) {
        const size_t end = output.find('\n', from);
        const std::string line = output.substr(from, end == std::string::npos ? std::string::npos : end - from);
        const size_t at = line.find("PTP: ");
        if (at != std::string::npos) last = line.substr(at + 5);
        if (end == std::string::npos) break;
        from = end + 1;
    }
    return last;
}

std::string shortHostName() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return "Mac";
    std::string text = name;
    const size_t dot = text.find('.');
    if (dot != std::string::npos) text.resize(dot);
    return text.empty() ? "Mac" : text;
}

}  // namespace Ctl
}  // namespace AES67
