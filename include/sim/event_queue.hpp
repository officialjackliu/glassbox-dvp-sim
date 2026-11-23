#pragma once

// Deterministic min-heap event scheduling for the discrete-event simulation.
//
// EventQueue is a low-level collection: callers assign virtual time and payload
// data, while the queue assigns insertion sequence and returns earliest events
// first. It performs no real-time or wall-clock operations.

#include "sim/core_types.hpp"

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

enum class EventKind : std::uint8_t {
    deliver_message,
    drop_message,
    fire_timer,
    node_crash,
    node_restart,
    script_action,
};

enum class TimerKind : std::uint8_t {
    vote_timeout,
    retransmit,
    decision_wait,
};

enum class DropReason : std::uint8_t {
    random,
    partition,
    node_down,
    next_message,
};

enum class MessageType : std::uint8_t {
    propose,
    prepare,
    vote,
    commit,
    abort,
    ack,
    outcome_query,
};

constexpr std::string_view to_string(MessageType type) noexcept
{
    switch (type) {
    case MessageType::propose:
        return "PROPOSE";
    case MessageType::prepare:
        return "PREPARE";
    case MessageType::vote:
        return "VOTE";
    case MessageType::commit:
        return "COMMIT";
    case MessageType::abort:
        return "ABORT";
    case MessageType::ack:
        return "ACK";
    case MessageType::outcome_query:
        return "OUTCOME_QUERY";
    }
    return "UNKNOWN";
}

struct DeliverMessageEvent {
    MessageType msg_type;
    NodeId from;
    NodeId to;
    std::optional<TradeId> trade_id;
    std::uint64_t msg_id;
    SimTimeUs sent_at_us;
    SimTimeUs deliver_at_us;
    SimTimeUs simulated_latency_us;
};

struct DropMessageEvent {
    MessageType msg_type;
    NodeId from;
    NodeId to;
    DropReason reason;
    SimTimeUs sent_at_us;
    SimTimeUs deliver_at_us;
};

struct FireTimerEvent {
    TimerKind kind;
    std::optional<TradeId> trade_id;
    std::string expected_state;
};

struct NodeCrashEvent {
    NodeId node;
};

struct NodeRestartEvent {
    NodeId node;
};

struct ScriptActionEvent {
    std::string name;
};

using EventPayload = std::variant<
    std::monostate,
    DeliverMessageEvent,
    DropMessageEvent,
    FireTimerEvent,
    NodeCrashEvent,
    NodeRestartEvent,
    ScriptActionEvent>;

struct Event {
    SimTimeUs time_us;
    SequenceId seq;
    EventKind kind;
    EventPayload payload;
};

class EventQueue {
  public:
    // Returns heap size for tests and loop diagnostics.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return queue_.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return queue_.empty();
    }

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
