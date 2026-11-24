#pragma once

// Crash recovery: rebuild a node's state from its durable log alone.
//
// replay_participant() starts from the bank's genesis ledger and applies the
// records in LSN order through LedgerOp, so the ledger, the final trade states
// and the trades still in doubt (PREPARED with no outcome) come back exactly.
// replay_coordinator() recovers each trade's decision and the highest trade id.
// Both are pure and report inconsistent logs as ReplayErrors instead of
// throwing.

#include "domain/durable_log.hpp"
#include "domain/local_ledger.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace glassbox {

// A log that cannot have been written by a correct node.
enum class ReplayErrorKind : std::uint8_t {
    committed_without_prepared,
    prepared_twice,
    record_after_final,
    prepared_wrong_party,
    ledger_op_failed,
    coordinator_record_in_participant,
    participant_record_in_coordinator,
    two_decisions,
    decision_without_proposed,
    end_before_decision,
};

// Returns the trace spelling of `kind`, e.g. "PREPARED_TWICE".
[[nodiscard]] constexpr std::string_view to_string(
    ReplayErrorKind kind) noexcept
{
    switch (kind) {
    case ReplayErrorKind::committed_without_prepared:
        return "COMMITTED_WITHOUT_PREPARED";
    case ReplayErrorKind::prepared_twice:
        return "PREPARED_TWICE";
    case ReplayErrorKind::record_after_final:
        return "RECORD_AFTER_FINAL";
    case ReplayErrorKind::prepared_wrong_party:
        return "PREPARED_WRONG_PARTY";
    case ReplayErrorKind::ledger_op_failed:
        return "LEDGER_OP_FAILED";
    case ReplayErrorKind::coordinator_record_in_participant:
        return "COORDINATOR_RECORD_IN_PARTICIPANT";
    case ReplayErrorKind::participant_record_in_coordinator:
        return "PARTICIPANT_RECORD_IN_COORDINATOR";
    case ReplayErrorKind::two_decisions:
        return "TWO_DECISIONS";
    case ReplayErrorKind::decision_without_proposed:
        return "DECISION_WITHOUT_PROPOSED";
    case ReplayErrorKind::end_before_decision:
        return "END_BEFORE_DECISION";
    }
    return "UNKNOWN";
}

// One problem found during replay, with the trade it concerns if any.
struct ReplayError {
    ReplayErrorKind kind;
    std::optional<TradeId> trade_id;

    [[nodiscard]] friend bool operator==(
        const ReplayError&,
        const ReplayError&) noexcept = default;
};

// What a bank knows after replay: its ledger (locks of in-doubt trades
// included), each trade's final state, the trades still in doubt, and any
// errors.
struct ParticipantReplay {
    LocalLedger ledger;
    std::map<TradeId, TradeState> states;
    std::vector<Trade> in_doubt;
    std::vector<ReplayError> errors;

    [[nodiscard]] friend bool operator==(
        const ParticipantReplay&,
        const ParticipantReplay&) noexcept = default;
};

// What the coordinator's log says about one trade.
struct CoordinatorTradeReplay {
    Trade trade;
    bool committed = false;
    std::optional<ReasonCode> abort_reason;
    bool ended = false;

    [[nodiscard]] friend bool operator==(
        const CoordinatorTradeReplay&,
        const CoordinatorTradeReplay&) noexcept = default;
};

// What the coordinator knows after replay: every logged trade, the highest
// trade id seen (so new ids continue after it), and any errors.
struct CoordinatorReplay {
    std::map<TradeId, CoordinatorTradeReplay> trades;
    std::optional<TradeId> max_trade_id;
    std::vector<ReplayError> errors;

    [[nodiscard]] friend bool operator==(
        const CoordinatorReplay&,
        const CoordinatorReplay&) noexcept = default;
};

// Replays bank `self`'s log `entries` on top of `genesis`.
[[nodiscard]] ParticipantReplay replay_participant(
    const LocalLedger& genesis,
    NodeId self,
    std::span<const LogEntry> entries);

// Replays the coordinator's log `entries`.
[[nodiscard]] CoordinatorReplay replay_coordinator(
    std::span<const LogEntry> entries);

} // namespace glassbox
