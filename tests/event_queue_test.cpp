#include "sim/event_queue.hpp"

#include <cassert>
#include <optional>
#include <string>

int main()
{
    glassbox::EventQueue queue;

    assert(queue.empty());
    assert(queue.size() == 0);
    assert(!queue.peek());
    assert(!queue.pop());

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

    assert(first && first->value() == 0);
    assert(second && second->value() == 1);
    assert(earlier && earlier->value() == 2);
    assert(queue.size() == 3);

    auto earliest = queue.pop();
    assert(earliest);
    assert(earliest->time_us == 50);
    assert(earliest->seq.value() == 2);

    auto first_at_equal_time = queue.pop();
    assert(first_at_equal_time);
    assert(first_at_equal_time->time_us == 100);
    assert(first_at_equal_time->seq.value() == 0);
    assert(std::get<glassbox::ScriptActionEvent>(
               first_at_equal_time->payload)
               .name == "first");

    auto second_at_equal_time = queue.pop();
    assert(second_at_equal_time);
    assert(second_at_equal_time->time_us == 100);
    assert(second_at_equal_time->seq.value() == 1);
    assert(std::get<glassbox::ScriptActionEvent>(
               second_at_equal_time->payload)
               .name == "second");

    assert(queue.empty());
    assert(queue.size() == 0);
    assert(!queue.pop());

    assert(!queue.schedule(-1, glassbox::EventKind::node_crash,
                           glassbox::NodeCrashEvent{glassbox::NodeId::jpm}));

    glassbox::EventQueue harness;
    assert(harness.schedule(
        10,
        glassbox::EventKind::node_crash,
        glassbox::NodeCrashEvent{glassbox::NodeId::goldman}));
    assert(harness.schedule(
        20,
        glassbox::EventKind::node_restart,
        glassbox::NodeRestartEvent{glassbox::NodeId::goldman}));

    glassbox::SimTimeUs virtual_time = 0;
    virtual_time = harness.pop()->time_us;
    assert(virtual_time == 10);
    virtual_time = harness.pop()->time_us;
    assert(virtual_time == 20);
    assert(harness.empty());

    return 0;
}
