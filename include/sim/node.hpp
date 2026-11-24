#pragma once

// The interface every simulated node implements (coordinator and banks).
//
// A node is a pure state machine: the simulator hands it one message or timer
// at a time with the current simulated time, and it returns Effects (messages
// to send, records to log, timers to set). It never touches the network, RNG,
// clock, event queue, simulator or another node, which is what makes runs
// deterministic and every step visible in the trace.

#include "sim/effects.hpp"
#include "domain/durable_log.hpp"
#include <span>

namespace glassbox {

// A timer firing at a node, as set by an earlier TimerRequest. `attempt` counts
// retries so backoff can grow.
struct TimerEvent {
    NodeId node;
    std::uint64_t epoch;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    TradeState expected_state;
    std::uint32_t attempt;
};

// Base class of the protocol state machines. Not thread-safe; the simulator
// calls one handler at a time.
class Node {
  public:
    virtual ~Node() = default;

    // Handles one delivered message and returns what to do next.
    [[nodiscard]] virtual Effects on_message(const Message& message,
                                             SimTimeUs now) = 0;
    // Handles one timer that is still relevant (the simulator already dropped
    // stale ones).
    [[nodiscard]] virtual Effects on_timer(const TimerEvent& event,
                                           SimTimeUs now) = 0;
    // Returns this node's state for `trade_id` (TradeState::none if unknown). The
    // simulator compares it with a timer's expected state before firing it.
    [[nodiscard]] virtual TradeState trade_state(
        TradeId trade_id) const = 0;
    // Called by the simulator after a record from this node becomes durable
    // (same epoch, node up). Pure state update: it returns no Effects, so the
    // write-ahead structure of Effects batches is unchanged. Protocol nodes
    // use it so they never announce a promise (a yes vote, a decision) before
    // the record behind it is durable.
    virtual void on_log_durable(const LogRecord& record, SimTimeUs now)
    {
        static_cast<void>(record);
        static_cast<void>(now);
    }
    // Forgets everything in memory; the durable log is kept by the simulator.
    virtual void on_crash() = 0;
    // Rebuilds state from `durable_log` after a crash and returns the recovery
    // actions (for example asking about trades still in doubt).
    [[nodiscard]] virtual Effects on_restart(
        std::span<const LogEntry> durable_log,
        SimTimeUs now) = 0;
};

} // namespace glassbox
