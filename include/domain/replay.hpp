#pragma once

#include "domain/durable_log.hpp"
#include "domain/local_ledger.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace glassbox {

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

struct ReplayError {
    ReplayErrorKind kind;
    std::optional<TradeId> trade_id;

    [[nodiscard]] friend bool operator==(
        const ReplayError&,
        const ReplayError&) noexcept = default;
};

struct ParticipantReplay {
    LocalLedger ledger;
    std::map<TradeId, TradeState> states;
    std::vector<Trade> in_doubt;
    std::vector<ReplayError> errors;

    [[nodiscard]] friend bool operator==(
        const ParticipantReplay&,
        const ParticipantReplay&) noexcept = default;
};

struct CoordinatorTradeReplay {
    Trade trade;
    bool committed = false;
    std::optional<ReasonCode> abort_reason;
    bool ended = false;

    [[nodiscard]] friend bool operator==(
        const CoordinatorTradeReplay&,
        const CoordinatorTradeReplay&) noexcept = default;
};

struct CoordinatorReplay {
    std::map<TradeId, CoordinatorTradeReplay> trades;
    std::optional<TradeId> max_trade_id;
    std::vector<ReplayError> errors;

    [[nodiscard]] friend bool operator==(
        const CoordinatorReplay&,
        const CoordinatorReplay&) noexcept = default;
};

[[nodiscard]] ParticipantReplay replay_participant(
    const LocalLedger& genesis,
    NodeId self,
    std::span<const LogEntry> entries);

[[nodiscard]] CoordinatorReplay replay_coordinator(
    std::span<const LogEntry> entries);

} // namespace glassbox
