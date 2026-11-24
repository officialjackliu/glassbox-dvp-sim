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

// Result of a ledger change: ok, or why it was refused. A refused change
// leaves the ledger exactly as it was.
enum class LedgerStatus : std::uint8_t {
    ok,
    invalid_amount,
    insufficient_funds,
    insufficient_bonds,
    account_frozen,
    unlock_exceeds_locked,
    overflow,
};

// Defined below; returned by LocalLedger::genesis().
struct LedgerOutcome;

// One bank's balances: total cash and bonds, the part of each that is locked
// for pending trades, and whether the account is frozen. Totals include locked
// amounts; "available" is total minus locked. Frozen blocks new locks only;
// unlocking and settling an existing lock still work.
//
//   auto ledger = *LocalLedger::genesis(Cents{100'000'000}, Units{25'000}).ledger;
//   if (ledger.lock_cash(Cents{2'487'500}) == LedgerStatus::ok) { ... }
class LocalLedger {
  public:
    // Returns a ledger with these starting totals and no locks, or
    // invalid_amount if `cash` or `bonds` is negative.
    [[nodiscard]] static LedgerOutcome genesis(
        Cents cash,
        Units bonds,
        bool frozen = false) noexcept;

    // Balances. available_* is total minus locked.
    [[nodiscard]] Cents cash_total() const noexcept;
    [[nodiscard]] Cents cash_locked() const noexcept;
    [[nodiscard]] Units bonds_total() const noexcept;
    [[nodiscard]] Units bonds_locked() const noexcept;
    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] Cents available_cash() const noexcept;
    [[nodiscard]] Units available_bonds() const noexcept;

    // Locks part of the available balance for a pending trade. Fails with
    // invalid_amount (not positive), account_frozen, insufficient_funds /
    // insufficient_bonds (more than available) or overflow.
    [[nodiscard]] LedgerStatus lock_cash(Cents amount) noexcept;
    [[nodiscard]] LedgerStatus lock_bonds(Units amount) noexcept;
    // Releases a lock (the trade aborted). Fails with invalid_amount or
    // unlock_exceeds_locked.
    [[nodiscard]] LedgerStatus unlock_cash(Cents amount) noexcept;
    [[nodiscard]] LedgerStatus unlock_bonds(Units amount) noexcept;
    // Settles the buyer's leg: pays `cash` out of the locked cash and adds `qty`
    // bonds.
    [[nodiscard]] LedgerStatus commit_buyer_leg(Cents cash,
                                                Units qty) noexcept;
    // Settles the seller's leg: delivers `qty` of the locked bonds and adds
    // `cash`.
    [[nodiscard]] LedgerStatus commit_seller_leg(Units qty,
                                                 Cents cash) noexcept;
    // Freezes or unfreezes the account. Always ok.
    [[nodiscard]] LedgerStatus set_frozen(bool frozen) noexcept;
    // Returns true if no balance is negative and no lock exceeds its total.
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

// A new ledger, or the status explaining why none was created.
struct LedgerOutcome {
    std::optional<LocalLedger> ledger;
    LedgerStatus status = LedgerStatus::ok;

    // Returns true if `ledger` holds a value.
    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return status == LedgerStatus::ok;
    }
};

// Cash and bonds summed over several ledgers (for conservation checks).
struct LedgerTotals {
    Cents cash;
    Units bonds;

    [[nodiscard]] friend bool operator==(
        const LedgerTotals&,
        const LedgerTotals&) noexcept = default;
};

// Ledger operations: one struct per kind of change. apply() is the only way
// to change a ledger, both live and during crash-recovery replay, so the two
// always agree.
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

// Any one ledger operation.
using LedgerOp = std::variant<
    LockCash,
    LockBonds,
    UnlockCash,
    UnlockBonds,
    CommitBuyer,
    CommitSeller,
    SetFrozen>;

// Applies `op` to `ledger` through the matching member function and returns
// its status.
[[nodiscard]] LedgerStatus apply(
    LocalLedger& ledger,
    const LedgerOp& op) noexcept;

// Returns the cash and bond totals of `ledgers` added up, or std::nullopt if
// the sum overflows.
[[nodiscard]] std::optional<LedgerTotals> totals(
    std::span<const LocalLedger> ledgers) noexcept;

} // namespace glassbox
