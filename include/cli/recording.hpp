#pragma once

// Glass-box player, step 1: record a scenario run event by event.
//
// record() builds the normal ScenarioRun and calls Simulator::step() one
// event at a time. After each event it copies what every node looks like
// (balances, trades, votes, durable log, up/down) and runs the anytime
// invariant check. It only reads the run: the trace it collects is exactly
// the trace ScenarioRun::run() produces.
//
// Because runs are deterministic, the whole recording exists before playback
// starts, so the player can pair each MSG_SEND with the delivery or drop it
// leads to (Flight) and each TIMER_SET with its firing (TimerSpan).

#include "protocol/coordinator.hpp"
#include "protocol/participant.hpp"
#include "scenarios/invariant_checker.hpp"
#include "scenarios/scenario.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace glassbox::cli {

// What one node looks like after an event.
struct NodeView {
    NodeId node = NodeId::coordinator;
    bool up = true;
    bool frozen = false;
    std::optional<BalanceView> balances;                    // banks
    std::map<TradeId, ParticipantTrade> trades;             // banks
    std::map<TradeId, CoordinatorTrade> coordinator_trades; // COORD
    // Durable records, oldest first, as "<KIND> t<trade id>".
    std::vector<std::string> disk;
};

// Index order of Frame::nodes.
inline constexpr std::array<NodeId, 4> k_view_order{
    NodeId::coordinator, NodeId::bank_a, NodeId::bank_b, NodeId::bank_c};

// Returns `node`'s index in Frame::nodes (k_view_order).
[[nodiscard]] std::size_t view_index(NodeId node) noexcept;

struct Frame {
    SimTimeUs time_us = 0;
    std::vector<TraceRecord> records; // what this event produced
    std::array<NodeView, 4> nodes;    // COORD, BANK_A, BANK_B, BANK_C
    std::vector<InvariantViolation> violations; // anytime check
    // The scenario script action this event ran, if any (SCRIPT_ACTION
    // records carry no detail, so the recording attaches the action).
    std::optional<ScriptActionEvent> action;
};

// ends_at_us of a message still on the wire when the recording stopped.
inline constexpr SimTimeUs k_open_end = std::numeric_limits<SimTimeUs>::max();

// One message on the wire: from its MSG_SEND to the delivery or drop.
// A duplicated message yields one Flight per copy (same msg_id).
struct Flight {
    std::uint64_t msg_id = 0;
    MessageType type = MessageType::propose;
    NodeId from = NodeId::coordinator;
    NodeId to = NodeId::coordinator;
    SimTimeUs sent_at_us = 0;
    SimTimeUs ends_at_us = k_open_end;
    std::optional<std::string> detail;      // YES, BUYER, VOTE_TIMEOUT, ...
    std::optional<std::string> drop_reason; // set if it never arrives
};

// One timer from TIMER_SET (handler time) to its TIMER_FIRE/TIMER_IGNORED.
struct TimerSpan {
    NodeId node = NodeId::coordinator;
    TradeId trade_id{0};
    std::string kind; // VOTE_TIMEOUT, RETRANSMIT, DECISION_WAIT
    SimTimeUs set_at_us = 0;
    std::optional<SimTimeUs> fires_at_us; // unset if it never fired
};

// A whole run, event by event.
struct Recording {
    std::vector<Frame> frames; // frames[0] = before the first event
    std::vector<Flight> flights;   // in send order
    std::vector<TimerSpan> timers; // in set order
    bool quiesced = false;
};

// Runs `setup` to the end (or for at most `max_events` events) and records
// it. `quiesced` says whether the run finished.
[[nodiscard]] Recording record(const ScenarioSetup& setup,
                               std::size_t max_events = 100'000);

// glassbox::snapshot(run) with the INVARIANT_VIOLATION count supplied by
// the caller (record() keeps it incrementally; snapshot() rescans the trace).
[[nodiscard]] SystemSnapshot snapshot_counted(const ScenarioRun& run,
                                              std::size_t invariant_events);

// Flights on the wire at sim time t: sent_at_us <= t < ends_at_us.
[[nodiscard]] std::vector<const Flight*> in_flight(const Recording& recording,
                                                   SimTimeUs t);

} // namespace glassbox::cli
