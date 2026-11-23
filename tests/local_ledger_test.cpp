#include "domain/local_ledger.hpp"
#include "check.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <string>

int main()
{
    // Genesis and invalid genesis balances.
    {
        const auto valid =
            glassbox::LocalLedger::genesis(
                glassbox::Cents{1'000'000},
                glassbox::Units{100},
                false);
        CHECK(valid.ok());
        CHECK(valid.ledger.has_value());
        const glassbox::LocalLedger ledger =
            *valid.ledger;
        CHECK(ledger.cash_total().value() == 1'000'000);
        CHECK(ledger.cash_locked().value() == 0);
        CHECK(ledger.bonds_total().value() == 100);
        CHECK(ledger.bonds_locked().value() == 0);
        CHECK(!ledger.frozen());
        CHECK(ledger.check_invariants());

        const auto negative_cash =
            glassbox::LocalLedger::genesis(
                glassbox::Cents{-1},
                glassbox::Units{100});
        CHECK(!negative_cash.ok());
        CHECK(negative_cash.status ==
              glassbox::LedgerStatus::invalid_amount);

        const auto negative_bonds =
            glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{-1});
        CHECK(!negative_bonds.ok());
        CHECK(negative_bonds.status ==
              glassbox::LedgerStatus::invalid_amount);

        const auto frozen =
            glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10},
                true);
        CHECK(frozen.ok());
        CHECK(frozen.ledger->frozen());
    }

    // Exact lock, unlock, and commit arithmetic.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{1'000},
                glassbox::Units{100}).ledger;
        CHECK(ledger.lock_cash(glassbox::Cents{250}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.cash_locked().value() == 250);
        CHECK(ledger.available_cash().value() == 750);

        CHECK(ledger.lock_bonds(glassbox::Units{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.bonds_locked().value() == 40);
        CHECK(ledger.available_bonds().value() == 60);

        CHECK(ledger.unlock_cash(glassbox::Cents{250}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.cash_locked().value() == 0);
        CHECK(ledger.unlock_bonds(glassbox::Units{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.bonds_locked().value() == 0);

        CHECK(ledger.lock_cash(glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{100},
                  glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.cash_total().value() == 900);
        CHECK(ledger.cash_locked().value() == 0);
        CHECK(ledger.bonds_total().value() == 110);

        CHECK(ledger.lock_bonds(glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.commit_seller_leg(
                  glassbox::Units{10},
                  glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.bonds_total().value() == 100);
        CHECK(ledger.bonds_locked().value() == 0);
        CHECK(ledger.cash_total().value() == 1'000);
        CHECK(ledger.check_invariants());
    }

    // Sufficiency boundaries and amount validation.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(ledger.lock_cash(glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.lock_cash(glassbox::Cents{1}) ==
              glassbox::LedgerStatus::insufficient_funds);

        auto bonds =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(bonds.lock_bonds(glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(bonds.lock_bonds(glassbox::Units{1}) ==
              glassbox::LedgerStatus::insufficient_bonds);

        CHECK(ledger.lock_cash(glassbox::Cents{0}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger.lock_cash(glassbox::Cents{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.lock_bonds(glassbox::Units{0}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.lock_bonds(glassbox::Units{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger.unlock_cash(glassbox::Cents{0}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger.unlock_cash(glassbox::Cents{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.unlock_bonds(glassbox::Units{0}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.unlock_bonds(glassbox::Units{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{0},
                  glassbox::Units{1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{1},
                  glassbox::Units{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.commit_seller_leg(
                  glassbox::Units{0},
                  glassbox::Cents{1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(bonds.commit_seller_leg(
                  glassbox::Units{1},
                  glassbox::Cents{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
    }

    // Unlock exact locked and over-unlock.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(ledger.lock_cash(glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.unlock_cash(glassbox::Cents{101}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
        CHECK(ledger.unlock_cash(glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.cash_locked().value() == 0);

        CHECK(ledger.lock_bonds(glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.unlock_bonds(glassbox::Units{11}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
        CHECK(ledger.unlock_bonds(glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.bonds_locked().value() == 0);
    }

    // Frozen blocks only new locks.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(ledger.lock_cash(glassbox::Cents{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.lock_bonds(glassbox::Units{4}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.set_frozen(true) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.lock_cash(glassbox::Cents{1}) ==
              glassbox::LedgerStatus::account_frozen);
        CHECK(ledger.lock_bonds(glassbox::Units{1}) ==
              glassbox::LedgerStatus::account_frozen);
        CHECK(ledger.unlock_cash(glassbox::Cents{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.unlock_bonds(glassbox::Units{4}) ==
              glassbox::LedgerStatus::ok);

        CHECK(ledger.lock_cash(glassbox::Cents{40}) ==
              glassbox::LedgerStatus::account_frozen);
        CHECK(ledger.set_frozen(false) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.lock_cash(glassbox::Cents{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.set_frozen(true) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{40},
                  glassbox::Units{4}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.check_invariants());
    }

    // Seller frozen commit uses an existing bond lock.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(ledger.lock_bonds(glassbox::Units{4}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.set_frozen(true) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.commit_seller_leg(
                  glassbox::Units{4},
                  glassbox::Cents{40}) ==
              glassbox::LedgerStatus::ok);
        CHECK(ledger.bonds_locked().value() == 0);
        CHECK(ledger.check_invariants());
    }

    // Every failed mutating operation leaves the ledger unchanged.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        const glassbox::LocalLedger before = ledger;
        CHECK(ledger.lock_cash(glassbox::Cents{-1}) ==
              glassbox::LedgerStatus::invalid_amount);
        CHECK(ledger == before);
        CHECK(ledger.lock_bonds(glassbox::Units{11}) ==
              glassbox::LedgerStatus::insufficient_bonds);
        CHECK(ledger == before);
        CHECK(ledger.unlock_cash(glassbox::Cents{1}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
        CHECK(ledger == before);
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{1},
                  glassbox::Units{1}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
        CHECK(ledger == before);
    }

    // Credit overflow is atomic.
    {
        auto buyer_bond_overflow =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{
                    std::numeric_limits<std::int64_t>::max()}).ledger;
        CHECK(buyer_bond_overflow.lock_cash(glassbox::Cents{1}) ==
              glassbox::LedgerStatus::ok);
        const glassbox::LocalLedger buyer_before =
            buyer_bond_overflow;
        CHECK(buyer_bond_overflow.commit_buyer_leg(
                  glassbox::Cents{1},
                  glassbox::Units{1}) ==
              glassbox::LedgerStatus::overflow);
        CHECK(buyer_bond_overflow == buyer_before);

        auto seller_cash_overflow =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{
                    std::numeric_limits<std::int64_t>::max()},
                glassbox::Units{100}).ledger;
        CHECK(seller_cash_overflow.lock_bonds(glassbox::Units{1}) ==
              glassbox::LedgerStatus::ok);
        const glassbox::LocalLedger seller_before =
            seller_cash_overflow;
        CHECK(seller_cash_overflow.commit_seller_leg(
                  glassbox::Units{1},
                  glassbox::Cents{1}) ==
              glassbox::LedgerStatus::overflow);
        CHECK(seller_cash_overflow == seller_before);
    }

    // Commit debit cannot exceed the locked amount.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{10}).ledger;
        CHECK(ledger.commit_buyer_leg(
                  glassbox::Cents{1},
                  glassbox::Units{1}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
        CHECK(ledger.commit_seller_leg(
                  glassbox::Units{1},
                  glassbox::Cents{1}) ==
              glassbox::LedgerStatus::unlock_exceeds_locked);
    }

    // Randomized invariant sequence.
    {
        auto ledger =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{10'000},
                glassbox::Units{10'000}).ledger;
        std::mt19937_64 rng{42};
        for (int i = 0; i < 10'000; ++i) {
            const std::uint64_t operation =
                rng() % 7;
            glassbox::LedgerStatus status =
                glassbox::LedgerStatus::ok;
            if (operation == 0) {
                status = ledger.lock_cash(
                    glassbox::Cents{
                        static_cast<std::int64_t>(rng() % 500)});
            } else if (operation == 1) {
                status = ledger.lock_bonds(
                    glassbox::Units{
                        static_cast<std::int64_t>(rng() % 500)});
            } else if (operation == 2) {
                status = ledger.unlock_cash(
                    glassbox::Cents{
                        static_cast<std::int64_t>(rng() % 500)});
            } else if (operation == 3) {
                status = ledger.unlock_bonds(
                    glassbox::Units{
                        static_cast<std::int64_t>(rng() % 500)});
            } else if (operation == 4) {
                status = ledger.commit_buyer_leg(
                    glassbox::Cents{
                        static_cast<std::int64_t>(rng() % 500)},
                    glassbox::Units{
                        static_cast<std::int64_t>(rng() % 500)});
            } else if (operation == 5) {
                status = ledger.commit_seller_leg(
                    glassbox::Units{
                        static_cast<std::int64_t>(rng() % 500)},
                    glassbox::Cents{
                        static_cast<std::int64_t>(rng() % 500)});
            } else {
                status = ledger.set_frozen(
                    (rng() % 2) != 0);
            }
            static_cast<void>(status);
            CHECK(ledger.check_invariants());
        }
    }

    // Buyer/seller conservation.
    {
        auto buyer =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{1'000},
                glassbox::Units{20}).ledger;
        auto seller =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{30}).ledger;

        std::array<glassbox::LocalLedger, 2> before{
            buyer,
            seller,
        };
        const auto totals_before =
            glassbox::totals(before);
        CHECK(totals_before.has_value());

        CHECK(buyer.lock_cash(glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);
        CHECK(seller.lock_bonds(glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(buyer.commit_buyer_leg(
                  glassbox::Cents{100},
                  glassbox::Units{10}) ==
              glassbox::LedgerStatus::ok);
        CHECK(seller.commit_seller_leg(
                  glassbox::Units{10},
                  glassbox::Cents{100}) ==
              glassbox::LedgerStatus::ok);

        std::array<glassbox::LocalLedger, 2> after{
            buyer,
            seller,
        };
        const auto totals_after =
            glassbox::totals(after);
        CHECK(totals_after.has_value());
        CHECK(totals_before == totals_after);
        CHECK(buyer.cash_total().value() == 900);
        CHECK(buyer.bonds_total().value() == 30);
        CHECK(seller.cash_total().value() == 200);
        CHECK(seller.bonds_total().value() == 20);
    }

    // totals overflow.
    {
        std::array<glassbox::LocalLedger, 2> ledgers{
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{
                    std::numeric_limits<std::int64_t>::max()},
                glassbox::Units{1}).ledger,
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{1},
                glassbox::Units{1}).ledger,
        };
        CHECK(!glassbox::totals(ledgers).has_value());
    }

    // Replay and lock/unlock equality.
    {
        const auto genesis =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{500},
                glassbox::Units{50}).ledger;
        auto replay_a = genesis;
        auto replay_b = genesis;

        glassbox::LedgerOp first =
            glassbox::LockCash{glassbox::Cents{100}};
        glassbox::LedgerOp second =
            glassbox::UnlockCash{glassbox::Cents{100}};
        CHECK(glassbox::apply(replay_a, first) ==
              glassbox::LedgerStatus::ok);
        CHECK(glassbox::apply(replay_b, first) ==
              glassbox::LedgerStatus::ok);
        CHECK(glassbox::apply(replay_a, second) ==
              glassbox::LedgerStatus::ok);
        CHECK(glassbox::apply(replay_b, second) ==
              glassbox::LedgerStatus::ok);
        CHECK(replay_a == replay_b);
        CHECK(replay_a == genesis);

        glassbox::LedgerOp frozen =
            glassbox::SetFrozen{true};
        glassbox::LedgerOp lock =
            glassbox::LockBonds{glassbox::Units{5}};
        CHECK(glassbox::apply(replay_a, frozen) ==
              glassbox::LedgerStatus::ok);
        CHECK(glassbox::apply(replay_b, frozen) ==
              glassbox::LedgerStatus::ok);
        CHECK(glassbox::apply(replay_a, lock) ==
              glassbox::LedgerStatus::account_frozen);
        CHECK(glassbox::apply(replay_b, lock) ==
              glassbox::LedgerStatus::account_frozen);
        CHECK(replay_a == replay_b);
    }

    // Static grep: ledger source has no float or double.
    {
        std::ifstream header{
            "include/domain/local_ledger.hpp"};
        std::ifstream source{
            "src/domain/local_ledger.cpp"};
        CHECK(header.is_open());
        CHECK(source.is_open());
        std::string line;
        while (std::getline(header, line)) {
            CHECK(line.find("float") == std::string::npos);
            CHECK(line.find("double") == std::string::npos);
        }
        while (std::getline(source, line)) {
            CHECK(line.find("float") == std::string::npos);
            CHECK(line.find("double") == std::string::npos);
        }
    }

    return 0;
}
