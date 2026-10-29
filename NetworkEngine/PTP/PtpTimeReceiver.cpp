/// @file PtpTimeReceiver.cpp

#include "PtpTimeReceiver.h"
#include "../Clock/HostTime.h"
#include "../NetworkInterfaceDetection.h"
#include <arpa/inet.h>
#include <poll.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AES67 {
namespace Ptp {

namespace {

constexpr int kPollMilliseconds = 20;     // how often silent masters are checked for
constexpr int8_t kMinLogDelayInterval = -7;
constexpr int8_t kMaxLogDelayInterval = 6;
constexpr int8_t kUnspecifiedLogInterval = 0x7F;

uint64_t nowNanos() { return HostTimebase::current().ticksToNanos(hostTimeNow()); }

double seconds(uint64_t nanos) { return static_cast<double>(nanos) / 1e9; }

} // namespace

TimeReceiver::TimeReceiver(Config config) : config_(std::move(config)), random_(std::random_device{}()) {}

TimeReceiver::~TimeReceiver() { stop(); }

ClockIdentity TimeReceiver::identityFor(const std::string& interfaceName) {
    // EUI-48 to EUI-64: FF-FE in the middle (IEEE 1588-2008 7.5.2.2.2)
    const std::string mac = NetworkInterfaceDetection::getInterfaceMACAddress(interfaceName);
    unsigned b[6];
    ClockIdentity identity;
    if (std::sscanf(mac.c_str(), "%2X-%2X-%2X-%2X-%2X-%2X", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        identity.bytes = {static_cast<uint8_t>(b[0]), static_cast<uint8_t>(b[1]), static_cast<uint8_t>(b[2]), 0xFF,
                          0xFE, static_cast<uint8_t>(b[3]), static_cast<uint8_t>(b[4]), static_cast<uint8_t>(b[5])};
        return identity;
    }
    // No hardware address: a random one, marked locally administered
    std::random_device device;
    for (auto& byte : identity.bytes) byte = static_cast<uint8_t>(device());
    identity.bytes[0] = static_cast<uint8_t>((identity.bytes[0] | 0x02) & 0xFE);
    return identity;
}

bool TimeReceiver::start() {
    if (running_) {
        return true;
    }
    unsigned index = 0;
    std::string description;
    if (!NetworkInterfaceDetection::socketInterfaceIndex(config_.networkInterface, index, description)) {
        return false;
    }
    std::string interfaceName = description;
    if (const auto state = NetworkInterfaceDetection::currentState(config_.networkInterface)) {
        interfaceName = state->name;
    }
    self_ = PortIdentity{config_.identity ? *config_.identity : identityFor(interfaceName), 1};

    if (!event_.open(Socket::Options{config_.eventPort, "", config_.group, index, config_.ttl}) ||
        !general_.open(Socket::Options{config_.generalPort, "", config_.group, index, config_.ttl})) {
        event_.close();
        general_.close();
        return false;
    }

    BestMaster::Config bestMaster = config_.bestMaster;
    bestMaster.domain = config_.domain;
    bestMaster.self = self_.clock;
    bestMaster_.emplace(bestMaster);
    if (servo_) {
        generationBase_ += servo_->generation();
    }
    servo_.emplace(config_.servo);
    master_.reset();
    servoMaster_.reset();
    addresses_.clear();
    pairer_.reset();
    delaySentNs_.reset();
    logDelayInterval_ = 0;
    counters_ = Status{};
    publish();

    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void TimeReceiver::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    event_.close();
    general_.close();
}

TimeReceiver::Status TimeReceiver::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void TimeReceiver::publish() {
    Status s = counters_;
    if (servo_) {
        s.state = servo_->state();
        s.generation = generationBase_ + servo_->generation();
        s.estimate = servo_->estimate();
        s.pathDelayNs = servo_->pathDelayNs();
    }
    if (bestMaster_) {
        s.master = bestMaster_->selected();
    }
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = s;
}

void TimeReceiver::run() {
    while (running_) {
        pollfd fds[2] = {{event_.fd(), POLLIN, 0}, {general_.fd(), POLLIN, 0}};
        ::poll(fds, 2, kPollMilliseconds);
        // Event first: a Sync is handled before its Follow_Up where both wait
        drain(event_);
        drain(general_);
        const uint64_t now = nowNanos();
        if (bestMaster_->tick(seconds(now))) {
            masterChanged();
        }
        sendDelayRequestIfDue(now);
        publish();
    }
}

void TimeReceiver::drain(Socket& socket) {
    uint8_t buffer[1500];  // an Ethernet frame: Announces may carry TLVs
    uint64_t ticks = 0;
    uint32_t source = 0;
    long received;
    while ((received = socket.receive(buffer, sizeof(buffer), ticks, source)) > 0) {
        const auto message = parse(buffer, static_cast<size_t>(received));
        if (!message) continue;
        const uint64_t receiveNs = ticks != 0 ? HostTimebase::current().ticksToNanos(ticks) : nowNanos();
        handle(*message, receiveNs, source);
    }
}

void TimeReceiver::handle(const Message& message, uint64_t hostReceiveNs, uint32_t sourceAddress) {
    if (message.header.domain != config_.domain || message.header.source.clock == self_.clock) {
        return;  // another domain, or our own Delay_Req looped back
    }
    switch (message.header.type) {
        case MessageType::Announce: onAnnounce(message, sourceAddress); break;
        case MessageType::Sync: onSync(message, hostReceiveNs); break;
        case MessageType::FollowUp: onFollowUp(message); break;
        case MessageType::DelayResp: onDelayResp(message); break;
        default: break;
    }
}

void TimeReceiver::onAnnounce(const Message& message, uint32_t sourceAddress) {
    ++counters_.announces;
    addresses_[message.header.source] = sourceAddress;
    if (bestMaster_->onAnnounce(message, seconds(nowNanos()))) {
        masterChanged();
    }
}

void TimeReceiver::masterChanged() {
    const auto selected = bestMaster_->selected();
    master_ = selected ? std::optional<PortIdentity>(selected->sender) : std::nullopt;
    pairer_.reset();
    delaySentNs_.reset();
    if (!master_) {
        servo_->holdover();  // keep the line until a master returns
        return;
    }
    if (servoMaster_ && *servoMaster_ != *master_) {
        servo_->reset();  // a different master's time: a new timeline
        servoMaster_.reset();
    }
    logDelayInterval_ = 0;
    scheduleDelayRequest(nowNanos());
}

bool TimeReceiver::fromSelectedMaster(const Message& message) const {
    return master_ && message.header.source == *master_;
}

void TimeReceiver::onSync(const Message& message, uint64_t hostReceiveNs) {
    if (!fromSelectedMaster(message)) return;
    ++counters_.syncs;
    const double correction = message.header.correctionNanoseconds();
    if (!message.header.twoStep()) {
        servo_->onSync(hostReceiveNs, message.timestamp.totalNanoseconds(), correction);
        servoMaster_ = master_;
        return;
    }
    if (const auto sample = pairer_.onSync(message.header.sequenceId, hostReceiveNs, correction)) {
        servo_->onSync(sample->hostReceiveNs, sample->preciseOriginNs, sample->correctionNs);
        servoMaster_ = master_;
    }
}

void TimeReceiver::onFollowUp(const Message& message) {
    if (!fromSelectedMaster(message)) return;
    ++counters_.followUps;
    const uint64_t precise = message.timestamp.totalNanoseconds();
    const double correction = message.header.correctionNanoseconds();
    if (const auto sample = pairer_.onFollowUp(message.header.sequenceId, precise, correction)) {
        servo_->onSync(sample->hostReceiveNs, sample->preciseOriginNs, sample->correctionNs);
        servoMaster_ = master_;
    }
}

void TimeReceiver::onDelayResp(const Message& message) {
    if (!fromSelectedMaster(message) || !message.requestingPort || *message.requestingPort != self_ ||
        !delaySentNs_ || message.header.sequenceId != static_cast<uint16_t>(delaySequence_ - 1)) {
        return;
    }
    ++counters_.delayResponses;
    servo_->onDelay(*delaySentNs_, message.timestamp.totalNanoseconds(), message.header.correctionNanoseconds());
    delaySentNs_.reset();
    // The master says how often it wants Delay_Req (logMinDelayReqInterval)
    const int8_t log = message.header.logMessageInterval;
    if (log != kUnspecifiedLogInterval) {
        logDelayInterval_ = std::clamp(log, kMinLogDelayInterval, kMaxLogDelayInterval);
    }
}

void TimeReceiver::scheduleDelayRequest(uint64_t fromNs) {
    // Spread around the interval so many receivers do not send in step
    std::uniform_real_distribution<double> spread(0.5, 1.5);
    nextDelayNs_ = fromNs + static_cast<uint64_t>(std::ldexp(1e9, logDelayInterval_) * spread(random_));
}

void TimeReceiver::sendDelayRequestIfDue(uint64_t nowNs) {
    if (!master_ || !servoMaster_ || nowNs < nextDelayNs_) {
        return;  // no point measuring the delay before there is a line to measure it against
    }
    const bool unicast = config_.unicastDelayRequests && addresses_.count(*master_) != 0;
    const auto request = buildDelayReq(self_, config_.domain, delaySequence_, unicast);
    const uint64_t t3 = nowNanos();
    const bool sent = unicast
        ? event_.sendTo(addresses_[*master_], config_.eventPort, request.data(), request.size())
        : event_.sendTo(config_.group, config_.eventPort, request.data(), request.size());
    ++delaySequence_;
    delaySentNs_ = sent ? std::optional<uint64_t>(t3) : std::nullopt;
    if (sent) ++counters_.delayRequests;
    scheduleDelayRequest(nowNs);
}

} // namespace Ptp
} // namespace AES67
