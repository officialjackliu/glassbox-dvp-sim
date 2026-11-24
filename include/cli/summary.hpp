#pragma once

// Glass-box player, step 6: the end card's numbers, measured from the
// recording (simulated time, never wall-clock time).
//
//   settled_after_us  from the trade's submission to the last bank reaching
//                     COMMITTED or ABORTED for it (END if no bank did)
//   lock_hold_us      per bank (A, B, C): LOCK to its UNLOCK or APPLY,
//                     summed over trades; a lock restored from disk after a
//                     crash keeps counting from the original LOCK, one not
//                     yet on disk ends at the crash
//   longest_in_doubt  a bank's PREPARED on disk to its outcome, the longest
//   sent / dropped    MSG_SEND and MSG_DROP* records
//   retransmits       messages the coordinator resent (one per target)
//   checks_run        events, each followed by the invariant check
//   violations        problems those checks found
// A lock or doubt still open when the recording ends counts until its last
// event. Trades without a decision (an unfinished run) are left out.

#include "cli/recording.hpp"

#include <array>
#include <vector>

namespace glassbox::cli {

// How one trade ended.
struct TradeOutcome {
    TradeId trade_id{0};
    NodeId buyer = NodeId::bank_a;
    NodeId seller = NodeId::bank_b;
    std::int64_t qty = 0;
    Cents cash{0};
    Decision decision = Decision::abort;
    ReasonCode reason = ReasonCode::none;
    SimTimeUs settled_after_us = 0;

    friend bool operator==(const TradeOutcome&, const TradeOutcome&) = default;
};

// The numbers on the end card; see the file comment for each.
struct RunSummary {
    std::vector<TradeOutcome> trades;     // in trade id order
    std::array<SimTimeUs, 3> lock_hold_us{}; // BANK_A, BANK_B, BANK_C
    SimTimeUs longest_in_doubt_us = 0;
    int sent = 0;
    int dropped = 0;
    int retransmits = 0;
    std::size_t checks_run = 0;
    std::size_t violations = 0;

    friend bool operator==(const RunSummary&, const RunSummary&) = default;
};

// Measures `recording`.
[[nodiscard]] RunSummary summarize(const Recording& recording);

} // namespace glassbox::cli
