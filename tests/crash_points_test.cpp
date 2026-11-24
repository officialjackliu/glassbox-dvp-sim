// Task 19: crash points relative to one Effects batch, one-shot matching,
// timer discard, drops while down, restart from the durable log, and
// recovery idempotence of the protocol nodes at quiescence.

#include "faults/crash_spec.hpp"
#include "scenarios/scenario_library.hpp"
#include "sim/simulator.hpp"
#include "scenario_checks.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

using glassbox::CrashPoint;
using glassbox::CrashSpec;
using glassbox::Effects;
using glassbox::Message;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::TimerKind;
using glassbox::TradeId;
using glassbox::TradeState;

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }
    std::vector<glassbox::TraceRecord> records;
};

// On every message: two log records, two sends to BANK_B, one timer.
// On its timer: one log record and one send. Counts everything.
class Probe : public glassbox::Node {
  public:
    Effects on_message(const Message& message, glassbox::SimTimeUs) override
    {
        ++messages;
        return batch(message.trade_id.value_or(TradeId{0}), 2);
    }

    Effects on_timer(const glassbox::TimerEvent&, glassbox::SimTimeUs) override
    {
        ++timers;
        Effects e;
        e.logs.push_back(glassbox::Aborted{TradeId{99}});
        e.sends.push_back(glassbox::Outgoing{
            glassbox::make_commit(NodeId::bank_b, TradeId{99})});
        return e;
    }

    TradeState trade_state(TradeId) const override
    {
        return TradeState::none;
    }

    void on_crash() override { ++crashes; }

    Effects on_restart(std::span<const glassbox::LogEntry> log,
                       glassbox::SimTimeUs) override
    {
        restart_log_sizes.push_back(log.size());
        return {};
    }

    int messages = 0;
    int timers = 0;
    int crashes = 0;
    std::vector<std::size_t> restart_log_sizes;

  private:
    static Effects batch(TradeId id, int sends)
    {
        Effects e;
        e.logs.push_back(glassbox::Aborted{id});
        e.logs.push_back(glassbox::End{id});
        for (int i = 0; i < sends; ++i) {
            e.sends.push_back(glassbox::Outgoing{
                glassbox::make_commit(NodeId::bank_b, id)});
        }
        e.timers.push_back(glassbox::TimerRequest{
            50'000, TimerKind::retransmit, std::nullopt, TradeState::none,
            0});
        return e;
    }
};

// Only counts what arrives; produces nothing (so the bench stays finite).
class Receiver : public Probe {
  public:
    Effects on_message(const Message&, glassbox::SimTimeUs) override
    {
        ++messages;
        return {};
    }
};

struct Bench {
    CollectorSink sink;
    glassbox::Simulator simulator;
    Probe* a = nullptr;
    Probe* b = nullptr;

    explicit Bench(glassbox::SimTimeUs processing = 100)
        : simulator(make_config(processing), sink)
    {
        auto pa = std::make_unique<Probe>();
        a = pa.get();
        simulator.set_node(NodeId::bank_a, std::move(pa));
        auto pb = std::make_unique<Receiver>();
        b = pb.get();
        simulator.set_node(NodeId::bank_b, std::move(pb));
        // A->B 35 ms; COORD->A 1 ms (defaults).
    }

    static glassbox::SimulatorConfig make_config(glassbox::SimTimeUs p)
    {
        glassbox::SimulatorConfig config;
        config.processing_delay_us = p;
        return config;
    }

    // COORD releases a PREPARE-typed message to BANK_A at `at` (+processing).
    void poke_a(glassbox::SimTimeUs at, TradeId id = TradeId{1})
    {
        Effects e;
        Message m;
        m.from = NodeId::coordinator;
        m.to = NodeId::bank_a;
        m.type = MessageType::prepare;
        m.trade_id = id;
        e.sends.push_back(glassbox::Outgoing{m});
        while (simulator.events().peek() &&
               simulator.events().peek()->time_us < at) {
            CHECK(simulator.step());
        }
        CHECK(simulator.apply_effects(NodeId::coordinator, std::move(e), at)
                  .has_value());
    }

