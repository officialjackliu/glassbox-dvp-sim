#include "sim/virtual_clock.hpp"

#include <cassert>

int main()
{
    glassbox::VirtualClock clock;
    glassbox::EventQueue queue;

    assert(clock.now_us() == 0);
    assert(!clock.pop_next(queue));
    assert(clock.now_us() == 0);

    assert(queue.schedule(
        100,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{"first"}));
    assert(queue.schedule(
        250,
        glassbox::EventKind::node_crash,
        glassbox::NodeCrashEvent{glassbox::NodeId::jpm}));
    assert(queue.schedule(
        250,
        glassbox::EventKind::node_restart,
        glassbox::NodeRestartEvent{glassbox::NodeId::jpm}));

    const auto first = clock.pop_next(queue);
    assert(first);
    assert(clock.now_us() == 100);

    const auto crash = clock.pop_next(queue);
    assert(crash);
    assert(clock.now_us() == 250);

    const auto restart = clock.pop_next(queue);
    assert(restart);
    assert(clock.now_us() == 250);
    assert(!clock.pop_next(queue));

    glassbox::MeasuredNs measured_ns = 0;
    int state = 1;
    const auto measured = glassbox::measure_handler([&]() noexcept {
        state = state == 1 ? 2 : 1;
    }, measured_ns);
    assert(measured == measured_ns);

    const glassbox::SyntheticTraceEvent trace{
        .sim_time_us = clock.now_us(),
        .measured_ns = measured_ns,
        .kind = "STATE_CHANGE",
    };

    assert(trace.sim_time_us == 250);
    assert(trace.measured_ns >= 0);
    assert(state == 2);
    assert(trace.sim_time_us == 250);

    return 0;
}
