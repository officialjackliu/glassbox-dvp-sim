// Task 16: S2 (buyer lacks funds) and S3 (seller account frozen).
// Each vote-no path must release the other party's lock, write a tombstone
// for the refusing party, move no balance, and survive a restart.

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
using Lines = std::vector<std::string>;

[[nodiscard]] ReasonCode decision_reason(const glassbox::ScenarioRun& run)
{
    const auto log = run.simulator().durable_log(NodeId::coordinator);
    CHECK(log.size() >= 2);
    return std::get<glassbox::DecisionAbort>(log[1].record).reason;
}

void check_no_balance_moved(const glassbox::ScenarioRun& run)
{
    CHECK(run.nodes().bank_a->ledger() == run.genesis().bank_a);
    CHECK(run.nodes().bank_b->ledger() == run.genesis().bank_b);
    CHECK(run.nodes().bank_c->ledger() == run.genesis().bank_c);
}

// Crash and restart both parties after quiescence: replaying the logs must
// reproduce exactly the same trades and ledgers (tombstones included).
void check_restart_reproduces_state(std::uint64_t seed,
                                    glassbox::ScenarioSetup (*make)(
                                        std::uint64_t))
{
    glassbox::ScenarioSetup setup = make(seed);
    for (NodeId bank : {NodeId::bank_a, NodeId::bank_b}) {
        setup.script.push_back({20'000'000,
                                glassbox::ScriptActionEvent{
                                    glassbox::ScriptCrash{bank}}});
        setup.script.push_back({20'000'001,
                                glassbox::ScriptActionEvent{
                                    glassbox::ScriptRestart{bank}}});
    }
    glassbox::ScenarioRun run{setup};
    scenario_checks::run_until(run, 20'000'000);
    const auto a_trades = run.nodes().bank_a->trades();
    const auto b_trades = run.nodes().bank_b->trades();
    const glassbox::LocalLedger a_ledger = run.nodes().bank_a->ledger();
    const glassbox::LocalLedger b_ledger = run.nodes().bank_b->ledger();
    CHECK(run.run());
    CHECK(count(run, "RESTART") == 2);
    CHECK(count(run, "RECOVERY_ERROR") == 0);
    // Aborted trades are not in doubt: no query after restart.
    CHECK(count(run, "MSG_SEND", MessageType::outcome_query) == 0);
    CHECK(run.nodes().bank_a->ledger() == a_ledger);
    CHECK(run.nodes().bank_b->ledger() == b_ledger);
    // Restored entries keep the state; the trade body is not needed for a
    // finished trade, so compare states only.
    CHECK(run.nodes().bank_a->trades().size() == a_trades.size());
    CHECK(run.nodes().bank_b->trades().size() == b_trades.size());
    for (const auto& [id, before] : a_trades) {
        CHECK(run.nodes().bank_a->trade_state(id) == before.state);
    }
    for (const auto& [id, before] : b_trades) {
        CHECK(run.nodes().bank_b->trade_state(id) == before.state);
    }
    scenario_checks::check_quiescent_invariants(run);
}