    [[nodiscard]] std::vector<std::string> a_events() const
    {
        // Trace of BANK_A's own effects: logs, sends, timer sets, crashes.
        std::vector<std::string> out;
        for (const glassbox::TraceRecord& r : sink.records) {
            if (r.node != NodeId::bank_a) {
                continue;
            }
            if (r.kind == "LOG_APPEND" || r.kind == "MSG_SEND" ||
                r.kind == "TIMER_SET" || r.kind == "CRASH" ||
                r.kind == "RESTART") {
                out.push_back(std::to_string(r.sim_time_us) + " " + r.kind +
                              (r.reason ? " " + *r.reason : ""));
            }
        }
        return out;
    }
};

[[nodiscard]] CrashSpec spec(CrashPoint point,
                             std::optional<glassbox::SimTimeUs> restart =
                                 std::nullopt)
{
    return CrashSpec{NodeId::bank_a, std::nullopt,
                     glassbox::CrashTrigger{MessageType::prepare}, point,
                     restart, 1};
}

using Lines = std::vector<std::string>;

void test_no_crash_baseline()
{
    Bench bench;
    bench.poke_a(0);
    bench.simulator.run();
    // Handler at 1,100; logs durable 2,100; sends 2,200; the timer fires at
    // 2,200 + 50,000 and its own batch is durable at 53,200, sent at 53,300.
    CHECK(bench.a_events() ==
          (Lines{"1100 TIMER_SET", "2100 LOG_APPEND", "2100 LOG_APPEND",
                 "2200 MSG_SEND", "2200 MSG_SEND", "53200 LOG_APPEND",
                 "53300 MSG_SEND"}));
    CHECK(bench.b->messages == 3);
    CHECK(bench.a->timers == 1);
}

