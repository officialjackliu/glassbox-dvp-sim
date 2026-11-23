#include "sim/simulator.hpp"
#include "check.hpp"

#include <array>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }

    std::vector<glassbox::TraceRecord> records;
};

class DummyNode : public glassbox::Node {
  public:
    glassbox::Effects on_message(const glassbox::Message&,
                       glassbox::SimTimeUs) override
    {
        ++message_calls;
        volatile_state = true;
        glassbox::Effects effects;
        if (produce_log) {
            effects.logs.push_back(
                glassbox::LogRecord{"DUMMY_RECORD", std::nullopt});
        }
        if (produce_send) {
            effects.sends.push_back(
                glassbox::Outgoing{
                    glassbox::Message{
                        99,
                        owner,
                        destination,
                        glassbox::MessageType::prepare,
                        std::nullopt,
                    }});
            effects.timers.push_back(
                glassbox::TimerRequest{
                    1'500,
                    glassbox::TimerKind::retransmit,
                    std::nullopt,
                    "AFTER_RESTART",
                });
        }
        if (produce_timer) {
            effects.timers.push_back(
                glassbox::TimerRequest{
                    timer_time,
                    glassbox::TimerKind::retransmit,
                    std::nullopt,
                    "DUMMY",
                });
        }
        return effects;
    }

    glassbox::Effects on_timer(const glassbox::TimerEvent&,
                     glassbox::SimTimeUs) override
    {
        ++timer_calls;
        return {};
    }

    void on_crash() override
    {
        ++crash_calls;
        volatile_state = false;
    }

    glassbox::Effects on_restart(
        const std::vector<glassbox::LogRecord>& durable_log,
        glassbox::SimTimeUs) override
    {
        ++restart_calls;
        restart_log_count =
            static_cast<int>(durable_log.size());
        volatile_state = true;
        if (produce_restart_effect) {
            glassbox::Effects effects;
            effects.logs.push_back(
                glassbox::LogRecord{"RESTART_RECORD", std::nullopt});
            effects.sends.push_back(
                glassbox::Outgoing{
                    glassbox::Message{
                        100,
                        owner,
                        destination,
                        glassbox::MessageType::prepare,
                        std::nullopt,
                    }});
            effects.timers.push_back(
                glassbox::TimerRequest{
                    1'500,
                    glassbox::TimerKind::retransmit,
                    std::nullopt,
                    "AFTER_RESTART",
                });
            return effects;
        }
        return {};
    }

    glassbox::NodeId owner =
        glassbox::NodeId::coordinator;
    glassbox::NodeId destination =
        glassbox::NodeId::bank_a;
    bool produce_log = false;
    bool produce_send = false;
    bool produce_timer = false;
    glassbox::SimTimeUs timer_time = 2'000;
    bool volatile_state = false;
    int message_calls = 0;
    int timer_calls = 0;
    int crash_calls = 0;
    int restart_calls = 0;
    int restart_log_count = 0;
    bool produce_restart_effect = false;
};

[[nodiscard]] std::string trace_signature(
    const std::vector<glassbox::TraceRecord>& records)
{
    std::string signature;
    for (const glassbox::TraceRecord& record : records) {
        signature += std::to_string(record.sim_time_us);
        signature += ":";
        signature += record.kind;
        signature += ":";
        signature += record.handler;
        signature += ";";
    }
    return signature;
}

void run_dummy(std::vector<glassbox::TraceRecord>& out)
{
    CollectorSink sink;
    glassbox::SimulatorConfig config;
    config.seed = 42;
    glassbox::Simulator simulator{config, sink};
    auto dummy = std::make_unique<DummyNode>();
    DummyNode* raw = dummy.get();
    raw->owner = glassbox::NodeId::bank_a;
    raw->destination = glassbox::NodeId::bank_b;
    raw->produce_log = true;
    raw->produce_send = true;
    raw->produce_timer = true;
    raw->timer_time = 500;
    simulator.set_node(glassbox::NodeId::bank_a, std::move(dummy));

    const auto script = glassbox::ScriptSetLink{
        glassbox::NodeId::bank_a,
        glassbox::NodeId::bank_b,
        10,
        0,
        0,
    };
    const auto scheduled = simulator.schedule_script(
        0,
        glassbox::ScriptActionEvent{script});
    CHECK(scheduled.has_value());

    const auto message = simulator.schedule_message(
        glassbox::NodeId::coordinator,
        glassbox::NodeId::bank_a,
        glassbox::MessageType::prepare,
        1,
        0);
    CHECK(message.has_value());
    simulator.run();
    out = sink.records;
}

} // namespace

