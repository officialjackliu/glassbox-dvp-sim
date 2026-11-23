#pragma once

// Pure account ledger used by one participant node.
//
// This header owns no trades or protocol state. It contains only balances,
// locks, frozen state, checked mutations, and replayable ledger operations.

#include "sim/core_types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace glassbox {

enum class LedgerStatus : std::uint8_t {
    ok,
    invalid_amount,
    insufficient_funds,
    insufficient_bonds,
    account_frozen,
    unlock_exceeds_locked,
    overflow,
};

struct LedgerOutcome;

class LocalLedger {
  public:
    [[nodiscard]] static LedgerOutcome genesis(
        Cents cash,
        Units bonds,
        bool frozen = false) noexcept;

    [[nodiscard]] Cents cash_total() const noexcept;
    [[nodiscard]] Cents cash_locked() const noexcept;
    [[nodiscard]] Units bonds_total() const noexcept;
    [[nodiscard]] Units bonds_locked() const noexcept;
    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] Cents available_cash() const noexcept;
    [[nodiscard]] Units available_bonds() const noexcept;

    [[nodiscard]] LedgerStatus lock_cash(Cents amount) noexcept;
    [[nodiscard]] LedgerStatus lock_bonds(Units amount) noexcept;
    [[nodiscard]] LedgerStatus unlock_cash(Cents amount) noexcept;
    [[nodiscard]] LedgerStatus unlock_bonds(Units amount) noexcept;
    [[nodiscard]] LedgerStatus commit_buyer_leg(Cents cash,
                                                Units qty) noexcept;
    [[nodiscard]] LedgerStatus commit_seller_leg(Units qty,
                                                 Cents cash) noexcept;
    [[nodiscard]] LedgerStatus set_frozen(bool frozen) noexcept;
    [[nodiscard]] bool check_invariants() const noexcept;

    [[nodiscard]] friend bool operator==(
        const LocalLedger&,
        const LocalLedger&) noexcept = default;

  private:
    LocalLedger(Cents cash,
                Units bonds,
                Cents cash_locked,
                Units bonds_locked,
                bool frozen) noexcept;

    Cents cash_total_;
    Units bonds_total_;
    Cents cash_locked_;
    Units bonds_locked_;
    bool frozen_;
};

struct LedgerOutcome {
    std::optional<LocalLedger> ledger;
    LedgerStatus status = LedgerStatus::ok;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return status == LedgerStatus::ok;
    }
};

struct LedgerTotals {
    Cents cash;
    Units bonds;

    [[nodiscard]] friend bool operator==(
        const LedgerTotals&,
        const LedgerTotals&) noexcept = default;
};

struct LockCash {
    Cents amount;
};

struct LockBonds {
    Units amount;
};

struct UnlockCash {
    Cents amount;
};

struct UnlockBonds {
    Units amount;
};

struct CommitBuyer {
    Cents cash;
    Units qty;
};

struct CommitSeller {
    Units qty;
    Cents cash;
};

struct SetFrozen {
    bool frozen;
};

using LedgerOp = std::variant<
    LockCash,
    LockBonds,
    UnlockCash,
    UnlockBonds,
    CommitBuyer,
    CommitSeller,
    SetFrozen>;

[[nodiscard]] LedgerStatus apply(
    LocalLedger& ledger,
    const LedgerOp& op) noexcept;

[[nodiscard]] std::optional<LedgerTotals> totals(
    std::span<const LocalLedger> ledgers) noexcept;

} // namespace glassbox
