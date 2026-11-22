#include "sim/event_queue.hpp"
#include "check.hpp"

#include <string>

// Focused EventQueue integration test:
// verifies empty behavior, equal-time FIFO sequence, earlier-time ordering,
// negative-time rejection, and a virtual-time crash/restart harness.
int main()
{
    glassbox::EventQueue queue;

    CHECK(queue.empty());
    CHECK(queue.size() == 0);
    CHECK(!queue.peek());
    CHECK(!queue.pop());

    const auto first = queue.schedule(
        100,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{"first"});
    const auto second = queue.schedule(
        100,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{"second"});
    const auto earlier = queue.schedule(
        50,
        glassbox::EventKind::script_action,
        glassbox::ScriptActionEvent{"earlier"});

    CHECK(first && first->value() == 0);
    CHECK(second && second->value() == 1);
    CHECK(earlier && earlier->value() == 2);
    CHECK(queue.size() == 3);

    const auto earliest = queue.pop();
    CHECK(earliest);
    CHECK(earliest->time_us == 50);
    CHECK(earliest->seq.value() == 2);

    const auto first_at_equal_time = queue.pop();
    CHECK(first_at_equal_time);
    CHECK(first_at_equal_time->time_us == 100);
    CHECK(first_at_equal_time->seq.value() == 0);
    CHECK(std::get<glassbox::ScriptActionEvent>(
              first_at_equal_time->payload)
              .name == "first");

    const auto second_at_equal_time = queue.pop();
    CHECK(second_at_equal_time);
    CHECK(second_at_equal_time->time_us == 100);
    CHECK(second_at_equal_time->seq.value() == 1);
    CHECK(std::get<glassbox::ScriptActionEvent>(
              second_at_equal_time->payload)
              .name == "second");

    CHECK(queue.empty());
    CHECK(queue.size() == 0);
    CHECK(!queue.pop());

    CHECK(!queue.schedule(-1, glassbox::EventKind::node_crash,
                          glassbox::NodeCrashEvent{glassbox::NodeId::bank_b}));

    glassbox::EventQueue harness;
    const auto crash_seq = harness.schedule(
        10,
        glassbox::EventKind::node_crash,
        glassbox::NodeCrashEvent{glassbox::NodeId::bank_a});
    const auto restart_seq = harness.schedule(
        20,
        glassbox::EventKind::node_restart,
        glassbox::NodeRestartEvent{glassbox::NodeId::bank_a});
    CHECK(crash_seq.has_value());
    CHECK(restart_seq.has_value());

    glassbox::SimTimeUs virtual_time = 0;
    const auto crash = harness.pop();
    CHECK(crash);
    virtual_time = crash->time_us;
    CHECK(virtual_time == 10);
    const auto restart = harness.pop();
    CHECK(restart);
    virtual_time = restart->time_us;
    CHECK(virtual_time == 20);
    CHECK(harness.empty());

    return 0;
}
