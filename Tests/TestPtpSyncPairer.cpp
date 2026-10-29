//
// TestPtpSyncPairer.cpp
// AES67 macOS Driver
// Pairing a two-step master's Syncs with their Follow_Ups. A message whose
// partner was lost must not wait for the 16-bit sequenceId to come round
// again and pair with an unrelated message (#56).
//

#include "../NetworkEngine/PTP/PtpSyncPairer.h"
#include <iostream>

using namespace AES67::Ptp;

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

// Sync n is received at host time n ms and was sent at master time
// 1000 s + n ms, so a sample from a stale message is easy to spot
constexpr uint64_t kMs = 1000000;
constexpr uint64_t kMasterBase = 1000000 * kMs;
uint64_t hostAt(uint64_t n) { return n * kMs; }
uint64_t masterAt(uint64_t n) { return kMasterBase + n * kMs; }

bool pairsTo(const std::optional<SyncSample>& s, uint64_t n) {
    return s && s->hostReceiveNs == hostAt(n) && s->preciseOriginNs == masterAt(n);
}

void testInOrder() {
    std::cout << "A Sync then its Follow_Up make one sample" << std::endl;
    SyncPairer p;
    CHECK(!p.onSync(1, hostAt(1), 10.0), "nothing until the Follow_Up");
    const auto s = p.onFollowUp(1, masterAt(1), 5.0);
    CHECK(pairsTo(s, 1) && s->correctionNs == 15.0, "t1, t2 and both corrections");
    CHECK(!p.onFollowUp(1, masterAt(1), 0.0), "a duplicate Follow_Up pairs with nothing");
}

void testFollowUpFirst() {
    std::cout << "A Follow_Up that overtakes its Sync still pairs" << std::endl;
    SyncPairer p;
    CHECK(!p.onFollowUp(1, masterAt(1), 5.0), "nothing until the Sync");
    const auto s = p.onSync(1, hostAt(1), 10.0);
    CHECK(pairsTo(s, 1) && s->correctionNs == 15.0, "t1, t2 and both corrections");
}

void testDifferentSequencesNeverPair() {
    std::cout << "Messages with different sequenceIds never pair" << std::endl;
    SyncPairer p;
    p.onSync(1, hostAt(1), 0.0);
    CHECK(!p.onFollowUp(2, masterAt(2), 0.0), "Sync 1 with Follow_Up 2");
    SyncPairer q;
    q.onFollowUp(1, masterAt(1), 0.0);
    CHECK(!q.onSync(2, hostAt(2), 0.0), "Follow_Up 1 with Sync 2");
}

// Sequences from `first` for one full turn of the 16-bit counter, ending
// just before `first` comes round again
template <typename Each>
void oneWrap(uint16_t first, Each each) {
    for (uint32_t i = 1; i < 65536; ++i) each(static_cast<uint16_t>(first + i), first + i);
}

void testOrphanFollowUpDoesNotWaitForTheWrap() {
    std::cout << "A Follow_Up whose Sync was lost is not paired after the sequenceId wraps" << std::endl;
    SyncPairer p;
    p.onFollowUp(5, masterAt(5), 0.0);  // its Sync was lost
    bool allPaired = true;
    oneWrap(5, [&](uint16_t seq, uint64_t n) {
        p.onSync(seq, hostAt(n), 0.0);
        allPaired = allPaired && pairsTo(p.onFollowUp(seq, masterAt(n), 0.0), n);
    });
    CHECK(allPaired, "every Sync in between pairs with its own Follow_Up");
    const uint64_t n = 5 + 65536;
    const auto early = p.onSync(5, hostAt(n), 0.0);
    CHECK(!early, "Sync 5 after the wrap waits for its own Follow_Up (paired with the orphan, 65.536 s stale)");
    CHECK(pairsTo(p.onFollowUp(5, masterAt(n), 0.0), n), "and pairs with it");
}

void testOrphanSyncDoesNotWaitForTheWrap() {
    std::cout << "A Sync whose Follow_Up was lost is not paired after the sequenceId wraps" << std::endl;
    SyncPairer p;
    p.onSync(5, hostAt(5), 0.0);  // its Follow_Up was lost
    bool allPaired = true;
    oneWrap(5, [&](uint16_t seq, uint64_t n) {
        p.onFollowUp(seq, masterAt(n), 0.0);  // every Follow_Up overtakes its Sync
        allPaired = allPaired && pairsTo(p.onSync(seq, hostAt(n), 0.0), n);
    });
    CHECK(allPaired, "every Sync in between pairs with its own Follow_Up");
    const uint64_t n = 5 + 65536;
    CHECK(!p.onFollowUp(5, masterAt(n), 0.0), "Follow_Up 5 after the wrap waits for its own Sync");
    CHECK(pairsTo(p.onSync(5, hostAt(n), 0.0), n), "and pairs with it");
}

void testReset() {
    std::cout << "Reset forgets anything waiting" << std::endl;
    SyncPairer p;
    p.onSync(1, hostAt(1), 0.0);
    p.reset();
    CHECK(!p.onFollowUp(1, masterAt(1), 0.0), "a Sync from before the reset");
    p.reset();
    CHECK(!p.onSync(1, hostAt(1), 0.0), "a Follow_Up from before the reset");
}

} // namespace

int main() {
    testInOrder();
    testFollowUpFirst();
    testDifferentSequencesNeverPair();
    testOrphanFollowUpDoesNotWaitForTheWrap();
    testOrphanSyncDoesNotWaitForTheWrap();
    testReset();

    std::cout << "\nPTP sync pairer: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
