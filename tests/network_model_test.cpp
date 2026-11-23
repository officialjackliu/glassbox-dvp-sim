#include "sim/network_model.hpp"
#include "check.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

int main()
{
    glassbox::DeterministicRng rng{42};
    glassbox::NetworkModel network{rng};

    CHECK(network.link(glassbox::NodeId::coordinator,
                       glassbox::NodeId::bank_a)
              .base_latency_us == 1'000);
    CHECK(network.link(glassbox::NodeId::coordinator,
                       glassbox::NodeId::bank_b)
              .base_latency_us == 35'000);
    CHECK(network.link(glassbox::NodeId::coordinator,
                       glassbox::NodeId::bank_c)
              .base_latency_us == 120'000);
    CHECK(network.link(glassbox::NodeId::bank_b,
                       glassbox::NodeId::bank_c)
              .base_latency_us == 85'000);

    network.set_link(
        glassbox::NodeId::bank_a,
        glassbox::NodeId::bank_b,
        glassbox::LinkConfig{500, 100, 0});
    network.set_link(
        glassbox::NodeId::bank_b,
        glassbox::NodeId::bank_a,
        glassbox::LinkConfig{900, 20, 0});

    CHECK(network.link(glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b)
              .base_latency_us == 500);
    CHECK(network.link(glassbox::NodeId::bank_b,
                       glassbox::NodeId::bank_a)
              .base_latency_us == 900);

    for (int i = 0; i < 1000; ++i) {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{static_cast<std::uint64_t>(i)};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{1, 100, 0});
        const auto result = local_network.schedule_delivery(
            queue,
            0,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(result.delivered);
        CHECK(result.deliver_time_us >= 1);
        CHECK(result.deliver_time_us <= 101);
    }

    {
        glassbox::EventQueue queue;
        network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{1, 100, 0});
        const auto clamped = network.schedule_delivery(
            queue,
            0,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(clamped.deliver_time_us >= 1);
        CHECK(clamped.deliver_time_us <= 101);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{7};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{300, 0, 0});
        const auto first = local_network.schedule_delivery(
            queue,
            100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{50, 0, 0});
        const auto second = local_network.schedule_delivery(
            queue,
            110,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::vote);

        CHECK(first.delivered);
        CHECK(second.delivered);
        CHECK(second.deliver_time_us == 160);
        CHECK(first.deliver_time_us == 400);

        const auto earlier = queue.pop();
        const auto later = queue.pop();
        CHECK(earlier);
        CHECK(later);
        CHECK(earlier->time_us == 160);
        CHECK(later->time_us == 400);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{11};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 0, 1'000'000});
        const auto dropped = local_network.schedule_delivery(
            queue,
            20,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(!dropped.delivered);
        CHECK(dropped.drop_reason ==
              glassbox::DropReason::random);
        CHECK(!queue.empty());
        const auto drop = queue.pop();
        CHECK(drop);
        CHECK(drop->kind == glassbox::EventKind::drop_message);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{12};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            true);
        const auto dropped = local_network.schedule_delivery(
            queue,
            30,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::MessageType::prepare);
        CHECK(!dropped.delivered);
        CHECK(dropped.drop_reason ==
              glassbox::DropReason::partition);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{13};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_node_down(glassbox::NodeId::bank_c, true);
        const auto dropped = local_network.schedule_delivery(
            queue,
            40,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::MessageType::prepare);
        CHECK(!dropped.delivered);
        CHECK(dropped.drop_reason ==
              glassbox::DropReason::node_down);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{14};
        glassbox::NetworkModel local_network{local_rng};
        local_network.drop_next(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        const auto dropped = local_network.schedule_delivery(
            queue,
            50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(!dropped.delivered);
        CHECK(dropped.drop_reason ==
              glassbox::DropReason::next_message);
        CHECK(!queue.empty());
        const auto drop = queue.pop();
        CHECK(drop);
        CHECK(drop->kind == glassbox::EventKind::drop_message);

        const auto delivered = local_network.schedule_delivery(
            queue,
            60,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(delivered.delivered);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{15};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 0, 0});
        local_network.set_link(
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            glassbox::LinkConfig{900, 0, 0});
        const auto forward = local_network.schedule_delivery(
            queue,
            70,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        const auto reverse = local_network.schedule_delivery(
            queue,
            70,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            glassbox::MessageType::vote);
        CHECK(forward.delivered);
        CHECK(reverse.delivered);
        CHECK(forward.deliver_time_us == 170);
        CHECK(reverse.deliver_time_us == 970);
    }

    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{16};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 0, 0});
        const auto first = local_network.schedule_delivery(
            queue,
            50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        const auto second = local_network.schedule_delivery(
            queue,
            50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::vote);
        CHECK(first.delivered);
        CHECK(second.delivered);
        const auto first_event = queue.pop();
        const auto second_event = queue.pop();
        CHECK(first_event);
        CHECK(second_event);
        CHECK(first_event->time_us == second_event->time_us);
        CHECK(first_event->seq.value() < second_event->seq.value());
        const auto& first_payload =
            std::get<glassbox::DeliverMessageEvent>(first_event->payload);
        CHECK(first_payload.msg_type == glassbox::MessageType::prepare);
    }

    // All directed defaults come from the DESIGN.md regional placement.
    {
        const std::array<glassbox::NodeId, 4> nodes{
            glassbox::NodeId::coordinator,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_c,
        };
        glassbox::DeterministicRng local_rng{17};
        glassbox::NetworkModel local_network{local_rng};

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                const glassbox::NodeId from = nodes[i];
                const glassbox::NodeId to = nodes[j];
                const auto latency =
                    local_network.link(from, to).base_latency_us;

                if (from == to ||
                    (from == glassbox::NodeId::coordinator &&
                     to == glassbox::NodeId::bank_a) ||
                    (from == glassbox::NodeId::bank_a &&
                     to == glassbox::NodeId::coordinator)) {
                    CHECK(latency == 1'000);
                } else if ((from == glassbox::NodeId::coordinator &&
                            to == glassbox::NodeId::bank_b) ||
                           (from == glassbox::NodeId::bank_b &&
                            to == glassbox::NodeId::coordinator) ||
                           (from == glassbox::NodeId::bank_a &&
                            to == glassbox::NodeId::bank_b) ||
                           (from == glassbox::NodeId::bank_b &&
                            to == glassbox::NodeId::bank_a)) {
                    CHECK(latency == 35'000);
                } else if ((from == glassbox::NodeId::coordinator &&
                            to == glassbox::NodeId::bank_c) ||
                           (from == glassbox::NodeId::bank_c &&
                            to == glassbox::NodeId::coordinator) ||
                           (from == glassbox::NodeId::bank_a &&
                            to == glassbox::NodeId::bank_c) ||
                           (from == glassbox::NodeId::bank_c &&
                            to == glassbox::NodeId::bank_a)) {
                    CHECK(latency == 120'000);
                } else {
                    CHECK(latency == 85'000);
                }
            }
        }
    }

    // Jitter must actually change delivery latency.
    {
        glassbox::DeterministicRng local_rng{18};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{1'000, 100, 0});

        glassbox::SimTimeUs min_latency =
            std::numeric_limits<glassbox::SimTimeUs>::max();
        glassbox::SimTimeUs max_latency = 0;
        bool found_distinct = false;
        glassbox::SimTimeUs first_latency = 0;

        for (int i = 0; i < 5'000; ++i) {
            glassbox::EventQueue queue;
            const auto result = local_network.schedule_delivery(
                queue,
                i,
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::MessageType::prepare);
            CHECK(result.delivered);
            const auto latency =
                result.deliver_time_us - result.send_time_us;
            if (i == 0) {
                first_latency = latency;
            } else if (latency != first_latency) {
                found_distinct = true;
            }
            min_latency = latency < min_latency ? latency : min_latency;
            max_latency = latency > max_latency ? latency : max_latency;
        }
        CHECK(min_latency >= 895);
        CHECK(min_latency <= 905);
        CHECK(max_latency >= 1'095);
        CHECK(max_latency <= 1'105);
        CHECK(found_distinct);

        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{1'000, 0, 0});
        glassbox::EventQueue zero_queue;
        const auto zero = local_network.schedule_delivery(
            zero_queue,
            10,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(zero.deliver_time_us - zero.send_time_us == 1'000);
    }

    // Bad link input is normalized and documented in stored state.
    {
        glassbox::DeterministicRng local_rng{19};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{-7, -3, 2'000'000});
        const glassbox::LinkConfig& normalized =
            local_network.link(glassbox::NodeId::bank_a,
                               glassbox::NodeId::bank_b);
        CHECK(normalized.base_latency_us == 1);
        CHECK(normalized.jitter_us == 0);
        CHECK(normalized.drop_probability_ppm == 1'000'000);
    }

    // Drop precedence is partition, node-down, next-message, random.
    {
        for (int mode = 0; mode < 4; ++mode) {
            glassbox::EventQueue queue;
            glassbox::DeterministicRng local_rng{20};
            glassbox::NetworkModel local_network{local_rng};
            local_network.set_link(
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::LinkConfig{100, 0, 1'000'000});

            if (mode <= 0) {
                local_network.set_partition(
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                    true);
            }
            if (mode <= 1) {
                local_network.set_node_down(glassbox::NodeId::bank_b, true);
            }
            if (mode <= 2) {
                local_network.drop_next(
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                    glassbox::MessageType::commit);
            }

            const auto result = local_network.schedule_delivery(
                queue,
                100,
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::MessageType::commit);
            CHECK(!result.delivered);
            CHECK(result.status == glassbox::NetworkScheduleStatus::dropped);
            if (mode == 0) {
                CHECK(result.drop_reason ==
                      glassbox::DropReason::partition);
            } else if (mode == 1) {
                CHECK(result.drop_reason ==
                      glassbox::DropReason::node_down);
            } else if (mode == 2) {
                CHECK(result.drop_reason ==
                      glassbox::DropReason::next_message);
            } else {
                CHECK(result.drop_reason ==
                      glassbox::DropReason::random);
            }
        }
    }

    // Partition symmetry and isolation.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{21};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            true);
        for (const auto pair : std::array<std::pair<glassbox::NodeId,
                                                   glassbox::NodeId>, 2>{
                 std::pair{glassbox::NodeId::bank_a,
                           glassbox::NodeId::bank_b},
                 std::pair{glassbox::NodeId::bank_b,
                           glassbox::NodeId::bank_a}}) {
            const auto result = local_network.schedule_delivery(
                queue,
                100,
                pair.first,
                pair.second,
                glassbox::MessageType::prepare);
            CHECK(!result.delivered);
            CHECK(result.drop_reason ==
                  glassbox::DropReason::partition);
        }
        const auto unaffected = local_network.schedule_delivery(
            queue,
            100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::MessageType::prepare);
        CHECK(unaffected.delivered);

        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            false);
        const auto ab = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        const auto ba = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            glassbox::MessageType::prepare);
        CHECK(ab.delivered);
        CHECK(ba.delivered);
    }

    // Node down blocks inbound, not outbound.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{22};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_node_down(glassbox::NodeId::bank_b, true);
        const auto inbound = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        const auto outbound = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            glassbox::MessageType::vote);
        CHECK(!inbound.delivered);
        CHECK(inbound.drop_reason ==
              glassbox::DropReason::node_down);
        CHECK(outbound.delivered);
    }

    // drop_next matches type and directed link, and is consumed on a match.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{23};
        glassbox::NetworkModel local_network{local_rng};
        local_network.drop_next(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);

        const auto wrong_type = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        const auto wrong_direction = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            glassbox::MessageType::commit);
        const auto wrong_destination = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::MessageType::commit);
        const auto matching = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        const auto after = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);

        CHECK(wrong_type.delivered);
        CHECK(wrong_direction.delivered);
        CHECK(wrong_destination.delivered);
        CHECK(!matching.delivered);
        CHECK(matching.drop_reason ==
              glassbox::DropReason::next_message);
        CHECK(after.delivered);
    }

    // wildcard drop_next affects the next message on the directed link.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{24};
        glassbox::NetworkModel local_network{local_rng};
        local_network.drop_next(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b);
        const auto first = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::vote);
        const auto second = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(!first.delivered);
        CHECK(first.drop_reason ==
              glassbox::DropReason::next_message);
        CHECK(second.delivered);
    }

    // drop_next is not consumed by partitioned sends, and then waits for
    // the next send that would otherwise be delivered.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{25};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 0, 0});
        local_network.drop_next(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            true);
        const auto partitioned = local_network.schedule_delivery(
            queue, 100,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(!partitioned.delivered);
        CHECK(partitioned.drop_reason ==
              glassbox::DropReason::partition);

        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            false);
        const auto dropped = local_network.schedule_delivery(
            queue, 110,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(!dropped.delivered);
        CHECK(dropped.drop_reason ==
              glassbox::DropReason::next_message);
        const auto after = local_network.schedule_delivery(
            queue, 120,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit);
        CHECK(after.delivered);
    }

    // Drop event payload and delivery timing fields are complete.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{26};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 10, 1'000'000});

        const auto random = local_network.schedule_delivery(
            queue, 50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::commit,
            41,
            glassbox::TradeId{7});
        CHECK(!random.delivered);
        CHECK(random.status == glassbox::NetworkScheduleStatus::dropped);

        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            true);
        const auto partitioned = local_network.schedule_delivery(
            queue, 70,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare,
            42,
            glassbox::TradeId{8});
        CHECK(!partitioned.delivered);

        local_network.set_partition(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            false);
        local_network.drop_next(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::ack);
        const auto next = local_network.schedule_delivery(
            queue, 90,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::ack,
            43,
            glassbox::TradeId{9});
        CHECK(!next.delivered);

        for (int i = 0; i < 3; ++i) {
            const auto event = queue.pop();
            CHECK(event);
            CHECK(event->kind == glassbox::EventKind::drop_message);
            const auto& payload =
                std::get<glassbox::DropMessageEvent>(event->payload);
            CHECK(payload.from == glassbox::NodeId::bank_a);
            CHECK(payload.to == glassbox::NodeId::bank_b);
            CHECK(payload.deliver_at_us >= payload.sent_at_us);
        }
    }

    // Delivered payload has send, arrival and simulated latency fields.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{27};
        glassbox::NetworkModel local_network{local_rng};
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{100, 10, 0});
        const auto result = local_network.schedule_delivery(
            queue, 500,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare,
            44,
            glassbox::TradeId{10});
        CHECK(result.delivered);
        const auto event = queue.pop();
        CHECK(event);
        const auto& payload =
            std::get<glassbox::DeliverMessageEvent>(event->payload);
        CHECK(payload.msg_type == glassbox::MessageType::prepare);
        CHECK(payload.sent_at_us == 500);
        CHECK(payload.deliver_at_us ==
              result.deliver_time_us);
        CHECK(payload.simulated_latency_us ==
              payload.deliver_at_us - payload.sent_at_us);
        CHECK(payload.msg_id == 44);
        CHECK(payload.trade_id &&
              payload.trade_id->value() == 10);
    }

    // Search seeds until jitter reorders two nearby sends.
    {
        bool reordered = false;
        for (std::uint64_t seed = 28; seed < 100 && !reordered; ++seed) {
            glassbox::EventQueue queue;
            glassbox::DeterministicRng local_rng{seed};
            glassbox::NetworkModel local_network{local_rng};
            local_network.set_link(
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::LinkConfig{500, 500, 0});
            const auto later_send = local_network.schedule_delivery(
                queue, 10,
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::MessageType::vote);
            const auto earlier_send = local_network.schedule_delivery(
                queue, 0,
                glassbox::NodeId::bank_a,
                glassbox::NodeId::bank_b,
                glassbox::MessageType::prepare);
            CHECK(later_send.delivered);
            CHECK(earlier_send.delivered);

            const auto first = queue.pop();
            const auto second = queue.pop();
            CHECK(first);
            CHECK(second);
            if (first->time_us > second->time_us) {
                CHECK(false);
            }
            const auto& first_payload =
                std::get<glassbox::DeliverMessageEvent>(first->payload);
            if (first_payload.sent_at_us == 10) {
                reordered = true;
            }
        }
        CHECK(reordered);
    }

    // Same seed schedules identically; changing one drop probability must not
    // change the fixed jitter draw sequence.
    {
        const auto run_schedule =
            [](std::uint64_t seed,
               std::uint32_t drop_ppm)
            {
                glassbox::EventQueue queue;
                glassbox::DeterministicRng local_rng{seed};
                glassbox::NetworkModel local_network{local_rng};
                local_network.set_link(
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                    glassbox::LinkConfig{1'000, 100, drop_ppm});
                std::vector<std::pair<glassbox::SimTimeUs, bool>>
                    schedule;
                for (int i = 0; i < 100; ++i) {
                    const auto result = local_network.schedule_delivery(
                        queue,
                        i,
                        glassbox::NodeId::bank_a,
                        glassbox::NodeId::bank_b,
                        i % 2 == 0
                            ? glassbox::MessageType::prepare
                            : glassbox::MessageType::vote);
                    schedule.emplace_back(
                        result.deliver_time_us,
                        result.delivered);
                }
                std::vector<glassbox::SimTimeUs> latencies;
                while (const auto event = queue.pop()) {
                    latencies.push_back(event->time_us);
                }
                return std::pair{schedule, latencies};
            };

        const auto first = run_schedule(42, 0);
        const auto repeated = run_schedule(42, 0);
        const auto changed_drop = run_schedule(42, 1);
        CHECK(first == repeated);
        for (int i = 0; i < 100; ++i) {
            const glassbox::SimTimeUs first_latency =
                first.first[i].first - i;
            const glassbox::SimTimeUs changed_latency =
                changed_drop.first[i].first - i;
            CHECK(first_latency == changed_latency);
        }
    }

    // Scheduling failures have explicit statuses.
    {
        glassbox::EventQueue queue;
        glassbox::DeterministicRng local_rng{100};
        glassbox::NetworkModel local_network{local_rng};
        const auto negative = local_network.schedule_delivery(
            queue, -1,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(!negative.delivered);
        CHECK(negative.status ==
              glassbox::NetworkScheduleStatus::invalid_send_time);
        CHECK(!negative.drop_reason);

        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{
                std::numeric_limits<glassbox::SimTimeUs>::max(),
                0,
                0});
        const auto overflow = local_network.schedule_delivery(
            queue, 1,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::MessageType::prepare);
        CHECK(!overflow.delivered);
        CHECK(overflow.status ==
              glassbox::NetworkScheduleStatus::delivery_time_overflow);
        CHECK(!overflow.drop_reason);
    }

    return 0;
}
