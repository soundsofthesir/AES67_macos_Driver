//
// PtpTestMaster.h
// AES67 macOS Driver
// A scripted PTP master on lo0 for tests: its own clock (an epoch and a
// skew against host time), Announce and Sync on a schedule, and answers to
// Delay_Req by multicast or unicast. Test ports and TTL 0, so nothing
// reaches the network.
//
// Two-step, its Follow_Up carries the kernel's time for the first copy of
// the Sync delivered on lo0, as a master's hardware stamps the time a Sync
// leaves. The kernel stamps each socket's copy as it delivers it, newest
// socket first, so the master opens a witness socket afresh before each
// Sync: its copy comes before any receiver's. A t1 later than a receiver's
// copy lies above the master's true line, where the receiver's
// upper-envelope fit rests on it. A shared CI runner can stall between
// copies for hundreds of us, which moved the estimate by +624 us and
// +46 ppm, and +1.4 ms and +120 ppm, while t1 came from the master's own
// socket, delivered after the receiver's. A time read before
// sending would include the send call itself, which takes 40 to 300 us
// depending on how fast the CPU is running, and Syncs whose fastest delay
// moves like that defeat the servo's lock test.
//

#pragma once

#include "../NetworkEngine/PTP/PtpMessages.h"
#include "../NetworkEngine/PTP/PtpSocket.h"
#include "../NetworkEngine/PTP/PtpTimeReceiver.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include <arpa/inet.h>
#include <poll.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <net/if.h>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace AES67 {
namespace PtpTest {

using namespace AES67::Ptp;

// Each test binary with a master and receiver on these ports gets its own
// pair (AES67_PTP_TEST_EVENT_PORT, set in Tests/CMakeLists.txt): ctest -j runs
// binaries side by side, and on a shared pair each master counts and answers
// the other binary's Delay_Req, and its Syncs reach the other's receiver (#79).
#ifndef AES67_PTP_TEST_EVENT_PORT
#define AES67_PTP_TEST_EVENT_PORT 31319
#endif
inline constexpr uint16_t kEventPort = AES67_PTP_TEST_EVENT_PORT;
inline constexpr uint16_t kGeneralPort = kEventPort + 1;
inline constexpr const char* kGroup = "224.0.1.129";

inline uint64_t hostNanos() { return HostTimebase::current().ticksToNanos(hostTimeNow()); }

// A PTP master on lo0. Its clock: epoch + (1 + skew) * host time.
class ScriptedMaster {
public:
    struct Settings {
        uint8_t identityByte{0x01};
        uint8_t priority1{128};
        uint64_t epochNs{261432ULL * 1000000000ULL};  // a console's power-on time
        double skewPpm{100.0};
        bool twoStep{true};
        int64_t followUpCorrectionNs{0};  // carried in Follow_Up (or Sync, one-step)
        int64_t delayRespCorrectionNs{0};
        int8_t logSyncInterval{-4};       // 16 per second
        int8_t logAnnounceInterval{-2};   // 4 per second
        int8_t logDelayReqInterval{-3};   // tells the receiver: 8 Delay_Req per second
        uint16_t eventPort{kEventPort};
        uint16_t generalPort{kGeneralPort};
    };

    explicit ScriptedMaster(Settings settings) : s_(settings) {
        identity_ = PortIdentity{ClockIdentity{{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x00, 0x00, s_.identityByte}}, 1};
        const unsigned lo0 = if_nametoindex("lo0");
        lo0_ = lo0;
        send_.open(Socket::Options{0, "", "", lo0, 0});
        multicastEvent_.open(Socket::Options{s_.eventPort, "", kGroup, lo0, 0});
        unicastEvent_.open(Socket::Options{s_.eventPort, "127.0.0.1", "", lo0, 0});
        thread_ = std::thread([this] { run(); });
    }
    ~ScriptedMaster() { stop(); }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    uint64_t clockAt(uint64_t hostNs) const {
        return s_.epochNs + static_cast<uint64_t>(std::llround(static_cast<double>(hostNs) * (1.0 + s_.skewPpm * 1e-6)));
    }
    const PortIdentity& identity() const { return identity_; }
    void stopSyncs() { sendingSyncs_ = false; }  // keeps announcing and answering Delay_Req
    uint64_t multicastDelayRequests() const { return multicastRequests_.load(); }
    uint64_t unicastDelayRequests() const { return unicastRequests_.load(); }

