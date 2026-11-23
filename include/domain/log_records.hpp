#pragma once

#include "domain/trade.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace glassbox {

enum class Leg : std::uint8_t {
    buyer,
    seller,
};

struct Prepared {
    Trade trade;
    Leg leg;
};

struct Committed {
    TradeId trade_id;
    Leg leg;
};

struct Aborted {
    TradeId trade_id;
};

struct Proposed {
    Trade trade;
};

struct DecisionCommit {
    TradeId trade_id;
    NodeId buyer;
    NodeId seller;
};

struct DecisionAbort {
    TradeId trade_id;
    ReasonCode reason;
};

struct End {
    TradeId trade_id;
};

using LogRecord = std::variant<
    Prepared,
    Committed,
    Aborted,
    Proposed,
    DecisionCommit,
    DecisionAbort,
    End>;

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
