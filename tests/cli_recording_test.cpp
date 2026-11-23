// Glass-box player: record() captures a run event by event without changing
// it, and pairs messages and timers exactly.

#include "cli/recording.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <map>
#include <string>
#include <vector>

namespace {

using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::SimTimeUs;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::cli::Flight;
using glassbox::cli::k_open_end;
using glassbox::cli::Recording;
using Lines = std::vector<std::string>;

[[nodiscard]] Lines flight_lines(const Recording& r)
{
    Lines out;
    for (const Flight& f : r.flights) {
        out.push_back("#" + std::to_string(f.msg_id) + " " +
                      std::string{glassbox::to_string(f.type)} + " " +
                      std::string{glassbox::to_string(f.from)} + "->" +
                      std::string{glassbox::to_string(f.to)} + " " +
                      std::to_string(f.sent_at_us) + "->" +
                      (f.ends_at_us == k_open_end
                           ? std::string{"open"}
                           : std::to_string(f.ends_at_us)) +
                      " " + f.detail.value_or("-") +
                      (f.drop_reason ? " DROP:" + *f.drop_reason : ""));
    }
    return out;
}

[[nodiscard]] Lines timer_lines(const Recording& r)
{
    Lines out;
    for (const auto& t : r.timers) {
        out.push_back(std::string{glassbox::to_string(t.node)} + " t" +
                      std::to_string(t.trade_id.value()) + " " + t.kind +
                      " " + std::to_string(t.set_at_us) + "->" +
                      (t.fires_at_us ? std::to_string(*t.fires_at_us)
                                     : std::string{"never"}));
    }
    return out;
}

// Every paired timer has a TIMER_FIRE/TIMER_IGNORED record for the same
// node, trade and kind at its fire time.
void check_timer_pairing(const Recording& rec)
{
    for (const auto& t : rec.timers) {
        if (!t.fires_at_us) {
            continue;
        }
        bool found = false;
        for (const auto& frame : rec.frames) {
            for (const auto& r : frame.records) {
                found = found ||
                        ((r.kind == "TIMER_FIRE" || r.kind == "TIMER_IGNORED") &&
                         r.sim_time_us == *t.fires_at_us && r.node == t.node &&
                         r.trade_id == t.trade_id && r.detail == t.kind);
            }
        }
        CHECK(found);
    }
}

void test_trace_identical_to_a_plain_run()
{
    // Recording must not change the run: the concatenated per-event records
    // equal ScenarioRun::run()'s trace, for every scenario.
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        for (std::uint64_t seed : {1, 7}) {
            const Recording rec = glassbox::cli::record(info.make(seed));
            glassbox::ScenarioRun plain{info.make(seed)};
            CHECK(plain.run());
            CHECK(rec.quiesced);
            std::vector<glassbox::TraceRecord> joined;
            for (const auto& frame : rec.frames) {
                joined.insert(joined.end(), frame.records.begin(),
                              frame.records.end());
            }
            CHECK(joined.size() == plain.trace().size());
            for (std::size_t i = 0; i < joined.size(); ++i) {
                const auto& a = joined[i];
                const auto& b = plain.trace()[i];
                CHECK(a.seq == b.seq);
                CHECK(a.sim_time_us == b.sim_time_us);
                CHECK(a.kind == b.kind);
                CHECK(a.handler == b.handler);
                CHECK(a.node == b.node);
                CHECK(a.trade_id == b.trade_id);
                CHECK(a.msg_id == b.msg_id);
                CHECK(a.detail == b.detail);
                CHECK(a.reason == b.reason);
            }
            // Frame 0 is before the first event; every later frame holds
            // the records of exactly one event, at one time.
            CHECK(rec.frames[0].records.empty());
            CHECK(rec.frames[0].time_us == 0);
            for (std::size_t i = 1; i < rec.frames.size(); ++i) {
                const auto& frame = rec.frames[i];
                CHECK(!frame.records.empty());
                CHECK(frame.time_us >= rec.frames[i - 1].time_us);
                for (const auto& r : frame.records) {
                    CHECK(r.sim_time_us == frame.time_us);
                }
                CHECK(frame.violations.empty());
            }
            check_timer_pairing(rec);
        }
    }
}