    /// For a failed test: how far apart the kernel put the two copies of
    /// each Sync. The Follow_Up carries the earlier (normally the witness's);
    /// the gap shows how long delivery stalled between sockets.
    std::string syncReport() const {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        std::vector<int64_t> sendCall, ownMinusWitness;
        size_t fallback = 0, noWitness = 0;
        struct Outlier { int64_t ahead; double atSeconds; uint16_t seq; };
        std::vector<Outlier> outliers;
        const uint64_t start = records_.empty() ? 0 : records_.front().preNs;
        for (const auto& r : records_) {
            sendCall.push_back(static_cast<int64_t>(r.postNs - r.preNs));
            if (r.ownNs == 0 && r.witnessNs == 0) ++fallback;
            if (r.ownNs == 0 || r.witnessNs == 0) { ++noWitness; continue; }
            const int64_t ahead = static_cast<int64_t>(r.ownNs - r.witnessNs);
            ownMinusWitness.push_back(ahead);
            outliers.push_back({ahead, static_cast<double>(r.preNs - start) / 1e9, r.seq});
        }
        auto summary = [](std::vector<int64_t> v) {
            std::ostringstream o;
            if (v.empty()) return std::string("none");
            std::sort(v.begin(), v.end());
            o << "min " << v.front() / 1000.0 << " us, median " << v[v.size() / 2] / 1000.0 << " us, p99 "
              << v[v.size() * 99 / 100] / 1000.0 << " us, max " << v.back() / 1000.0 << " us";
            return o.str();
        };
        std::sort(outliers.begin(), outliers.end(), [](const Outlier& a, const Outlier& b) { return a.ahead > b.ahead; });
        std::ostringstream o;
        o << "  master: " << records_.size() << " Syncs, " << fallback << " Follow_Ups with the time read before sending, "
          << noWitness << " with a copy missing\n"
          << "  send call: " << summary(sendCall) << "\n"
          << "  own copy minus witness copy: " << summary(ownMinusWitness) << "\n"
          << "  furthest apart:";
        std::sort(outliers.begin(), outliers.end(),
                  [](const Outlier& a, const Outlier& b) { return std::llabs(a.ahead) > std::llabs(b.ahead); });
        for (size_t i = 0; i < outliers.size() && i < 8; ++i) {
            o << " " << outliers[i].ahead / 1000.0 << " us at " << outliers[i].atSeconds << " s (seq " << outliers[i].seq << ");";
        }
        return o.str();
    }

private:
    static Timestamp timestampOf(uint64_t ns) { return Timestamp{ns / 1000000000ULL, static_cast<uint32_t>(ns % 1000000000ULL)}; }

    void sendGroup(uint16_t port, const std::vector<uint8_t>& bytes) { send_.sendTo(kGroup, port, bytes.data(), bytes.size()); }

    // The correction field carries part of t1 (as a transparent clock
    // would): t1 = origin + correction
    Timestamp originOf(uint64_t t1) const { return timestampOf(t1 - static_cast<uint64_t>(s_.followUpCorrectionNs)); }
    int64_t scaledCorrection() const { return s_.followUpCorrectionNs * 65536; }

    void sendSync() {
        const uint16_t seq = syncSequence_++;
        if (s_.twoStep) {
            // The newest socket gets the first copy
            witness_.close();
            witness_.open(Socket::Options{s_.eventPort, "", kGroup, lo0_, 0});
        }
        const uint64_t preNs = hostNanos();
        const uint64_t t1 = clockAt(preNs);  // just before it leaves
        if (s_.twoStep) {
            // The Follow_Up goes when the Sync comes back on lo0, with the
            // kernel's time for it (or this one, should it not come back)
            sendGroup(s_.eventPort, buildSync(identity_, 0, seq, s_.logSyncInterval, true, Timestamp{}));
            pendingFollowUp_ = PendingFollowUp{seq, t1, hostNanos(), 0, 0};
        } else {
            sendGroup(s_.eventPort, buildSync(identity_, 0, seq, s_.logSyncInterval, false, originOf(t1), scaledCorrection()));
        }
        const uint64_t postNs = hostNanos();
        note(seq, [preNs, postNs](SyncRecord& r) { r.preNs = preNs; r.postNs = postNs; });
    }

