#pragma once

// Shared assertions for scenario tests (DESIGN.md section 8 invariants
// 1-5). Task 23 adds the reusable InvariantChecker; these helpers keep the
// scenario tests readable.

#include "scenarios/scenario.hpp"
#include "check.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace scenario_checks {

[[nodiscard]] inline int count(const glassbox::ScenarioRun& run,
                               const std::string& kind,
                               std::optional<glassbox::MessageType> type =
                                   std::nullopt,
                               std::optional<glassbox::NodeId> node =
                                   std::nullopt)
{
    int n = 0;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == kind && (!type || r.msg_type == type) &&
            (!node || r.node == node)) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] inline std::vector<std::string> log_kinds(
    const glassbox::ScenarioRun& run,
    glassbox::NodeId node)
{
    std::vector<std::string> kinds;
    for (const glassbox::LogEntry& entry : run.simulator().durable_log(node)) {
        kinds.emplace_back(glassbox::record_kind(entry.record));
    }
    return kinds;
}

[[nodiscard]] inline std::vector<glassbox::SimTimeUs> times(
    const glassbox::ScenarioRun& run,
    const std::string& kind,
    glassbox::NodeId node,
    glassbox::MessageType type)
{
    std::vector<glassbox::SimTimeUs> out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == kind && r.node == node && r.msg_type == type) {
            out.push_back(r.sim_time_us);
        }
    }
    return out;
}

// Invariants 2-5 at quiescence: conservation including locked balances,
// no negative balances / locked <= total, no leaked locks, no
// INVARIANT_VIOLATION events. Invariant 1 (atomicity) over every trade the
// coordinator ever proposed.
inline void check_quiescent_invariants(const glassbox::ScenarioRun& run)
{
    const glassbox::BankGenesis& g = run.genesis();
    const std::array<glassbox::LocalLedger, 3> before{g.bank_a, g.bank_b,
                                                      g.bank_c};
    const glassbox::ProtocolNodes& nodes = run.nodes();
    const std::array<glassbox::LocalLedger, 3> after{
        nodes.bank_a->ledger(), nodes.bank_b->ledger(),
        nodes.bank_c->ledger()};
    CHECK(glassbox::totals(before).has_value());
    CHECK(glassbox::totals(before) == glassbox::totals(after));
    for (const glassbox::LocalLedger& l : after) {
        CHECK(l.check_invariants());
        CHECK(l.cash_total().value() >= 0);
        CHECK(l.bonds_total().value() >= 0);
        CHECK(l.cash_locked() == glassbox::Cents{0});
        CHECK(l.bonds_locked() == glassbox::Units{0});
    }
    CHECK(count(run, "INVARIANT_VIOLATION") == 0);
    for (const glassbox::LogEntry& entry :
         run.simulator().durable_log(glassbox::NodeId::coordinator)) {
        const auto* proposed = std::get_if<glassbox::Proposed>(&entry.record);
        if (!proposed) {
            continue;
        }
        const glassbox::Trade& trade = proposed->trade;
        const bool buyer_committed =
            nodes.bank(trade.buyer())->trade_state(trade.trade_id()) ==
            glassbox::TradeState::committed;
        const bool seller_committed =
            nodes.bank(trade.seller())->trade_state(trade.trade_id()) ==
            glassbox::TradeState::committed;
        CHECK(buyer_committed == seller_committed);
    }
}

} // namespace scenario_checks
