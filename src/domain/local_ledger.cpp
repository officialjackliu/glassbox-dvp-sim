#include "domain/local_ledger.hpp"

#include <type_traits>
#include <variant>

namespace glassbox {

LocalLedger::LocalLedger(Cents cash,
                         Units bonds,
                         Cents cash_locked,
                         Units bonds_locked,
                         bool frozen) noexcept
    : cash_total_(cash),
      bonds_total_(bonds),
      cash_locked_(cash_locked),
      bonds_locked_(bonds_locked),
      frozen_(frozen)
{
}

LedgerOutcome LocalLedger::genesis(Cents cash,
                                   Units bonds,
                                   bool frozen) noexcept
{
    if (cash.value() < 0 || bonds.value() < 0) {
        return LedgerOutcome{
            std::nullopt,
            LedgerStatus::invalid_amount,
        };
    }
    return LedgerOutcome{
        LocalLedger{
            cash,
            bonds,
            Cents{0},
            Units{0},
            frozen,
        },
        LedgerStatus::ok,
    };
}

Cents LocalLedger::cash_total() const noexcept
{
    return cash_total_;
}

Cents LocalLedger::cash_locked() const noexcept
{
    return cash_locked_;
}

Units LocalLedger::bonds_total() const noexcept
{
    return bonds_total_;
}

Units LocalLedger::bonds_locked() const noexcept
{
    return bonds_locked_;
}

bool LocalLedger::frozen() const noexcept
{
    return frozen_;
}

Cents LocalLedger::available_cash() const noexcept
{
    const auto available =
        checked_sub_i64(cash_total_.value(), cash_locked_.value());
    return Cents{available.value_or(0)};
}

Units LocalLedger::available_bonds() const noexcept
{
    const auto available =
        checked_sub_i64(bonds_total_.value(), bonds_locked_.value());
    return Units{available.value_or(0)};
}

LedgerStatus LocalLedger::lock_cash(Cents amount) noexcept
{
    if (amount.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (frozen_) {
        return LedgerStatus::account_frozen;
    }
    const auto available =
        checked_sub_i64(cash_total_.value(), cash_locked_.value());
    if (!available || *available < amount.value()) {
        return LedgerStatus::insufficient_funds;
    }
    const auto new_locked =
        checked_add_i64(cash_locked_.value(), amount.value());
    if (!new_locked || *new_locked < 0 ||
        *new_locked > cash_total_.value()) {
        return LedgerStatus::overflow;
    }
    cash_locked_ = Cents{*new_locked};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::lock_bonds(Units amount) noexcept
{
    if (amount.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (frozen_) {
        return LedgerStatus::account_frozen;
    }
    const auto available =
        checked_sub_i64(bonds_total_.value(), bonds_locked_.value());
    if (!available || *available < amount.value()) {
        return LedgerStatus::insufficient_bonds;
    }
    const auto new_locked =
        checked_add_i64(bonds_locked_.value(), amount.value());
    if (!new_locked || *new_locked < 0 ||
        *new_locked > bonds_total_.value()) {
        return LedgerStatus::overflow;
    }
    bonds_locked_ = Units{*new_locked};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::unlock_cash(Cents amount) noexcept
{
    if (amount.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (amount.value() > cash_locked_.value()) {
        return LedgerStatus::unlock_exceeds_locked;
    }
    const auto new_locked =
        checked_sub_i64(cash_locked_.value(), amount.value());
    if (!new_locked || *new_locked < 0) {
        return LedgerStatus::overflow;
    }
    cash_locked_ = Cents{*new_locked};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::unlock_bonds(Units amount) noexcept
{
    if (amount.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (amount.value() > bonds_locked_.value()) {
        return LedgerStatus::unlock_exceeds_locked;
    }
    const auto new_locked =
        checked_sub_i64(bonds_locked_.value(), amount.value());
    if (!new_locked || *new_locked < 0) {
        return LedgerStatus::overflow;
    }
    bonds_locked_ = Units{*new_locked};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::commit_buyer_leg(Cents cash,
                                           Units qty) noexcept
{
    if (cash.value() <= 0 || qty.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (cash.value() > cash_locked_.value()) {
        return LedgerStatus::unlock_exceeds_locked;
    }

    const auto new_cash_total =
        checked_sub_i64(cash_total_.value(), cash.value());
    const auto new_cash_locked =
        checked_sub_i64(cash_locked_.value(), cash.value());
    const auto new_bonds_total =
        checked_add_i64(bonds_total_.value(), qty.value());
    if (!new_cash_total || !new_cash_locked || !new_bonds_total) {
        return LedgerStatus::overflow;
    }
    if (*new_cash_total < 0 || *new_cash_locked < 0 ||
        *new_bonds_total < 0 ||
        *new_cash_locked > *new_cash_total) {
        return LedgerStatus::overflow;
    }

    cash_total_ = Cents{*new_cash_total};
    cash_locked_ = Cents{*new_cash_locked};
    bonds_total_ = Units{*new_bonds_total};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::commit_seller_leg(Units qty,
                                            Cents cash) noexcept
{
    if (qty.value() <= 0 || cash.value() <= 0) {
        return LedgerStatus::invalid_amount;
    }
    if (qty.value() > bonds_locked_.value()) {
        return LedgerStatus::unlock_exceeds_locked;
    }

    const auto new_bonds_total =
        checked_sub_i64(bonds_total_.value(), qty.value());
    const auto new_bonds_locked =
        checked_sub_i64(bonds_locked_.value(), qty.value());
    const auto new_cash_total =
        checked_add_i64(cash_total_.value(), cash.value());
    if (!new_bonds_total || !new_bonds_locked || !new_cash_total) {
        return LedgerStatus::overflow;
    }
    if (*new_bonds_total < 0 || *new_bonds_locked < 0 ||
        *new_cash_total < 0 ||
        *new_bonds_locked > *new_bonds_total) {
        return LedgerStatus::overflow;
    }

    bonds_total_ = Units{*new_bonds_total};
    bonds_locked_ = Units{*new_bonds_locked};
    cash_total_ = Cents{*new_cash_total};
    return LedgerStatus::ok;
}

LedgerStatus LocalLedger::set_frozen(bool frozen) noexcept
{
    frozen_ = frozen;
    return LedgerStatus::ok;
}

bool LocalLedger::check_invariants() const noexcept
{
    if (cash_total_.value() < 0 ||
        cash_locked_.value() < 0 ||
        bonds_total_.value() < 0 ||
        bonds_locked_.value() < 0) {
        return false;
    }
    return cash_locked_.value() <= cash_total_.value() &&
           bonds_locked_.value() <= bonds_total_.value();
}

LedgerStatus apply(LocalLedger& ledger,
                   const LedgerOp& op) noexcept
{
    return std::visit([&](const auto& operation) -> LedgerStatus {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, LockCash>) {
            return ledger.lock_cash(operation.amount);
        } else if constexpr (std::is_same_v<Operation, LockBonds>) {
            return ledger.lock_bonds(operation.amount);
        } else if constexpr (std::is_same_v<Operation, UnlockCash>) {
            return ledger.unlock_cash(operation.amount);
        } else if constexpr (std::is_same_v<Operation, UnlockBonds>) {
            return ledger.unlock_bonds(operation.amount);
        } else if constexpr (std::is_same_v<Operation, CommitBuyer>) {
            return ledger.commit_buyer_leg(
                operation.cash,
                operation.qty);
        } else if constexpr (std::is_same_v<Operation, CommitSeller>) {
            return ledger.commit_seller_leg(
                operation.qty,
                operation.cash);
        } else {
            return ledger.set_frozen(operation.frozen);
        }
    }, op);
}

std::optional<LedgerTotals> totals(
    std::span<const LocalLedger> ledgers) noexcept
{
    std::int64_t cash = 0;
    std::int64_t bonds = 0;
    for (const LocalLedger& ledger : ledgers) {
        const auto next_cash =
            checked_add_i64(cash, ledger.cash_total().value());
        const auto next_bonds =
            checked_add_i64(bonds, ledger.bonds_total().value());
        if (!next_cash || !next_bonds) {
            return std::nullopt;
        }
        cash = *next_cash;
        bonds = *next_bonds;
    }
    return LedgerTotals{
        Cents{cash},
        Units{bonds},
    };
}

} // namespace glassbox
