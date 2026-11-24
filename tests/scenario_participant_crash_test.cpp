// Task 20: S6-S8 participant crash/recovery. Each crash point on BANK_A's
// PREPARE batch leads to a different, correct cleanup path.

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
using scenario_checks::count;
using scenario_checks::log_kinds;
using scenario_checks::notes;
using scenario_checks::sends;
using scenario_checks::times;
using Lines = std::vector<std::string>;
using Times = std::vector<glassbox::SimTimeUs>;

constexpr std::int64_t k_price = 2'487'500;

[[nodiscard]] Lines events(const glassbox::ScenarioRun& run,
                           const std::string& kind)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == kind) {
            out.push_back(std::to_string(r.sim_time_us) + " " +
                          std::string{glassbox::to_string(*r.node)} +
                          (r.reason ? " " + *r.reason : ""));
        }
    }
    return out;
}

[[nodiscard]] Lines drops_to(const glassbox::ScenarioRun& run, NodeId to)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == "MSG_DROP" && r.to == to) {
            out.push_back(std::to_string(r.sim_time_us) + " " +
                          std::string{glassbox::to_string(*r.msg_type)} +
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

void test_s6_in_doubt_participant_commits_after_restart()
{
    glassbox::ScenarioRun run{glassbox::make_s6_in_doubt_participant(1)};

    // Just after the restart: the PREPARED record restored the lock and
    // the in-doubt state.
    scenario_checks::run_until(run, 5'003'201);
    CHECK(events(run, "CRASH") == Lines{"3200 BANK_A AFTER_FIRST_SEND"});
    CHECK(events(run, "RESTART") == Lines{"5003200 BANK_A"});
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_a->ledger().cash_locked() == Cents{k_price});
    CHECK(run.nodes().bank_a->find(TradeId{1})->trade.has_value());
    CHECK(notes(run, "RECOVERY_STEP", NodeId::bank_a) ==
          Lines{"5003200 IN_DOUBT_QUERY"});
    // Meanwhile the rest of the system already committed.
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::committing);

    CHECK(run.run());
    // The vote got out before the crash; the coordinator committed.
    CHECK(sends(run, NodeId::bank_a, MessageType::vote) ==
          Lines{"3200 YES ->COORD"});
    CHECK(durable_at(run, NodeId::coordinator, "DECISION_COMMIT") == 73'200);
    // COMMITs to the crashed BANK_A are dropped: broadcast + 2 retransmits.
    CHECK(drops_to(run, NodeId::bank_a) ==
          (Lines{"74300 COMMIT NODE_DOWN", "1074400 COMMIT NODE_DOWN",
                 "3074500 COMMIT NODE_DOWN"}));
    // After restart: query, COMMIT, apply, ACK, END.
    CHECK(sends(run, NodeId::bank_a, MessageType::outcome_query) ==
          Lines{"5003300 - ->COORD"});
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::commit) ==
          Times{5'005'400});
    CHECK(notes(run, "APPLY", NodeId::bank_a) ==
          Lines{"5005400 CASH -2487500 BONDS +25"});
    CHECK(durable_at(run, NodeId::bank_a, "COMMITTED") == 5'006'400);
    CHECK(sends(run, NodeId::bank_a, MessageType::ack) ==
          Lines{"5006500 COMMIT ->COORD"});
    CHECK(durable_at(run, NodeId::coordinator, "END") == 5'008'500);

    // Lock-hold time spans the outage: locked at 2,100, applied at
    // 5,005,400 (5,003,300 us > the 5 s outage).
    CHECK(notes(run, "LOCK", NodeId::bank_a) == Lines{"2100 CASH 2487500"});
    CHECK(5'005'400 - 2'100 > glassbox::k_s6_restart_after_us);

    CHECK(log_kinds(run, NodeId::bank_a) == (Lines{"PREPARED", "COMMITTED"}));
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->ledger().cash_total() ==
          Cents{100'000'000 - k_price});
    CHECK(count(run, "RECOVERY_ERROR") == 0);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s7_logged_but_unsent_vote_aborts()
{
    glassbox::ScenarioRun run{glassbox::make_s7_crash_before_vote_sent(1)};
    scenario_checks::run_until(run, 2'003'101);
    CHECK(events(run, "CRASH") == Lines{"3100 BANK_A AFTER_LOG_BEFORE_SEND"});
    CHECK(events(run, "RESTART") == Lines{"2003100 BANK_A"});
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_a->ledger().cash_locked() == Cents{k_price});

    CHECK(run.run());
    // No vote ever left BANK_A before the restart.
    CHECK(sends(run, NodeId::bank_a, MessageType::vote).empty());
    CHECK(durable_at(run, NodeId::bank_a, "PREPARED") == 3'100);
    // Coordinator timed out (1,100 + 500,000) and aborted.
    const auto log = run.simulator().durable_log(NodeId::coordinator);
    CHECK(std::get<glassbox::DecisionAbort>(log[1].record).reason ==
          ReasonCode::vote_timeout);
    CHECK(log[1].durable_at_us == 502'100);
    CHECK(drops_to(run, NodeId::bank_a) ==
          (Lines{"503200 ABORT NODE_DOWN", "1503300 ABORT NODE_DOWN"}));
    // BANK_B released at the broadcast ABORT.
    CHECK(notes(run, "UNLOCK", NodeId::bank_b) == Lines{"537200 BONDS 25"});
    // BANK_A restarts in doubt, asks, gets ABORT(VOTE_TIMEOUT), releases.
    CHECK(sends(run, NodeId::bank_a, MessageType::outcome_query) ==
          Lines{"2003200 - ->COORD"});
    CHECK(sends(run, NodeId::coordinator, MessageType::abort).back() ==
          "2004300 VOTE_TIMEOUT ->BANK_A");
    CHECK(notes(run, "UNLOCK", NodeId::bank_a) ==
          Lines{"2005300 CASH 2487500"});
    CHECK(log_kinds(run, NodeId::bank_a) == (Lines{"PREPARED", "ABORTED"}));
    CHECK(durable_at(run, NodeId::coordinator, "END") == 2'008'400);

    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(run.nodes().bank_b->ledger() == run.genesis().bank_b);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s8_crash_before_log_restarts_clean()
{
    glassbox::ScenarioRun run{glassbox::make_s8_crash_before_log(1)};
    scenario_checks::run_until(run, 2'002'101);
    CHECK(events(run, "CRASH") == Lines{"2100 BANK_A BEFORE_LOG"});
    CHECK(events(run, "RESTART") == Lines{"2002100 BANK_A"});
    // Nothing survived: no record, no lock, no trade, nothing to ask about.
    CHECK(run.simulator().durable_log(NodeId::bank_a).empty());
    CHECK(run.nodes().bank_a->trades().empty());
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(notes(run, "RECOVERY_STEP", NodeId::bank_a).empty());

    CHECK(run.run());
    CHECK(notes(run, "LOCK", NodeId::bank_a).empty());
    CHECK(sends(run, NodeId::bank_a, MessageType::vote).empty());
    CHECK(sends(run, NodeId::bank_a, MessageType::outcome_query).empty());
    CHECK(drops_to(run, NodeId::bank_a) ==
          (Lines{"503200 ABORT NODE_DOWN", "1503300 ABORT NODE_DOWN"}));
    // The second retransmit (3,502,300; sent 3,502,400) finds BANK_A up
    // with no state: tombstone + ACK.
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::abort) ==
          Times{3'503'400});
    CHECK(notes(run, "STATE_CHANGE", NodeId::bank_a) ==
          Lines{"3503400 NONE->ABORTED"});
    CHECK(log_kinds(run, NodeId::bank_a) == Lines{"ABORTED"});
    CHECK(durable_at(run, NodeId::bank_a, "ABORTED") == 3'504'400);
    CHECK(durable_at(run, NodeId::coordinator, "END") == 3'506'500);
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    scenario_checks::check_quiescent_invariants(run);
}

