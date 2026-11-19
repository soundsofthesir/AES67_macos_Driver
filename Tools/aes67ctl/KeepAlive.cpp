/// @file KeepAlive.cpp

#include "KeepAlive.h"
#include "System.h"
#include <CoreAudio/CoreAudio.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace AES67 {
namespace Ctl {

namespace {

constexpr auto kPollInterval = std::chrono::seconds(5);

OSStatus writeSilence(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                      AudioBufferList* output, const AudioTimeStamp*, void*) {
    if (output) {
        for (UInt32 i = 0; i < output->mNumberBuffers; ++i) {
            if (output->mBuffers[i].mData) std::memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
        }
    }
    return noErr;
}

// Don't take input: it needs microphone permission and isn't needed to keep
// IO running
void disableInput(AudioObjectID device, AudioDeviceIOProcID proc) {
    const AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &size) != noErr) return;
    const UInt32 count = size / sizeof(AudioStreamID);
    if (count == 0) return;

    std::vector<UInt8> storage(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) + count * sizeof(UInt32));
    auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(storage.data());
    usage->mIOProc = reinterpret_cast<void*>(proc);
    usage->mNumberStreams = count;
    for (UInt32 i = 0; i < count; ++i) usage->mStreamIsOn[i] = 0;
    const AudioObjectPropertyAddress address{kAudioDevicePropertyIOProcStreamUsage, kAudioObjectPropertyScopeInput,
                                             kAudioObjectPropertyElementMain};
    const OSStatus status =
        AudioObjectSetPropertyData(device, &address, 0, nullptr, static_cast<UInt32>(storage.size()), storage.data());
    if (status != noErr) std::cerr << "keepalive: couldn't turn input off (" << status << ")" << std::endl;
}

bool isRunning(AudioObjectID device) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    UInt32 running = 0;
    UInt32 size = sizeof(running);
    return AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &running) == noErr && running != 0;
}

class SilentIO {
public:
    ~SilentIO() { stop(); }

    bool running() const { return proc_ != nullptr; }
    AudioObjectID device() const { return device_; }

    bool start(AudioObjectID device) {
        AudioDeviceIOProcID proc = nullptr;
        if (AudioDeviceCreateIOProcID(device, writeSilence, nullptr, &proc) != noErr || !proc) return false;
        disableInput(device, proc);
        if (AudioDeviceStart(device, proc) != noErr) {
            AudioDeviceDestroyIOProcID(device, proc);
            return false;
        }
        device_ = device;
        proc_ = proc;
        return true;
    }

    void stop() {
        if (!proc_) return;
        AudioDeviceStop(device_, proc_);
        AudioDeviceDestroyIOProcID(device_, proc_);
        proc_ = nullptr;
        device_ = kAudioObjectUnknown;
    }

private:
    AudioObjectID device_{kAudioObjectUnknown};
    AudioDeviceIOProcID proc_{nullptr};
};

}  // namespace

int keepAlive(const std::function<std::optional<bool>()>& wantsIO) {
    SilentIO io;
    bool wanted = false;
    for (;;) {
        if (const auto answer = wantsIO()) wanted = *answer;
        const auto device = findDevice();

        // Not wanted any more, the driver is gone, or coreaudiod restarted
        // (a new device ID, or the same one no longer running)
        if (io.running() && (!wanted || !device || *device != io.device() || !isRunning(*device))) {
            io.stop();
            std::cout << "keepalive: stopped" << std::endl;
        }
        if (wanted && device && !io.running()) {
            if (io.start(*device)) {
                std::cout << "keepalive: running IO on the AES67 device" << std::endl;
            } else {
                std::cerr << "keepalive: couldn't start IO on the AES67 device" << std::endl;
            }
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

}  // namespace Ctl
}  // namespace AES67
