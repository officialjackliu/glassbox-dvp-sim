#pragma once

// Effects is the only data a node handler returns to the simulator.
// Handlers receive input and virtual time; they never access the network,
// event queue, RNG, clock, simulator, or another node.

#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"
#include "domain/log_records.hpp"

#include <optional>
#include <string>
#include <vector>

namespace glassbox {

// A message the node wants sent.
struct Outgoing {
    Message message;
};

// A timer the node wants set: fires `delay_us` after the batch's messages are
// released, and only if the trade is still in `expected_state` then.
struct TimerRequest {
    SimTimeUs delay_us;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    TradeState expected_state;
    std::uint32_t attempt = 0;
};

// Extra trace detail a node attaches to a batch (STATE_CHANGE, LOCK, UNLOCK,
// APPLY, REJECTED, INVARIANT_VIOLATION, ...). The simulator emits it; the node
// never writes to the trace itself.
struct TraceNote {
    std::string kind;
    std::string detail;
    std::optional<TradeId> trade_id = std::nullopt;
    std::string handler = "NodeEffect";
};

// Everything one handler call asks for, applied by the simulator as one batch:
// logs become durable first, then the sends leave, then timers start (the
// write-ahead rule). `notes` only add trace detail.
struct Effects {
    std::vector<Outgoing> sends;
    std::vector<LogRecord> logs;
    std::vector<TimerRequest> timers;
    std::vector<TraceNote> notes;
};

} // namespace glassbox