void test_registry_and_setups()
{
    CHECK(glassbox::find_scenario("S6")->make ==
          &glassbox::make_s6_in_doubt_participant);
    CHECK(glassbox::find_scenario("S7")->make ==
          &glassbox::make_s7_crash_before_vote_sent);
    CHECK(glassbox::find_scenario("S8")->make ==
          &glassbox::make_s8_crash_before_log);
    const auto s6 = glassbox::make_s6_in_doubt_participant(1);
    CHECK(s6.crashes.size() == 1);
    CHECK(s6.crashes[0].node == NodeId::bank_a);
    CHECK(s6.crashes[0].trade_id == TradeId{1});
    CHECK(s6.crashes[0].point == glassbox::CrashPoint::after_first_send);
    CHECK(s6.crashes[0].restart_after_us == 5'000'000);
    CHECK(glassbox::make_s7_crash_before_vote_sent(1).crashes[0].point ==
          glassbox::CrashPoint::after_log_before_send);
    CHECK(glassbox::make_s8_crash_before_log(1).crashes[0].point ==
          glassbox::CrashPoint::before_log);
    // Registry is in id order.
    const auto registry = glassbox::scenario_registry();
    for (std::size_t i = 1; i < registry.size(); ++i) {
        const auto number = [](std::string_view id) {
            return std::stoi(std::string{id.substr(1)});
        };
        CHECK(number(registry[i - 1].id) < number(registry[i].id));
    }
}

void test_outcome_independent_of_restart_delay()
{
    // Restarting sooner or later changes only timing, never the outcome.
    for (glassbox::SimTimeUs delay : {1'000, 100'000, 2'500'000, 9'000'000}) {
        glassbox::ScenarioSetup s6 = glassbox::make_s6_in_doubt_participant(1);
        s6.crashes[0].restart_after_us = delay;
        glassbox::ScenarioRun r6{s6};
        CHECK(r6.run());
        CHECK(r6.nodes().bank_a->trade_state(TradeId{1}) ==
              TradeState::committed);
        scenario_checks::check_quiescent_invariants(r6);

        glassbox::ScenarioSetup s7 = glassbox::make_s7_crash_before_vote_sent(1);
        s7.crashes[0].restart_after_us = delay;
        glassbox::ScenarioRun r7{s7};
        CHECK(r7.run());
        // Even a fast restart does not save the trade: BANK_A never re-sends
        // its vote (no PREPARE retransmit, DESIGN.md open question 2), so the
        // coordinator times out and aborts; BANK_A learns it by querying.
        CHECK(r7.nodes().bank_a->trade_state(TradeId{1}) ==
              TradeState::aborted);
        scenario_checks::check_quiescent_invariants(r7);

        glassbox::ScenarioSetup s8 = glassbox::make_s8_crash_before_log(1);
        s8.crashes[0].restart_after_us = delay;
        glassbox::ScenarioRun r8{s8};
        CHECK(r8.run());
        CHECK(r8.nodes().bank_a->trade_state(TradeId{1}) ==
              TradeState::aborted);
        scenario_checks::check_quiescent_invariants(r8);
    }
}

} // namespace

int main()
{
    test_registry_and_setups();
    test_s6_in_doubt_participant_commits_after_restart();
    test_s7_logged_but_unsent_vote_aborts();
    test_s8_crash_before_log_restarts_clean();
    test_outcome_independent_of_restart_delay();
    return 0;
}
