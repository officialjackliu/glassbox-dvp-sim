#pragma once

// Configurable directed-link network model for the discrete-event simulator.
//
// NetworkModel consumes scenario RNG draws, applies partitions/node-down state
// and explicit next-message drops, then schedules either a MSG_DELIVER or
// MSG_DROP event through EventQueue. It has no threads or real-time operations.

#include "sim/deterministic_rng.hpp"
#include "sim/event_queue.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace glassbox {

// Number of nodes (COORD, BANK_A, BANK_B, BANK_C); sizes per-node tables.
inline constexpr std::size_t k_node_count = 4;

// One direction of a link: base one-way delay, random extra delay of up to
// `jitter_us`, and chance of losing a message in parts per million.
struct LinkConfig {
    // Illustrative defaults, not measured network performance.
    SimTimeUs base_latency_us = 1;
    SimTimeUs jitter_us = 0;
    std::uint32_t drop_probability_ppm = 0;
};

// A pending one-shot fault on a directed link: affects the next message (of
// `message_type`, if set).
struct NextDrop {
    NodeId from;
    NodeId to;
    std::optional<MessageType> message_type;
};

// Outcome of handing a message to the network.
enum class NetworkScheduleStatus : std::uint8_t {
    scheduled,
    dropped,
    invalid_send_time,
    delivery_time_overflow,
    event_queue_failure,
};

// What schedule_delivery() did with a message: delivered or dropped (and
// why), its event, and when it was sent and arrives (or would have).
struct NetworkScheduleResult {
    NetworkScheduleStatus status = NetworkScheduleStatus::event_queue_failure;
    bool delivered = false;
    std::optional<DropReason> drop_reason;
    std::optional<SequenceId> event_seq;
    SimTimeUs send_time_us = 0;
    SimTimeUs deliver_time_us = 0;
};

