//
// TestDeviceChannelCount.cpp
// AES67 macOS Driver
// Regression test for issue #152: the 128-channel device advertised a stereo
// default because aspl::DeviceParameters::ChannelCount was never set (libASPL
// defaults it to 2).
//
// libASPL and CoreAudio aren't available on Linux, so this checks the source
// of Driver/AES67Device.cpp rather than constructing the device. It runs
// anywhere a C++17 compiler and a file system are available.
//

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    // __FILE__ is Tests/TestDeviceChannelCount.cpp, so the repo root is two levels up
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const std::string device = ReadFile(root / "Driver" / "AES67Device.cpp");
    const std::string header = ReadFile(root / "Driver" / "AES67Device.h");

    Check(!device.empty(), "could not read Driver/AES67Device.cpp");
    Check(!header.empty(), "could not read Driver/AES67Device.h");

    // The device's channel count is the kNumChannels constant (128)
    Check(header.find("kNumChannels = 128") != std::string::npos,
          "kNumChannels is not 128 in Driver/AES67Device.h");

    // The DeviceParameters initializer must set ChannelCount, or libASPL
    // advertises a 2-channel device to Core Audio clients
    const std::string paramsStart = "aspl::DeviceParameters{";
    const auto start = device.find(paramsStart);
    Check(start != std::string::npos, "aspl::DeviceParameters initializer not found in AES67Device.cpp");
    if (start != std::string::npos) {
        const auto end = device.find("})", start);
        Check(end != std::string::npos, "end of aspl::DeviceParameters initializer not found");
        if (end != std::string::npos) {
            const std::string params = device.substr(start, end - start);
            Check(params.find(".ChannelCount = kNumChannels") != std::string::npos,
                  "ChannelCount is not set in aspl::DeviceParameters (device would report 2 channels)");
        }
    }

    if (failures == 0) {
        std::printf("PASS: device advertises kNumChannels channels\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
