#pragma once

// Global invariant checker (DESIGN.md section 8, invariants 1-5).
//
// check_invariants() is a pure function of a SystemSnapshot: plain integers
// and states, so it does not rely on LocalLedger's own guarantees and can be
// fed synthetic bad states. snapshot() reads a ScenarioRun without changing
// it.
//
// Modes:
//   anytime    holds between any two events:
//              - balances non-negative, locked <= total (per bank);
//              - lock accounting: each bank's locks equal the leg amounts of
//                its PREPARING/PREPARED trades;
//              - conservation: totals equal genesis plus the legs committed
//                so far (both legs together net to zero);
//              - no trade COMMITTED at one party and ABORTED at the other;
//              - no INVARIANT_VIOLATION trace events.
//   quiescent  everything above, plus atomicity (both parties COMMITTED or
//              neither) and no leaked locks (all locks zero).

#include "scenarios/scenario.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glassbox {

enum class InvariantKind : std::uint8_t {
    negative_balance,
    locked_exceeds_total,
    lock_mismatch,
    conservation,
    atomicity,
    leaked_lock,
    invariant_event,
    arithmetic_overflow,
};

[[nodiscard]] constexpr std::string_view to_string(InvariantKind kind) noexcept
{
    switch (kind) {
    case InvariantKind::negative_balance:
        return "NEGATIVE_BALANCE";
    case InvariantKind::locked_exceeds_total:
        return "LOCKED_EXCEEDS_TOTAL";
    case InvariantKind::lock_mismatch:
        return "LOCK_MISMATCH";
    case InvariantKind::conservation:
        return "CONSERVATION";
    case InvariantKind::atomicity:
        return "ATOMICITY";
    case InvariantKind::leaked_lock:
        return "LEAKED_LOCK";
    case InvariantKind::invariant_event:
        return "INVARIANT_EVENT";
    case InvariantKind::arithmetic_overflow:
        return "ARITHMETIC_OVERFLOW";
    }
    return "UNKNOWN";
}

enum class CheckMode : std::uint8_t {
    anytime,
    quiescent,
};

struct BalanceView {
    std::int64_t cash_total = 0;
    std::int64_t cash_locked = 0;
    std::int64_t bonds_total = 0;
    std::int64_t bonds_locked = 0;

    [[nodiscard]] friend bool operator==(const BalanceView&,
                                         const BalanceView&) = default;
};

struct TradeTerms {
    TradeId trade_id;
    NodeId buyer;
    NodeId seller;
    std::int64_t qty = 0;
    std::int64_t cash_cents = 0;

    [[nodiscard]] friend bool operator==(const TradeTerms&,
                                         const TradeTerms&) = default;
};

// Index 0, 1, 2 = BANK_A, BANK_B, BANK_C.
struct SystemSnapshot {
    std::array<BalanceView, 3> genesis{};
    std::array<BalanceView, 3> current{};
    std::array<std::map<TradeId, TradeState>, 3> states{};
    std::vector<TradeTerms> trades{};
    std::size_t invariant_events = 0;

    [[nodiscard]] friend bool operator==(const SystemSnapshot&,
                                         const SystemSnapshot&) = default;
};

struct InvariantViolation {
    InvariantKind kind;
    std::optional<NodeId> node;
    std::optional<TradeId> trade_id;
    std::string detail;

    [[nodiscard]] friend bool operator==(const InvariantViolation&,
                                         const InvariantViolation&) = default;
};

[[nodiscard]] BalanceView balance_view(const LocalLedger& ledger) noexcept;

// Reads: genesis and current ledgers, participants' trade states, trade
// terms from the coordinator's durable PROPOSED records, and the count of
// INVARIANT_VIOLATION trace records.
[[nodiscard]] SystemSnapshot snapshot(const ScenarioRun& run);

// Violations in a fixed order: per bank (A, B, C) bounds then lock
// accounting; conservation (cash, then bonds); per trade in id order
// atomicity; per bank leaked locks (quiescent only); invariant events.
[[nodiscard]] std::vector<InvariantViolation> check_invariants(
    const SystemSnapshot& snapshot,
    CheckMode mode);

} // namespace glassbox
