#pragma once

#include "domain/trade.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glassbox {

enum class Leg : std::uint8_t {
    buyer,
    seller,
};

[[nodiscard]] constexpr std::string_view to_string(Leg leg) noexcept
{
    return leg == Leg::buyer ? "BUYER" : "SELLER";
}

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
