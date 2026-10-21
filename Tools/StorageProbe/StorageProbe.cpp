//
// StorageProbe.cpp
// AES67 macOS Driver - issue #42
//
// A throwaway AudioServerPlugIn that publishes no device. When Core Audio
// loads it into the driver host, it checks where the driver can keep its
// config: file operations in the config directory as the installer makes it
// (root:wheel 755), in one owned by _coreaudiod, and in a few other places,
// then the host's own storage (WriteToStorage). It uses no network.
// Results go to the unified log:
//
//   sudo /usr/bin/log show --last 5m --info --debug --predicate 'subsystem == "com.aes67driver.probe"'
//
// Setup and clean-up commands are in Docs/Spikes/S4-Config-Storage.md.
//

#include <aspl/Driver.hpp>
#include <aspl/DriverRequestHandler.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Storage.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <fcntl.h>
#include <os/log.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

os_log_t probeLog() {
    static os_log_t log = os_log_create("com.aes67driver.probe", "storage");
    return log;
}

// Every result is also kept and written to kResultsPath at the end: the
// unified log can only show these messages while the probe's binary is still
// installed, and a run removes it.
constexpr const char* kResultsPath = "/tmp/aes67-storage-probe.txt";
std::string results;

void note(const char* format, ...) __attribute__((format(printf, 1, 2)));
void note(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    os_log(probeLog(), "%{public}s", line);
    results += line;
    results += '\n';
}

void logResult(const std::string& step, bool ok, int err) {
    if (ok) {
        note("%s: PASS", step.c_str());
    } else {
        note("%s: FAIL errno=%d (%s)", step.c_str(), err, strerror(err));
    }
}

void probeIdentity() {
    const char* home = std::getenv("HOME");
    const passwd* pw = getpwuid(geteuid());
    char tempDir[1024] = {};
    confstr(_CS_DARWIN_USER_TEMP_DIR, tempDir, sizeof(tempDir));
    note("identity: pid=%d uid=%d euid=%d process=%s HOME=%s pw_dir=%s temp=%s",
              getpid(), getuid(), geteuid(), getprogname(), home ? home : "(unset)",
              pw ? pw->pw_dir : "(none)", tempDir);
}

/// The steps StreamConfigManager takes to save streams.json, in `dir`.
void probeDirectory(const std::string& dir) {
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0) {
        logResult(dir + ": stat", false, errno);
        return;
    }
    note("%s: owner uid=%d gid=%d mode=%o", dir.c_str(), st.st_uid, st.st_gid, st.st_mode & 07777);

    // Reading what is already there (streams.json, if the setup made one)
    const std::string existing = dir + "/streams.json";
    if (stat(existing.c_str(), &st) == 0) {
        const int fd = open(existing.c_str(), O_RDONLY);
        logResult(dir + ": read streams.json", fd >= 0, errno);
        if (fd >= 0) close(fd);
        // The old save: truncate and write in place
        const int wfd = open(existing.c_str(), O_WRONLY | O_APPEND);
        logResult(dir + ": open streams.json for writing in place", wfd >= 0, errno);
        if (wfd >= 0) close(wfd);
    }

    // The new save: temp file, write, flush, rename
    std::string temp = dir + "/probe.json.XXXXXX";
    const int fd = mkostemp(temp.data(), O_CLOEXEC);
    logResult(dir + ": create temp file", fd >= 0, errno);
    if (fd >= 0) {
        const char text[] = "{\"probe\": true}\n";
        logResult(dir + ": write", write(fd, text, sizeof(text) - 1) == static_cast<ssize_t>(sizeof(text) - 1), errno);
        logResult(dir + ": F_FULLFSYNC", fcntl(fd, F_FULLFSYNC) == 0, errno);
        close(fd);
        const std::string target = dir + "/probe.json";
        logResult(dir + ": rename over probe.json", rename(temp.c_str(), target.c_str()) == 0, errno);
        unlink(temp.c_str());
        logResult(dir + ": remove probe.json", unlink(target.c_str()) == 0, errno);
    }

    const std::string sub = dir + "/probe-subdir";
    logResult(dir + ": mkdir", mkdir(sub.c_str(), 0755) == 0, errno);
    rmdir(sub.c_str());
}

void probeHostStorage(const std::shared_ptr<aspl::Storage>& storage) {
    // About the size of a streams.json with 64 streams
    std::string value(64 * 1024, 'x');
    value.replace(0, 7, "{probe:");
    logResult("host storage: write 64 KB string", storage->WriteString("probe", value), 0);
    const auto [read, ok] = storage->ReadString("probe");
    logResult("host storage: read it back", ok && read == value, 0);
    logResult("host storage: delete", storage->Delete("probe"), 0);
}

void runProbe(std::shared_ptr<aspl::Storage> storage) {
    note("=== storage probe start ===");
    probeIdentity();

    probeDirectory("/Library/Application Support/AES67Driver");        // root:wheel 755, as installed
    probeDirectory("/Library/Application Support/AES67Driver-owned");  // chown _coreaudiod (the #42 fix)
    probeDirectory("/Library/Application Support");
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') probeDirectory(home);
    char tempDir[1024] = {};
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, tempDir, sizeof(tempDir)) > 0) probeDirectory(tempDir);
    probeDirectory("/tmp");

    probeHostStorage(storage);
    note("=== storage probe end ===");

    const int fd = open(kResultsPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        write(fd, results.data(), results.size());
        close(fd);
    }
}

/// The host's storage only works once the host has initialised the driver
class ProbeHandler : public aspl::DriverRequestHandler {
public:
    explicit ProbeHandler(std::shared_ptr<aspl::Storage> storage) : storage_(std::move(storage)) {}

    OSStatus OnInitialize() override {
        // Off the HAL's thread so plug-in loading is never delayed
        static std::once_flag started;
        auto storage = storage_;
        std::call_once(started, [storage] { std::thread(runProbe, storage).detach(); });
        return kAudioHardwareNoError;
    }

private:
    std::shared_ptr<aspl::Storage> storage_;
};

} // namespace

extern "C" void* AES67StorageProbeCreate(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    static std::shared_ptr<aspl::Driver> driver = [] {
        auto context = std::make_shared<aspl::Context>();
        auto plugin = std::make_shared<aspl::Plugin>(context);
        auto driver = std::make_shared<aspl::Driver>(context, plugin);
        static ProbeHandler handler(driver->GetStorage());
        driver->SetDriverHandler(&handler);
        return driver;
    }();

    return driver->GetReference();
}
