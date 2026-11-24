#pragma once

// Deterministic min-heap event scheduling for the discrete-event simulation.
//
// EventQueue is a low-level collection: callers assign virtual time and payload
// data, while the queue assigns insertion sequence and returns earliest events
// first. It performs no real-time or wall-clock operations.

#include "sim/core_types.hpp"
#include "domain/log_records.hpp"
#include "domain/trade.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

namespace glassbox {

// What a queued event does when the simulator pops it.
enum class EventKind : std::uint8_t {
    deliver_message,
    drop_message,
    flush_log,
    release_send,
    fire_timer,
    node_crash,
    node_restart,
    script_action,
};

// The three protocol timers (DESIGN.md section 6.2): the coordinator's vote
// timeout and decision retransmit, and a participant's wait before asking for
// an outcome it has not heard.
enum class TimerKind : std::uint8_t {
    vote_timeout,
    retransmit,
    decision_wait,
};

// Why the network did not deliver a message.
enum class DropReason : std::uint8_t {
    random,
    partition,
    node_down,
    next_message,
};

// Returns the trace spelling of `kind`, e.g. "VOTE_TIMEOUT".
[[nodiscard]] constexpr std::string_view to_string(TimerKind kind) noexcept
{
    switch (kind) {
    case TimerKind::vote_timeout:
        return "VOTE_TIMEOUT";
    case TimerKind::retransmit:
        return "RETRANSMIT";
    case TimerKind::decision_wait:
        return "DECISION_WAIT";
    }
    return "UNKNOWN";
}

// Returns the trace spelling of `reason`, e.g. "PARTITION".
[[nodiscard]] constexpr std::string_view to_string(DropReason reason) noexcept
{
    switch (reason) {
    case DropReason::random:
        return "DROP_RANDOM";
    case DropReason::partition:
        return "PARTITION";
    case DropReason::node_down:
        return "NODE_DOWN";
    case DropReason::next_message:
        return "DROP_NEXT";
    }
    return "UNKNOWN";
}

// A message arriving at `target`, with its send time and simulated latency.
struct DeliverMessageEvent {
    Message message;
    NodeId target;
    SimTimeUs sent_at_us;
    SimTimeUs deliver_at_us;
    SimTimeUs simulated_latency_us;
};

// A message the network lost; traced at the time it would have arrived.
struct DropMessageEvent {
    Message message;
    DropReason reason;
    SimTimeUs sent_at_us;
    SimTimeUs deliver_at_us;
};

// A timer going off at `node`. `epoch` is the node's crash count when it was
// set and `expected_state` the trade state it was waiting in; the simulator
// ignores the timer if either no longer matches.
struct FireTimerEvent {
    NodeId node;
    std::uint64_t epoch;
    TimerKind kind;
    std::optional<TradeId> trade_id;
    TradeState expected_state;
    std::uint32_t attempt;
};

// A node crashing: its memory (trades, locks, pending sends) is lost.
struct NodeCrashEvent {
    NodeId node;
    // Set when the crash comes from a CrashSpec; traced as the reason.
    std::optional<std::string> reason = std::nullopt;
};

// A crashed node starting again and rebuilding itself from its durable log.
struct NodeRestartEvent {
    NodeId node;
};

// Script actions: faults and network changes a scenario applies at a chosen
// simulated time.
//
// Sets one direction of a link: latency, jitter and drop rate (parts per
// million).
struct ScriptSetLink {
    NodeId from;
    NodeId to;
    SimTimeUs base_latency_us;
    SimTimeUs jitter_us;
    std::uint32_t drop_probability_ppm;
};

// Cuts (or restores) the link between `a` and `b`, both directions.
struct ScriptSetPartition {
    NodeId a;
    NodeId b;
    bool partitioned;
};

// Loses the next message from `from` to `to` (of `message_type`, if set).
struct ScriptDropNext {
    NodeId from;
    NodeId to;
    std::optional<MessageType> message_type;
};

// Crashes `node` now.
struct ScriptCrash {
    NodeId node;
};

// Restarts `node` now.
struct ScriptRestart {
    NodeId node;
};

// Takes `node` offline without a crash: it keeps its memory but receives
// nothing until ScriptNodeUp.
struct ScriptNodeDown {
    NodeId node;
};

// Brings an offline `node` back online.
struct ScriptNodeUp {
    NodeId node;
};

// Delivers the next `message_type` message from `from` to `to` twice.
struct ScriptDuplicateNext {
    NodeId from;
    NodeId to;
    MessageType message_type;
};

// Client submission of a PROPOSE. Delivered directly to the coordinator at the
// scripted time (the client is co-located), so it consumes no RNG draws.
struct ScriptSubmitTrade {
    TradeRequest request;
};

// One scripted action of any of the kinds above.
struct ScriptActionEvent {
    std::variant<
        ScriptSetLink,
        ScriptSetPartition,
        ScriptDropNext,
        ScriptCrash,
        ScriptRestart,
        ScriptNodeDown,
        ScriptNodeUp,
        ScriptSubmitTrade,
        ScriptDuplicateNext> action;
};

// Log records of one handler batch becoming durable on `node`'s disk. Ignored
// if the node crashed in between (its `epoch` changed).
struct FlushLogEvent {
    NodeId node;
    std::uint64_t epoch;
    std::vector<LogRecord> records;
};

// A message of one handler batch leaving `node` once its log records are
// durable (the write-ahead rule). Ignored after a crash.
struct ReleaseSendEvent {
    NodeId node;
    std::uint64_t epoch;
    Message send;
};

// The data of an Event; the alternative matches its EventKind.
using EventPayload = std::variant<
    std::monostate,
    DeliverMessageEvent,
    DropMessageEvent,
    FlushLogEvent,
    ReleaseSendEvent,
    FireTimerEvent,
    NodeCrashEvent,
    NodeRestartEvent,
    ScriptActionEvent>;

// One scheduled event. Events run in (time_us, seq) order, so events at the
// same time run in the order they were scheduled.
struct Event {
    SimTimeUs time_us;
    SequenceId seq;
    EventKind kind;
    EventPayload payload;
};

// Min-heap of Events ordered by (time_us, seq). The queue numbers events as
// they are scheduled; it never reads a real clock.
//
//   EventQueue queue;
//   queue.schedule(1'000, EventKind::fire_timer, FireTimerEvent{...});
//   while (auto event = queue.pop()) { ... }
class EventQueue {
  public:
    // Returns heap size for tests and loop diagnostics.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return queue_.size();
    }

