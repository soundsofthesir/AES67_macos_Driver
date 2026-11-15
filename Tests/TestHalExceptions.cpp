// Tests/TestHalExceptions.cpp
//
// #147: no C++ exception may leave a HAL callback. libASPL's bridge has no
// catch, so an exception from StartIO, StopIO or a rate change unwinds through
// the AudioServerPlugIn C ABI and terminates coreaudiod.
//
// This binary replaces the global operator new so a test can make the n-th
// allocation on its own thread throw std::bad_alloc. Sweeping n over every
// allocation a callback makes (std::thread's state among them) reaches each
// throw point without a seam in production code. The replacement lives in its
// own binary so no other suite runs with it.
//
// The callbacks are called as AES67Device's Impl overrides, the code this
// driver owns. libASPL's own wrappers around them (tracer, property-change
// notifications) allocate outside the driver's control.
//
// Host-only: the receive stream joins a group but nothing is sent to it, and
// the transmitter sends with TTL 0 on the loopback interface.

#include "../Driver/AES67Device.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../NetworkEngine/RTP/RTPTransmitter.h"
#include "../NetworkEngine/RTP/TxContext.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include "RxTestSupport.h"
#include <aspl/Context.hpp>
#include <aspl/ControlRequestHandler.hpp>
#include <aspl/Tracer.hpp>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

using namespace AES67;
using namespace AES67::TestSupport;

