// Glass-box player: the end card's numbers are exact, derived from the
// recorded events, and the card shows them faithfully.

#include "cli/screen.hpp"
#include "cli/summary.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::Decision;
using glassbox::NodeId;
using glassbox::ReasonCode;
using glassbox::TradeId;
using glassbox::cli::RunSummary;
using glassbox::cli::summarize;
using glassbox::cli::TradeOutcome;
using Lines = std::vector<std::string>;

[[nodiscard]] RunSummary of(const glassbox::ScenarioSetup& setup,
                            std::size_t max_events = 100'000)
{
    return summarize(glassbox::cli::record(setup, max_events));
}

void test_s1()
{
    // Locks: A 2,100 -> 74,300, B 36,100 -> 108,300. In doubt: PREPARED on
    // disk (3,100 / 37,100) to COMMITTED (74,300 / 108,300).
    const RunSummary s = of(glassbox::make_s1_happy_path(1));
    CHECK(s.trades == (std::vector<TradeOutcome>{
                          {TradeId{1}, NodeId::bank_a, NodeId::bank_b, 25,
                           Cents{2'487'500}, Decision::commit, ReasonCode::none,
                           108'300}}));
    CHECK(s.lock_hold_us == (std::array<glassbox::SimTimeUs, 3>{72'200, 72'200, 0}));
    CHECK(s.longest_in_doubt_us == 71'200);
    CHECK(s.sent == 8);
    CHECK(s.dropped == 0);
    CHECK(s.retransmits == 0);
    CHECK(s.checks_run == 28);
    CHECK(s.violations == 0);
}

void test_s2_refusal()
{
    const RunSummary s = of(glassbox::make_s2_buyer_lacks_funds(1));
    CHECK(s.trades.size() == 1);
    CHECK(s.trades[0].decision == Decision::abort);
    CHECK(s.trades[0].reason == ReasonCode::insufficient_funds);
    CHECK(s.trades[0].settled_after_us == 40'300);
    CHECK(s.lock_hold_us == (std::array<glassbox::SimTimeUs, 3>{0, 4'200, 0}));
    CHECK(s.longest_in_doubt_us == 3'200);
    CHECK(s.sent == 10);
    CHECK(s.checks_run == 30);
}

void test_s6_crash_spans_the_outage()
{
    // BANK_A's lock is on disk, so it lasts through the 5 s outage.
    const RunSummary s = of(glassbox::make_s6_in_doubt_participant(1));
    CHECK(s.trades[0].decision == Decision::commit);
    CHECK(s.trades[0].settled_after_us == 5'005'400);
    CHECK(s.lock_hold_us == (std::array<glassbox::SimTimeUs, 3>{5'003'300, 72'200, 0}));
    CHECK(s.longest_in_doubt_us == 5'002'300);
    CHECK(s.sent == 12);
    CHECK(s.dropped == 3);
    CHECK(s.retransmits == 2);
    CHECK(s.checks_run == 40);
    CHECK(s.violations == 0);
}

void test_s13_two_trades()
{
    const RunSummary s = of(glassbox::make_s13_lock_conflict(1));
    CHECK(s.trades.size() == 2);
    CHECK(s.trades[0].trade_id == TradeId{1});
    CHECK(s.trades[0].decision == Decision::commit);
    CHECK(s.trades[0].settled_after_us == 108'300);
    CHECK(s.trades[1].trade_id == TradeId{2});
    CHECK(s.trades[1].buyer == NodeId::bank_c);
    CHECK(s.trades[1].decision == Decision::abort);
    CHECK(s.trades[1].reason == ReasonCode::insufficient_bonds);
    CHECK(s.trades[1].settled_after_us == 193'300);
    // B locks only for trade 1; C locks 121,100 -> 193,300 for trade 2.
    CHECK(s.lock_hold_us == (std::array<glassbox::SimTimeUs, 3>{72'200, 72'200, 72'200}));
    CHECK(s.longest_in_doubt_us == 71'200);
    CHECK(s.sent == 18);
}

void test_lock_lost_in_a_crash_before_disk()
{
    // BANK_B locks at 36,100 and crashes at 36,600, before PREPARED is on
    // disk (37,100): the lock lasted 500 us, it is not restored.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.script.push_back(glassbox::TimedAction{
        36'600, glassbox::ScriptActionEvent{glassbox::ScriptCrash{NodeId::bank_b}}});
    setup.script.push_back(glassbox::TimedAction{
        200'000,
        glassbox::ScriptActionEvent{glassbox::ScriptRestart{NodeId::bank_b}}});
    const RunSummary s = of(setup);
    CHECK(s.lock_hold_us[1] == 500);
    CHECK(s.trades[0].decision == Decision::abort);
    CHECK(s.trades[0].reason == ReasonCode::vote_timeout);
    // BANK_A's durable lock is unaffected by BANK_B's crash.
    CHECK(s.lock_hold_us[0] > 0);
}

void test_retransmit_counts_each_target()
{
    // Both COMMITs lost and the banks wait 2 s before asking (as in S11),
    // so the first retransmit round goes to both banks.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.config.timers.decision_wait = glassbox::BackoffPolicy{2'000'000, 8'000'000};
    for (const NodeId bank : {NodeId::bank_a, NodeId::bank_b}) {
        setup.script.push_back(glassbox::TimedAction{
            0, glassbox::ScriptActionEvent{glassbox::ScriptDropNext{
                   NodeId::coordinator, bank, glassbox::MessageType::commit}}});
    }
    const auto rec = glassbox::cli::record(setup);
    bool both = false;
    for (const auto& frame : rec.frames) {
        for (const auto& r : frame.records) {
            both = both || (r.kind == "RETRANSMIT" && r.detail == "BANK_A,BANK_B");
        }
    }
    CHECK(both);
    const RunSummary s = summarize(rec);
    CHECK(s.retransmits == 2);
    CHECK(s.dropped == 2);
    CHECK(s.trades[0].decision == Decision::commit);
}

void test_unfinished_run()
{
    // Stopped after 6 events (t = 3,100): no decision yet, BANK_A's lock
    // and doubt are still open and count to the last event.
    const RunSummary s = of(glassbox::make_s1_happy_path(1), 6);
    CHECK(s.trades.empty());
    CHECK(s.lock_hold_us == (std::array<glassbox::SimTimeUs, 3>{1'000, 0, 0}));
    CHECK(s.longest_in_doubt_us == 0);
    CHECK(s.checks_run == 6);
    CHECK(s.sent == 2);
}

void test_other_banks_and_later_trades()
{
    // BANK_A crashes at 36,600 (its lock is durable); BANK_B's lock, not
    // yet on disk at that moment, must not end with BANK_A's crash.
    glassbox::ScenarioSetup a_crash = glassbox::make_s1_happy_path(1);
    a_crash.script.push_back(glassbox::TimedAction{
        36'600, glassbox::ScriptActionEvent{glassbox::ScriptCrash{NodeId::bank_a}}});
    a_crash.script.push_back(glassbox::TimedAction{
        200'000,
        glassbox::ScriptActionEvent{glassbox::ScriptRestart{NodeId::bank_a}}});
    CHECK(of(a_crash).lock_hold_us[1] == 72'200);

    // A second trade at 6 s, after S6's long doubt: the longest doubt is
    // still S6's, and the late trade is timed from its own submission.
    glassbox::ScenarioSetup later = glassbox::make_s6_in_doubt_participant(1);
    later.script.push_back(glassbox::submit(
        6'000'000,
        glassbox::standard_trade(NodeId::bank_a, NodeId::bank_b, 6'000'000)));
    const RunSummary s = of(later);
    CHECK(s.trades.size() == 2);
    CHECK(s.longest_in_doubt_us == 5'002'300);
    CHECK(s.trades[1].settled_after_us == 108'300);
}

void test_open_at_the_end_and_node_down_drops()
{
    // Stopped at 4,200 (8 events): BANK_A in doubt since 3,100.
    const RunSummary open = of(glassbox::make_s1_happy_path(1), 8);
    CHECK(open.longest_in_doubt_us == 1'100);
    CHECK(open.lock_hold_us[0] == 2'100);

    // A crash while a PREPARE is in flight drops it as MSG_DROP_NODE_DOWN;
    // every drop kind counts.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.script.push_back(glassbox::TimedAction{
        20'000, glassbox::ScriptActionEvent{glassbox::ScriptCrash{NodeId::bank_b}}});
    setup.script.push_back(glassbox::TimedAction{
        2'000'000,
        glassbox::ScriptActionEvent{glassbox::ScriptRestart{NodeId::bank_b}}});
    const auto rec = glassbox::cli::record(setup);
    int drops = 0;
    int node_down_kind = 0;
    for (const auto& frame : rec.frames) {
        for (const auto& r : frame.records) {
            drops += r.kind.starts_with("MSG_DROP") ? 1 : 0;
            node_down_kind += r.kind == "MSG_DROP_NODE_DOWN" ? 1 : 0;
        }
    }
    CHECK(node_down_kind == 1);
    CHECK(drops > node_down_kind);
    CHECK(summarize(rec).dropped == drops);
}

void test_undecided_trade_before_a_decided_one()
{
    // Trade 1 (BANK_A buys from BANK_C in Singapore) is still collecting
    // votes when trade 2 (BANK_B, with no cash) has already been refused.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.script.clear();
    setup.genesis.bank_b = glassbox::make_ledger(0, 25'000);
    setup.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_a, NodeId::bank_c, 0)));
    setup.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_b, NodeId::bank_a, 0)));
    const auto full = glassbox::cli::record(setup);
    std::size_t events = 0;
    while (full.frames[events].time_us < 100'000) {
        ++events;
    }
    const RunSummary s = of(setup, events);
    CHECK(s.trades.size() == 1);
    CHECK(s.trades[0].trade_id == TradeId{2});
    CHECK(s.trades[0].reason == ReasonCode::insufficient_funds);
}

void test_faults_injected_into_a_recording()
{
    // No correct run produces these, so inject them into a copy.
    glassbox::cli::Recording rec =
        glassbox::cli::record(glassbox::make_s1_happy_path(1));
    rec.frames[3].violations.push_back(glassbox::InvariantViolation{
        glassbox::InvariantKind::conservation, std::nullopt, std::nullopt,
        "test"});
    rec.frames[4].violations = rec.frames[3].violations;
    CHECK(summarize(rec).violations == 2);
    rec.frames.back().nodes[2].trades.at(TradeId{1}).state =
        glassbox::TradeState::aborted;
    const Lines card =
        glassbox::cli::draw_summary(rec, *glassbox::cli::find_guide("S1"), {100, false});
    const auto has = [&](const std::string& part) {
        for (const std::string& l : card) {
            if (l.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("28 events, invariants checked after each: 2 problems ✗"));
    CHECK(has("the banks disagree on a trade ✗"));
    CHECK(!has("atomic ✓"));
}

void test_end_card()
{
    const auto rec = glassbox::cli::record(glassbox::make_s2_buyer_lacks_funds(1));
    Lines got;
    for (std::string line : glassbox::cli::draw_summary(
             rec, *glassbox::cli::find_guide("S2"), {100, false})) {
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        got.push_back(line);
    }
    const Lines expected{
        "  S2 · Buyer lacks funds · finished",
        "",
        "  Outcome",
        "    #1  CANCELLED (insufficient funds): nothing moved at BANK_A or BANK_B",
        "        finished 40.3 ms after it was submitted",
        "",
        "  Balances, start → end",
        "    BANK_A  cash $24,874.99 (unchanged)   bonds 25,000 (unchanged)",
        "    BANK_B  cash $1,000,000.00 (unchanged)   bonds 25,000 (unchanged)",
        "",
        "  Timing (simulated)",
        "    assets locked: BANK_A 0 µs · BANK_B 4.2 ms",
        "    longest wait in doubt (voted YES, outcome unknown): 3.2 ms",
        "",
        "  Network",
        "    10 messages sent · 0 lost · 0 resent by the coordinator",
        "",
        "  Safety",
        "    30 events, invariants checked after each: 0 problems ✓",
        "    both banks agree on every trade: atomic ✓",
    };
    if (got != expected) {
        for (const std::string& l : got) {
            std::fprintf(stderr, "%s|\n", l.c_str());
        }
    }
    CHECK(got == expected);

    // A settled trade names both legs; balances show the move.
    const auto s1 = glassbox::cli::record(glassbox::make_s1_happy_path(1));
    const Lines card =
        glassbox::cli::draw_summary(s1, *glassbox::cli::find_guide("S1"), {140, false});
    const auto has = [&](const std::string& part) {
        for (const std::string& l : card) {
            if (l.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("#1  SETTLED: BANK_A paid $24,875.00 and received 25 UST 10Y "
              "bonds; BANK_B delivered them and received $24,875.00"));
    CHECK(has("BANK_A  cash $1,000,000.00 → $975,125.00   bonds 25,000 → 25,025"));
    CHECK(has("BANK_B  cash $1,000,000.00 → $1,024,875.00   bonds 25,000 → 24,975"));
    CHECK(has("assets locked: BANK_A 72.2 ms · BANK_B 72.2 ms"));

    // Several trades: plural heading.
    const auto s13 = glassbox::cli::record(glassbox::make_s13_lock_conflict(1));
    const Lines two =
        glassbox::cli::draw_summary(s13, *glassbox::cli::find_guide("S13"), {100, false});
    CHECK(two[2].starts_with("  Outcomes "));

    // An unfinished recording says so.
    const auto partial = glassbox::cli::record(glassbox::make_s1_happy_path(1), 6);
    const Lines stopped =
        glassbox::cli::draw_summary(partial, *glassbox::cli::find_guide("S1"),
                                    {100, false});
    CHECK(stopped[0].starts_with("  S1 · Happy path: both legs settle · "
                                 "stopped before the end"));
    CHECK(stopped[2].starts_with("  Outcome"));
    CHECK(stopped[3].starts_with("    no trade was decided"));
}

void test_cards_fit()
{
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        const auto rec = glassbox::cli::record(info.make(1));
        for (const int width : {80, 100, 200}) {
            for (const bool color : {false, true}) {
                for (const std::string& l : glassbox::cli::draw_summary(
                         rec, *glassbox::cli::find_guide(info.id),
                         {width, color})) {
                    CHECK(glassbox::cli::visible_width(l) ==
                          static_cast<std::size_t>(width));
                }
            }
        }
        CHECK(summarize(rec).violations == 0);
    }
}

} // namespace

int main()
{
    test_s1();
    test_s2_refusal();
    test_s6_crash_spans_the_outage();
    test_s13_two_trades();
    test_lock_lost_in_a_crash_before_disk();
    test_retransmit_counts_each_target();
    test_unfinished_run();
    test_other_banks_and_later_trades();
    test_open_at_the_end_and_node_down_drops();
    test_undecided_trade_before_a_decided_one();
    test_faults_injected_into_a_recording();
    test_end_card();
    test_cards_fit();
    return 0;
}
