#include "sim/network_model.hpp"
#include "check.hpp"

#include <cstdint>
#include <string>

int main()
{
    glassbox::DeterministicRng rng{42};
    glassbox::NetworkModel network{rng};

    CHECK(network.link(glassbox::NodeId::coordinator,
                       glassbox::NodeId::bank_a)
              .base_latency_us == 1);
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
            "PREPARE");
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
            "PREPARE");
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
            "PREPARE");
        local_network.set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            glassbox::LinkConfig{50, 0, 0});
        const auto second = local_network.schedule_delivery(
            queue,
            110,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            "VOTE");

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
            "COMMIT");
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
            "PREPARE");
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
            "PREPARE");
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
            "COMMIT");
        const auto dropped = local_network.schedule_delivery(
            queue,
            50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            "COMMIT");
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
            "COMMIT");
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
            "PREPARE");
        const auto reverse = local_network.schedule_delivery(
            queue,
            70,
            glassbox::NodeId::bank_b,
            glassbox::NodeId::bank_a,
            "VOTE");
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
            "FIRST");
        const auto second = local_network.schedule_delivery(
            queue,
            50,
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
            "SECOND");
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
        CHECK(first_payload.msg_type == "FIRST");
    }

    return 0;
}
