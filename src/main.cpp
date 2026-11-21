#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"
#include "sim/virtual_clock.hpp"

#include <cassert>
#include <iostream>

int main()
{
    const glassbox::TradeId trade_id{1};
    const glassbox::Cents cash{100};
    glassbox::EventQueue queue;
    assert(queue.schedule(
        10,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{"submit trade"}));
    assert(queue.schedule(
        20,
        glassbox::EventKind::deliver_message,
        glassbox::DeliverMessageEvent{
            "PREPARE",
            glassbox::NodeId::coordinator,
            glassbox::NodeId::goldman,
        }));

    std::cout << "glassbox-dvp-sim\n";
    std::cout << "trade=" << trade_id.value()
              << " cents=" << cash.value() << '\n';
    glassbox::VirtualClock clock;
    while (const auto event = clock.pop_next(queue)) {
        std::cout << "t=" << event->time_us << " seq=" << event->seq.value() << '\n';
    }
    glassbox::MeasuredNs measured_ns = 0;
    static_cast<void>(
        glassbox::measure_handler([]() noexcept {}, measured_ns));
    glassbox::write_synthetic_trace(
        std::cout,
        glassbox::SyntheticTraceEvent{
            .sim_time_us = clock.now_us(),
            .measured_ns = measured_ns,
            .kind = "SCRIPT_ACTION",
        });
    return 0;
}
