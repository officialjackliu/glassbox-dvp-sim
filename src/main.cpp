#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"
#include "sim/virtual_clock.hpp"
#include "../tests/check.hpp"

#include <iostream>

// Integration smoke executable:
// 1. Builds representative core values.
// 2. Inserts script and message events into the virtual queue.
// 3. Steps events through VirtualClock.
// 4. Emits a synthetic trace line with separate simulation and measured time.
int main()
{
    const glassbox::TradeId trade_id{1};
    const glassbox::Cents cash{100};
    glassbox::EventQueue queue;
    const auto script_seq = queue.schedule(
        10,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{
            glassbox::ScriptSetLink{
                glassbox::NodeId::coordinator,
                glassbox::NodeId::bank_a,
                1'000,
                0,
                0,
            },
        });
    const auto message_seq = queue.schedule(
        20,
        glassbox::EventKind::deliver_message,
        glassbox::DeliverMessageEvent{
            glassbox::Message{
                41,
                glassbox::NodeId::coordinator,
                glassbox::NodeId::bank_a,
                glassbox::MessageType::prepare,
                glassbox::TradeId{1},
            },
            glassbox::NodeId::bank_a,
            20,
            21,
            1,
        });
    CHECK(script_seq.has_value());
    CHECK(message_seq.has_value());

    std::cout << "glassbox-dvp-sim\n";
    std::cout << "trade=" << trade_id.value()
              << " cents=" << cash.value() << '\n';
    glassbox::VirtualClock clock;
    while (const auto event = clock.pop_next(queue)) {
        std::cout << "t=" << event->time_us << " seq=" << event->seq.value() << '\n';
    }
    const glassbox::MeasuredNs measured_ns =
        glassbox::measure_handler([]() noexcept {});
    glassbox::write_synthetic_trace(
        std::cout,
        glassbox::SyntheticTraceEvent{
            .sim_time_us = clock.now_us(),
            .measured_ns = measured_ns,
            .kind = "SCRIPT_ACTION",
        });
    return 0;
}
