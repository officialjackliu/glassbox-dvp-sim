#pragma once

// Pure node interface for Task 6. Implementations return Effects and have no
// network, RNG, clock, event queue, simulator, or peer-node dependencies.

#include "sim/effects.hpp"
#include "domain/durable_log.hpp"
#include <span>

namespace glassbox {

struct TimerEvent {
    NodeId node;
    std::uint64_t epoch;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    TradeState expected_state;
    std::uint32_t attempt;
};

class Node {
  public:
    virtual ~Node() = default;

    [[nodiscard]] virtual Effects on_message(const Message& message,
                                             SimTimeUs now) = 0;
    [[nodiscard]] virtual Effects on_timer(const TimerEvent& event,
                                           SimTimeUs now) = 0;
    [[nodiscard]] virtual TradeState trade_state(
        TradeId trade_id) const = 0;
    virtual void on_crash() = 0;
    [[nodiscard]] virtual Effects on_restart(
        std::span<const LogEntry> durable_log,
        SimTimeUs now) = 0;
};

} // namespace glassbox