    void sendFollowUp(uint16_t seq, uint64_t t1) {
        sendGroup(s_.generalPort, buildFollowUp(identity_, 0, seq, s_.logSyncInterval, originOf(t1), scaledCorrection()));
        pendingFollowUp_.reset();
    }

    // A copy of a Sync came back: its own (the master's event socket) or
    // the witness's. The Follow_Up goes once both have, with the earlier
    // time, which is the witness's unless something went wrong.
    void onSyncCopy(const Message& m, uint64_t ticks, bool own) {
        if (ticks == 0 || m.header.type != MessageType::Sync) return;
        const uint64_t ns = HostTimebase::current().ticksToNanos(ticks);
        note(m.header.sequenceId, [ns, own](SyncRecord& r) { (own ? r.ownNs : r.witnessNs) = ns; });
        if (!pendingFollowUp_ || m.header.sequenceId != pendingFollowUp_->sequence) return;
        auto& copy = own ? pendingFollowUp_->ownNs : pendingFollowUp_->witnessNs;
        copy = ns;
        if (pendingFollowUp_->ownNs != 0 && pendingFollowUp_->witnessNs != 0) {
            sendFollowUp(pendingFollowUp_->sequence, clockAt(std::min(pendingFollowUp_->ownNs, pendingFollowUp_->witnessNs)));
        }
    }

    void readWitness() {
        uint8_t buffer[256];
        uint64_t ticks = 0;
        uint32_t source = 0;
        long n;
        while ((n = witness_.receive(buffer, sizeof(buffer), ticks, source)) > 0) {
            const auto m = parse(buffer, static_cast<size_t>(n));
            if (m && m->header.source.clock == identity_.clock) onSyncCopy(*m, ticks, false);
        }
    }

    struct SyncRecord {
        uint16_t seq{0};
        uint64_t preNs{0};      // read before sending
        uint64_t postNs{0};     // read after the send call returned
        uint64_t ownNs{0};      // kernel time of the master's own copy (0: fallback t1 used)
        uint64_t witnessNs{0};  // kernel time of another socket's copy
    };

    // Updates the record for a Sync sequence, made on first use
    template <typename Update>
    void note(uint16_t seq, Update update) {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        for (auto it = records_.rbegin(); it != records_.rend() && it - records_.rbegin() < 64; ++it) {
            if (it->seq == seq) {
                update(*it);
                return;
            }
        }
        records_.push_back(SyncRecord{});
        records_.back().seq = seq;
        update(records_.back());
    }

    void sendAnnounce() {
        AnnounceBody body;
        body.grandmasterIdentity = identity_.clock;
        body.grandmasterPriority1 = s_.priority1;
        sendGroup(s_.generalPort, buildAnnounce(identity_, 0, announceSequence_++, s_.logAnnounceInterval, body));
    }

    // Delay_Req to answer, and the master's own Syncs coming back
    void readEventPort(Socket& socket, bool unicast) {
        uint8_t buffer[256];
        uint64_t ticks = 0;
        uint32_t source = 0;
        long n;
        while ((n = socket.receive(buffer, sizeof(buffer), ticks, source)) > 0) {
            const auto m = parse(buffer, static_cast<size_t>(n));
            if (m && m->header.source.clock == identity_.clock) onSyncCopy(*m, ticks, true);
            if (!m || m->header.type != MessageType::DelayReq || m->header.source.clock == identity_.clock) continue;
            (unicast ? unicastRequests_ : multicastRequests_).fetch_add(1);
            const uint64_t t4 = clockAt(HostTimebase::current().ticksToNanos(ticks));
            // The correction field carries part of t4: t4 = receive - correction
            const int64_t c = s_.delayRespCorrectionNs;
            const auto resp = buildDelayResp(identity_, 0, m->header.sequenceId, s_.logDelayReqInterval,
                                             timestampOf(t4 + static_cast<uint64_t>(c)), m->header.source, c * 65536);
            if (unicast) {
                send_.sendTo(source, s_.generalPort, resp.data(), resp.size());
            } else {
                sendGroup(s_.generalPort, resp);
            }
        }
    }

