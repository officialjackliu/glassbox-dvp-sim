#pragma once

#include "sim/core_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <variant>
#include <utility>
#include <vector>

namespace glassbox {

enum class EventKind : std::uint8_t {
    deliver_message,
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

struct DeliverMessageEvent {
    std::string msg_type;
    NodeId from;
    NodeId to;
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

    [[nodiscard]] std::optional<Event> peek() const
    {
        if (queue_.empty()) {
            return std::nullopt;
        }
        return queue_.top();
    }

    [[nodiscard]] std::optional<Event> pop()
    {
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
