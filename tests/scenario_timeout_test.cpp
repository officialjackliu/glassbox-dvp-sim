// Task 17: S4 (slow link, tight vote timeout -> false abort; raised timeout
// commits) and S5 (participant down before voting -> timeout abort, then
// tombstone resolution once it is back).

#include "scenarios/scenario_library.hpp"
#include "scenario_checks.hpp"

#include <string>
#include <vector>

namespace {

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

[[nodiscard]] std::optional<ReasonCode> abort_reason(
    const glassbox::ScenarioRun& run)
{
    for (const glassbox::LogEntry& e :
         run.simulator().durable_log(NodeId::coordinator)) {
        if (const auto* a = std::get_if<glassbox::DecisionAbort>(&e.record)) {
            return a->reason;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Times timer_fires(const glassbox::ScenarioRun& run,
                                const std::string& timer_kind)
{
    Times out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == "TIMER_FIRE" && r.detail == timer_kind) {
            out.push_back(r.sim_time_us);
        }
    }
    return out;
}

void test_s4_false_abort()
{
    glassbox::ScenarioRun run{glassbox::make_s4_slow_link_tight_timeout(1)};
    CHECK(run.run());

    // The link is slow, but nothing failed: no crash, no drop.
    CHECK(count(run, "CRASH") == 0);
    CHECK(count(run, "MSG_DROP") == 0);
    CHECK(run.simulator().network().link(NodeId::coordinator, NodeId::bank_c)
              .base_latency_us == 300'000);
    CHECK(run.simulator().network().link(NodeId::bank_c, NodeId::coordinator)
              .base_latency_us == 300'000);

    // BANK_C's PREPARE arrives at 1,100 + 300,000; its yes leaves at
    // 302,200 and arrives at 602,200, after the timeout at 1,100 + 500,000.
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_c, MessageType::prepare) ==
          Times{301'100});
    CHECK(sends(run, NodeId::bank_c, MessageType::vote) ==
          Lines{"302200 YES ->COORD"});
    CHECK(sends(run, NodeId::bank_a, MessageType::vote) ==
          Lines{"3200 YES ->COORD"});
    CHECK(timer_fires(run, "VOTE_TIMEOUT") == Times{501'100});
    CHECK(abort_reason(run) == ReasonCode::vote_timeout);
    CHECK(run.simulator().durable_log(NodeId::coordinator)[1].durable_at_us ==
          502'100);
    CHECK(times(run, "MSG_DELIVER", NodeId::coordinator, MessageType::vote) ==
          (Times{4'200, 602'200}));
    // The late yes is answered with ABORT again.
    CHECK(sends(run, NodeId::coordinator, MessageType::abort) ==
          (Lines{"502200 VOTE_TIMEOUT ->BANK_A",
                 "502200 VOTE_TIMEOUT ->BANK_C",
                 "602300 VOTE_TIMEOUT ->BANK_C"}));

    // Both release exactly when their first ABORT arrives, never earlier:
    // after a yes vote a participant does not abort on its own.
    CHECK(notes(run, "UNLOCK", NodeId::bank_a) ==
          Lines{"503200 CASH 2487500"});
    CHECK(notes(run, "UNLOCK", NodeId::bank_c) == Lines{"802200 BONDS 25"});
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::abort) ==
          Times{503'200});
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_c, MessageType::abort) ==
          (Times{802'200, 902'300}));
    CHECK(count(run, "MSG_SEND", MessageType::outcome_query) == 0);

    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.simulator().durable_log(NodeId::coordinator)[2].durable_at_us ==
          1'104'300);
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(run.nodes().bank_c->ledger() == run.genesis().bank_c);
    CHECK(count(run, "ACK_IGNORED") == 1); // second ACK from BANK_C
    scenario_checks::check_quiescent_invariants(run);
}

void test_s4_raised_timeout_commits()
{
    glassbox::ScenarioRun run{glassbox::make_s4_with_vote_timeout(1, 1'000'000)};
    CHECK(run.run());
    CHECK(timer_fires(run, "VOTE_TIMEOUT").empty());
    CHECK(!abort_reason(run).has_value());
    CHECK(log_kinds(run, NodeId::coordinator) ==
          (Lines{"PROPOSED", "DECISION_COMMIT", "END"}));
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_c->ledger().bonds_total() ==
          glassbox::Units{25'000 - 25});
    CHECK(run.nodes().bank_c->ledger().cash_total() ==
          glassbox::Cents{100'000'000 + 2'487'500});
    scenario_checks::check_quiescent_invariants(run);
}

[[nodiscard]] bool commits_with_timeout(glassbox::SimTimeUs timeout)
{
    glassbox::ScenarioRun run{glassbox::make_s4_with_vote_timeout(1, timeout)};
    CHECK(run.run());
    scenario_checks::check_quiescent_invariants(run);
    return run.nodes().bank_c->trade_state(TradeId{1}) ==
           TradeState::committed;
}

void test_s4_timeout_boundary()
{
    // The yes arrives at 602,200; the timer fires at 1,100 + timeout. At
    // equal times the timer (scheduled first) runs first and aborts.
    CHECK(!commits_with_timeout(601'099));
    CHECK(!commits_with_timeout(601'100));
    CHECK(commits_with_timeout(601'101));
}

void test_s4_deadline_caps_vote_timeout()
{
    // Raised timeout, but the trade's deadline is 400 ms after submission:
    // the vote timer is capped to 400 ms and the trade aborts.
    glassbox::ScenarioSetup setup =
        glassbox::make_s4_with_vote_timeout(1, 1'000'000);
    auto& submit = std::get<glassbox::ScriptSubmitTrade>(
        setup.script.back().action.action);
    submit.request.valid_until_us = 400'000;
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    CHECK(timer_fires(run, "VOTE_TIMEOUT") == Times{401'100});
    CHECK(abort_reason(run) == ReasonCode::vote_timeout);
    // BANK_C saw PREPARE (301,100) before the deadline and voted yes.
    CHECK(sends(run, NodeId::bank_c, MessageType::vote) ==
          Lines{"302200 YES ->COORD"});
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) == TradeState::aborted);
    scenario_checks::check_quiescent_invariants(run);
}

[[nodiscard]] Lines drops(const glassbox::ScenarioRun& run)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == "MSG_DROP") {
            out.push_back(std::to_string(r.sim_time_us) + " " +
                          std::string{glassbox::to_string(*r.msg_type)} +
                          " " + r.reason.value_or("-"));
        }
    }
    return out;
}

void check_s5_outcome(const glassbox::ScenarioRun& run)
{
    // Everything sent to BANK_B while it is down is dropped: the PREPARE,
    // the broadcast ABORT, and the first retransmit.
    CHECK(drops(run) == (Lines{"36100 PREPARE NODE_DOWN",
                               "537200 ABORT NODE_DOWN",
                               "1537300 ABORT NODE_DOWN"}));
    CHECK(timer_fires(run, "VOTE_TIMEOUT") == Times{501'100});
    CHECK(abort_reason(run) == ReasonCode::vote_timeout);

    // Buyer releases on the first ABORT.
    CHECK(notes(run, "LOCK", NodeId::bank_a) == Lines{"2100 CASH 2487500"});
    CHECK(notes(run, "UNLOCK", NodeId::bank_a) ==
          Lines{"503200 CASH 2487500"});

    // Retransmits target only the non-acked BANK_B: 502,200 + 1 s, then
    // + 2 s; the second one reaches it after it is back.
    CHECK(notes(run, "RETRANSMIT", NodeId::coordinator) ==
          (Lines{"1502200 BANK_B", "3502300 BANK_B"}));
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_b, MessageType::abort) ==
          Times{3'537'400});
    CHECK(notes(run, "STATE_CHANGE", NodeId::bank_b) ==
          Lines{"3537400 NONE->ABORTED"});
    CHECK(log_kinds(run, NodeId::bank_b) == Lines{"ABORTED"});
    CHECK(run.simulator().durable_log(NodeId::bank_b)[0].durable_at_us ==
          3'538'400);
    CHECK(sends(run, NodeId::bank_b, MessageType::ack) ==
          Lines{"3538500 ABORT ->COORD"});
    CHECK(notes(run, "LOCK", NodeId::bank_b).empty());
    CHECK(sends(run, NodeId::bank_b, MessageType::vote).empty());
    CHECK(run.simulator().durable_log(NodeId::coordinator)[2].durable_at_us ==
          3'574'500);

    const glassbox::ParticipantTrade* tombstone =
        run.nodes().bank_b->find(TradeId{1});
    CHECK(tombstone != nullptr);
    CHECK(tombstone->state == TradeState::aborted);
    CHECK(!tombstone->trade.has_value());
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(run.nodes().bank_b->ledger() == run.genesis().bank_b);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s5_node_down()
{
    glassbox::ScenarioRun run{glassbox::make_s5_participant_down(1)};
    CHECK(run.run());
    CHECK(count(run, "CRASH") == 0);
    CHECK(run.simulator().node_up(NodeId::bank_b));
    check_s5_outcome(run);
}

void test_s5_crash_and_restart_variant()
{
    // Same failure as a real crash/restart: same drops, same resolution.
    glassbox::ScenarioSetup setup = glassbox::make_s5_participant_down(1);
    setup.script.front().action.action = glassbox::ScriptCrash{NodeId::bank_b};
    setup.script.back().action.action = glassbox::ScriptRestart{NodeId::bank_b};
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    CHECK(count(run, "CRASH") == 1);
    CHECK(count(run, "RESTART") == 1);
    CHECK(run.simulator().epoch(NodeId::bank_b) == 1);
    check_s5_outcome(run);
}

void test_registry_entries()
{
    CHECK(glassbox::find_scenario("S4")->make ==
          &glassbox::make_s4_slow_link_tight_timeout);
    CHECK(glassbox::find_scenario("S5")->make ==
          &glassbox::make_s5_participant_down);
    CHECK(glassbox::make_s4_slow_link_tight_timeout(1)
              .config.timers.vote_timeout_us == 500'000);
    CHECK(glassbox::make_s4_with_vote_timeout(1, 7)
              .config.timers.vote_timeout_us == 7);
    // Seeds do not change S4/S5 outcomes (no jitter or random drops).
    for (std::uint64_t seed : {2u, 3u}) {
        glassbox::ScenarioRun s4{glassbox::make_s4_slow_link_tight_timeout(seed)};
        CHECK(s4.run());
        CHECK(abort_reason(s4) == ReasonCode::vote_timeout);
        glassbox::ScenarioRun s5{glassbox::make_s5_participant_down(seed)};
        CHECK(s5.run());
        check_s5_outcome(s5);
    }
}

} // namespace

int main()
{
    test_s4_false_abort();
    test_s4_raised_timeout_commits();
    test_s4_timeout_boundary();
    test_s4_deadline_caps_vote_timeout();
    test_s5_node_down();
    test_s5_crash_and_restart_variant();
    test_registry_entries();
    return 0;
}
