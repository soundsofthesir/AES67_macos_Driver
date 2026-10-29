/// @file PtpBestMaster.h
/// @brief Which PTP master to follow (IEEE 1588-2008 clause 9.3, slave-only).

#pragma once

#include "PtpMessages.h"
#include <deque>
#include <map>
#include <optional>

namespace AES67 {
namespace Ptp {

/// A master as its latest Announce describes it.
struct Candidate {
    PortIdentity sender;
    AnnounceBody announce;
    int8_t logAnnounceInterval{1};
};

/// The best master algorithm for a slave-only ordinary clock: tracks the
/// masters announcing on our domain, qualifies each once it has announced
/// often enough (so one stray Announce cannot take over), compares their
/// datasets in the standard's order, and drops a master that falls silent
/// for announceReceiptTimeout of its own announce intervals. Announces with
/// an interval outside 1/8 s to 16 s are ignored.
///
/// Time is passed in, in seconds, so it is deterministic. Not thread-safe.
class BestMaster {
public:
    struct Config {
        uint8_t domain{0};
        ClockIdentity self;                 // our identity: our own Announces are ignored
        uint8_t announceReceiptTimeout{3};  // intervals of silence before a master is dropped
        int foreignMasterThreshold{2};      // announces needed within the window to qualify
        int foreignMasterWindow{4};         // the window, in the master's announce intervals
    };

    BestMaster() : BestMaster(Config{}) {}
    explicit BestMaster(Config config);

    /// Feed a received message (non-Announces are ignored).
    /// @return true if the selected master changed.
    bool onAnnounce(const Message& message, double now);

    /// Drop masters that have gone silent. Call regularly.
    /// @return true if the selected master changed.
    bool tick(double now);

    std::optional<Candidate> selected() const;

    /// Dataset comparison (clause 9.3.4): negative if `a` is the better master.
    static int compare(const Candidate& a, const Candidate& b);

private:
    struct Record {
        Candidate latest;
        std::deque<double> arrivals;  // within the qualification window
        double lastSeen{0.0};
    };

    static double intervalOf(const Candidate& candidate);
    bool qualified(const Record& record, double now) const;
    bool alive(const Record& record, double now) const;
    bool reselect(double now);

    const Config config_;
    std::map<PortIdentity, Record> masters_;
    std::optional<PortIdentity> selected_;
};

} // namespace Ptp
} // namespace AES67
