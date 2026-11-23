#include "domain/replay.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace glassbox {

namespace {

void update_max(
    std::optional<TradeId>& max_trade_id,
    TradeId trade_id) noexcept
{
    if (!max_trade_id ||
        trade_id.value() > max_trade_id->value()) {
        max_trade_id = trade_id;
    }
}

bool is_participant_record(const LogRecord& record) noexcept
{
    return std::holds_alternative<Prepared>(record) ||
           std::holds_alternative<Committed>(record) ||
           std::holds_alternative<Aborted>(record);
}

bool is_coordinator_record(const LogRecord& record) noexcept
{
    return std::holds_alternative<Proposed>(record) ||
           std::holds_alternative<DecisionCommit>(record) ||
           std::holds_alternative<DecisionAbort>(record) ||
           std::holds_alternative<End>(record);
}

} // namespace

ParticipantReplay replay_participant(
    const LocalLedger& genesis,
    NodeId self,
    std::span<const LogEntry> entries)
{
    ParticipantReplay replay{
        genesis,
        {},
        {},
        {},
    };

    std::map<TradeId, const Prepared*> prepared;

    for (const LogEntry& entry : entries) {
        const LogRecord& record = entry.record;
        if (is_coordinator_record(record)) {
            replay.errors.push_back(
                ReplayError{
                    ReplayErrorKind::coordinator_record_in_participant,
                    std::nullopt,
                });
            continue;
        }

        if (const auto* prepared_record =
                std::get_if<Prepared>(&record)) {
            const Trade& trade = prepared_record->trade;
            const TradeId trade_id = trade.trade_id();

            if (replay.states.contains(trade_id) &&
                (replay.states[trade_id] == TradeState::committed ||
                 replay.states[trade_id] == TradeState::aborted)) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::record_after_final,
                        trade_id,
                    });
                continue;
            }

            const bool wrong_party =
                (prepared_record->leg == Leg::buyer &&
                 trade.buyer() != self) ||
                (prepared_record->leg == Leg::seller &&
                 trade.seller() != self);
            if (wrong_party) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::prepared_wrong_party,
                        trade_id,
                    });
                continue;
            }

            if (replay.states.contains(trade_id)) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::prepared_twice,
                        trade_id,
                    });
                continue;
            }

            LedgerStatus status =
                LedgerStatus::ok;
            if (prepared_record->leg == Leg::buyer) {
                status = replay.ledger.lock_cash(trade.cash_cents());
            } else {
                status = replay.ledger.lock_bonds(trade.qty());
            }
            if (status != LedgerStatus::ok) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::ledger_op_failed,
                        trade_id,
                    });
                continue;
            }

            replay.states[trade_id] = TradeState::prepared;
            prepared[trade_id] = prepared_record;
            replay.in_doubt.push_back(trade);
        } else if (const auto* committed =
                       std::get_if<Committed>(&record)) {
            const TradeId trade_id = committed->trade_id;
            const auto prepared_it = prepared.find(trade_id);
            if (prepared_it == prepared.end()) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::committed_without_prepared,
                        trade_id,
                    });
                continue;
            }
            if (replay.states[trade_id] == TradeState::committed ||
                replay.states[trade_id] == TradeState::aborted) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::record_after_final,
                        trade_id,
                    });
                continue;
            }

            const Trade& trade = prepared_it->second->trade;
            LedgerStatus status = LedgerStatus::ok;
            if (committed->leg == Leg::buyer) {
                status = replay.ledger.commit_buyer_leg(
                    trade.cash_cents(),
                    trade.qty());
            } else {
                status = replay.ledger.commit_seller_leg(
                    trade.qty(),
                    trade.cash_cents());
            }
            if (status != LedgerStatus::ok) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::ledger_op_failed,
                        trade_id,
                    });
                continue;
            }

            replay.states[trade_id] = TradeState::committed;
            replay.in_doubt.erase(
                std::remove_if(
                    replay.in_doubt.begin(),
                    replay.in_doubt.end(),
                    [trade_id](const Trade& candidate) {
                        return candidate.trade_id() == trade_id;
                    }),
                replay.in_doubt.end());
        } else if (const auto* aborted =
                       std::get_if<Aborted>(&record)) {
            const TradeId trade_id = aborted->trade_id;
            const auto prepared_it = prepared.find(trade_id);
            if (replay.states.contains(trade_id) &&
                (replay.states[trade_id] == TradeState::committed ||
                 replay.states[trade_id] == TradeState::aborted)) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::record_after_final,
                        trade_id,
                    });
                continue;
            }

            if (prepared_it != prepared.end()) {
                const Trade& trade = prepared_it->second->trade;
                const Leg leg = prepared_it->second->leg;
                LedgerStatus status =
                    leg == Leg::buyer
                        ? replay.ledger.unlock_cash(trade.cash_cents())
                        : replay.ledger.unlock_bonds(trade.qty());
                if (status != LedgerStatus::ok) {
                    replay.errors.push_back(
                        ReplayError{
                            ReplayErrorKind::ledger_op_failed,
                            trade_id,
                        });
                    continue;
                }
            }

            replay.states[trade_id] = TradeState::aborted;
            replay.in_doubt.erase(
                std::remove_if(
                    replay.in_doubt.begin(),
                    replay.in_doubt.end(),
                    [trade_id](const Trade& candidate) {
                        return candidate.trade_id() == trade_id;
                    }),
                replay.in_doubt.end());
        }
    }

    return replay;
}