// ---------------------------------------------------------------------------
// Allocation failure injection
// ---------------------------------------------------------------------------
namespace {

// 0: off. n > 0: the n-th allocation on this thread from now throws.
thread_local long failCountdown = 0;
thread_local bool failureInjected = false;

void* allocate(std::size_t size) {
    if (failCountdown > 0 && --failCountdown == 0) {
        failureInjected = true;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

// Fail the n-th allocation this thread makes while in scope.
class FailNthAllocation {
public:
    explicit FailNthAllocation(long n) {
        failureInjected = false;
        failCountdown = n;
    }
    ~FailNthAllocation() { failCountdown = 0; }
    FailNthAllocation(const FailNthAllocation&) = delete;
    FailNthAllocation& operator=(const FailNthAllocation&) = delete;
};

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

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

// Every allocation in each callback is failed once; none takes this many
constexpr long kMaxAllocations = 2000;

// Point the config search at a file that doesn't exist: the device then starts
// with its built-in RX stream on 239.1.1.1:5004 (8 channels).
void useNoConfig(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp/") + "aes67_halexceptions_" + name + ".json";
    std::remove(path.c_str());
    setenv("AES67_CONFIG_PATH", path.c_str(), 1);
}

// A device whose rate change can be called directly (it is protected)
class TestDevice : public AES67Device {
public:
    using AES67Device::AES67Device;
    using AES67Device::SetNominalSampleRateImpl;
};

std::shared_ptr<TestDevice> makeDevice() {
    // A quiet tracer: the default one logs every libASPL operation
    auto tracer = std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Noop);
    auto device = std::make_shared<TestDevice>(std::make_shared<aspl::Context>(tracer));
    device->Initialize();
    return device;
}

// Receive routes the IO thread can see: one per running receiver
size_t publishedRoutes(AES67Device& device) {
    size_t routes = 0;
    device.GetRTInterface()->rxRouting().read([&](const RxRouting::Route&) { ++routes; });
    return routes;
}

size_t publishedRoutes(const RxRouting& routing) {
    size_t routes = 0;
    routing.read([&](const RxRouting::Route&) { ++routes; });
    return routes;
}

constexpr UInt32 kClient = 1;

// ---------------------------------------------------------------------------
// StartIO with every one of its allocations failing in turn. Each time no
// exception may escape; a failed start must leave the network side stopped
// and the device not running; and the next start must work.
// ---------------------------------------------------------------------------
void testStartIOAllocationFailures() {
    std::cout << "#147: StartIO turns a throw into an error and rolls back" << std::endl;
    useNoConfig("startio");
    auto device = makeDevice();
    CHECK(device->GetStreamManager()->getStreamCount() == 1, "the device should load its built-in RX stream");

    long n = 1;
    int escaped = 0;
    int refused = 0;
    int notRolledBack = 0;
    int noRestart = 0;
    for (; n <= kMaxAllocations; ++n) {
        OSStatus status = kAudioHardwareNoError;
        bool injected = false;
        try {
            FailNthAllocation fail(n);
            status = device->StartIOImpl(kClient, 0);
            injected = failureInjected;
        } catch (...) {
            injected = true;
            ++escaped;
        }
        if (!injected) {
            device->StopIOImpl(kClient, 0);
            break;  // StartIO made fewer than n allocations: every one was failed once
        }

        if (status != kAudioHardwareNoError) {
            ++refused;
            if (device->GetRTInterface()->isIORunning() || publishedRoutes(*device) != 0) {
                ++notRolledBack;
            }
        } else {
            device->StopIOImpl(kClient, 0);  // started anyway (the stream may be down): stop as the HAL would
        }

        // A clean start now has to bring the stream up
        const OSStatus again = device->StartIOImpl(kClient, 0);
        if (again != kAudioHardwareNoError || publishedRoutes(*device) != 1 ||
            !device->GetRTInterface()->isIORunning()) {
            ++noRestart;
        }
        device->StopIOImpl(kClient, 0);
    }

    CHECK(n > 1 && n <= kMaxAllocations, "the sweep should fail at least one allocation and then run clean (n=" << n << ")");
    CHECK(escaped == 0, "no exception may leave StartIOImpl (" << escaped << " of " << n - 1 << " failures escaped)");
    std::cout << "  " << n - 1 << " allocations failed, " << refused << " reported as an error" << std::endl;
    CHECK(notRolledBack == 0, "a failed StartIO must leave IO stopped and no receiver running (" << notRolledBack
                              << " did not)");
    CHECK(noRestart == 0, "the next StartIO should start the stream (" << noRestart << " of " << n - 1 << " did not)");
    CHECK(publishedRoutes(*device) == 0 && !device->GetRTInterface()->isIORunning(), "IO should end stopped");
}

// libASPL's control handler is the one hook into the base StartIO/StopIO.
class ThrowingControl : public aspl::ControlRequestHandler {
public:
    bool throwOnStart = false;
    bool throwOnStop = false;
    int stops = 0;

    OSStatus OnStartIO() override {
        if (throwOnStart) {
            throw std::runtime_error("start refused");
        }
        return kAudioHardwareNoError;
    }
    void OnStopIO() override {
        ++stops;
        if (throwOnStop) {
            throw std::runtime_error("stop refused");
        }
    }
};

// A throw from the base StartIO (after the network side started) is an error,
// and the receivers it started are stopped again.
void testBaseStartIOThrow() {
    std::cout << "#147: a throw in the base StartIO stops the network side" << std::endl;
    useNoConfig("basestart");
    auto device = makeDevice();
    auto control = std::make_shared<ThrowingControl>();
    device->SetControlHandler(control);

    control->throwOnStart = true;
    OSStatus status = kAudioHardwareNoError;
    bool escaped = false;
    try {
        status = device->StartIOImpl(kClient, 0);
    } catch (...) {
        escaped = true;
    }
    CHECK(!escaped, "the exception should not leave StartIOImpl");
    CHECK(status != kAudioHardwareNoError, "StartIOImpl should report an error");
    CHECK(publishedRoutes(*device) == 0, "the receiver started before the throw should be stopped");
    CHECK(!device->GetRTInterface()->isIORunning(), "IO should not be marked running");

    control->throwOnStart = false;
    CHECK(device->StartIOImpl(kClient, 0) == kAudioHardwareNoError && publishedRoutes(*device) == 1,
          "the next StartIO should start the stream");
    device->StopIOImpl(kClient, 0);
}

// A throw from the base StopIO is an error, not an exception, and the network
// side is already stopped by then.
void testBaseStopIOThrow() {
    std::cout << "#147: a throw in the base StopIO is an error" << std::endl;
    useNoConfig("basestop");
    auto device = makeDevice();
    auto control = std::make_shared<ThrowingControl>();
    device->SetControlHandler(control);
    CHECK(device->StartIOImpl(kClient, 0) == kAudioHardwareNoError, "IO should start");

    control->throwOnStop = true;
    OSStatus status = kAudioHardwareNoError;
    bool escaped = false;
    try {
        status = device->StopIOImpl(kClient, 0);
    } catch (...) {
        escaped = true;
    }
    CHECK(!escaped, "the exception should not leave StopIOImpl");
    CHECK(status != kAudioHardwareNoError, "StopIOImpl should report an error");
    CHECK(control->stops == 1, "the base StopIO should have run");
    CHECK(publishedRoutes(*device) == 0 && !device->GetRTInterface()->isIORunning(),
          "the network side should be stopped");
}

// ---------------------------------------------------------------------------
// Rate change with every one of its allocations failing in turn. No exception
// may escape, and a failed change must leave the HAL's nominal rate, the
// driver, StreamManager and both streams' formats on the old rate.
// ---------------------------------------------------------------------------
bool allAt(TestDevice& device, Float64 rate) {
    return device.GetNominalSampleRate() == rate && device.GetSampleRate() == rate &&
           device.GetStreamManager()->getDeviceSampleRate() == rate &&
           device.GetInputStream()->GetPhysicalFormat().mSampleRate == rate &&
           device.GetInputStream()->GetVirtualFormat().mSampleRate == rate &&
           device.GetOutputStream()->GetPhysicalFormat().mSampleRate == rate &&
           device.GetOutputStream()->GetVirtualFormat().mSampleRate == rate;
}

void testSampleRateAllocationFailures() {
    std::cout << "#147: a rate change turns a throw into an error and keeps the old rate" << std::endl;
    useNoConfig("rate");
    auto device = makeDevice();
    device->GetStreamManager()->removeAllStreams();  // no stream pins the rate
    CHECK(allAt(*device, 48000.0), "the device should start at 48 kHz");

    long n = 1;
    int escaped = 0;
    int refused = 0;
    int inconsistent = 0;
    int noRetry = 0;
    for (; n <= kMaxAllocations; ++n) {
        OSStatus status = kAudioHardwareNoError;
        bool injected = false;
        try {
            FailNthAllocation fail(n);
            status = device->SetNominalSampleRateImpl(96000.0);
            injected = failureInjected;
        } catch (...) {
            injected = true;
            ++escaped;
        }
        if (!injected) {
            break;
        }

        if (status != kAudioHardwareNoError) {
            ++refused;
        }
        if (!allAt(*device, status == kAudioHardwareNoError ? 96000.0 : 48000.0)) {
            ++inconsistent;
        }
        device->SetNominalSampleRateImpl(48000.0);

        // Without a failure the change goes through, and back
        if (device->SetNominalSampleRateImpl(96000.0) != kAudioHardwareNoError || !allAt(*device, 96000.0)) {
            ++noRetry;
        }
        device->SetNominalSampleRateImpl(48000.0);
    }

    CHECK(n > 1 && n <= kMaxAllocations, "the sweep should fail at least one allocation and then run clean (n=" << n << ")");
    CHECK(escaped == 0, "no exception may leave SetNominalSampleRateImpl (" << escaped << " of " << n - 1
                        << " failures escaped)");
    std::cout << "  " << n - 1 << " allocations failed, " << refused << " reported as an error" << std::endl;
    CHECK(refused > 0, "some failures should be reported as an error");
    CHECK(inconsistent == 0, "after each failure everything should be on one rate, 48 kHz if refused (" << inconsistent
                             << " of " << n - 1 << " were not)");
    CHECK(noRetry == 0, "a later rate change should work (" << noRetry << " did not)");
}

// ---------------------------------------------------------------------------
// The receiver and transmitter on their own: whatever allocation fails during
// start(), nothing is left half started, and the next start works.
// ---------------------------------------------------------------------------
SDPSession makeSdp(const char* group, uint16_t port, const char* direction) {
    SDPSession sdp;
    sdp.sessionName = "HAL exceptions";
    sdp.connectionAddress = group;
    sdp.port = port;
    sdp.numChannels = 2;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = 97;
    sdp.ptime = 1;
    sdp.framecount = 48;
    sdp.direction = direction;
    sdp.ttl = 0;  // host-only
    return sdp;
}

ChannelMapping makeMapping() {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "HAL exceptions";
    mapping.streamChannelCount = 2;
    mapping.deviceChannelStart = 0;
    mapping.deviceChannelCount = 2;
    return mapping;
}

void testReceiverStartAllocationFailures() {
    std::cout << "#147: a receiver start that throws leaves nothing behind" << std::endl;
    RxHarness harness;
    RTPReceiver receiver(makeSdp("239.69.99.50", 55090, "recvonly"), makeMapping(), harness.context());

    long n = 1;
    int leftBehind = 0;
    int noRetry = 0;
    for (; n <= kMaxAllocations; ++n) {
        bool started = false;
        bool injected = false;
        try {
            FailNthAllocation fail(n);
            started = receiver.start();
            injected = failureInjected;
        } catch (...) {
            injected = true;  // before anything was opened: StartIO's guard catches it
        }
        if (!injected) {
            receiver.stop();
            break;
        }
        if (started) {
            receiver.stop();
        } else if (receiver.isRunning() || publishedRoutes(harness.routing) != 0) {
            ++leftBehind;
        }
        if (!receiver.start() || publishedRoutes(harness.routing) != 1) {
            ++noRetry;
        }
        receiver.stop();
    }

    std::cout << "  " << n - 1 << " allocations failed" << std::endl;
    CHECK(n > 1 && n <= kMaxAllocations, "the sweep should fail at least one allocation and then run clean (n=" << n << ")");
    CHECK(leftBehind == 0, "a failed start must not leave the receiver running or routed (" << leftBehind << ")");
    CHECK(noRetry == 0, "the next start should work (" << noRetry << " of " << n - 1 << " did not)");
}

void testTransmitterStartAllocationFailures() {
    std::cout << "#147: a transmitter start that throws leaves nothing behind" << std::endl;
    MediaClock clock;
    clock.reset(hostTimeNow(), 0, MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current()));
    TxRouting routing;
    // TTL 0 on the loopback interface: never leaves this machine
    RTPTransmitter transmitter(makeSdp("239.69.99.51", 55092, "sendonly"), makeMapping(), TxContext{clock, routing},
                               "127.0.0.1");

    auto routes = [&] {
        size_t count = 0;
        routing.read([&](const TxRouting::Route&) { ++count; });
        return count;
    };

    long n = 1;
    int leftBehind = 0;
    int noRetry = 0;
    for (; n <= kMaxAllocations; ++n) {
        bool started = false;
        bool injected = false;
        try {
            FailNthAllocation fail(n);
            started = transmitter.start();
            injected = failureInjected;
        } catch (...) {
            injected = true;
        }
        if (!injected) {
            transmitter.stop();
            break;
        }
        if (started) {
            transmitter.stop();
        } else if (transmitter.isRunning() || routes() != 0) {
            ++leftBehind;
        }
        if (!transmitter.start() || routes() != 1) {
            ++noRetry;
        }
        transmitter.stop();
    }

    std::cout << "  " << n - 1 << " allocations failed" << std::endl;
    CHECK(n > 1 && n <= kMaxAllocations, "the sweep should fail at least one allocation and then run clean (n=" << n << ")");
    CHECK(leftBehind == 0, "a failed start must not leave the transmitter running or routed (" << leftBehind << ")");
    CHECK(noRetry == 0, "the next start should work (" << noRetry << " of " << n - 1 << " did not)");
}

} // namespace

int main() {
    std::cout << "=== HAL exception tests (#147) ===" << std::endl;
    testReceiverStartAllocationFailures();
    testTransmitterStartAllocationFailures();
    testStartIOAllocationFailures();
    testBaseStartIOThrow();
    testBaseStopIOThrow();
    testSampleRateAllocationFailures();

    std::cout << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
