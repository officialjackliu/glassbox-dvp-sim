// Task 22: S14 partition during voting and heal. The coordinator times out
// and aborts; once healed, the in-doubt participant resolves by query (or,
// if it never saw PREPARE, by a tombstone from a retransmitted ABORT).
// Partition drops are reasoned PARTITION and never treated as node-down.

#include "scenarios/scenario_library.hpp"
#include "scenario_checks.hpp"

#include <string>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::ReasonCode;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::Units;
using scenario_checks::count;
using scenario_checks::log_kinds;
using scenario_checks::notes;
using scenario_checks::sends;
using scenario_checks::times;
using Lines = std::vector<std::string>;
using Times = std::vector<glassbox::SimTimeUs>;

[[nodiscard]] Lines drops(const glassbox::ScenarioRun& run)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind.starts_with("MSG_DROP")) {
            out.push_back(std::to_string(r.sim_time_us) + " " + r.kind + " " +
                          std::string{glassbox::to_string(*r.msg_type)} +
                          " " + std::string{glassbox::to_string(*r.from)} +
                          "->" + std::string{glassbox::to_string(*r.to)} +
                          " " + r.reason.value_or("-"));
        }
    }
    return out;
}

[[nodiscard]] glassbox::SimTimeUs durable_at(const glassbox::ScenarioRun& run,
                                             NodeId node,
                                             const std::string& kind)
{
    for (const glassbox::LogEntry& e : run.simulator().durable_log(node)) {
        if (glassbox::record_kind(e.record) == kind) {
            return e.durable_at_us;
        }
    }
    return -1;
}

void check_not_node_down(const glassbox::ScenarioRun& run)
{
    // A partition is not a node failure: nobody crashed or went down, and
    // no timer was ignored for node_down / stale_epoch.
    CHECK(count(run, "CRASH") == 0);
    CHECK(count(run, "MSG_DROP_NODE_DOWN") == 0);
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == "MSG_DROP") {
            CHECK(r.reason == "PARTITION");
        }
        if (r.kind == "TIMER_IGNORED") {
            CHECK(r.reason == "state_mismatch");
        }
    }
    for (NodeId node : {NodeId::coordinator, NodeId::bank_a, NodeId::bank_b,
                        NodeId::bank_c}) {
        CHECK(run.simulator().node_up(node));
    }
}