CoordinatorReplay replay_coordinator(
    std::span<const LogEntry> entries)
{
    CoordinatorReplay replay;

    for (const LogEntry& entry : entries) {
        const LogRecord& record = entry.record;
        if (is_participant_record(record)) {
            replay.errors.push_back(
                ReplayError{
                    ReplayErrorKind::participant_record_in_coordinator,
                    std::nullopt,
                });
            continue;
        }

        if (const auto* proposed =
                std::get_if<Proposed>(&record)) {
            const TradeId trade_id = proposed->trade.trade_id();
            update_max(replay.max_trade_id, trade_id);
            replay.trades.try_emplace(
                trade_id,
                CoordinatorTradeReplay{
                    proposed->trade,
                    false,
                    std::nullopt,
                    false,
                });
        } else if (const auto* commit =
                       std::get_if<DecisionCommit>(&record)) {
            update_max(replay.max_trade_id, commit->trade_id);
            const auto trade_it = replay.trades.find(commit->trade_id);
            if (trade_it == replay.trades.end()) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::decision_without_proposed,
                        commit->trade_id,
                    });
                continue;
            }
            if (trade_it->second.committed ||
                trade_it->second.abort_reason) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::two_decisions,
                        commit->trade_id,
                    });
                continue;
            }
            trade_it->second.committed = true;
        } else if (const auto* abort =
                       std::get_if<DecisionAbort>(&record)) {
            update_max(replay.max_trade_id, abort->trade_id);
            const auto trade_it = replay.trades.find(abort->trade_id);
            if (trade_it == replay.trades.end()) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::decision_without_proposed,
                        abort->trade_id,
                    });
                continue;
            }
            if (trade_it->second.committed ||
                trade_it->second.abort_reason) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::two_decisions,
                        abort->trade_id,
                    });
                continue;
            }
            trade_it->second.abort_reason = abort->reason;
        } else if (const auto* end =
                       std::get_if<End>(&record)) {
            update_max(replay.max_trade_id, end->trade_id);
            const auto trade_it = replay.trades.find(end->trade_id);
            if (trade_it == replay.trades.end()) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::decision_without_proposed,
                        end->trade_id,
                    });
                continue;
            }
            if (!trade_it->second.committed &&
                !trade_it->second.abort_reason) {
                replay.errors.push_back(
                    ReplayError{
                        ReplayErrorKind::end_before_decision,
                        end->trade_id,
                    });
                continue;
            }
            trade_it->second.ended = true;
        }
    }

    return replay;
}

} // namespace glassbox