    void run() {
        const uint64_t syncNs = static_cast<uint64_t>(std::ldexp(1e9, s_.logSyncInterval));
        const uint64_t announceNs = static_cast<uint64_t>(std::ldexp(1e9, s_.logAnnounceInterval));
        uint64_t nextSync = hostNanos();
        uint64_t nextAnnounce = nextSync;
        while (running_) {
            const uint64_t now = hostNanos();
            if (now >= nextAnnounce) { sendAnnounce(); nextAnnounce += announceNs; }
            if (now >= nextSync) {
                if (sendingSyncs_) sendSync();
                nextSync += syncNs;
            }
            pollfd fds[3] = {{multicastEvent_.fd(), POLLIN, 0}, {unicastEvent_.fd(), POLLIN, 0}, {witness_.fd(), POLLIN, 0}};
            ::poll(fds, 3, 2);
            readEventPort(multicastEvent_, false);
            readEventPort(unicastEvent_, true);
            readWitness();
            if (pendingFollowUp_ && hostNanos() - pendingFollowUp_->sentNs > kOwnSyncWaitNs) {
                // A copy never came back: the one that did, or the time read before sending
                const uint64_t own = pendingFollowUp_->ownNs, witness = pendingFollowUp_->witnessNs;
                sendFollowUp(pendingFollowUp_->sequence,
                             own || witness ? clockAt(std::min(own ? own : witness, witness ? witness : own))
                                            : pendingFollowUp_->fallbackT1);
            }
        }
    }

    struct PendingFollowUp {
        uint16_t sequence;
        uint64_t fallbackT1;  // master time read before sending
        uint64_t sentNs;
        uint64_t ownNs{0};      // kernel times of the copies back so far
        uint64_t witnessNs{0};
    };
    static constexpr uint64_t kOwnSyncWaitNs = 10000000;  // 10 ms

    Settings s_;
    PortIdentity identity_;
    Socket send_, multicastEvent_, unicastEvent_, witness_;
    unsigned lo0_{0};
    mutable std::mutex recordsMutex_;
    std::deque<SyncRecord> records_;
    std::optional<PendingFollowUp> pendingFollowUp_;
    uint16_t syncSequence_{0};
    uint16_t announceSequence_{0};
    std::atomic<uint64_t> multicastRequests_{0};
    std::atomic<uint64_t> unicastRequests_{0};
    std::atomic<bool> running_{true};
    std::atomic<bool> sendingSyncs_{true};
    std::thread thread_;
};

/// A receiver on lo0 with the test ports.
inline TimeReceiver::Config receiverConfig(bool unicast = false) {
    TimeReceiver::Config c;
    c.networkInterface = "lo0";
    c.eventPort = kEventPort;
    c.generalPort = kGeneralPort;
    c.unicastDelayRequests = unicast;
    c.ttl = 0;
    c.identity = ClockIdentity{{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF}};
    return c;
}

// For a failed test: what the receiver saw, against the master's true clock
inline std::string describe(const TimeReceiver::Status& status, const ScriptedMaster& master) {
    static const char* const kStates[] = {"Acquiring", "Locked", "Holdover"};
    std::ostringstream o;
    o << "  receiver: " << kStates[static_cast<int>(status.state)] << ", generation " << status.generation << ", "
      << status.syncs << " Syncs, " << status.followUps << " Follow_Ups, " << status.delayRequests << " Delay_Req, "
      << status.delayResponses << " Delay_Resp, " << status.announces << " Announces, path delay "
      << status.pathDelayNs / 1000 << " us";
    if (status.estimate) {
        const uint64_t now = hostNanos();
        o << ", rate " << (status.estimate->rate - 1.0) * 1e6 << " ppm, phase "
          << static_cast<int64_t>(status.estimate->masterAt(now) - master.clockAt(now)) / 1000.0 << " us";
    } else {
        o << ", no estimate";
    }
    return o.str() + "\n" + master.syncReport();
}

} // namespace PtpTest
} // namespace AES67
