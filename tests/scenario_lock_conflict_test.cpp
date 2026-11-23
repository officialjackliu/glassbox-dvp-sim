// Task 18: S13 lock conflict. Two trades contend for one seller's bonds;
// exactly one commits and the other aborts with INSUFFICIENT_BONDS. The
// winner is deterministic (submission order) and the losing trade's
// buyer releases its own lock.

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
using scenario_checks::log_kinds;
using scenario_checks::notes;
using scenario_checks::sends;
using Lines = std::vector<std::string>;

constexpr std::int64_t k_price = 2'487'500;

[[nodiscard]] std::vector<std::string> decisions(
    const glassbox::ScenarioRun& run)
{
    std::vector<std::string> out;
    for (const glassbox::LogEntry& e :
         run.simulator().durable_log(NodeId::coordinator)) {
        if (const auto* c = std::get_if<glassbox::DecisionCommit>(&e.record)) {
            out.push_back(std::to_string(c->trade_id.value()) + " COMMIT");
        }
        if (const auto* a = std::get_if<glassbox::DecisionAbort>(&e.record)) {
            out.push_back(std::to_string(a->trade_id.value()) + " ABORT " +
                          std::string{glassbox::to_string(a->reason)});
        }
    }
    return out;
}

void test_s13_first_submitted_wins()
{
    glassbox::ScenarioRun run{glassbox::make_s13_lock_conflict(1)};

    // Both PREPAREs reach BANK_B at 36,100; right after, one lock is held.
    scenario_checks::run_until(run, 36'101);
    const glassbox::LocalLedger& seller = run.nodes().bank_b->ledger();
    CHECK(seller.bonds_locked() == Units{25});
    CHECK(seller.available_bonds() == Units{15});
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) ==
          TradeState::preparing);
    CHECK(run.nodes().bank_b->trade_state(TradeId{2}) == TradeState::aborted);

    CHECK(run.run());
    CHECK(notes(run, "LOCK", NodeId::bank_b) == Lines{"36100 BONDS 25"});
    CHECK(notes(run, "VOTE_NO", NodeId::bank_b) ==
          Lines{"36100 INSUFFICIENT_BONDS"});
    CHECK(sends(run, NodeId::bank_b, MessageType::vote) ==
          (Lines{"37200 YES ->COORD", "37200 NO:INSUFFICIENT_BONDS ->COORD"}));
    CHECK(decisions(run) ==
          (Lines{"1 COMMIT", "2 ABORT INSUFFICIENT_BONDS"}));

    // Seller log: PREPARED(1), tombstone(2), COMMITTED(1), in LSN order.
    CHECK(log_kinds(run, NodeId::bank_b) ==
          (Lines{"PREPARED", "ABORTED", "COMMITTED"}));
    const auto b_log = run.simulator().durable_log(NodeId::bank_b);
    CHECK(glassbox::record_trade_id(b_log[0].record) == TradeId{1});
    CHECK(glassbox::record_trade_id(b_log[1].record) == TradeId{2});
    CHECK(glassbox::record_trade_id(b_log[2].record) == TradeId{1});

    // The loser's buyer (BANK_C) locked cash and released it on ABORT.
    CHECK(notes(run, "LOCK", NodeId::bank_c) == Lines{"121100 CASH 2487500"});
    CHECK(notes(run, "UNLOCK", NodeId::bank_c) ==
          Lines{"193300 CASH 2487500"});

    const glassbox::ProtocolNodes& nodes = run.nodes();
    CHECK(nodes.bank_a->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(nodes.bank_b->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(nodes.bank_b->trade_state(TradeId{2}) == TradeState::aborted);
    CHECK(nodes.bank_c->trade_state(TradeId{2}) == TradeState::aborted);
    CHECK(nodes.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK(nodes.coordinator->trade_state(TradeId{2}) == TradeState::done);

    CHECK(nodes.bank_b->ledger().bonds_total() == Units{40 - 25});
    CHECK(nodes.bank_b->ledger().cash_total() ==
          Cents{100'000'000 + k_price});
    CHECK(nodes.bank_a->ledger().bonds_total() == Units{25'000 + 25});
    CHECK(nodes.bank_a->ledger().cash_total() ==
          Cents{100'000'000 - k_price});
    CHECK(nodes.bank_c->ledger() == run.genesis().bank_c);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s13_reversed_submission_flips_winner()
{
    glassbox::ScenarioSetup setup = glassbox::make_s13_lock_conflict(1);
    std::swap(setup.script[0], setup.script[1]);
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    // Trade 1 is now BANK_C's purchase, and it wins. Decisions are logged
    // in time order: trade 2 aborts at 72,200 (BANK_B's no), trade 1 can
    // only commit once BANK_C's yes arrives over the 120 ms link (242,200).
    CHECK(decisions(run) ==
          (Lines{"2 ABORT INSUFFICIENT_BONDS", "1 COMMIT"}));
    std::vector<glassbox::SimTimeUs> decision_times;
    for (const glassbox::LogEntry& e :
         run.simulator().durable_log(NodeId::coordinator)) {
        if (std::holds_alternative<glassbox::DecisionAbort>(e.record) ||
            std::holds_alternative<glassbox::DecisionCommit>(e.record)) {
            decision_times.push_back(e.durable_at_us);
        }
    }
    CHECK(decision_times ==
          (std::vector<glassbox::SimTimeUs>{73'200, 243'200}));
    CHECK(run.nodes().bank_c->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_a->trade_state(TradeId{2}) == TradeState::aborted);
    CHECK(run.nodes().bank_c->ledger().bonds_total() == Units{25'000 + 25});
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    scenario_checks::check_quiescent_invariants(run);
}

[[nodiscard]] int commits_with_seller_bonds(std::int64_t bonds)
{
    glassbox::ScenarioSetup setup = glassbox::make_s13_lock_conflict(1);
    setup.genesis.bank_b = glassbox::make_ledger(100'000'000, bonds);
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    scenario_checks::check_quiescent_invariants(run);
    int commits = 0;
    for (TradeId id : {TradeId{1}, TradeId{2}}) {
        commits += run.nodes().bank_b->trade_state(id) ==
                           TradeState::committed
                       ? 1
                       : 0;
    }
    return commits;
}

void test_s13_capacity_boundaries()
{
    CHECK(commits_with_seller_bonds(24) == 0);
    CHECK(commits_with_seller_bonds(25) == 1);
    CHECK(commits_with_seller_bonds(49) == 1);
    CHECK(commits_with_seller_bonds(50) == 2);
}

void test_s13_deterministic()
{
    CHECK(glassbox::find_scenario("S13")->make ==
          &glassbox::make_s13_lock_conflict);
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        glassbox::ScenarioRun run{glassbox::make_s13_lock_conflict(seed)};
        CHECK(run.run());
        CHECK(decisions(run) ==
              (Lines{"1 COMMIT", "2 ABORT INSUFFICIENT_BONDS"}));
    }
}

} // namespace

int main()
{
    test_s13_first_submitted_wins();
    test_s13_reversed_submission_flips_winner();
    test_s13_capacity_boundaries();
    test_s13_deterministic();
    return 0;
}
