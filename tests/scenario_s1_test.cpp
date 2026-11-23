// Task 15: S1 happy path through the scenario registry, the trace-line
// formatter, bounded runs, and the expected trace sequence.

#include "scenarios/scenario_library.hpp"
#include "trace/trace_format.hpp"
#include "scenario_checks.hpp"

#include <string>
#include <vector>

namespace {

using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::TradeId;
using glassbox::TradeState;
using scenario_checks::count;
using scenario_checks::log_kinds;

void test_registry()
{
    const auto registry = glassbox::scenario_registry();
    CHECK(!registry.empty());
    CHECK(registry[0].id == "S1");
    CHECK(registry[0].name == "Happy path");
    const glassbox::ScenarioInfo* s1 = glassbox::find_scenario("S1");
    CHECK(s1 != nullptr);
    CHECK(s1->make == &glassbox::make_s1_happy_path);
    CHECK(glassbox::find_scenario("S99") == nullptr);
    CHECK(glassbox::find_scenario("s1") == nullptr);
    CHECK(glassbox::find_scenario("") == nullptr);
}

void test_defaults()
{
    const glassbox::TradeRequest t = glassbox::standard_trade();
    CHECK(t.buyer == NodeId::bank_a);
    CHECK(t.seller == NodeId::bank_b);
    CHECK(t.asset == "UST_10Y");
    CHECK(t.qty == glassbox::Units{25});
    CHECK(t.cash_cents == glassbox::Cents{2'487'500});
    CHECK(t.valid_until_us == 30'000'000);
    CHECK(glassbox::standard_trade(NodeId::bank_c, NodeId::bank_a, 5'000)
              .valid_until_us == 30'005'000);
    const glassbox::BankGenesis g = glassbox::default_genesis();
    for (const glassbox::LocalLedger& l : {g.bank_a, g.bank_b, g.bank_c}) {
        CHECK(l.cash_total() == glassbox::Cents{100'000'000});
        CHECK(l.bonds_total() == glassbox::Units{25'000});
        CHECK(l.cash_locked() == glassbox::Cents{0});
        CHECK(!l.frozen());
    }
    CHECK(glassbox::make_ledger(5, 6, true).frozen());
}

void test_s1_outcome()
{
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(1)};
    CHECK(run.run());

    const glassbox::ProtocolNodes& nodes = run.nodes();
    CHECK(nodes.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK(nodes.bank_a->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(nodes.bank_b->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(nodes.bank_c->trades().empty());

    // Balances changed by exactly the trade.
    CHECK(nodes.bank_a->ledger().cash_total() ==
          glassbox::Cents{100'000'000 - 2'487'500});
    CHECK(nodes.bank_a->ledger().bonds_total() == glassbox::Units{25'025});
    CHECK(nodes.bank_b->ledger().cash_total() ==
          glassbox::Cents{100'000'000 + 2'487'500});
    CHECK(nodes.bank_b->ledger().bonds_total() == glassbox::Units{24'975});
    CHECK(nodes.bank_c->ledger() == run.genesis().bank_c);
    scenario_checks::check_quiescent_invariants(run);

    CHECK((log_kinds(run, NodeId::coordinator) ==
           std::vector<std::string>{"PROPOSED", "DECISION_COMMIT", "END"}));
    CHECK((log_kinds(run, NodeId::bank_a) ==
           std::vector<std::string>{"PREPARED", "COMMITTED"}));
    CHECK((log_kinds(run, NodeId::bank_b) ==
           std::vector<std::string>{"PREPARED", "COMMITTED"}));
    CHECK(count(run, "MSG_SEND") == 8);
    CHECK(count(run, "MSG_DROP") == 0);
}

// The protocol-visible part of the trace (messages, logs, state changes),
// in order. Timer bookkeeping is checked separately.
void test_s1_expected_trace_sequence()
{
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(1)};
    CHECK(run.run());
    std::vector<std::string> steps;
    for (const glassbox::TraceRecord& r : run.trace()) {
        std::string step = std::to_string(r.sim_time_us) + " " + r.kind;
        if (r.node) {
            step += " " + std::string{glassbox::to_string(*r.node)};
        }
        if (r.kind == "MSG_SEND" || r.kind == "MSG_DELIVER") {
            step += " " + std::string{glassbox::to_string(*r.msg_type)};
        } else if (r.kind == "LOG_APPEND") {
            step += " " + *r.log_record;
        } else if (r.kind == "STATE_CHANGE") {
            step += " " + *r.detail;
        } else {
            continue;
        }
        steps.push_back(step);
    }
    const std::vector<std::string> expected{
        "0 MSG_DELIVER COORD PROPOSE",
        "0 STATE_CHANGE COORD NONE->COLLECTING",
        "1000 LOG_APPEND COORD PROPOSED",
        "1100 MSG_SEND COORD PREPARE",
        "1100 MSG_SEND COORD PREPARE",
        "2100 MSG_DELIVER BANK_A PREPARE",
        "2100 STATE_CHANGE BANK_A NONE->PREPARING",
        "3100 LOG_APPEND BANK_A PREPARED",
        "3200 MSG_SEND BANK_A VOTE",
        "4200 MSG_DELIVER COORD VOTE",
        "36100 MSG_DELIVER BANK_B PREPARE",
        "36100 STATE_CHANGE BANK_B NONE->PREPARING",
        "37100 LOG_APPEND BANK_B PREPARED",
        "37200 MSG_SEND BANK_B VOTE",
        "72200 MSG_DELIVER COORD VOTE",
        "72200 STATE_CHANGE COORD COLLECTING->COMMITTING",
        "73200 LOG_APPEND COORD DECISION_COMMIT",
        "73300 MSG_SEND COORD COMMIT",
        "73300 MSG_SEND COORD COMMIT",
        "74300 MSG_DELIVER BANK_A COMMIT",
        "74300 STATE_CHANGE BANK_A PREPARED->COMMITTED",
        "75300 LOG_APPEND BANK_A COMMITTED",
        "75400 MSG_SEND BANK_A ACK",
        "76400 MSG_DELIVER COORD ACK",
        "108300 MSG_DELIVER BANK_B COMMIT",
        "108300 STATE_CHANGE BANK_B PREPARED->COMMITTED",
        "109300 LOG_APPEND BANK_B COMMITTED",
        "109400 MSG_SEND BANK_B ACK",
        "144400 MSG_DELIVER COORD ACK",
        "144400 STATE_CHANGE COORD COMMITTING->DONE",
        "145400 LOG_APPEND COORD END",
    };
    CHECK(steps == expected);

    // Timers: set 4, all later ignored because the trade moved on.
    CHECK(count(run, "TIMER_SET") == 4);
    CHECK(count(run, "TIMER_IGNORED") == 4);
    CHECK(count(run, "TIMER_FIRE") == 0);
    // The last event is the decision-wait of BANK_B (37,200 + 1 s) or the
    // retransmit (73,300 + 1 s), whichever is later.
    CHECK(run.trace().back().sim_time_us == 1'073'300);
    CHECK(run.trace().back().kind == "TIMER_IGNORED");
    CHECK(run.trace().back().detail == "RETRANSMIT");
}

[[nodiscard]] std::vector<std::string> lines(std::uint64_t seed)
{
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(seed)};
    CHECK(run.run());
    std::vector<std::string> out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        out.push_back(glassbox::format_trace_line(r));
    }
    return out;
}

void test_s1_deterministic_and_seed_independent_outcome()
{
    CHECK(lines(1) == lines(1));
    // Default links have no jitter or drops, so every seed gives the same
    // trace; the outcome must be identical.
    for (std::uint64_t seed = 2; seed <= 20; ++seed) {
        CHECK(lines(seed) == lines(1));
    }
}

void test_format_trace_line()
{
    glassbox::TraceRecord full;
    full.seq = 17;
    full.sim_time_us = 120'500;
    full.kind = "MSG_DELIVER";
    full.handler = "Node::on_message";
    full.measured_ns = 1'840;
    full.reason = "PARTITION";
    full.node = NodeId::bank_a;
    full.trade_id = TradeId{3};
    full.msg_type = MessageType::prepare;
    full.msg_id = 41;
    full.from = NodeId::coordinator;
    full.to = NodeId::bank_a;
    full.log_record = "PREPARED";
    full.detail = "NONE->PREPARING";
    CHECK(glassbox::format_trace_line(full) ==
          "17 t=120500 MSG_DELIVER BANK_A trade=3 msg=PREPARE#41 "
          "COORD->BANK_A log=PREPARED reason=PARTITION "
          "detail=NONE->PREPARING @Node::on_message");

    glassbox::TraceRecord bare;
    bare.seq = 1;
    bare.sim_time_us = 0;
    bare.kind = "SCRIPT_ACTION";
    bare.handler = "Simulator::dispatch_script";
    bare.measured_ns = 999;
    CHECK(glassbox::format_trace_line(bare) ==
          "1 t=0 SCRIPT_ACTION @Simulator::dispatch_script");

    // measured_ns never appears; msg type without id; from without to.
    glassbox::TraceRecord partial = bare;
    partial.msg_type = MessageType::ack;
    partial.from = NodeId::bank_b;
    CHECK(glassbox::format_trace_line(partial) ==
          "1 t=0 SCRIPT_ACTION msg=ACK @Simulator::dispatch_script");
}

void test_run_bounded()
{
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(1)};
    CHECK(!run.run(5));
    const std::size_t after_five = run.trace().size();
    CHECK(after_five > 0);
    CHECK(run.run());
    CHECK(run.run(0)); // already drained
    CHECK(run.run(1));

    glassbox::ScenarioRun exact{glassbox::make_s1_happy_path(1)};
    std::size_t events = 0;
    while (exact.simulator().step()) {
        ++events;
    }
    glassbox::ScenarioRun bounded{glassbox::make_s1_happy_path(1)};
    CHECK(!bounded.run(events - 1));
    CHECK(bounded.run(1));
    glassbox::ScenarioRun enough{glassbox::make_s1_happy_path(1)};
    CHECK(enough.run(events));
}

} // namespace

int main()
{
    test_registry();
    test_defaults();
    test_s1_outcome();
    test_s1_expected_trace_sequence();
    test_s1_deterministic_and_seed_independent_outcome();
    test_format_trace_line();
    test_run_bounded();
    return 0;
}
