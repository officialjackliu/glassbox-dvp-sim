#pragma once

// Trace records: the glass box. The simulator emits one TraceRecord for
// every step it takes (message sent, delivered or lost, record made durable,
// timer set or fired, crash, restart, and notes from the nodes such as LOCK or
// STATE_CHANGE), each naming the function that caused it. Nodes never write
// traces themselves.

#include "sim/core_types.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <string>
#include <optional>

namespace glassbox {

// One trace step (DESIGN.md section 9). `kind` says what happened ("MSG_SEND",
// "LOCK", ...), `handler` which function did it, `sim_time_us` when in
// simulated time. `measured_ns` is real handler CPU time, kept for display
// only and never used by the simulation. Optional fields are set when they
// apply to the kind.
struct TraceRecord {
    std::uint64_t seq = 0;
    SimTimeUs sim_time_us = 0;
    std::string kind;
    std::string handler;
    std::uint64_t measured_ns = 0;
    std::optional<std::string> reason;

    // Glass-box context (DESIGN.md section 9). Unset when not applicable.
    std::optional<NodeId> node;
    std::optional<TradeId> trade_id;
    std::optional<MessageType> msg_type;
    std::optional<std::uint64_t> msg_id;
    std::optional<NodeId> from;
    std::optional<NodeId> to;
    std::optional<std::string> log_record;
    std::optional<std::string> detail;
};

// Receives trace records as they are produced (collect them, print them, ...).
class TraceSink {
  public:
    virtual ~TraceSink() = default;
    // Called once per record, in order.
    virtual void write(const TraceRecord& record) = 0;
};

// A sink that discards everything.
class NullTraceSink : public TraceSink {
  public:
    void write(const TraceRecord&) override {}
};

} // namespace glassbox