void test_s2_buyer_lacks_funds()
{
    glassbox::ScenarioRun run{glassbox::make_s2_buyer_lacks_funds(1)};
    CHECK(run.run());

    // Buyer refuses immediately; its no reaches the coordinator at 4,200,
    // before BANK_B even sees PREPARE (36,100).
    CHECK(sends(run, NodeId::bank_a, MessageType::vote) ==
          Lines{"3200 NO:INSUFFICIENT_FUNDS ->COORD"});
    CHECK(notes(run, "VOTE_NO", NodeId::bank_a) ==
          Lines{"2100 INSUFFICIENT_FUNDS"});
    CHECK(decision_reason(run) == ReasonCode::insufficient_funds);
    CHECK(log_kinds(run, NodeId::bank_a) == Lines{"ABORTED"});

    // Seller had already locked when ABORT arrived, then released.
    CHECK(notes(run, "LOCK", NodeId::bank_b) == Lines{"36100 BONDS 25"});
    CHECK(notes(run, "UNLOCK", NodeId::bank_b) == Lines{"40300 BONDS 25"});
    CHECK(log_kinds(run, NodeId::bank_b) == (Lines{"PREPARED", "ABORTED"}));
    // Every ABORT carries the coherent reason: two broadcast copies plus
    // one reply to BANK_B's late yes (arrives 72,200, before its ACK).
    CHECK(sends(run, NodeId::coordinator, MessageType::abort) ==
          (Lines{"5300 INSUFFICIENT_FUNDS ->BANK_A",
                 "5300 INSUFFICIENT_FUNDS ->BANK_B",
                 "72300 INSUFFICIENT_FUNDS ->BANK_B"}));
    CHECK(sends(run, NodeId::bank_b, MessageType::vote) ==
          Lines{"37200 YES ->COORD"});

    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(log_kinds(run, NodeId::coordinator) ==
          (Lines{"PROPOSED", "DECISION_ABORT", "END"}));
    CHECK(count(run, "MSG_SEND", MessageType::commit) == 0);
    check_no_balance_moved(run);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s3_account_frozen()
{
    glassbox::ScenarioRun run{glassbox::make_s3_account_frozen(1)};
    CHECK(run.run());
    CHECK(run.genesis().bank_b.frozen());

    CHECK(sends(run, NodeId::bank_b, MessageType::vote) ==
          Lines{"37200 NO:ACCOUNT_FROZEN ->COORD"});
    CHECK(notes(run, "VOTE_NO", NodeId::bank_b) ==
          Lines{"36100 ACCOUNT_FROZEN"});
    CHECK(notes(run, "LOCK", NodeId::bank_b).empty());
    CHECK(log_kinds(run, NodeId::bank_b) == Lines{"ABORTED"});
    CHECK(decision_reason(run) == ReasonCode::account_frozen);

    // Buyer locked at 2,100 and voted yes; ABORT (released 73,300) reaches
    // it at 74,300 and the lock is released.
    CHECK(notes(run, "LOCK", NodeId::bank_a) == Lines{"2100 CASH 2487500"});
    CHECK(notes(run, "UNLOCK", NodeId::bank_a) ==
          Lines{"74300 CASH 2487500"});
    CHECK(log_kinds(run, NodeId::bank_a) == (Lines{"PREPARED", "ABORTED"}));
    CHECK(sends(run, NodeId::coordinator, MessageType::abort) ==
          (Lines{"73300 ACCOUNT_FROZEN ->BANK_A",
                 "73300 ACCOUNT_FROZEN ->BANK_B"}));
    CHECK(sends(run, NodeId::bank_a, MessageType::ack) ==
          Lines{"75400 ABORT ->COORD"});

    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::aborted);
    check_no_balance_moved(run);
    scenario_checks::check_quiescent_invariants(run);
}

void test_registry_entries()
{
    const glassbox::ScenarioInfo* s2 = glassbox::find_scenario("S2");
    const glassbox::ScenarioInfo* s3 = glassbox::find_scenario("S3");
    CHECK(s2 && s2->make == &glassbox::make_s2_buyer_lacks_funds);
    CHECK(s3 && s3->make == &glassbox::make_s3_account_frozen);
    CHECK(s2->name == "Buyer lacks funds");
    CHECK(s3->name == "Account frozen");
    // Only the intended change versus the default setup.
    const glassbox::ScenarioSetup s2_setup = s2->make(1);
    CHECK(s2_setup.genesis.bank_a.cash_total() ==
          glassbox::Cents{2'487'500 - 1});
    CHECK(s2_setup.genesis.bank_b == glassbox::default_genesis().bank_b);
    const glassbox::ScenarioSetup s3_setup = s3->make(1);
    CHECK(s3_setup.genesis.bank_a == glassbox::default_genesis().bank_a);
    CHECK(s3_setup.genesis.bank_b.cash_total() ==
          glassbox::Cents{100'000'000});
    CHECK(!s3_setup.genesis.bank_a.frozen());
}

void test_buyer_with_exact_funds_commits()
{
    // The other side of the S2 boundary: exactly the price is enough.
    glassbox::ScenarioSetup setup = glassbox::make_s2_buyer_lacks_funds(1);
    setup.genesis.bank_a = glassbox::make_ledger(2'487'500, 25'000);
    glassbox::ScenarioRun run{setup};
    CHECK(run.run());
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_a->ledger().cash_total() == glassbox::Cents{0});
    scenario_checks::check_quiescent_invariants(run);
}

} // namespace

int main()
{
    test_registry_entries();
    test_s2_buyer_lacks_funds();
    test_s3_account_frozen();
    test_buyer_with_exact_funds_commits();
    for (std::uint64_t seed : {1u, 2u, 99u}) {
        check_restart_reproduces_state(seed,
                                       &glassbox::make_s2_buyer_lacks_funds);
        check_restart_reproduces_state(seed,
                                       &glassbox::make_s3_account_frozen);
    }
    return 0;
}
