// Task 23: global invariant checker. Synthetic bad states are detected with
// exact kinds and details; valid commit/abort/in-flight states pass; the
// checker is side-effect free; every S1-S14 scenario passes it at
// quiescence and after every single event.

#include "scenarios/invariant_checker.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <limits>
#include <string>
#include <vector>

namespace {

using glassbox::BalanceView;
using glassbox::CheckMode;
using glassbox::InvariantKind;
using glassbox::InvariantViolation;
using glassbox::NodeId;
using glassbox::SystemSnapshot;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::TradeTerms;

constexpr std::int64_t k_cash = 1'000;
constexpr std::int64_t k_bonds = 100;
constexpr std::int64_t k_price = 300;
constexpr std::int64_t k_qty = 10;

// One trade: BANK_A buys k_qty from BANK_B for k_price.
[[nodiscard]] SystemSnapshot base()
{
    SystemSnapshot s;
    for (std::size_t i = 0; i < 3; ++i) {
        s.genesis[i] = BalanceView{k_cash, 0, k_bonds, 0};
        s.current[i] = s.genesis[i];
    }
    s.trades.push_back(
        TradeTerms{TradeId{1}, NodeId::bank_a, NodeId::bank_b, k_qty, k_price});
    return s;
}

void set_states(SystemSnapshot& s, TradeState a, TradeState b)
{
    if (a != TradeState::none) {
        s.states[0][TradeId{1}] = a;
    }
    if (b != TradeState::none) {
        s.states[1][TradeId{1}] = b;
    }
}

// Ledger values consistent with each party's state.
void apply_buyer(SystemSnapshot& s, TradeState state)
{
    if (state == TradeState::preparing || state == TradeState::prepared) {
        s.current[0].cash_locked += k_price;
    } else if (state == TradeState::committed) {
        s.current[0].cash_total -= k_price;
        s.current[0].bonds_total += k_qty;
    }
}

void apply_seller(SystemSnapshot& s, TradeState state)
{
    if (state == TradeState::preparing || state == TradeState::prepared) {
        s.current[1].bonds_locked += k_qty;
    } else if (state == TradeState::committed) {
        s.current[1].bonds_total -= k_qty;
        s.current[1].cash_total += k_price;
    }
}

[[nodiscard]] SystemSnapshot consistent(TradeState a, TradeState b)
{
    SystemSnapshot s = base();
    set_states(s, a, b);
    apply_buyer(s, a);
    apply_seller(s, b);
    return s;
}

[[nodiscard]] std::vector<std::string> kinds(
    const std::vector<InvariantViolation>& v)
{
    std::vector<std::string> out;
    for (const InvariantViolation& x : v) {
        out.push_back(std::string{glassbox::to_string(x.kind)} + ":" +
                      x.detail);
    }
    return out;
}

using Lines = std::vector<std::string>;

void test_valid_states_pass()
{
    // Both committed, both aborted, never started, refused before lock.
    for (const auto& [a, b] :
         {std::pair{TradeState::committed, TradeState::committed},
          std::pair{TradeState::aborted, TradeState::aborted},
          std::pair{TradeState::none, TradeState::none},
          std::pair{TradeState::aborted, TradeState::none}}) {
        const SystemSnapshot s = consistent(a, b);
        CHECK(glassbox::check_invariants(s, CheckMode::anytime).empty());
        CHECK(glassbox::check_invariants(s, CheckMode::quiescent).empty());
    }
    // In flight: fine at any time, not at quiescence.
    for (const auto& [a, b] :
         {std::pair{TradeState::committed, TradeState::prepared},
          std::pair{TradeState::prepared, TradeState::preparing},
          std::pair{TradeState::aborted, TradeState::prepared}}) {
        const SystemSnapshot s = consistent(a, b);
        CHECK(glassbox::check_invariants(s, CheckMode::anytime).empty());
        CHECK(!glassbox::check_invariants(s, CheckMode::quiescent).empty());
    }
}

void test_atomicity()
{
    // Split outcome: a violation in both modes.
    const SystemSnapshot split =
        consistent(TradeState::committed, TradeState::aborted);
    const auto anytime = glassbox::check_invariants(split, CheckMode::anytime);
    // Conservation is measured against the legs committed so far, so the
    // lone committed buyer leg is accounted for: only atomicity breaks.
    CHECK(kinds(anytime) ==
          Lines{"ATOMICITY:BANK_A COMMITTED, BANK_B ABORTED"});
    CHECK(kinds(glassbox::check_invariants(split, CheckMode::quiescent)) ==
          Lines{"ATOMICITY:BANK_A COMMITTED, BANK_B ABORTED"});
    CHECK(anytime.back().kind == InvariantKind::atomicity);
    CHECK(anytime.back().trade_id == TradeId{1});
    CHECK(anytime.back().node == std::nullopt);
    CHECK(anytime.back().detail == "BANK_A COMMITTED, BANK_B ABORTED");

    // Seller-side split too.
    const SystemSnapshot other =
        consistent(TradeState::aborted, TradeState::committed);
    CHECK(glassbox::check_invariants(other, CheckMode::anytime).back().detail ==
          "BANK_A ABORTED, BANK_B COMMITTED");

    // One-sided commit with the other in doubt: only at quiescence.
    const SystemSnapshot pending =
        consistent(TradeState::committed, TradeState::prepared);
    CHECK(kinds(glassbox::check_invariants(pending, CheckMode::quiescent)) ==
          (Lines{"ATOMICITY:BANK_A COMMITTED, BANK_B PREPARED",
                 "LEAKED_LOCK:cash 0 bonds 10"}));
}

void test_conservation_counts_committed_legs_and_locked_balances()
{
    // Only the buyer leg committed: totals move by exactly that leg.
    SystemSnapshot s = consistent(TradeState::committed, TradeState::prepared);
    CHECK(glassbox::check_invariants(s, CheckMode::anytime).empty());
    // One extra cent anywhere breaks it (in either direction).
    s.current[2].cash_total += 1;
    CHECK(kinds(glassbox::check_invariants(s, CheckMode::anytime)) ==
          Lines{"CONSERVATION:cash 2701 expected 2700"});
    s.current[2].cash_total -= 2;
    CHECK(kinds(glassbox::check_invariants(s, CheckMode::anytime)) ==
          Lines{"CONSERVATION:cash 2699 expected 2700"});
    // Bonds conservation is checked separately.
    SystemSnapshot b = consistent(TradeState::committed, TradeState::committed);
    b.current[1].bonds_total += 1;
    CHECK(kinds(glassbox::check_invariants(b, CheckMode::quiescent)) ==
          Lines{"CONSERVATION:bonds 301 expected 300"});
    // Locked amounts are part of totals: locking does not change totals.
    SystemSnapshot locked = consistent(TradeState::prepared, TradeState::prepared);
    CHECK(locked.current[0].cash_locked == k_price);
    CHECK(glassbox::check_invariants(locked, CheckMode::anytime).empty());
}

void test_bounds()
{
    SystemSnapshot s = consistent(TradeState::none, TradeState::none);
    s.current[2].bonds_total = -1;
    s.genesis[2].bonds_total = -1; // keep conservation out of the picture
    // A negative total also means locked (0) exceeds total (-1).
    CHECK(kinds(glassbox::check_invariants(s, CheckMode::anytime)) ==
          (Lines{"NEGATIVE_BALANCE:cash 1000/0 bonds -1/0",
                 "LOCKED_EXCEEDS_TOTAL:bonds locked 0 > total -1"}));
    CHECK(glassbox::check_invariants(s, CheckMode::anytime)[0].node ==
          NodeId::bank_c);

    // Negative locked amounts are negative balances too (and leave the
    // lock accounting off).
    SystemSnapshot neg_cash_lock = consistent(TradeState::none, TradeState::none);
    neg_cash_lock.current[0].cash_locked = -1;
    CHECK(kinds(glassbox::check_invariants(neg_cash_lock, CheckMode::anytime)) ==
          (Lines{"NEGATIVE_BALANCE:cash 1000/-1 bonds 100/0",
                 "LOCK_MISMATCH:cash locked -1 expected 0"}));
    SystemSnapshot neg_bond_lock = consistent(TradeState::none, TradeState::none);
    neg_bond_lock.current[1].bonds_locked = -3;
    CHECK(kinds(glassbox::check_invariants(neg_bond_lock, CheckMode::anytime)) ==
          (Lines{"NEGATIVE_BALANCE:cash 1000/0 bonds 100/-3",
                 "LOCK_MISMATCH:bonds locked -3 expected 0"}));

    // locked == total is fine; locked = total + 1 is not.
    SystemSnapshot edge = consistent(TradeState::prepared, TradeState::none);
    edge.current[0].cash_total = k_price;
    edge.genesis[0].cash_total = k_price;
    CHECK(glassbox::check_invariants(edge, CheckMode::anytime).empty());
    edge.current[0].cash_total = k_price - 1;
    edge.genesis[0].cash_total = k_price - 1;
    CHECK(kinds(glassbox::check_invariants(edge, CheckMode::anytime)) ==
          Lines{"LOCKED_EXCEEDS_TOTAL:cash locked 300 > total 299"});

    SystemSnapshot bonds = consistent(TradeState::none, TradeState::prepared);
    bonds.current[1].bonds_total = k_qty - 1;
    bonds.genesis[1].bonds_total = k_qty - 1;
    CHECK(kinds(glassbox::check_invariants(bonds, CheckMode::anytime)) ==
          Lines{"LOCKED_EXCEEDS_TOTAL:bonds locked 10 > total 9"});
}

void test_lock_accounting()
{
    // A lock with no trade holding it (leaked while running).
    SystemSnapshot stray = consistent(TradeState::aborted, TradeState::aborted);
    stray.current[1].bonds_locked = 4;
    CHECK(kinds(glassbox::check_invariants(stray, CheckMode::anytime)) ==
          Lines{"LOCK_MISMATCH:bonds locked 4 expected 0"});
    CHECK(kinds(glassbox::check_invariants(stray, CheckMode::quiescent)) ==
          (Lines{"LOCK_MISMATCH:bonds locked 4 expected 0",
                 "LEAKED_LOCK:cash 0 bonds 4"}));
    // A PREPARED trade whose lock is missing.
    SystemSnapshot missing = consistent(TradeState::prepared, TradeState::none);
    missing.current[0].cash_locked = 0;
    CHECK(kinds(glassbox::check_invariants(missing, CheckMode::anytime)) ==
          Lines{"LOCK_MISMATCH:cash locked 0 expected 300"});
    // PREPARING counts as holding a lock, COMMITTED/ABORTED do not.
    CHECK(glassbox::check_invariants(
              consistent(TradeState::preparing, TradeState::preparing),
              CheckMode::anytime)
              .empty());
    // Two trades locking at the same bank add up.
    SystemSnapshot two = base();
    two.trades.push_back(
        TradeTerms{TradeId{2}, NodeId::bank_a, NodeId::bank_c, 1, 50});
    two.states[0][TradeId{1}] = TradeState::prepared;
    two.states[0][TradeId{2}] = TradeState::prepared;
    two.current[0].cash_locked = k_price + 50;
    CHECK(glassbox::check_invariants(two, CheckMode::anytime).empty());
    two.current[0].cash_locked = k_price;
    CHECK(kinds(glassbox::check_invariants(two, CheckMode::anytime)) ==
          Lines{"LOCK_MISMATCH:cash locked 300 expected 350"});
}

void test_invariant_events_and_overflow()
{
    SystemSnapshot s = consistent(TradeState::committed, TradeState::committed);
    s.invariant_events = 2;
    CHECK(kinds(glassbox::check_invariants(s, CheckMode::anytime)) ==
          Lines{"INVARIANT_EVENT:2"});
    s.invariant_events = 0;
    CHECK(glassbox::check_invariants(s, CheckMode::anytime).empty());

    SystemSnapshot big = base();
    big.genesis[0].cash_total = std::numeric_limits<std::int64_t>::max();
    big.genesis[1].cash_total = 1;
    big.current = big.genesis;
    const auto v = glassbox::check_invariants(big, CheckMode::anytime);
    CHECK(kinds(v) == Lines{"ARITHMETIC_OVERFLOW:conservation"});
}

void test_multiple_violations_in_fixed_order()
{
    SystemSnapshot s = consistent(TradeState::committed, TradeState::aborted);
    s.current[2].cash_locked = 5;     // C: lock mismatch (+ leak)
    s.current[0].bonds_total = -100;  // A: negative
    s.invariant_events = 1;
    const auto v = glassbox::check_invariants(s, CheckMode::quiescent);
    // Fixed order: bank A bounds, bank C locks, conservation (cash is still
    // balanced: the committed buyer leg is accounted for; bonds are not),
    // atomicity, leaks, events.
    CHECK(kinds(v) ==
          (Lines{"NEGATIVE_BALANCE:cash 700/0 bonds -100/0",
                 "LOCKED_EXCEEDS_TOTAL:bonds locked 0 > total -100",
                 "LOCK_MISMATCH:cash locked 5 expected 0",
                 "CONSERVATION:bonds 100 expected 310",
                 "ATOMICITY:BANK_A COMMITTED, BANK_B ABORTED",
                 "LEAKED_LOCK:cash 5 bonds 0",
                 "INVARIANT_EVENT:1"}));
    CHECK(v[0].node == NodeId::bank_a);
    CHECK(v[1].node == NodeId::bank_a);
    CHECK(v[2].node == NodeId::bank_c);
    CHECK(v[3].node == std::nullopt);
    CHECK(v[4].trade_id == TradeId{1});
    CHECK(v[5].node == NodeId::bank_c);
}

void test_names()
{
    CHECK(glassbox::to_string(InvariantKind::negative_balance) ==
          "NEGATIVE_BALANCE");
    CHECK(glassbox::to_string(InvariantKind::locked_exceeds_total) ==
          "LOCKED_EXCEEDS_TOTAL");
    CHECK(glassbox::to_string(InvariantKind::lock_mismatch) ==
          "LOCK_MISMATCH");
    CHECK(glassbox::to_string(InvariantKind::conservation) == "CONSERVATION");
    CHECK(glassbox::to_string(InvariantKind::atomicity) == "ATOMICITY");
    CHECK(glassbox::to_string(InvariantKind::leaked_lock) == "LEAKED_LOCK");
    CHECK(glassbox::to_string(InvariantKind::invariant_event) ==
          "INVARIANT_EVENT");
    CHECK(glassbox::to_string(InvariantKind::arithmetic_overflow) ==
          "ARITHMETIC_OVERFLOW");
}

// --- Integration with real runs -------------------------------------------

void test_snapshot_reads_run_without_side_effects()
{
    glassbox::ScenarioRun run{glassbox::make_s1_happy_path(1)};
    CHECK(run.run());
    const std::size_t trace_size = run.trace().size();
    const SystemSnapshot first = glassbox::snapshot(run);
    const SystemSnapshot second = glassbox::snapshot(run);
    CHECK(first == second);
    CHECK(run.trace().size() == trace_size);
    static_cast<void>(glassbox::check_invariants(first, CheckMode::quiescent));
    CHECK(glassbox::snapshot(run) == first);

    CHECK(first.trades.size() == 1);
    CHECK(first.trades[0] == (TradeTerms{TradeId{1}, NodeId::bank_a,
                                         NodeId::bank_b, 25, 2'487'500}));
    CHECK(first.genesis[0] == (BalanceView{100'000'000, 0, 25'000, 0}));
    CHECK(first.current[0] ==
          (BalanceView{100'000'000 - 2'487'500, 0, 25'025, 0}));
    CHECK(first.states[0].at(TradeId{1}) == TradeState::committed);
    CHECK(first.states[1].at(TradeId{1}) == TradeState::committed);
    CHECK(first.states[2].empty());
    CHECK(first.invariant_events == 0);
}

void test_every_scenario_passes_at_quiescence()
{
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        for (std::uint64_t seed = 1; seed <= 3; ++seed) {
            glassbox::ScenarioRun run{info.make(seed)};
            CHECK(run.run());
            const auto violations = glassbox::check_invariants(
                glassbox::snapshot(run), CheckMode::quiescent);
            if (!violations.empty()) {
                std::fprintf(stderr, "%s seed %llu: %s %s\n",
                             std::string{info.id}.c_str(),
                             static_cast<unsigned long long>(seed),
                             std::string{glassbox::to_string(
                                             violations[0].kind)}
                                 .c_str(),
                             violations[0].detail.c_str());
            }
            CHECK(violations.empty());
        }
    }
}

void test_every_scenario_passes_after_every_event()
{
    // The anytime invariants hold between any two events, including while
    // nodes are crashed and trades are half-committed.
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        glassbox::ScenarioRun run{info.make(1)};
        std::size_t events = 0;
        while (run.simulator().step()) {
            ++events;
            CHECK(events < 1'000'000);
            const auto violations = glassbox::check_invariants(
                glassbox::snapshot(run), CheckMode::anytime);
            if (!violations.empty()) {
                std::fprintf(stderr, "%s event %zu: %s %s\n",
                             std::string{info.id}.c_str(), events,
                             std::string{glassbox::to_string(
                                             violations[0].kind)}
                                 .c_str(),
                             violations[0].detail.c_str());
            }
            CHECK(violations.empty());
        }
        CHECK(events > 20);
    }
}

void test_detects_a_real_atomicity_break()
{
    // S6 without the restart: BANK_A crashed after voting yes and never
    // comes back, so the system cannot reach quiescence. The coordinator and
    // BANK_B committed; BANK_A (wiped) did not. The checker reports it.
    glassbox::ScenarioSetup setup = glassbox::make_s6_in_doubt_participant(1);
    setup.crashes[0].restart_after_us = std::nullopt;
    glassbox::ScenarioRun run{setup};
    CHECK(!run.run(20'000)); // retransmits to the dead node never stop
    const SystemSnapshot s = glassbox::snapshot(run);
    CHECK(s.states[1].at(TradeId{1}) == TradeState::committed);
    CHECK(s.states[0].empty());
    const auto v = glassbox::check_invariants(s, CheckMode::quiescent);
    CHECK(kinds(v) == (Lines{"ATOMICITY:BANK_A NONE, BANK_B COMMITTED"}));
    // Between events this is a legal in-flight state (BANK_A may restart).
    CHECK(glassbox::check_invariants(s, CheckMode::anytime).empty());
}

} // namespace

int main()
{
    test_names();
    test_valid_states_pass();
    test_atomicity();
    test_conservation_counts_committed_legs_and_locked_balances();
    test_bounds();
    test_lock_accounting();
    test_invariant_events_and_overflow();
    test_multiple_violations_in_fixed_order();
    test_snapshot_reads_run_without_side_effects();
    test_every_scenario_passes_at_quiescence();
    test_every_scenario_passes_after_every_event();
    test_detects_a_real_atomicity_break();
    return 0;
}