void test_s14_partition_during_voting_resolved_by_query()
{
    glassbox::ScenarioRun run{glassbox::make_s14_partition(1)};

    // While partitioned (1.5 s): coordinator has aborted, BANK_A released,
    // BANK_C still in doubt holding its lock.
    scenario_checks::run_until(run, 1'500'000);
    CHECK(run.simulator().network().partitioned(NodeId::coordinator,
                                                NodeId::bank_c));
    CHECK(run.simulator().network().partitioned(NodeId::bank_c,
                                                NodeId::coordinator));
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::aborting);
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_c->ledger().bonds_locked() == Units{25});

    CHECK(run.run());
    CHECK(!run.simulator().network().partitioned(NodeId::coordinator,
                                                 NodeId::bank_c));
    // BANK_C got PREPARE before the partition and voted yes into it.
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_c, MessageType::prepare) ==
          Times{121'100});
    CHECK(sends(run, NodeId::bank_c, MessageType::vote) ==
          Lines{"122200 YES ->COORD"});
    CHECK(drops(run) ==
          (Lines{"242200 MSG_DROP VOTE BANK_C->COORD PARTITION",
                 "622200 MSG_DROP ABORT COORD->BANK_C PARTITION",
                 "1242300 MSG_DROP OUTCOME_QUERY BANK_C->COORD PARTITION",
                 "1622300 MSG_DROP ABORT COORD->BANK_C PARTITION"}));
    const auto log = run.simulator().durable_log(NodeId::coordinator);
    CHECK(std::get<glassbox::DecisionAbort>(log[1].record).reason ==
          ReasonCode::vote_timeout);
    CHECK(notes(run, "UNLOCK", NodeId::bank_a) ==
          Lines{"503200 CASH 2487500"});
    // After the heal at 2 s: BANK_C's second query (3,122,400) gets through
    // and is answered with ABORT(VOTE_TIMEOUT) before the next retransmit.
    CHECK(sends(run, NodeId::bank_c, MessageType::outcome_query) ==
          (Lines{"1122300 - ->COORD", "3122400 - ->COORD"}));
    CHECK(times(run, "MSG_DELIVER", NodeId::coordinator,
                MessageType::outcome_query) == Times{3'242'400});
    CHECK(sends(run, NodeId::coordinator, MessageType::abort).back() ==
          "3242500 VOTE_TIMEOUT ->BANK_C");
    CHECK(notes(run, "UNLOCK", NodeId::bank_c) == Lines{"3362500 BONDS 25"});
    CHECK(log_kinds(run, NodeId::bank_c) == (Lines{"PREPARED", "ABORTED"}));
    CHECK(durable_at(run, NodeId::coordinator, "END") == 3'484'600);
    // The retransmit due at 3,502,300 found the trade done.
    CHECK(notes(run, "RETRANSMIT", NodeId::coordinator) ==
          Lines{"1502200 BANK_C"});

    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(run.nodes().bank_c->ledger() == run.genesis().bank_c);
    check_not_node_down(run);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s14_partition_before_prepare_resolved_by_tombstone()
{
    glassbox::ScenarioRun run{
        glassbox::make_s14_with_partition(1, 0, 2'000'000)};
    CHECK(run.run());
    CHECK(drops(run) ==
          (Lines{"121100 MSG_DROP PREPARE COORD->BANK_C PARTITION",
                 "622200 MSG_DROP ABORT COORD->BANK_C PARTITION",
                 "1622300 MSG_DROP ABORT COORD->BANK_C PARTITION"}));
    CHECK(notes(run, "LOCK", NodeId::bank_c).empty());
    CHECK(sends(run, NodeId::bank_c, MessageType::outcome_query).empty());
    // The retransmit at 3,502,300 (sent 3,502,400) reaches BANK_C after the
    // heal: tombstone + ACK.
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_c, MessageType::abort) ==
          Times{3'622'400});
    CHECK(notes(run, "STATE_CHANGE", NodeId::bank_c) ==
          Lines{"3622400 NONE->ABORTED"});
    CHECK(log_kinds(run, NodeId::bank_c) == Lines{"ABORTED"});
    CHECK(durable_at(run, NodeId::coordinator, "END") == 3'744'500);
    CHECK(!run.nodes().bank_c->find(TradeId{1})->trade.has_value());
    check_not_node_down(run);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s14_partition_after_vote_does_not_matter()
{
    // Partition applies at send time: once BANK_C's yes has left (122,200),
    // partitioning does not stop it. But the COMMIT to BANK_C is then sent
    // into the partition, so BANK_C commits only after the heal.
    glassbox::ScenarioRun run{
        glassbox::make_s14_with_partition(1, 122'201, 2'000'000)};
    CHECK(run.run());
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(times(run, "MSG_DELIVER", NodeId::coordinator, MessageType::vote) ==
          (Times{4'200, 242'200}));
    int partition_drops = 0;
    for (const glassbox::TraceRecord& r : run.trace()) {
        partition_drops += r.reason == "PARTITION" ? 1 : 0;
    }
    CHECK(partition_drops >= 1);
    check_not_node_down(run);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s14_outcome_independent_of_heal_time()
{
    for (glassbox::SimTimeUs heal : {600'000, 1'000'000, 5'000'000,
                                     20'000'000}) {
        glassbox::ScenarioRun run{glassbox::make_s14_with_partition(
            1, glassbox::k_s14_partition_at_us, heal)};
        CHECK(run.run());
        CHECK(run.nodes().bank_c->trade_state(TradeId{1}) ==
              TradeState::aborted);
        CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
              TradeState::aborted);
        check_not_node_down(run);
        scenario_checks::check_quiescent_invariants(run);
    }
}

void test_registry()
{
    CHECK(glassbox::find_scenario("S14")->make == &glassbox::make_s14_partition);
    CHECK(glassbox::scenario_registry().size() == 14);
    const auto setup = glassbox::make_s14_partition(1);
    CHECK(setup.script.size() == 3);
    CHECK(setup.script[1].at == 122'000);
    CHECK(setup.script[2].at == 2'000'000);
    CHECK(std::get<glassbox::ScriptSetPartition>(setup.script[1].action.action)
              .partitioned);
    CHECK(!std::get<glassbox::ScriptSetPartition>(
               setup.script[2].action.action)
               .partitioned);
}

} // namespace

int main()
{
    test_registry();
    test_s14_partition_during_voting_resolved_by_query();
    test_s14_partition_before_prepare_resolved_by_tombstone();
    test_s14_partition_after_vote_does_not_matter();
    test_s14_outcome_independent_of_heal_time();
    return 0;
}