// The simulated network: a latency/jitter/drop setting for every directed
// link, partitions, offline nodes, and one-shot drop/duplicate faults. It turns
// a send into a delivery or drop event on the queue. Every message consumes
// exactly one jitter draw and one drop draw from the shared RNG, so runs with
// the same seed are identical.
//
// Default one-way latencies (illustrative): 1 ms within New York (COORD,
// BANK_A), 35 ms New York - London (BANK_B), 120 ms New York - Singapore
// (BANK_C), 85 ms London - Singapore.
class NetworkModel {
  public:
    // Uses `rng` (which must outlive the model) for all random draws and starts
    // with the default links.
    explicit NetworkModel(DeterministicRng& rng)
        : rng_(rng)
    {
        for (std::size_t i = 0; i < k_node_count; ++i) {
            for (std::size_t j = 0; j < k_node_count; ++j) {
                link_[i][j] =
                    i == j
                        ? LinkConfig{1'000, 0, 0}
                        : LinkConfig{35'000, 0, 0};
            }
        }

        // Placeholder regional defaults:
        // NY=COORD/BANK_A, London=BANK_B, Singapore=BANK_C.
        set_link(NodeId::coordinator, NodeId::coordinator, LinkConfig{1'000, 0, 0});
        set_link(NodeId::bank_a, NodeId::bank_a, LinkConfig{1'000, 0, 0});
        set_link(NodeId::bank_b, NodeId::bank_b, LinkConfig{1'000, 0, 0});
        set_link(NodeId::bank_c, NodeId::bank_c, LinkConfig{1'000, 0, 0});
        set_link(NodeId::coordinator, NodeId::bank_a, LinkConfig{1'000, 0, 0});
        set_link(NodeId::bank_a, NodeId::coordinator, LinkConfig{1'000, 0, 0});

        set_link(NodeId::coordinator, NodeId::bank_b, LinkConfig{35'000, 0, 0});
        set_link(NodeId::bank_b, NodeId::coordinator, LinkConfig{35'000, 0, 0});
        set_link(NodeId::bank_a, NodeId::bank_b, LinkConfig{35'000, 0, 0});
        set_link(NodeId::bank_b, NodeId::bank_a, LinkConfig{35'000, 0, 0});

        set_link(NodeId::coordinator, NodeId::bank_c, LinkConfig{120'000, 0, 0});
        set_link(NodeId::bank_c, NodeId::coordinator, LinkConfig{120'000, 0, 0});
        set_link(NodeId::bank_a, NodeId::bank_c, LinkConfig{120'000, 0, 0});
        set_link(NodeId::bank_c, NodeId::bank_a, LinkConfig{120'000, 0, 0});

        set_link(NodeId::bank_b, NodeId::bank_c, LinkConfig{85'000, 0, 0});
        set_link(NodeId::bank_c, NodeId::bank_b, LinkConfig{85'000, 0, 0});
    }

    // All configuration is independent for (from, to). This does not imply
    // symmetry; callers set both directions explicitly for a symmetric link.
    void set_link(NodeId from, NodeId to, const LinkConfig& config) noexcept
    {
        const LinkConfig normalized{
            config.base_latency_us < 1 ? 1 : config.base_latency_us,
            config.jitter_us < 0 ? 0 : config.jitter_us,
            config.drop_probability_ppm > 1'000'000
                ? 1'000'000
                : config.drop_probability_ppm,
        };
        link_[node_index(from)][node_index(to)] = normalized;
    }

    // Returns the current setting of the `from` -> `to` link.
    [[nodiscard]] const LinkConfig& link(NodeId from, NodeId to) const noexcept
    {
        return link_[node_index(from)][node_index(to)];
    }

    // Cuts (`partitioned` true) or restores the link between `a` and `b`, both
    // directions. Checked when a message is sent.
    void set_partition(NodeId a, NodeId b, bool partitioned) noexcept
    {
        partition_[node_index(a)][node_index(b)] = partitioned;
        partition_[node_index(b)][node_index(a)] = partitioned;
    }

    // Returns true if the `a` -> `b` direction is cut.
    [[nodiscard]] bool partitioned(NodeId a, NodeId b) const noexcept
    {
        return partition_[node_index(a)][node_index(b)];
    }

    // Marks `node` as unreachable (or reachable again).
    void set_node_down(NodeId node, bool down) noexcept
    {
        node_down_[node_index(node)] = down;
    }

    // Returns true if `node` is marked unreachable.
    [[nodiscard]] bool node_down(NodeId node) const noexcept
    {
        return node_down_[node_index(node)];
    }

    // Removes and drops the next matching message on this directed from->to
    // link. The wildcard overload matches the next message of any type.
    void drop_next(NodeId from, NodeId to, MessageType message_type)
    {
        next_drops_.push_back(
            NextDrop{
                from,
                to,
                message_type,
            });
    }

    void drop_next(NodeId from, NodeId to)
    {
        next_drops_.push_back(NextDrop{from, to, std::nullopt});
    }

    // The next matching message on this directed link is delivered twice.
    // The copy keeps the msg_id and consumes its own jitter + drop draws
    // (it is a second message on the wire), right after the original's.
    void duplicate_next(NodeId from, NodeId to, MessageType message_type)
    {
        next_duplicates_.push_back(NextDrop{from, to, message_type});
    }

    // Called by send-side simulator code. It always consumes the configured
    // jitter and PPM drop draws before applying non-random fault precedence.
    // node_down(to) is a send-time check here; the simulator checks the
    // destination again on arrival (MSG_DROP_NODE_DOWN), which covers nodes
    // crashing or restarting while a message is in flight.
    [[nodiscard]] NetworkScheduleResult schedule_delivery(
        EventQueue& queue,
        SimTimeUs send_time_us,
        NodeId from,
        NodeId to,
        MessageType message_type,
        std::uint64_t msg_id = 0,
        std::optional<TradeId> trade_id = std::nullopt)
    {
        return schedule_delivery(
            queue,
            send_time_us,
            Message{
                msg_id,
                from,
                to,
                message_type,
                trade_id,
                std::monostate{},
            });
    }

    // Full-message overload: the body travels with the message unchanged.
    // Draw order is identical to the field overload (one jitter + one drop).
    [[nodiscard]] NetworkScheduleResult schedule_delivery(
        EventQueue& queue,
        SimTimeUs send_time_us,
        Message message)
    {
        bool duplicate = false;
        for (std::size_t i = 0; i < next_duplicates_.size(); ++i) {
            const NextDrop& next = next_duplicates_[i];
            if (next.from == message.from && next.to == message.to &&
                next.message_type == message.type) {
                next_duplicates_.erase(next_duplicates_.begin() +
                                       static_cast<std::ptrdiff_t>(i));
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            return schedule_one(queue, send_time_us, std::move(message));
        }
        Message copy = message;
        NetworkScheduleResult original =
            schedule_one(queue, send_time_us, std::move(message));
        static_cast<void>(schedule_one(queue, send_time_us, std::move(copy)));
        return original;
    }

  private:
    [[nodiscard]] NetworkScheduleResult schedule_one(
        EventQueue& queue,
        SimTimeUs send_time_us,
        Message message)
    {
        if (send_time_us < 0) {
            return {NetworkScheduleStatus::invalid_send_time,
                    false, std::nullopt, std::nullopt,
                    send_time_us, 0};
        }

        const NodeId from = message.from;
        const NodeId to = message.to;
        const LinkConfig& config = link(from, to);
        const DeterministicNetworkDraw random_draw =
            draw_network_delivery(
                rng_,
                config.base_latency_us,
                config.jitter_us,
                config.drop_probability_ppm);

        const auto deliver_at =
            checked_add_i64(send_time_us, random_draw.latency_us);
        if (!deliver_at) {
            return {NetworkScheduleStatus::delivery_time_overflow,
                    false, std::nullopt, std::nullopt,
                    send_time_us, 0};
        }

        std::optional<DropReason> reason;
        if (partitioned(from, to)) {
            reason = DropReason::partition;
        } else if (node_down(to)) {
            reason = DropReason::node_down;
        } else {
            for (std::size_t i = 0; i < next_drops_.size(); ++i) {
                const NextDrop& next = next_drops_[i];
                if (next.from == from && next.to == to &&
                    (!next.message_type ||
                     *next.message_type == message.type)) {
                    next_drops_.erase(next_drops_.begin() +
                                      static_cast<std::ptrdiff_t>(i));
                    reason = DropReason::next_message;
                    break;
                }
            }
        }

        if (!reason && random_draw.dropped) {
            reason = DropReason::random;
        }

        if (reason) {
            const auto seq = queue.schedule(
                *deliver_at,
                EventKind::drop_message,
                DropMessageEvent{
                    message,
                    *reason,
                    send_time_us,
                    *deliver_at,
                });
            return {NetworkScheduleStatus::dropped,
                    false, reason, seq,
                    send_time_us, *deliver_at};
        }

        const auto seq = queue.schedule(
            *deliver_at,
            EventKind::deliver_message,
            DeliverMessageEvent{
                std::move(message),
                to,
                send_time_us,
                *deliver_at,
                random_draw.latency_us,
            });
        if (!seq) {
            return {NetworkScheduleStatus::event_queue_failure,
                    false, std::nullopt, std::nullopt,
                    send_time_us, *deliver_at};
        }
        return {NetworkScheduleStatus::scheduled,
                true, std::nullopt, seq,
                send_time_us, *deliver_at};
    }

    [[nodiscard]] static std::size_t node_index(NodeId node) noexcept
    {
        return static_cast<std::size_t>(node);
    }

    DeterministicRng& rng_;
    std::array<std::array<LinkConfig, k_node_count>, k_node_count> link_{};
    std::array<std::array<bool, k_node_count>, k_node_count> partition_{};
    std::array<bool, k_node_count> node_down_{};
    std::vector<NextDrop> next_drops_;
    std::vector<NextDrop> next_duplicates_;
};

} // namespace glassbox
