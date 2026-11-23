#pragma once

// Pure node interface for Task 6. Implementations return Effects and have no
// network, RNG, clock, event queue, simulator, or peer-node dependencies.

#include "sim/effects.hpp"

namespace glassbox {

struct TimerEvent {
    NodeId node;
    std::uint64_t epoch;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    std::string expected_state;
};

class Node {
  public:
    virtual ~Node() = default;

    [[nodiscard]] virtual Effects on_message(const Message& message,
                                             SimTimeUs now) = 0;
    [[nodiscard]] virtual Effects on_timer(const TimerEvent& event,
                                           SimTimeUs now) = 0;
    virtual void on_crash() = 0;
    [[nodiscard]] virtual Effects on_restart(SimTimeUs now) = 0;
};

} // namespace glassbox