void test_s1_frames_and_views()
{
    const Recording rec =
        glassbox::cli::record(glassbox::make_s1_happy_path(1));
    CHECK(rec.frames.size() == 29);

    // Frame 0: genesis everywhere, nothing on disk.
    const auto& start = rec.frames[0];
    CHECK(start.nodes[0].node == NodeId::coordinator);
    CHECK(start.nodes[1].node == NodeId::bank_a);
    CHECK(start.nodes[2].node == NodeId::bank_b);
    CHECK(start.nodes[3].node == NodeId::bank_c);
    CHECK(!start.nodes[0].balances.has_value());
    CHECK(start.nodes[1].balances ==
          (glassbox::BalanceView{100'000'000, 0, 25'000, 0}));
    for (const auto& view : start.nodes) {
        CHECK(view.up);
        CHECK(view.disk.empty());
        CHECK(view.trades.empty());
        CHECK(view.coordinator_trades.empty());
    }

    // Frame 5 (t=2,100): BANK_A received PREPARE and locked the cash.
    const auto& locked = rec.frames[5];
    CHECK(locked.time_us == 2'100);
    CHECK(locked.nodes[1].balances ==
          (glassbox::BalanceView{100'000'000, 2'487'500, 25'000, 0}));
    CHECK(locked.nodes[1].trades.at(TradeId{1}).state == TradeState::preparing);
    CHECK(locked.nodes[1].disk.empty());
    CHECK(locked.nodes[0].disk == Lines{"PROPOSED t1"});
    CHECK(locked.nodes[0].coordinator_trades.at(TradeId{1}).state ==
          TradeState::collecting);

    // Frame 6 (t=3,100): PREPARED is durable, the participant is PREPARED.
    CHECK(rec.frames[6].nodes[1].disk == Lines{"PREPARED t1"});
    CHECK(rec.frames[6].nodes[1].trades.at(TradeId{1}).state ==
          TradeState::prepared);

    // Frame 12 (t=72,200): both votes in, decision taken, not yet durable.
    const auto& coord = rec.frames[12].nodes[0].coordinator_trades.at(TradeId{1});
    CHECK(coord.buyer_voted_yes);
    CHECK(coord.seller_voted_yes);
    CHECK(coord.state == TradeState::committing);
    CHECK(coord.decision == glassbox::Decision::commit);
    CHECK(!coord.decision_durable);

    // Final frame: settled balances and full disks.
    const auto& end = rec.frames.back();
    CHECK(end.nodes[1].balances ==
          (glassbox::BalanceView{97'512'500, 0, 25'025, 0}));
    CHECK(end.nodes[2].balances ==
          (glassbox::BalanceView{102'487'500, 0, 24'975, 0}));
    CHECK(end.nodes[0].disk ==
          (Lines{"PROPOSED t1", "DECISION_COMMIT t1", "END t1"}));
    CHECK(end.nodes[1].disk == (Lines{"PREPARED t1", "COMMITTED t1"}));

    // Only the submission carries a script action.
    CHECK(rec.frames[1].action.has_value());
    for (std::size_t i = 2; i < rec.frames.size(); ++i) {
        CHECK(!rec.frames[i].action.has_value());
    }
    CHECK(std::holds_alternative<glassbox::ScriptSubmitTrade>(
        rec.frames[1].action->action));
}

void test_s1_flights_and_timers()
{
    const Recording rec =
        glassbox::cli::record(glassbox::make_s1_happy_path(1));
    CHECK(flight_lines(rec) ==
          (Lines{"#2 PREPARE COORD->BANK_A 1100->2100 BUYER",
                 "#3 PREPARE COORD->BANK_B 1100->36100 SELLER",
                 "#4 VOTE BANK_A->COORD 3200->4200 YES",
                 "#5 VOTE BANK_B->COORD 37200->72200 YES",
                 "#6 COMMIT COORD->BANK_A 73300->74300 -",
                 "#7 COMMIT COORD->BANK_B 73300->108300 -",
                 "#8 ACK BANK_A->COORD 75400->76400 COMMIT",
                 "#9 ACK BANK_B->COORD 109400->144400 COMMIT"}));
    CHECK(timer_lines(rec) ==
          (Lines{"COORD t1 VOTE_TIMEOUT 0->501100",
                 "BANK_A t1 DECISION_WAIT 2100->1003200",
                 "BANK_B t1 DECISION_WAIT 36100->1037200",
                 "COORD t1 RETRANSMIT 72200->1073300"}));
}

void test_in_flight_boundaries()
{
    const Recording rec =
        glassbox::cli::record(glassbox::make_s1_happy_path(1));
    const auto ids = [&](SimTimeUs t) {
        std::vector<std::uint64_t> out;
        for (const Flight* f : glassbox::cli::in_flight(rec, t)) {
            out.push_back(f->msg_id);
        }
        return out;
    };
    using Ids = std::vector<std::uint64_t>;
    CHECK(ids(1'099).empty());           // before the PREPAREs leave
    CHECK(ids(1'100) == (Ids{2, 3}));    // sent_at is included
    CHECK(ids(2'099) == (Ids{2, 3}));
    CHECK(ids(2'100) == Ids{3});         // ends_at is excluded
    CHECK(ids(3'200) == (Ids{3, 4}));
    CHECK(ids(36'100).empty());
    CHECK(ids(73'300) == (Ids{6, 7}));
    CHECK(ids(144'399) == Ids{9});
    CHECK(ids(144'400).empty());
}

void test_duplicates_yield_one_flight_per_copy()
{
    const Recording rec = glassbox::cli::record(
        glassbox::make_s12_duplicates_and_reordering(1));
    int prepares = 0;
    int commits = 0;
    std::map<std::uint64_t, int> copies;
    for (std::size_t i = 0; i < rec.flights.size(); ++i) {
        const Flight& f = rec.flights[i];
        CHECK(f.ends_at_us != k_open_end);
        CHECK(f.ends_at_us >= f.sent_at_us);
        CHECK(!f.drop_reason.has_value());
        if (i > 0) {
            CHECK(rec.flights[i - 1].sent_at_us <= f.sent_at_us);
        }
        prepares += f.type == MessageType::prepare ? 1 : 0;
        commits += f.type == MessageType::commit ? 1 : 0;
        ++copies[f.msg_id];
    }
    CHECK(prepares == 4); // two PREPAREs, each duplicated
    // Two duplicated COMMITs, plus two re-sent to late duplicate votes.
    CHECK(commits == 6);
    std::vector<std::uint64_t> doubled;
    for (const auto& [id, n] : copies) {
        CHECK(n == 1 || n == 2);
        if (n == 2) {
            doubled.push_back(id);
        }
    }
    CHECK((doubled == std::vector<std::uint64_t>{2, 3, 8, 9}));
    const auto lines = flight_lines(rec);
    CHECK(lines[0] == "#2 PREPARE COORD->BANK_A 1100->14954 BUYER");
    CHECK(lines[1] == "#3 PREPARE COORD->BANK_B 1100->14641 SELLER");
    CHECK(lines[2] == "#3 PREPARE COORD->BANK_B 1100->23440 SELLER");
    CHECK(lines[3] == "#2 PREPARE COORD->BANK_A 1100->24165 BUYER");
}

void test_drops_partition_and_script_actions()
{
    const Recording rec = glassbox::cli::record(glassbox::make_s14_partition(1));
    Lines dropped;
    for (const std::string& line : flight_lines(rec)) {
        if (line.find("DROP:") != std::string::npos) {
            dropped.push_back(line);
        }
    }
    CHECK(dropped ==
          (Lines{"#5 VOTE BANK_C->COORD 122200->242200 YES DROP:PARTITION",
                 "#7 ABORT COORD->BANK_C 502200->622200 VOTE_TIMEOUT "
                 "DROP:PARTITION",
                 "#9 OUTCOME_QUERY BANK_C->COORD 1122300->1242300 - "
                 "DROP:PARTITION",
                 "#10 ABORT COORD->BANK_C 1502300->1622300 VOTE_TIMEOUT "
                 "DROP:PARTITION"}));

    // Script actions attach to the events that ran them, in order.
    std::vector<std::pair<SimTimeUs, std::size_t>> actions;
    for (const auto& frame : rec.frames) {
        if (frame.action) {
            actions.emplace_back(frame.time_us, frame.action->action.index());
        }
    }
    CHECK(actions.size() == 3);
    CHECK(actions[0].first == 0);
    CHECK(std::holds_alternative<glassbox::ScriptSubmitTrade>(
        rec.frames[1].action->action));
    CHECK(actions[1].first == 122'000);
    CHECK(actions[2].first == 2'000'000);
    for (const auto& frame : rec.frames) {
        if (frame.action && frame.time_us > 0) {
            const auto& p =
                std::get<glassbox::ScriptSetPartition>(frame.action->action);
            CHECK(p.a == NodeId::coordinator);
            CHECK(p.b == NodeId::bank_c);
            CHECK(p.partitioned == (frame.time_us == 122'000));
        }
    }

    // Messages to a down node end as NODE_DOWN drops.
    const Recording s5 =
        glassbox::cli::record(glassbox::make_s5_participant_down(1));
    int node_down = 0;
    for (const Flight& f : s5.flights) {
        node_down += f.drop_reason == "NODE_DOWN" ? 1 : 0;
    }
    CHECK(node_down == 3);
}

void test_timers_pair_by_crash_epoch()
{
    // S9 with the coordinator back after 1 ms: the pre-crash vote timer
    // fires after the restart (stale_epoch) while the post-restart
    // RETRANSMIT is a different, current timer.
    glassbox::ScenarioSetup setup =
        glassbox::make_s9_coordinator_crash_after_decision(1);
    setup.crashes[0].restart_after_us = 1'000;
    const Recording rec = glassbox::cli::record(setup);
    bool saw_stale = false;
    for (const auto& frame : rec.frames) {
        for (const auto& r : frame.records) {
            saw_stale = saw_stale || r.reason == "stale_epoch";
        }
    }
    CHECK(saw_stale);
    const Lines lines = timer_lines(rec);
    CHECK(lines[0] == "COORD t1 VOTE_TIMEOUT 0->501100");
    CHECK(lines[3] == "COORD t1 RETRANSMIT 74200->1074300");
    for (const auto& t : rec.timers) {
        CHECK(t.fires_at_us.has_value());
        CHECK(*t.fires_at_us > t.set_at_us);
    }

    // Normal S9: the vote timer fires while COORD is down (node_down).
    const Recording s9 = glassbox::cli::record(
        glassbox::make_s9_coordinator_crash_after_decision(1));
    CHECK(timer_lines(s9)[0] == "COORD t1 VOTE_TIMEOUT 0->501100");
    CHECK(timer_lines(s9).back() == "COORD t1 RETRANSMIT 5073200->6073300");
}

void test_bounded_recording()
{
    const Recording rec =
        glassbox::cli::record(glassbox::make_s1_happy_path(1), 6);
    CHECK(!rec.quiesced);
    CHECK(rec.frames.size() == 7); // frame 0 + 6 events
    CHECK(rec.frames.back().time_us == 3'100);
    // PREPARE to BANK_B never arrived within the recording.
    CHECK(flight_lines(rec) ==
          (Lines{"#2 PREPARE COORD->BANK_A 1100->2100 BUYER",
                 "#3 PREPARE COORD->BANK_B 1100->open SELLER"}));
    CHECK(timer_lines(rec) ==
          (Lines{"COORD t1 VOTE_TIMEOUT 0->never",
                 "BANK_A t1 DECISION_WAIT 2100->never"}));
    CHECK(glassbox::cli::in_flight(rec, 9'000'000'000).size() == 1);

    // A bound of exactly the event count still reports quiescence.
    const Recording exact =
        glassbox::cli::record(glassbox::make_s1_happy_path(1), 28);
    CHECK(exact.quiesced);
    CHECK(exact.frames.size() == 29);
    const Recording one_short =
        glassbox::cli::record(glassbox::make_s1_happy_path(1), 27);
    CHECK(!one_short.quiesced);
    CHECK(one_short.frames.size() == 28);
}

void test_timers_of_two_trades_fire_out_of_set_order()
{
    // Trade 1 loses its first COMMIT, so its second retransmit timer (2 s)
    // is set at 1,073,300. Trade 2, submitted at 1.5 s, sets its first
    // retransmit timer (1 s) later but fires earlier. Pairing must follow
    // the trade, not the set order.
    glassbox::ScenarioSetup setup = glassbox::make_s11_lost_commit(1);
    setup.script.push_back(glassbox::submit(
        1'500'000,
        glassbox::standard_trade(NodeId::bank_a, NodeId::bank_b, 1'500'000)));
    const Recording rec = glassbox::cli::record(setup);
    CHECK(rec.quiesced);
    check_timer_pairing(rec);
    const glassbox::cli::TimerSpan* t1 = nullptr;
    const glassbox::cli::TimerSpan* t2 = nullptr;
    for (const auto& t : rec.timers) {
        if (t.kind == "RETRANSMIT" && t.trade_id == TradeId{1} &&
            t.set_at_us == 1'073'300) {
            t1 = &t;
        }
        if (t.kind == "RETRANSMIT" && t.trade_id == TradeId{2} && !t2) {
            t2 = &t;
        }
    }
    CHECK(t1 != nullptr);
    CHECK(t2 != nullptr);
    CHECK(t1->fires_at_us == 3'073'400);
    CHECK(t2->set_at_us > t1->set_at_us);
    CHECK(*t2->fires_at_us < *t1->fires_at_us);
    CHECK(*t2->fires_at_us == t2->set_at_us + 1'001'100); // release +1.1 ms, then 1 s
}

void test_crash_while_message_in_flight()
{
    // BANK_B crashes while its PREPARE is on the wire: the drop happens on
    // arrival (MSG_DROP_NODE_DOWN, no reason field) and reads NODE_DOWN.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.script.push_back(glassbox::TimedAction{
        20'000, glassbox::ScriptActionEvent{
                    glassbox::ScriptCrash{NodeId::bank_b}}});
    // BANK_B never restarts, so retransmits never end: bound the run.
    const Recording rec = glassbox::cli::record(setup, 500);
    CHECK(!rec.quiesced);
    bool saw_kind = false;
    for (const auto& frame : rec.frames) {
        for (const auto& r : frame.records) {
            if (r.kind == "MSG_DROP_NODE_DOWN") {
                saw_kind = true;
                CHECK(!r.reason.has_value());
            }
        }
    }
    CHECK(saw_kind);
    CHECK(flight_lines(rec)[1] ==
          "#3 PREPARE COORD->BANK_B 1100->36100 SELLER DROP:NODE_DOWN");
}

void test_snapshot_counted_matches_backend()
{
    // After every event of every scenario, the incremental snapshot equals
    // the backend's snapshot(run).
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        glassbox::ScenarioRun run{info.make(1)};
        std::size_t seen = 0;
        std::size_t events = 0;
        do {
            for (std::size_t i = seen; i < run.trace().size(); ++i) {
                events += run.trace()[i].kind == "INVARIANT_VIOLATION" ? 1 : 0;
            }
            seen = run.trace().size();
            CHECK(glassbox::cli::snapshot_counted(run, events) ==
                  glassbox::snapshot(run));
        } while (run.simulator().step());
    }
    // The count is taken as given.
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(1)};
    CHECK(glassbox::cli::snapshot_counted(run, 3).invariant_events == 3);
}

void test_view_index()
{
    CHECK(glassbox::cli::view_index(NodeId::coordinator) == 0);
    CHECK(glassbox::cli::view_index(NodeId::bank_a) == 1);
    CHECK(glassbox::cli::view_index(NodeId::bank_b) == 2);
    CHECK(glassbox::cli::view_index(NodeId::bank_c) == 3);
}

void test_crash_view()
{
    // S6: right after BANK_A crashes it is down, its memory is wiped (empty
    // trades, genesis ledger) and its disk still holds PREPARED.
    const Recording rec =
        glassbox::cli::record(glassbox::make_s6_in_doubt_participant(1));
    const glassbox::cli::Frame* crash = nullptr;
    for (const auto& frame : rec.frames) {
        for (const auto& r : frame.records) {
            if (r.kind == "CRASH") {
                crash = &frame;
            }
        }
    }
    CHECK(crash != nullptr);
    CHECK(crash->time_us == 3'200);
    const auto& a = crash->nodes[1];
    CHECK(!a.up);
    CHECK(a.trades.empty());
    CHECK(a.disk == Lines{"PREPARED t1"});
    CHECK(a.balances == (glassbox::BalanceView{100'000'000, 0, 25'000, 0}));
    // After the restart the PREPARED record restored trade and lock.
    const auto& end = rec.frames.back().nodes[1];
    CHECK(end.up);
    CHECK(end.trades.at(TradeId{1}).state == TradeState::committed);
}

void test_frozen_flag()
{
    const Recording rec =
        glassbox::cli::record(glassbox::make_s3_account_frozen(1));
    CHECK(!rec.frames[0].nodes[1].frozen);
    CHECK(rec.frames[0].nodes[2].frozen);
}

} // namespace

int main()
{
    test_view_index();
    test_snapshot_counted_matches_backend();
    test_s1_frames_and_views();
    test_s1_flights_and_timers();
    test_in_flight_boundaries();
    test_duplicates_yield_one_flight_per_copy();
    test_drops_partition_and_script_actions();
    test_timers_pair_by_crash_epoch();
    test_bounded_recording();
    test_crash_view();
    test_frozen_flag();
    test_timers_of_two_trades_fire_out_of_set_order();
    test_crash_while_message_in_flight();
    test_trace_identical_to_a_plain_run();
    return 0;
}
