/// @file PtpTimeReceiver.h
/// @brief A PTP time receiver (slave-only ordinary clock) on real sockets.

#pragma once

#include "PtpBestMaster.h"
#include "PtpMessages.h"
#include "PtpServo.h"
#include "PtpSocket.h"
#include "PtpSyncPairer.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace AES67 {
namespace Ptp {

/// Listens for a PTP master on one interface and models its clock in host
/// time. Announces pick the master (BestMaster); Sync and Follow_Up give
/// t1 and t2, Delay_Req and Delay_Resp give t3 and t4 (end-to-end); the
/// Servo turns them into a line. Delay_Req goes to the PTP group, or to the
/// master's own address in hybrid mode.
///
/// Receive times are kernel timestamps. Send times (t3) are taken just
/// before the send: macOS has no transmit timestamps, and an early t3 only
/// makes a delay look longer, which the servo's minimum filter discards.
///
/// One thread does all the work; status() may be called from any thread.
class TimeReceiver {
public:
    struct Config {
        std::string networkInterface;      // as streams store it: "en0", an address, or "" (auto)
        uint8_t domain{0};
        std::string group{"224.0.1.129"};
        uint16_t eventPort{319};
        uint16_t generalPort{320};
        bool unicastDelayRequests{false};  // hybrid mode
        uint8_t ttl{1};
        std::optional<ClockIdentity> identity;  // default: from the interface's MAC address
        Servo::Config servo;
        BestMaster::Config bestMaster;     // domain and self are set from the fields above
    };

    struct Status {
        Servo::State state{Servo::State::Acquiring};
        uint32_t generation{0};
        std::optional<Servo::Estimate> estimate;
        std::optional<Candidate> master;
        double pathDelayNs{0.0};
        uint64_t syncs{0};
        uint64_t followUps{0};
        uint64_t delayRequests{0};
        uint64_t delayResponses{0};
        uint64_t announces{0};
    };

    explicit TimeReceiver(Config config);
    ~TimeReceiver();
    TimeReceiver(const TimeReceiver&) = delete;
    TimeReceiver& operator=(const TimeReceiver&) = delete;

    /// Open the sockets and start the thread.
    /// @return false if the interface does not exist or a socket cannot be opened.
    bool start();
    void stop();

    Status status() const;
    PortIdentity identity() const { return self_; }

private:
    void run();
    void drain(Socket& socket);
    void handle(const Message& message, uint64_t hostReceiveNs, uint32_t sourceAddress);
    void onAnnounce(const Message& message, uint32_t sourceAddress);
    void onSync(const Message& message, uint64_t hostReceiveNs);
    void onFollowUp(const Message& message);
    void onDelayResp(const Message& message);
    void masterChanged();
    void sendDelayRequestIfDue(uint64_t nowNs);
    void scheduleDelayRequest(uint64_t fromNs);
    bool fromSelectedMaster(const Message& message) const;
    void publish();

    static ClockIdentity identityFor(const std::string& interfaceName);

    const Config config_;
    PortIdentity self_;
    Socket event_;
    Socket general_;

    // Thread-owned state
    std::optional<BestMaster> bestMaster_;
    std::optional<Servo> servo_;
    uint32_t generationBase_{0};  // timelines of earlier runs, so numbers keep rising across restarts
    std::optional<PortIdentity> master_;       // the selected master
    std::optional<PortIdentity> servoMaster_;  // whose Syncs the servo holds
    std::map<PortIdentity, uint32_t> addresses_;  // each master's IPv4 address
    SyncPairer pairer_;
    uint16_t delaySequence_{0};
    std::optional<uint64_t> delaySentNs_;  // t3 of the outstanding Delay_Req
    int8_t logDelayInterval_{0};
    uint64_t nextDelayNs_{0};
    std::mt19937_64 random_;
    Status counters_;

    mutable std::mutex statusMutex_;
    Status status_;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace Ptp
} // namespace AES67
