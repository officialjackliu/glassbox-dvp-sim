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
#include <vector>

namespace glassbox {

inline constexpr std::size_t k_node_count = 4;

struct LinkConfig {
    // Illustrative defaults, not measured network performance.
    SimTimeUs base_latency_us = 1;
    SimTimeUs jitter_us = 0;
    std::uint32_t drop_probability_ppm = 0;
};

struct NextDrop {
    NodeId from;
    NodeId to;
    std::optional<MessageType> message_type;
};

enum class NetworkScheduleStatus : std::uint8_t {
    scheduled,
    dropped,
    invalid_send_time,
    delivery_time_overflow,
    event_queue_failure,
};

struct NetworkScheduleResult {
    NetworkScheduleStatus status = NetworkScheduleStatus::event_queue_failure;
    bool delivered = false;
    std::optional<DropReason> drop_reason;
    std::optional<SequenceId> event_seq;
    SimTimeUs send_time_us = 0;
    SimTimeUs deliver_time_us = 0;
};

class NetworkModel {
  public:
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

    [[nodiscard]] const LinkConfig& link(NodeId from, NodeId to) const noexcept
    {
        return link_[node_index(from)][node_index(to)];
    }

    void set_partition(NodeId a, NodeId b, bool partitioned) noexcept
    {
        partition_[node_index(a)][node_index(b)] = partitioned;
        partition_[node_index(b)][node_index(a)] = partitioned;
    }

    [[nodiscard]] bool partitioned(NodeId a, NodeId b) const noexcept
    {
        return partition_[node_index(a)][node_index(b)];
    }

    void set_node_down(NodeId node, bool down) noexcept
    {
        node_down_[node_index(node)] = down;
    }

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

    // Called by send-side simulator code. It always consumes the configured
    // jitter and PPM drop draws before applying non-random fault precedence.
    // node_down(to) is a send-time check in this component. Task 6's event loop
    // must re-check the destination when a deliver_message event is popped;
    // that arrival-time check handles nodes crashing or restarting mid-flight.
    [[nodiscard]] NetworkScheduleResult schedule_delivery(
        EventQueue& queue,
        SimTimeUs send_time_us,
        NodeId from,
        NodeId to,
        MessageType message_type,
        std::uint64_t msg_id = 0,
        std::optional<TradeId> trade_id = std::nullopt)
    {
        if (send_time_us < 0) {
            return {NetworkScheduleStatus::invalid_send_time,
                    false, std::nullopt, std::nullopt,
                    send_time_us, 0};
        }

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
                     *next.message_type == message_type)) {
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
                    message_type,
                    from,
                    to,
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
                message_type,
                from,
                to,
                trade_id,
                msg_id,
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

  private:
    [[nodiscard]] static std::size_t node_index(NodeId node) noexcept
    {
        return static_cast<std::size_t>(node);
    }

    DeterministicRng& rng_;
    std::array<std::array<LinkConfig, k_node_count>, k_node_count> link_{};
    std::array<std::array<bool, k_node_count>, k_node_count> partition_{};
    std::array<bool, k_node_count> node_down_{};
    std::vector<NextDrop> next_drops_;
};

} // namespace glassbox
