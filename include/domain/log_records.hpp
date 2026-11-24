#pragma once

// Durable log records (DESIGN.md section 5.2): what a node writes to its disk
// before acting, so that after a crash it can rebuild exactly what it promised.
// Participants write Prepared, Committed and Aborted; the coordinator writes
// Proposed, DecisionCommit, DecisionAbort and End.

#include "domain/trade.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glassbox {

// Which side of a trade a bank is on: the buyer pays cash, the seller
// delivers bonds.
enum class Leg : std::uint8_t {
    buyer,
    seller,
};

// Returns "BUYER" or "SELLER".
[[nodiscard]] constexpr std::string_view to_string(Leg leg) noexcept
{
    return leg == Leg::buyer ? "BUYER" : "SELLER";
}

// Participant: "I locked my leg and promise to vote YES". Written before the
// YES vote may leave; a restarted bank uses it to restore the lock.
struct Prepared {
    Trade trade;
    Leg leg;
};

// Participant: my leg of the trade was applied.
struct Committed {
    TradeId trade_id;
    Leg leg;
};

// Participant: this trade is closed for me without settling (my NO vote, an
// ABORT, or a tombstone for a trade I never prepared).
struct Aborted {
    TradeId trade_id;
};

// Coordinator: the validated trade, written before any PREPARE leaves, so a
// restarted coordinator never reuses its id.
struct Proposed {
    Trade trade;
};

// Coordinator: the decision COMMIT, the commit point of the trade.
struct DecisionCommit {
    TradeId trade_id;
    NodeId buyer;
    NodeId seller;
};

// Coordinator: the decision ABORT and its reason.
struct DecisionAbort {
    TradeId trade_id;
    ReasonCode reason;
};

// Coordinator: every bank acknowledged; nothing more to resend.
struct End {
    TradeId trade_id;
};

// Any one durable record.
using LogRecord = std::variant<
    Prepared,
    Committed,
    Aborted,
    Proposed,
    DecisionCommit,
    DecisionAbort,
    End>;

// Every record belongs to exactly one trade.
[[nodiscard]] inline TradeId record_trade_id(const LogRecord& record) noexcept
{
    return std::visit(
        [](const auto& typed) noexcept -> TradeId {
            using Record = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Record, Prepared> ||
                          std::is_same_v<Record, Proposed>) {
                return typed.trade.trade_id();
            } else {
                return typed.trade_id;
            }
        },
        record);
}

// Returns the trace name of the record's type, e.g. "DECISION_COMMIT".
[[nodiscard]] constexpr std::string_view record_kind(
    const LogRecord& record) noexcept
{
    if (std::holds_alternative<Prepared>(record)) {
        return "PREPARED";
    }
    if (std::holds_alternative<Committed>(record)) {
        return "COMMITTED";
    }
    if (std::holds_alternative<Aborted>(record)) {
        return "ABORTED";
    }
    if (std::holds_alternative<Proposed>(record)) {
        return "PROPOSED";
    }
    if (std::holds_alternative<DecisionCommit>(record)) {
        return "DECISION_COMMIT";
    }
    if (std::holds_alternative<DecisionAbort>(record)) {
        return "DECISION_ABORT";
    }
    return "END";
}

} // namespace glassbox
