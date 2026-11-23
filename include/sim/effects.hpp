#pragma once

// Effects is the only data a node handler returns to the simulator.
// Handlers receive input and virtual time; they never access the network,
// event queue, RNG, clock, simulator, or another node.

#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"

#include <optional>
#include <string>
#include <vector>

namespace glassbox {

struct LogRecord {
    std::string kind;
    std::optional<TradeId> trade_id;
};

struct Outgoing {
    Message message;
};

struct TimerRequest {
    SimTimeUs fire_time_us;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    std::string expected_state;
};

struct TraceNote {
    std::string kind;
    std::string detail;
};

struct Effects {
    std::vector<Outgoing> sends;
    std::vector<LogRecord> logs;
    std::vector<TimerRequest> timers;
    std::vector<TraceNote> notes;
};

} // namespace glassbox
