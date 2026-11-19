/// @file PtpClockSource.cpp

#include "PtpClockSource.h"
#include "HostTime.h"
#include "../../Driver/DebugLog.h"

namespace AES67 {

PtpClockSource::PtpClockSource(Config config, const MediaClock& clock, const std::atomic<double>& sampleRate,
                               RateWriter writer, ChangeListener listener, OffsetWriter offsetWriter)
    : config_(std::move(config)),
      clock_(clock),
      sampleRate_(sampleRate),
      writer_(std::move(writer)),
      listener_(std::move(listener)),
      offsetWriter_(std::move(offsetWriter)),
      receiver_(config_.receiver),
      control_(config_.control) {}

PtpClockSource::~PtpClockSource() { stop(); }

uint32_t PtpClockSource::clockDomainFor(const Ptp::ClockIdentity& grandmaster, uint8_t domain) {
    // FNV-1a over the identity and domain; 0 means "no domain" to Core Audio
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 16777619u;
    };
    for (const uint8_t byte : grandmaster.bytes) mix(byte);
    mix(domain);
    return hash == 0 ? 1 : hash;
}

void PtpClockSource::start() {
    if (thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = false;
    }
    monitor_ = std::make_unique<NetworkMonitor>(config_.monitor, [this](const std::string&) {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            interfaceChanged_ = true;
        }
        wake_.notify_all();
    });
    monitor_->watch(config_.receiver.networkInterface);
    thread_ = std::thread([this] { run(); });
}

void PtpClockSource::stop() {
    monitor_.reset();  // no more changes reported
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    receiver_.stop();
    receiving_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    locked_.store(false, std::memory_order_release);
    clockDomain_.store(0, std::memory_order_release);
}

PtpClockSource::Status PtpClockSource::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void PtpClockSource::run() {
    std::unique_lock<std::mutex> lock(wakeMutex_);
    while (!stopping_) {
        lock.unlock();
        if (interfaceChanged_.exchange(false)) {
            restartReceiver();
        }
        if (!receiving_.load(std::memory_order_relaxed) && interfaceExists() && receiver_.start()) {
            receiving_.store(true, std::memory_order_release);
            AES67_LOGF("PTP: receiving on '%s', domain %u, as %s", interfaceName().c_str(),
                       static_cast<unsigned>(config_.receiver.domain), receiver_.identity().clock.toString().c_str());
        }
        const bool receiving = receiving_.load(std::memory_order_relaxed);
        if (receiving) {
            step();
        }
        lock.lock();
        wake_.wait_for(lock, receiving ? config_.period : config_.retryPeriod,
                       [this] { return stopping_ || interfaceChanged_.load(); });
    }
}

bool PtpClockSource::interfaceExists() const {
    const std::string& setting = config_.receiver.networkInterface;
    return config_.monitor.provider ? config_.monitor.provider(setting).has_value()
                                    : NetworkInterfaceDetection::currentState(setting).has_value();
}

void PtpClockSource::restartReceiver() {
    if (!receiving_.load(std::memory_order_relaxed)) {
        return;  // not started yet: the next attempt uses the interface as it is
    }
    // Its sockets joined the interface as it was. Until it locks again the
    // clock keeps its rate, and Core Audio is told it is not locked.
    AES67_LOGF("PTP: interface '%s' changed: starting the receiver again", interfaceName().c_str());
    receiver_.stop();
    receiving_.store(false, std::memory_order_release);
    receiverRestarts_.fetch_add(1, std::memory_order_acq_rel);
    publishLock(false, 0);
}

void PtpClockSource::publishLock(bool locked, uint32_t domain) {
    // Both exchanged unconditionally: either may have changed
    const bool lockChanged = locked_.exchange(locked, std::memory_order_acq_rel) != locked;
    const bool domainChanged = clockDomain_.exchange(domain, std::memory_order_acq_rel) != domain;
    if ((lockChanged || domainChanged) && listener_) {
        listener_();
    }
}

void PtpClockSource::step() {
    const Ptp::TimeReceiver::Status received = receiver_.status();
    PtpReference reference;
    reference.following = received.estimate.has_value() && (received.state == Ptp::Servo::State::Locked ||
                                                             received.state == Ptp::Servo::State::Holdover);
    reference.generation = received.generation;
    if (received.estimate) {
        reference.estimate = *received.estimate;
    }

    const MediaClock::Snapshot clock = clock_.snapshot();
    const auto update = control_.update(clock, sampleRate_.load(), hostTimeNow(), reference);
    if (update.offsetChanged) {
        AES67_LOGF("PTP: media position = PTP time %+lld samples (timeline %u, PTP timeline %u, %.2f samples to slew)",
                   static_cast<long long>(*control_.offset()), clock.generation, received.generation,
                   control_.phaseErrorSamples());
        if (offsetWriter_) {
            offsetWriter_(clock.generation, *control_.offset());
        }
    }
    if (update.samplesPerTick) {
        writer_(clock.generation, *update.samplesPerTick);
    }

    const bool locked = reference.following;
    const uint32_t domain = locked && received.master
        ? clockDomainFor(received.master->announce.grandmasterIdentity, config_.receiver.domain)
        : 0;
    active_.store(control_.active(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.receiver = received;
        status_.active = control_.active();
        status_.offset = control_.offset();
        status_.phaseErrorSamples = control_.phaseErrorSamples();
    }
    if (locked != locked_.load(std::memory_order_acquire)) {
        if (locked && received.master) {
            AES67_NOTICEF("PTP: locked to grandmaster %s via %s, path delay %.1f us, rate %+.2f ppm",
                       received.master->announce.grandmasterIdentity.toString().c_str(),
                       received.master->sender.clock.toString().c_str(), received.pathDelayNs / 1000.0,
                       received.estimate ? (received.estimate->rate - 1.0) * 1e6 : 0.0);
        } else if (!locked) {
            AES67_NOTICEF("PTP: not locked (%s); the clock keeps its rate", received.master ? "acquiring" : "no master");
        }
    }
    publishLock(locked, domain);
}

std::string PtpClockSource::interfaceName() const {
    return config_.receiver.networkInterface.empty() ? "primary" : config_.receiver.networkInterface;
}

} // namespace AES67
