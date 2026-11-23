#include "domain/replay.hpp"
#include "check.hpp"

#include <array>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace {

glassbox::Trade make_trade(glassbox::TradeId id,
                           glassbox::NodeId buyer,
                           glassbox::NodeId seller)
{
    glassbox::TradeIdAllocator allocator;
    if (id.value() > 1) {
        allocator.observe(glassbox::TradeId{id.value() - 1});
    }
    glassbox::TradeRequest request{
        buyer,
        seller,
        "UST_10Y",
        glassbox::Units{10},
        glassbox::Cents{100},
        100'000,
        "test",
    };
    return std::get<glassbox::Trade>(
        glassbox::validate_and_assign(request, 0, allocator));
}

std::vector<glassbox::LogEntry> append_entries(
    const std::vector<glassbox::LogRecord>& records)
{
    std::vector<glassbox::LogEntry> entries;
    for (std::size_t i = 0; i < records.size(); ++i) {
        entries.push_back(
            glassbox::LogEntry{
                i + 1,
                static_cast<glassbox::SimTimeUs>((i + 1) * 100),
                records[i],
            });
    }
    return entries;
}

} // namespace

int main()
{
    auto genesis =
        *glassbox::LocalLedger::genesis(
            glassbox::Cents{1'000},
            glassbox::Units{100}).ledger;

    // Buyer Prepared restores a cash lock and in-doubt state.
    {
        const auto trade =
            make_trade(glassbox::TradeId{1},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
            });
        const auto replay = glassbox::replay_participant(
            genesis,
            glassbox::NodeId::bank_a,
            entries);
        CHECK(replay.errors.empty());
        CHECK(replay.ledger.cash_locked().value() == 100);
        CHECK(replay.states.at(glassbox::TradeId{1}) ==
              glassbox::TradeState::prepared);
        CHECK(replay.in_doubt.size() == 1);
    }

    // Seller Prepared restores a bond lock.
    {
        const auto trade =
            make_trade(glassbox::TradeId{2},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::seller},
            });
        const auto replay = glassbox::replay_participant(
            genesis,
            glassbox::NodeId::bank_b,
            entries);
        CHECK(replay.errors.empty());
        CHECK(replay.ledger.bonds_locked().value() == 10);
        CHECK(replay.in_doubt.size() == 1);
    }

    // Prepared + Committed buyer gives exact balances and zero lock.
    {
        const auto trade =
            make_trade(glassbox::TradeId{3},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::buyer,
                },
            });
        const auto replay = glassbox::replay_participant(
            genesis,
            glassbox::NodeId::bank_a,
            entries);
        CHECK(replay.errors.empty());
        CHECK(replay.ledger.cash_total().value() == 900);
        CHECK(replay.ledger.bonds_total().value() == 110);
        CHECK(replay.ledger.cash_locked().value() == 0);
        CHECK(replay.in_doubt.empty());
    }

    // Prepared + Aborted returns to genesis for this leg.
    {
        const auto trade =
            make_trade(glassbox::TradeId{4},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Aborted{trade.trade_id()},
            });
        const auto replay = glassbox::replay_participant(
            genesis,
            glassbox::NodeId::bank_a,
            entries);
        CHECK(replay.errors.empty());
        CHECK(replay.ledger == genesis);
        CHECK(replay.states.at(trade.trade_id()) ==
              glassbox::TradeState::aborted);
    }

    // Tombstone only.
    {
        const auto entries =
            append_entries({
                glassbox::Aborted{glassbox::TradeId{5}},
            });
        const auto replay = glassbox::replay_participant(
            genesis,
            glassbox::NodeId::bank_a,
            entries);
        CHECK(replay.errors.empty());
        CHECK(replay.ledger == genesis);
        CHECK(replay.states.at(glassbox::TradeId{5}) ==
              glassbox::TradeState::aborted);
    }

    // ReplayError cases.
    {
        const auto trade =
            make_trade(glassbox::TradeId{6},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);

        const auto committed_without =
            append_entries({
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::buyer,
                },
            });
        CHECK(!glassbox::replay_participant(
                  genesis,
                  glassbox::NodeId::bank_a,
                  committed_without).errors.empty());

        const auto prepared_twice =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Prepared{trade, glassbox::Leg::buyer},
            });
        const auto twice =
            glassbox::replay_participant(
                genesis,
                glassbox::NodeId::bank_a,
                prepared_twice);
        CHECK(!twice.errors.empty());

        const auto wrong_party =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::seller},
            });
        CHECK(!glassbox::replay_participant(
                  genesis,
                  glassbox::NodeId::bank_a,
                  wrong_party).errors.empty());

        const auto coordinator =
            append_entries({
                glassbox::Proposed{trade},
            });
        CHECK(!glassbox::replay_participant(
                  genesis,
                  glassbox::NodeId::bank_a,
                  coordinator).errors.empty());
    }

    // Coordinator replay commit, abort, end, undecided and errors.
    {
        const auto trade1 =
            make_trade(glassbox::TradeId{10},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Proposed{trade1},
                glassbox::DecisionCommit{
                    trade1.trade_id(),
                    trade1.buyer(),
                    trade1.seller(),
                },
                glassbox::End{trade1.trade_id()},
            });
        const auto replay =
            glassbox::replay_coordinator(entries);
        CHECK(replay.errors.empty());
        CHECK(replay.trades.at(trade1.trade_id()).committed);
        CHECK(replay.trades.at(trade1.trade_id()).ended);
        CHECK(replay.max_trade_id &&
              replay.max_trade_id->value() == 10);

        const auto trade2 =
            make_trade(glassbox::TradeId{11},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_c);
        const auto abort_entries =
            append_entries({
                glassbox::Proposed{trade2},
                glassbox::DecisionAbort{
                    trade2.trade_id(),
                    glassbox::ReasonCode::vote_timeout,
                },
            });
        const auto abort_replay =
            glassbox::replay_coordinator(abort_entries);
        CHECK(abort_replay.errors.empty());
        CHECK(abort_replay.trades.at(trade2.trade_id()).abort_reason ==
              glassbox::ReasonCode::vote_timeout);

        const auto undecided =
            make_trade(glassbox::TradeId{12},
                       glassbox::NodeId::bank_b,
                       glassbox::NodeId::bank_c);
        const auto undecided_replay =
            glassbox::replay_coordinator(
                append_entries({
                    glassbox::Proposed{undecided},
                }));
        CHECK(undecided_replay.errors.empty());
        const auto& undecided_trade =
            undecided_replay.trades.at(undecided.trade_id());
        CHECK(!undecided_trade.committed);
        CHECK(!undecided_trade.abort_reason);
        CHECK(!undecided_trade.ended);

        const auto error_entries =
            append_entries({
                glassbox::DecisionCommit{
                    glassbox::TradeId{13},
                    glassbox::NodeId::bank_a,
                    glassbox::NodeId::bank_b,
                },
                glassbox::DecisionAbort{
                    glassbox::TradeId{13},
                    glassbox::ReasonCode::expired,
                },
            });
        const auto error_replay =
            glassbox::replay_coordinator(error_entries);
        CHECK(!error_replay.errors.empty());

        const auto participant_error =
            append_entries({
                glassbox::Committed{
                    glassbox::TradeId{14},
                    glassbox::Leg::buyer,
                },
            });
        CHECK(!glassbox::replay_coordinator(
                  participant_error).errors.empty());
    }

    // Record-after-final and ledger failure replay errors.
    {
        const auto trade =
            make_trade(glassbox::TradeId{21},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto after_committed =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::buyer,
                },
                glassbox::Aborted{trade.trade_id()},
            });
        const auto after =
            glassbox::replay_participant(
                genesis,
                glassbox::NodeId::bank_a,
                after_committed);
        CHECK(!after.errors.empty());

        const auto trade2 =
            make_trade(glassbox::TradeId{22},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto ledger_failure =
            append_entries({
                glassbox::Prepared{trade2, glassbox::Leg::buyer},
                glassbox::Committed{
                    trade2.trade_id(),
                    glassbox::Leg::buyer,
                },
            });
        auto tiny =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{1},
                glassbox::Units{100}).ledger;
        const auto failed =
            glassbox::replay_participant(
                tiny,
                glassbox::NodeId::bank_a,
                ledger_failure);
        CHECK(!failed.errors.empty());
    }

    // End before decision and two decisions coordinator errors.
    {
        const auto trade =
            make_trade(glassbox::TradeId{23},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto end_before =
            append_entries({
                glassbox::Proposed{trade},
                glassbox::End{trade.trade_id()},
            });
        const auto end_replay =
            glassbox::replay_coordinator(end_before);
        CHECK(!end_replay.errors.empty());
        CHECK(end_replay.errors[0].kind ==
              glassbox::ReplayErrorKind::end_before_decision);

        const auto two =
            append_entries({
                glassbox::Proposed{trade},
                glassbox::DecisionCommit{
                    trade.trade_id(),
                    trade.buyer(),
                    trade.seller(),
                },
                glassbox::DecisionAbort{
                    trade.trade_id(),
                    glassbox::ReasonCode::expired,
                },
            });
        const auto two_replay =
            glassbox::replay_coordinator(two);
        CHECK(!two_replay.errors.empty());
        CHECK(two_replay.errors[0].kind ==
              glassbox::ReplayErrorKind::two_decisions);
    }

    // Buyer/seller committed replay conserves totals.
    {
        const auto trade =
            make_trade(glassbox::TradeId{24},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto buyer_entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::buyer,
                },
            });
        const auto seller_entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::seller},
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::seller,
                },
            });
        auto buyer_genesis =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{1'000},
                glassbox::Units{20}).ledger;
        auto seller_genesis =
            *glassbox::LocalLedger::genesis(
                glassbox::Cents{100},
                glassbox::Units{30}).ledger;
        const std::array<glassbox::LocalLedger, 2> before{
            buyer_genesis,
            seller_genesis,
        };
        const auto buyer =
            glassbox::replay_participant(
                buyer_genesis,
                glassbox::NodeId::bank_a,
                buyer_entries);
        const auto seller =
            glassbox::replay_participant(
                seller_genesis,
                glassbox::NodeId::bank_b,
                seller_entries);
        const std::array<glassbox::LocalLedger, 2> after{
            buyer.ledger,
            seller.ledger,
        };
        CHECK(glassbox::totals(before) ==
              glassbox::totals(after));
    }

    // Replay twice is identical.
    {
        const auto trade =
            make_trade(glassbox::TradeId{20},
                       glassbox::NodeId::bank_a,
                       glassbox::NodeId::bank_b);
        const auto entries =
            append_entries({
                glassbox::Prepared{trade, glassbox::Leg::buyer},
                glassbox::Committed{
                    trade.trade_id(),
                    glassbox::Leg::buyer,
                },
            });
        const auto first =
            glassbox::replay_participant(
                genesis,
                glassbox::NodeId::bank_a,
                entries);
        const auto second =
            glassbox::replay_participant(
                genesis,
                glassbox::NodeId::bank_a,
                entries);
        CHECK(first == second);
    }

    std::ifstream replay_header{
        "include/domain/replay.hpp"};
    CHECK(replay_header.is_open());
    std::string line;
    while (std::getline(replay_header, line)) {
        CHECK(line.find("float") == std::string::npos);
        CHECK(line.find("double") == std::string::npos);
    }

    return 0;
}