void test_before_log()
{
    Bench bench;
    CHECK(bench.simulator.add_crash_spec(spec(CrashPoint::before_log,
                                              10'000)));
    bench.poke_a(0);
    bench.simulator.run();
    CHECK(bench.a_events() ==
          (Lines{"1100 CRASH BEFORE_LOG", "11100 RESTART"}));
    CHECK(bench.simulator.durable_log(NodeId::bank_a).empty());
    CHECK(bench.b->messages == 0);
    CHECK(bench.a->timers == 0);
    CHECK(bench.a->crashes == 1);
    CHECK(bench.a->restart_log_sizes == std::vector<std::size_t>{0});
    CHECK(bench.simulator.epoch(NodeId::bank_a) == 1);
    CHECK(bench.simulator.pending_crash_specs() == 0);
}

void test_after_log_before_send()
{
    Bench bench;
    CHECK(bench.simulator.add_crash_spec(
        spec(CrashPoint::after_log_before_send, 10'000)));
    bench.poke_a(0);
    bench.simulator.run();
    // Records durable at 2,100, then the crash at the same instant; no
    // message leaves and no timer is set.
    CHECK(bench.a_events() ==
          (Lines{"2100 LOG_APPEND", "2100 LOG_APPEND",
                 "2100 CRASH AFTER_LOG_BEFORE_SEND", "12100 RESTART"}));
    CHECK(bench.simulator.durable_log(NodeId::bank_a).size() == 2);
    CHECK(bench.b->messages == 0);
    CHECK(bench.a->timers == 0);
    CHECK(bench.a->restart_log_sizes == std::vector<std::size_t>{2});
}

void test_after_first_send()
{
    Bench bench;
    CHECK(bench.simulator.add_crash_spec(
        spec(CrashPoint::after_first_send, 10'000)));
    bench.poke_a(0);
    bench.simulator.run();
    CHECK(bench.a_events() ==
          (Lines{"2100 LOG_APPEND", "2100 LOG_APPEND", "2200 MSG_SEND",
                 "2200 CRASH AFTER_FIRST_SEND", "12200 RESTART"}));
    CHECK(bench.b->messages == 1); // only the first message left
    CHECK(bench.a->timers == 0);
    CHECK(bench.a->restart_log_sizes == std::vector<std::size_t>{2});
}

void test_zero_processing_delay_keeps_order()
{
    // Flush, first send and crash all at 2,100: still in that order, and
    // AFTER_LOG_BEFORE_SEND still sends nothing.
    {
        Bench bench{0};
        CHECK(bench.simulator.add_crash_spec(
            spec(CrashPoint::after_first_send)));
        bench.poke_a(0);
        bench.simulator.run();
        // Probe batch is handled at 1,000 (COORD release at 0 with zero
        // processing, 1 ms link); timers are dropped by the crash.
        CHECK(bench.a_events() ==
              (Lines{"2000 LOG_APPEND", "2000 LOG_APPEND", "2000 MSG_SEND",
                     "2000 CRASH AFTER_FIRST_SEND"}));
        CHECK(bench.b->messages == 1);
    }
    {
        Bench bench{0};
        CHECK(bench.simulator.add_crash_spec(
            spec(CrashPoint::after_log_before_send)));
        bench.poke_a(0);
        bench.simulator.run();
        CHECK(bench.a_events() ==
              (Lines{"2000 LOG_APPEND", "2000 LOG_APPEND",
                     "2000 CRASH AFTER_LOG_BEFORE_SEND"}));
        CHECK(bench.b->messages == 0);
    }
}

// A batch with no logs: AFTER_LOG_BEFORE_SEND crashes right away (nothing
// to wait for) and nothing is sent; AFTER_FIRST_SEND sends one.
class NoLogProbe : public Probe {
  public:
    Effects on_message(const Message& message, glassbox::SimTimeUs) override
    {
        Effects e;
        e.sends.push_back(glassbox::Outgoing{
            glassbox::make_commit(NodeId::bank_b, *message.trade_id)});
        e.sends.push_back(glassbox::Outgoing{
            glassbox::make_commit(NodeId::bank_b, *message.trade_id)});
        return e;
    }
};

void test_batch_without_logs()
{
    for (CrashPoint point : {CrashPoint::after_log_before_send,
                             CrashPoint::after_first_send}) {
        Bench bench;
        auto probe = std::make_unique<NoLogProbe>();
        bench.a = probe.get();
        bench.simulator.set_node(NodeId::bank_a, std::move(probe));
        CHECK(bench.simulator.add_crash_spec(spec(point)));
        bench.poke_a(0);
        bench.simulator.run();
        if (point == CrashPoint::after_log_before_send) {
            CHECK(bench.a_events() ==
                  Lines{"1100 CRASH AFTER_LOG_BEFORE_SEND"});
            CHECK(bench.b->messages == 0);
        } else {
            CHECK(bench.a_events() ==
                  (Lines{"1200 MSG_SEND", "1200 CRASH AFTER_FIRST_SEND"}));
            CHECK(bench.b->messages == 1);
        }
        CHECK(!bench.simulator.node_up(NodeId::bank_a)); // never restarted
    }
}

void test_matching_rules()
{
    // Trade filter: only trade 2 matches.
    {
        Bench bench;
        CrashSpec s = spec(CrashPoint::before_log);
        s.trade_id = TradeId{2};
        CHECK(bench.simulator.add_crash_spec(s));
        bench.poke_a(0, TradeId{1});
        bench.poke_a(100'000, TradeId{2});
        bench.simulator.run();
        CHECK(bench.a->messages == 2);
        CHECK(bench.a->crashes == 1);
        CHECK(bench.a_events().back() == "101100 CRASH BEFORE_LOG");
    }
    // Occurrence 2 fires on the second match only, and only once.
    {
        Bench bench;
        CrashSpec s = spec(CrashPoint::before_log, 1'000);
        s.occurrence = 2;
        CHECK(bench.simulator.add_crash_spec(s));
        bench.poke_a(0);
        bench.poke_a(100'000);
        bench.poke_a(200'000);
        bench.simulator.run();
        CHECK(bench.a->messages == 3);
        CHECK(bench.a->crashes == 1);
        int crash_records = 0;
        for (const glassbox::TraceRecord& r : bench.sink.records) {
            if (r.kind == "CRASH") {
                ++crash_records;
                CHECK(r.sim_time_us == 101'100);
            }
        }
        CHECK(crash_records == 1);
        CHECK(bench.simulator.pending_crash_specs() == 0);
    }
    // Wrong node or wrong type never fires.
    {
        Bench bench;
        CrashSpec other_node = spec(CrashPoint::before_log);
        other_node.node = NodeId::bank_c;
        CrashSpec other_type = spec(CrashPoint::before_log);
        other_type.trigger = glassbox::CrashTrigger{MessageType::commit};
        CHECK(bench.simulator.add_crash_spec(other_node));
        CHECK(bench.simulator.add_crash_spec(other_type));
        bench.poke_a(0);
        bench.simulator.run();
        CHECK(bench.a->crashes == 0);
        CHECK(bench.simulator.pending_crash_specs() == 2);
    }
    // Timer trigger: crash on the probe's RETRANSMIT timer batch.
    {
        Bench bench;
        CrashSpec on_timer = spec(CrashPoint::after_log_before_send);
        on_timer.trigger = glassbox::CrashTrigger{TimerKind::retransmit};
        CHECK(bench.simulator.add_crash_spec(on_timer));
        bench.poke_a(0);
        bench.simulator.run();
        CHECK(bench.a->timers == 1);
        CHECK(bench.a_events().back() ==
              "53200 CRASH AFTER_LOG_BEFORE_SEND");
        CHECK(bench.simulator.durable_log(NodeId::bank_a).size() == 3);
        CHECK(bench.b->messages == 2); // the timer batch's send was lost
    }
    // Invalid specs are rejected.
    {
        Bench bench;
        CrashSpec zero = spec(CrashPoint::before_log);
        zero.occurrence = 0;
        CHECK(!bench.simulator.add_crash_spec(zero));
        CHECK(!bench.simulator.add_crash_spec(
            spec(CrashPoint::before_log, -1)));
        CHECK(bench.simulator.add_crash_spec(spec(CrashPoint::before_log, 0)));
        CHECK(bench.simulator.pending_crash_specs() == 1);
    }
}

void test_timers_discarded_and_messages_dropped_while_down()
{
    Bench bench;
    // First message (1,100) sets a timer for 52,200. Second message at
    // 20,100 crashes A BEFORE_LOG; restart at 40,100.
    CrashSpec s = spec(CrashPoint::before_log, 20'000);
    s.occurrence = 2;
    CHECK(bench.simulator.add_crash_spec(s));
    bench.poke_a(0);
    bench.poke_a(19'000);
    // A message to A while it is down (released 30,100, arrives 31,100).
    bench.poke_a(30'000);
    bench.simulator.run();
    CHECK(bench.a->messages == 2);
    CHECK(bench.a->timers == 0);
    std::vector<std::string> ignored;
    int dropped_down = 0;
    for (const glassbox::TraceRecord& r : bench.sink.records) {
        if (r.kind == "TIMER_IGNORED" && r.node == NodeId::bank_a) {
            ignored.push_back(std::to_string(r.sim_time_us) + " " +
                              r.reason.value_or(""));
        }
        if (r.kind == "MSG_DROP" && r.to == NodeId::bank_a) {
            CHECK(r.reason == "NODE_DOWN");
            CHECK(r.sim_time_us == 31'100);
            ++dropped_down;
        }
    }
    CHECK(ignored == std::vector<std::string>{"52200 stale_epoch"});
    CHECK(dropped_down == 1);
    CHECK(bench.a->restart_log_sizes == std::vector<std::size_t>{2});
}

// --- Protocol nodes: crash/restart across two nodes, idempotence ----------

struct View {
    std::vector<std::string> lines;
    [[nodiscard]] friend bool operator==(const View&, const View&) = default;
};

// The recoverable state: per-trade states/decisions/acks and ledgers.
[[nodiscard]] View recoverable_view(const glassbox::ScenarioRun& run)
{
    View view;
    for (const auto& [id, t] : run.nodes().coordinator->trades()) {
        view.lines.push_back(
            "COORD " + std::to_string(id.value()) + " " +
            std::string{glassbox::to_string(t.state)} + " " +
            (t.decision ? std::string{glassbox::to_string(*t.decision)}
                        : "-") +
            " " + std::string{glassbox::to_string(t.abort_reason)} + " " +
            (t.buyer_acked ? "A" : "a") + (t.seller_acked ? "A" : "a"));
    }
    for (const glassbox::Participant* bank : run.nodes().banks()) {
        const glassbox::LocalLedger& l = bank->ledger();
        view.lines.push_back(
            std::string{glassbox::to_string(bank->self())} + " " +
            std::to_string(l.cash_total().value()) + "/" +
            std::to_string(l.cash_locked().value()) + " " +
            std::to_string(l.bonds_total().value()) + "/" +
            std::to_string(l.bonds_locked().value()));
        for (const auto& [id, t] : bank->trades()) {
            view.lines.push_back(
                std::string{glassbox::to_string(bank->self())} + " " +
                std::to_string(id.value()) + " " +
                std::string{glassbox::to_string(t.state)});
        }
    }
    return view;
}

void check_restart_idempotent(glassbox::ScenarioSetup setup,
                              int restarts)
{
    constexpr glassbox::SimTimeUs k_quiet = 60'000'000;
    for (int i = 0; i < restarts; ++i) {
        const glassbox::SimTimeUs at = k_quiet + i * 10'000;
        for (NodeId node : {NodeId::coordinator, NodeId::bank_a,
                            NodeId::bank_b, NodeId::bank_c}) {
            setup.script.push_back(
                {at, glassbox::ScriptActionEvent{glassbox::ScriptCrash{node}}});
            setup.script.push_back(
                {at + 1, glassbox::ScriptActionEvent{
                             glassbox::ScriptRestart{node}}});
        }
    }
    glassbox::ScenarioRun run{setup};
    scenario_checks::run_until(run, k_quiet);
    const View before = recoverable_view(run);
    const std::size_t sends_before =
        static_cast<std::size_t>(scenario_checks::count(run, "MSG_SEND"));
    CHECK(run.run());
    CHECK(recoverable_view(run) == before);
    // Finished trades need no messages after a restart.
    CHECK(static_cast<std::size_t>(scenario_checks::count(run, "MSG_SEND")) ==
          sends_before);
    CHECK(scenario_checks::count(run, "RESTART") == 4 * restarts);
    CHECK(scenario_checks::count(run, "RECOVERY_ERROR") == 0);
    scenario_checks::check_quiescent_invariants(run);
}

void test_recovery_idempotence_at_quiescence()
{
    for (int restarts : {1, 2}) {
        check_restart_idempotent(glassbox::make_s1_happy_path(1), restarts);
        check_restart_idempotent(glassbox::make_s2_buyer_lacks_funds(1),
                                 restarts);
        check_restart_idempotent(glassbox::make_s13_lock_conflict(1),
                                 restarts);
        check_restart_idempotent(glassbox::make_s4_slow_link_tight_timeout(1),
                                 restarts);
    }
}

void test_participant_crash_after_vote_then_restart()
{
    // Across two nodes: BANK_A crashes right after its yes leaves
    // (AFTER_FIRST_SEND on PREPARE) and restarts 2 s later; the coordinator
    // is crashed and restarted at quiescence. The trade commits everywhere.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.crashes.push_back(CrashSpec{
        NodeId::bank_a, TradeId{1},
        glassbox::CrashTrigger{MessageType::prepare},
        CrashPoint::after_first_send, 2'000'000, 1});
    setup.script.push_back({30'000'000, glassbox::ScriptActionEvent{
                                            glassbox::ScriptCrash{
                                                NodeId::coordinator}}});
    setup.script.push_back({30'000'010, glassbox::ScriptActionEvent{
                                            glassbox::ScriptRestart{
                                                NodeId::coordinator}}});
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    // BANK_A's PREPARED survived the crash; it was in doubt and asked.
    CHECK(scenario_checks::log_kinds(run, NodeId::bank_a) ==
          (Lines{"PREPARED", "COMMITTED"}));
    CHECK(scenario_checks::count(run, "MSG_SEND", MessageType::outcome_query,
                                 NodeId::bank_a) >= 1);
    CHECK(scenario_checks::count(run, "CRASH") == 2);
    CHECK(scenario_checks::count(run, "RESTART") == 2);
    scenario_checks::check_quiescent_invariants(run);
}

void test_crash_point_names()
{
    CHECK(glassbox::to_string(CrashPoint::before_log) == "BEFORE_LOG");
    CHECK(glassbox::to_string(CrashPoint::after_log_before_send) ==
          "AFTER_LOG_BEFORE_SEND");
    CHECK(glassbox::to_string(CrashPoint::after_first_send) ==
          "AFTER_FIRST_SEND");
}

} // namespace

int main()
{
    test_crash_point_names();
    test_no_crash_baseline();
    test_before_log();
    test_after_log_before_send();
    test_after_first_send();
    test_zero_processing_delay_keeps_order();
    test_batch_without_logs();
    test_matching_rules();
    test_timers_discarded_and_messages_dropped_while_down();
    test_recovery_idempotence_at_quiescence();
    test_participant_crash_after_vote_then_restart();
    return 0;
}
