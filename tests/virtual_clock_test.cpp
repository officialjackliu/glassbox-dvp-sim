#include "sim/virtual_clock.hpp"
#include "check.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

void RunTimedScenario(glassbox::MeasuredNs& measured_ns,
                      bool slow,
                      glassbox::SimTimeUs& final_time,
                      int& state)
{
    glassbox::VirtualClock clock;
    glassbox::EventQueue queue;

    const auto first = queue.schedule(
        100,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{
            glassbox::ScriptNodeUp{glassbox::NodeId::bank_a}});
    const auto crash = queue.schedule(
        250,
        glassbox::EventKind::node_crash,
        glassbox::NodeCrashEvent{glassbox::NodeId::bank_b});
    CHECK(first.has_value());
    CHECK(crash.has_value());

    const auto pop_first = clock.pop_next(queue);
    CHECK(pop_first);
    CHECK(clock.now_us() == 100);

    const auto pop_crash = clock.pop_next(queue);
    CHECK(pop_crash);

    measured_ns = glassbox::measure_handler([&]() {
        if (slow) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        state = state == 1 ? 2 : 1;
    });
    final_time = clock.now_us();
}

} // namespace

int main()
{
    glassbox::VirtualClock clock;
    glassbox::EventQueue queue;
    CHECK(clock.now_us() == 0);
    CHECK(!clock.pop_next(queue));
    CHECK(clock.now_us() == 0);

    const auto earlier = queue.schedule(
        100,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{
            glassbox::ScriptNodeUp{glassbox::NodeId::bank_a}});
    CHECK(earlier.has_value());
    CHECK(clock.pop_next(queue).has_value());
    CHECK(clock.now_us() == 100);

    const auto past = queue.schedule(
        50,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{
            glassbox::ScriptNodeUp{glassbox::NodeId::bank_a}});
    CHECK(past.has_value());
    bool past_rejected = false;
    try {
        static_cast<void>(clock.pop_next(queue));
    } catch (const std::invalid_argument&) {
        past_rejected = true;
    }
    CHECK(past_rejected);
    CHECK(clock.now_us() == 100);

    glassbox::MeasuredNs fast_ns = 0;
    glassbox::MeasuredNs slow_ns = 0;
    glassbox::SimTimeUs fast_time = 0;
    glassbox::SimTimeUs slow_time = 0;
    int fast_state = 1;
    int slow_state = 1;

    RunTimedScenario(fast_ns, false, fast_time, fast_state);
    RunTimedScenario(slow_ns, true, slow_time, slow_state);

    CHECK(fast_time == slow_time);
    CHECK(fast_state == slow_state);
    CHECK(fast_ns != slow_ns);
    CHECK(fast_time == 250);
    CHECK(fast_state == 2);

    const glassbox::SyntheticTraceEvent fast_trace{
        .sim_time_us = fast_time,
        .measured_ns = fast_ns,
        .kind = "STATE_CHANGE",
    };
    const glassbox::SyntheticTraceEvent slow_trace{
        .sim_time_us = slow_time,
        .measured_ns = slow_ns,
        .kind = "STATE_CHANGE",
    };

    CHECK(fast_trace.sim_time_us == slow_trace.sim_time_us);
    CHECK(fast_trace.kind == slow_trace.kind);

    std::ostringstream trace_output;
    glassbox::write_synthetic_trace(trace_output, fast_trace);
    const std::string expected =
        "{\"sim_time_us\":250,\"measured_ns\":" +
        std::to_string(fast_ns) +
        ",\"kind\":\"STATE_CHANGE\"}\n";
    CHECK(trace_output.str() == expected);

    return 0;
}