    // Returns true when no event is waiting.
    [[nodiscard]] bool empty() const noexcept
    {
        return queue_.empty();
    }

    // Adds an event at `time_us` and returns its sequence number. Returns
    // std::nullopt (and adds nothing) for a negative time or once the sequence
    // numbers are used up.
    [[nodiscard]] std::optional<SequenceId> schedule(
        SimTimeUs time_us,
        EventKind kind,
        EventPayload payload = std::monostate{})
    {
        // Called by the simulator, network model, timer code, and scenarios.
        // Negative time is invalid because virtual time starts at zero.
        if (time_us < 0 || exhausted_) {
            return std::nullopt;
        }

        const SequenceValue raw_seq = next_sequence_;
        if (raw_seq == std::numeric_limits<SequenceValue>::max()) {
            exhausted_ = true;
        } else {
            ++next_sequence_;
        }

        queue_.push(Event{
            time_us,
            SequenceId{raw_seq},
            kind,
            std::move(payload),
        });
        return SequenceId{raw_seq};
    }

    // Returns the next event without removing it. Tests and loop diagnostics
    // call this before pop() to inspect the virtual-time head.
    [[nodiscard]] std::optional<Event> peek() const
    {
        if (queue_.empty()) {
            return std::nullopt;
        }
        return queue_.top();
    }

    // Removes and returns the earliest event, or std::nullopt if none is left.
    [[nodiscard]] std::optional<Event> pop()
    {
        // Removes and returns the earliest event. VirtualClock calls pop() once
        // per simulation step and advances from the returned event's timestamp.
        if (queue_.empty()) {
            return std::nullopt;
        }

        Event event = queue_.top();
        queue_.pop();
        return event;
    }

  private:
    struct EarlierEvent {
        [[nodiscard]] bool operator()(const Event& lhs, const Event& rhs) const noexcept
        {
            if (lhs.time_us != rhs.time_us) {
                return lhs.time_us > rhs.time_us;
            }
            return lhs.seq.value() > rhs.seq.value();
        }
    };

    std::priority_queue<Event, std::vector<Event>, EarlierEvent> queue_;
    SequenceValue next_sequence_ = 0;
    bool exhausted_ = false;
};

} // namespace glassbox