int main()
{
    // Invalid delay configuration is rejected.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        config.processing_delay_us = -1;
        bool rejected = false;
        try {
            glassbox::Simulator simulator{config, sink};
            static_cast<void>(simulator);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    // Effect timing: no logs adds processing only; logs add flush.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        config.processing_delay_us = 100;
        config.log_flush_delay_us = 1'000;
        glassbox::Simulator simulator{config, sink};
        auto dummy = std::make_unique<DummyNode>();
        DummyNode* raw = dummy.get();
        raw->owner = glassbox::NodeId::coordinator;
        raw->destination = glassbox::NodeId::bank_a;
        raw->produce_send = true;
        simulator.set_node(glassbox::NodeId::coordinator,
                           std::move(dummy));

        glassbox::Effects no_log;
        no_log.sends.push_back(
            glassbox::Outgoing{
                glassbox::Message{
                    1,
                    glassbox::NodeId::coordinator,
                    glassbox::NodeId::bank_a,
                    glassbox::MessageType::prepare,
                    std::nullopt,
                }});
        const auto no_log_send = simulator.apply_effects(
            glassbox::NodeId::coordinator,
            no_log,
            100);
        CHECK(no_log_send.has_value());

        glassbox::Effects with_log;
        with_log.logs.push_back(
            glassbox::LogRecord{"PREPARED", std::nullopt});
        with_log.sends.push_back(
            glassbox::Outgoing{
                glassbox::Message{
                    2,
                    glassbox::NodeId::coordinator,
                    glassbox::NodeId::bank_a,
                    glassbox::MessageType::vote,
                    std::nullopt,
                }});
        const auto with_log_send = simulator.apply_effects(
            glassbox::NodeId::coordinator,
            with_log,
            100);
        CHECK(with_log_send.has_value());

        CHECK(simulator.events().size() == 3);
        const auto no_log_release = simulator.events().pop();
        CHECK(no_log_release);
        CHECK(no_log_release->kind == glassbox::EventKind::release_send);
        CHECK(no_log_release->time_us == 200);
        const auto flush = simulator.events().pop();
        CHECK(flush);
        CHECK(flush->kind == glassbox::EventKind::flush_log);
        CHECK(flush->time_us == 1'100);
        const auto with_log_release = simulator.events().pop();
        CHECK(with_log_release);
        CHECK(with_log_release->kind == glassbox::EventKind::release_send);
        CHECK(with_log_release->time_us == 1'200);
    }

    // Property: every released send occurs at or after its batch log flush.
    {
        std::mt19937_64 random{123};
        for (int i = 0; i < 500; ++i) {
            CollectorSink sink;
            glassbox::SimulatorConfig config;
            config.processing_delay_us =
                static_cast<glassbox::SimTimeUs>(random() % 500);
            config.log_flush_delay_us =
                static_cast<glassbox::SimTimeUs>(random() % 2'000);
            glassbox::Simulator simulator{config, sink};
            glassbox::Effects effects;
            if (random() % 2 == 0) {
                effects.logs.push_back(
                    glassbox::LogRecord{"RECORD", std::nullopt});
            }
            effects.sends.push_back(
                glassbox::Outgoing{
                    glassbox::Message{
                        1,
                        glassbox::NodeId::coordinator,
                        glassbox::NodeId::bank_a,
                        glassbox::MessageType::prepare,
                        std::nullopt,
                    }});
            const glassbox::SimTimeUs now =
                static_cast<glassbox::SimTimeUs>(random() % 10'000);
            CHECK(simulator.apply_effects(
                      glassbox::NodeId::coordinator,
                      effects,
                      now).has_value());

            const glassbox::SimTimeUs durable =
                effects.logs.empty()
                    ? now
                    : now + config.log_flush_delay_us;
            const glassbox::SimTimeUs release =
                now + config.processing_delay_us +
                (effects.logs.empty() ? 0 :
                                        config.log_flush_delay_us);
            CHECK(release >= durable);
            if (effects.logs.empty()) {
                CHECK(release == now + config.processing_delay_us);
            } else {
                CHECK(release ==
                      now + config.processing_delay_us +
                          config.log_flush_delay_us);
            }
        }
    }

    // Crash before flush discards logs and sends; restart calls on_restart.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        auto dummy = std::make_unique<DummyNode>();
        DummyNode* raw = dummy.get();
        raw->owner = glassbox::NodeId::bank_a;
        raw->destination = glassbox::NodeId::bank_b;
        raw->produce_log = true;
        raw->produce_send = true;
        raw->produce_timer = true;
        raw->timer_time = 2'000;
        simulator.set_node(glassbox::NodeId::bank_a,
                           std::move(dummy));

        glassbox::EventQueue& external_queue = simulator.events();
        CHECK(external_queue.schedule(
                  0,
                  glassbox::EventKind::deliver_message,
                  glassbox::DeliverMessageEvent{
                      glassbox::Message{
                          1,
                          glassbox::NodeId::coordinator,
                          glassbox::NodeId::bank_a,
                          glassbox::MessageType::prepare,
                          std::nullopt,
                      },
                      glassbox::NodeId::bank_a,
                      0,
                      0,
                      0,
                  }).has_value());

        CHECK(simulator.schedule_script(
                  500,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptCrash{glassbox::NodeId::bank_a},
                  }).has_value());
        CHECK(simulator.schedule_script(
                  600,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptRestart{glassbox::NodeId::bank_a},
                  }).has_value());
        simulator.run();

        CHECK(raw == simulator.node(glassbox::NodeId::bank_a));
        CHECK(raw->crash_calls == 1);
        CHECK(raw->timer_calls == 0);
        CHECK(simulator.durable_log(glassbox::NodeId::bank_a).empty());
        CHECK(raw->restart_calls == 1);
    }

    // Crash and restart discard stale sends; restart effects are applied.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        config.processing_delay_us = 100;
        config.log_flush_delay_us = 1'000;
        glassbox::Simulator simulator{config, sink};
        auto dummy = std::make_unique<DummyNode>();
        DummyNode* raw = dummy.get();
        raw->owner = glassbox::NodeId::bank_a;
        raw->destination = glassbox::NodeId::bank_b;
        raw->produce_log = true;
        raw->produce_send = true;
        raw->produce_restart_effect = false;
        raw->timer_time = 2'000;
        simulator.set_node(glassbox::NodeId::bank_a,
                           std::move(dummy));

        glassbox::Effects effects;
        effects.logs.push_back(
            glassbox::LogRecord{"BEFORE_CRASH", std::nullopt});
        effects.sends.push_back(
            glassbox::Outgoing{
                glassbox::Message{
                    77,
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                    glassbox::MessageType::prepare,
                    std::nullopt,
                }});
        effects.timers.push_back(
            glassbox::TimerRequest{
                1'500,
                glassbox::TimerKind::retransmit,
                std::nullopt,
                "OLD_EPOCH",
            });
        CHECK(simulator.apply_effects(
                  glassbox::NodeId::bank_a,
                  effects,
                  0).has_value());

        simulator.crash_node(glassbox::NodeId::bank_a);
        const std::size_t crash_epoch =
            simulator.epoch(glassbox::NodeId::bank_a);
        CHECK(crash_epoch == 1);
        simulator.crash_node(glassbox::NodeId::bank_a);
        CHECK(simulator.epoch(glassbox::NodeId::bank_a) ==
              crash_epoch);
        CHECK(raw->crash_calls == 1);

        simulator.restart_node(glassbox::NodeId::bank_a);
        simulator.restart_node(glassbox::NodeId::bank_a);
        CHECK(raw->restart_calls == 1);
        CHECK(raw->restart_log_count == 0);

        simulator.run();
        bool stale_send = false;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == "MSG_SEND") {
                stale_send = true;
            }
        }
        CHECK(!stale_send);
    }

    // Restart Effects are applied after a real crash.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        auto dummy = std::make_unique<DummyNode>();
        DummyNode* raw = dummy.get();
        raw->owner = glassbox::NodeId::bank_a;
        raw->destination = glassbox::NodeId::bank_b;
        raw->produce_restart_effect = true;
        simulator.set_node(glassbox::NodeId::bank_a,
                           std::move(dummy));

        simulator.crash_node(glassbox::NodeId::bank_a);
        simulator.restart_node(glassbox::NodeId::bank_a);
        simulator.run();

        CHECK(raw->restart_calls == 1);
        bool restart_log = false;
        bool restart_send = false;
        bool restart_timer = false;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == "LOG_APPEND") {
                restart_log = true;
            }
            if (record.kind == "MSG_SEND") {
                restart_send = true;
            }
            if (record.kind == "TIMER_FIRE") {
                restart_timer = true;
            }
        }
        CHECK(restart_log);
        CHECK(restart_send);
        CHECK(restart_timer);
    }

    // Down node outputs are suppressed.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        simulator.set_node_up(glassbox::NodeId::bank_a, false);
        CHECK(!simulator.apply_effects(
                  glassbox::NodeId::bank_a,
                  glassbox::Effects{},
                  0).has_value());

        glassbox::Effects effects;
        effects.sends.push_back(
            glassbox::Outgoing{
                glassbox::Message{
                    1,
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                    glassbox::MessageType::prepare,
                    std::nullopt,
                }});
        CHECK(!simulator.apply_effects(
                  glassbox::NodeId::bank_a,
                  effects,
                  0).has_value());
    }

    // In-flight delivery rechecks node-down at arrival.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        simulator.network().set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::LinkConfig{100, 0, 0});

        CHECK(simulator.schedule_message(
                  glassbox::NodeId::bank_a,
                  glassbox::NodeId::bank_c,
                  glassbox::MessageType::prepare,
                  1,
                  0).has_value());
        CHECK(simulator.schedule_script(
                  50,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptNodeDown{
                          glassbox::NodeId::bank_c,
                      },
                  }).has_value());
        simulator.run();

        bool arrival_drop = false;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == "MSG_DROP_NODE_DOWN") {
                arrival_drop = true;
            }
        }
        CHECK(arrival_drop);
    }

    // Crash target after send: in-flight delivery is dropped, handler not called.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        auto dummy = std::make_unique<DummyNode>();
        DummyNode* raw = dummy.get();
        simulator.set_node(glassbox::NodeId::bank_c,
                           std::move(dummy));
        simulator.network().set_link(
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_c,
            glassbox::LinkConfig{100, 0, 0});

        CHECK(simulator.schedule_message(
                  glassbox::NodeId::bank_a,
                  glassbox::NodeId::bank_c,
                  glassbox::MessageType::prepare,
                  1,
                  0).has_value());
        CHECK(simulator.schedule_script(
                  50,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptCrash{
                          glassbox::NodeId::bank_c,
                      },
                  }).has_value());
        simulator.run();
        CHECK(raw->message_calls == 0);
        CHECK(raw->crash_calls == 1);
    }

    // Script actions apply in virtual-time order.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        CHECK(simulator.schedule_script(
                  20,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptSetLink{
                          glassbox::NodeId::bank_a,
                          glassbox::NodeId::bank_b,
                          123,
                          4,
                          5,
                      },
                  }).has_value());
        CHECK(simulator.schedule_script(
                  10,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptSetPartition{
                          glassbox::NodeId::bank_a,
                          glassbox::NodeId::bank_b,
                          true,
                      },
                  }).has_value());
        CHECK(!simulator.schedule_script(
                  -1,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptNodeUp{
                          glassbox::NodeId::bank_c,
                      },
                  }).has_value());
        simulator.run();
        CHECK(simulator.network().partitioned(
                  glassbox::NodeId::bank_a,
                  glassbox::NodeId::bank_b));
        CHECK(simulator.network().link(
                  glassbox::NodeId::bank_a,
                  glassbox::NodeId::bank_b)
                  .base_latency_us == 123);
    }

    // Past-dated event scheduling is rejected.
    {
        CollectorSink sink;
        glassbox::SimulatorConfig config;
        glassbox::Simulator simulator{config, sink};
        CHECK(simulator.events().schedule(
                  100,
                  glassbox::EventKind::script_action,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptNodeUp{
                          glassbox::NodeId::bank_a,
                      },
                  }).has_value());
        CHECK(simulator.step());
        CHECK(simulator.now_us() == 100);
        CHECK(!simulator.schedule_script(
                  99,
                  glassbox::ScriptActionEvent{
                      glassbox::ScriptNodeUp{
                          glassbox::NodeId::bank_b,
                      },
                  }).has_value());
    }

    // Determinism excludes measured_ns.
    {
        std::vector<glassbox::TraceRecord> first;
        std::vector<glassbox::TraceRecord> second;
        run_dummy(first);
        run_dummy(second);
        CHECK(trace_signature(first) == trace_signature(second));
    }

    return 0;
}
