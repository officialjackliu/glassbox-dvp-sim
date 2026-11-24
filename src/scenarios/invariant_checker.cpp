// Global invariant checks over a SystemSnapshot of plain integers, with
// checked arithmetic, and snapshot() which reads one from a ScenarioRun.

#include "scenarios/invariant_checker.hpp"

#include <variant>

namespace glassbox {

namespace {

constexpr std::array<NodeId, 3> k_banks{NodeId::bank_a, NodeId::bank_b,
                                        NodeId::bank_c};

[[nodiscard]] std::optional<std::size_t> bank_index(NodeId node) noexcept
{
    switch (node) {
    case NodeId::bank_a:
        return 0;
    case NodeId::bank_b:
        return 1;
    case NodeId::bank_c:
        return 2;
    case NodeId::coordinator:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] TradeState state_of(const SystemSnapshot& s,
                                  NodeId node,
                                  TradeId trade_id)
{
    const auto index = bank_index(node);
    if (!index) {
        return TradeState::none;
    }
    const auto& states = s.states[*index];
    const auto it = states.find(trade_id);
    return it == states.end() ? TradeState::none : it->second;
}

[[nodiscard]] bool holds_lock(TradeState state) noexcept
{
    return state == TradeState::preparing || state == TradeState::prepared;
}

// Accumulates with checked arithmetic; records overflow once.
class Sum {
  public:
    explicit Sum(std::int64_t start) : value_(start) {}
    void add(std::int64_t delta) noexcept
    {
        if (!value_) {
            return;
        }
        value_ = checked_add_i64(*value_, delta);
    }
    [[nodiscard]] const std::optional<std::int64_t>& value() const noexcept
    {
        return value_;
    }

  private:
    std::optional<std::int64_t> value_;
};

[[nodiscard]] std::string pair_text(std::int64_t a, std::int64_t b)
{
    return std::to_string(a) + " expected " + std::to_string(b);
}

void check_bank(const SystemSnapshot& s,
                std::size_t index,
                std::vector<InvariantViolation>& out)
{
    const NodeId node = k_banks[index];
    const BalanceView& b = s.current[index];
    if (b.cash_total < 0 || b.cash_locked < 0 || b.bonds_total < 0 ||
        b.bonds_locked < 0) {
        out.push_back({InvariantKind::negative_balance, node, std::nullopt,
                       "cash " + std::to_string(b.cash_total) + "/" +
                           std::to_string(b.cash_locked) + " bonds " +
                           std::to_string(b.bonds_total) + "/" +
                           std::to_string(b.bonds_locked)});
    }
    if (b.cash_locked > b.cash_total) {
        out.push_back({InvariantKind::locked_exceeds_total, node,
                       std::nullopt,
                       "cash locked " + std::to_string(b.cash_locked) +
                           " > total " + std::to_string(b.cash_total)});
    }
    if (b.bonds_locked > b.bonds_total) {
        out.push_back({InvariantKind::locked_exceeds_total, node,
                       std::nullopt,
                       "bonds locked " + std::to_string(b.bonds_locked) +
                           " > total " + std::to_string(b.bonds_total)});
    }

    Sum cash_locked{0};
    Sum bonds_locked{0};
    for (const TradeTerms& t : s.trades) {
        if (t.buyer == node && holds_lock(state_of(s, node, t.trade_id))) {
            cash_locked.add(t.cash_cents);
        }
        if (t.seller == node && holds_lock(state_of(s, node, t.trade_id))) {
            bonds_locked.add(t.qty);
        }
    }
    if (!cash_locked.value() || !bonds_locked.value()) {
        out.push_back({InvariantKind::arithmetic_overflow, node, std::nullopt,
                       "lock accounting"});
        return;
    }
    if (*cash_locked.value() != b.cash_locked) {
        out.push_back({InvariantKind::lock_mismatch, node, std::nullopt,
                       "cash locked " +
                           pair_text(b.cash_locked, *cash_locked.value())});
    }
    if (*bonds_locked.value() != b.bonds_locked) {
        out.push_back({InvariantKind::lock_mismatch, node, std::nullopt,
                       "bonds locked " +
                           pair_text(b.bonds_locked, *bonds_locked.value())});
    }
}

void check_conservation(const SystemSnapshot& s,
                        std::vector<InvariantViolation>& out)
{
    Sum expected_cash{0};
    Sum expected_bonds{0};
    Sum actual_cash{0};
    Sum actual_bonds{0};
    for (std::size_t i = 0; i < k_banks.size(); ++i) {
        expected_cash.add(s.genesis[i].cash_total);
        expected_bonds.add(s.genesis[i].bonds_total);
        actual_cash.add(s.current[i].cash_total);
        actual_bonds.add(s.current[i].bonds_total);
    }
    // Legs committed so far. Totals include locked amounts.
    for (const TradeTerms& t : s.trades) {
        if (state_of(s, t.buyer, t.trade_id) == TradeState::committed) {
            expected_cash.add(-t.cash_cents);
            expected_bonds.add(t.qty);
        }
        if (state_of(s, t.seller, t.trade_id) == TradeState::committed) {
            expected_cash.add(t.cash_cents);
            expected_bonds.add(-t.qty);
        }
    }
    if (!expected_cash.value() || !expected_bonds.value() ||
        !actual_cash.value() || !actual_bonds.value()) {
        out.push_back({InvariantKind::arithmetic_overflow, std::nullopt,
                       std::nullopt, "conservation"});
        return;
    }
    if (*actual_cash.value() != *expected_cash.value()) {
        out.push_back({InvariantKind::conservation, std::nullopt, std::nullopt,
                       "cash " + pair_text(*actual_cash.value(),
                                           *expected_cash.value())});
    }
    if (*actual_bonds.value() != *expected_bonds.value()) {
        out.push_back({InvariantKind::conservation, std::nullopt, std::nullopt,
                       "bonds " + pair_text(*actual_bonds.value(),
                                            *expected_bonds.value())});
    }
}

void check_atomicity(const SystemSnapshot& s,
                     CheckMode mode,
                     std::vector<InvariantViolation>& out)
{
    std::map<TradeId, const TradeTerms*> ordered;
    for (const TradeTerms& t : s.trades) {
        ordered.emplace(t.trade_id, &t);
    }
    for (const auto& [trade_id, t] : ordered) {
        const TradeState buyer = state_of(s, t->buyer, trade_id);
        const TradeState seller = state_of(s, t->seller, trade_id);
        const bool buyer_committed = buyer == TradeState::committed;
        const bool seller_committed = seller == TradeState::committed;
        const bool split =
            (buyer_committed && seller == TradeState::aborted) ||
            (seller_committed && buyer == TradeState::aborted);
        const bool one_sided = buyer_committed != seller_committed;
        if (split || (mode == CheckMode::quiescent && one_sided)) {
            out.push_back(
                {InvariantKind::atomicity, std::nullopt, trade_id,
                 std::string{to_string(t->buyer)} + " " +
                     std::string{to_string(buyer)} + ", " +
                     std::string{to_string(t->seller)} + " " +
                     std::string{to_string(seller)}});
        }
    }
}

} // namespace

BalanceView balance_view(const LocalLedger& ledger) noexcept
{
    return BalanceView{
        ledger.cash_total().value(),
        ledger.cash_locked().value(),
        ledger.bonds_total().value(),
        ledger.bonds_locked().value(),
    };
}

SystemSnapshot snapshot(const ScenarioRun& run)
{
    SystemSnapshot s;
    const BankGenesis& g = run.genesis();
    s.genesis = {balance_view(g.bank_a), balance_view(g.bank_b),
                 balance_view(g.bank_c)};
    const auto banks = run.nodes().banks();
    for (std::size_t i = 0; i < banks.size(); ++i) {
        s.current[i] = balance_view(banks[i]->ledger());
        for (const auto& [id, trade] : banks[i]->trades()) {
            s.states[i].emplace(id, trade.state);
        }
    }
    for (const LogEntry& entry :
         run.simulator().durable_log(NodeId::coordinator)) {
        if (const auto* p = std::get_if<Proposed>(&entry.record)) {
            s.trades.push_back(TradeTerms{
                p->trade.trade_id(),
                p->trade.buyer(),
                p->trade.seller(),
                p->trade.qty().value(),
                p->trade.cash_cents().value(),
            });
        }
    }
    for (const TraceRecord& r : run.trace()) {
        s.invariant_events += r.kind == "INVARIANT_VIOLATION" ? 1 : 0;
    }
    return s;
}

std::vector<InvariantViolation> check_invariants(const SystemSnapshot& s,
                                                 CheckMode mode)
{
    std::vector<InvariantViolation> out;
    for (std::size_t i = 0; i < k_banks.size(); ++i) {
        check_bank(s, i, out);
    }
    check_conservation(s, out);
    check_atomicity(s, mode, out);
    if (mode == CheckMode::quiescent) {
        for (std::size_t i = 0; i < k_banks.size(); ++i) {
            const BalanceView& b = s.current[i];
            if (b.cash_locked != 0 || b.bonds_locked != 0) {
                out.push_back({InvariantKind::leaked_lock, k_banks[i],
                               std::nullopt,
                               "cash " + std::to_string(b.cash_locked) +
                                   " bonds " +
                                   std::to_string(b.bonds_locked)});
            }
        }
    }
    if (s.invariant_events > 0) {
        out.push_back({InvariantKind::invariant_event, std::nullopt,
                       std::nullopt, std::to_string(s.invariant_events)});
    }
    return out;
}

} // namespace glassbox
