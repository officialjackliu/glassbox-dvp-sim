// Coordinator handlers: PROPOSE validation and PREPARE broadcast, vote
// collection, the decision (logged before it is announced), retransmits until
// every bank ACKs, OUTCOME_QUERY answers, presumed abort for unknown trades,
// and recovery from the durable log after a crash.

#include "protocol/coordinator.hpp"

#include "domain/replay.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace glassbox {

namespace {

void note(Effects& effects,
          const char* kind,
          std::string detail,
          std::optional<TradeId> trade_id,
          const char* handler)
{
    effects.notes.push_back(
        TraceNote{kind, std::move(detail), trade_id, handler});
}

[[nodiscard]] std::string transition(TradeState from, TradeState to)
{
    std::string text{to_string(from)};
    text += "->";
    text += to_string(to);
    return text;
}

[[nodiscard]] std::optional<Leg> party_leg(const Trade& trade,
                                           NodeId node) noexcept
{
    if (trade.buyer() == node) {
        return Leg::buyer;
    }
    if (trade.seller() == node) {
        return Leg::seller;
    }
    return std::nullopt;
}

[[nodiscard]] TradeState decided_state(Decision decision) noexcept
{
    return decision == Decision::commit ? TradeState::committing
                                        : TradeState::aborting;
}

void presumed_abort(const Message& message,
                    Effects& effects,
                    const char* handler)
{
    effects.sends.push_back(Outgoing{make_abort(
        message.from, *message.trade_id, ReasonCode::unknown_trade)});
    note(effects, "PRESUMED_ABORT", std::string{to_string(message.from)},
         message.trade_id, handler);
}

} // namespace

Coordinator::Coordinator(CoordinatorConfig config)
    : config_(config)
{
    if (config_.vote_timeout_us <= 0 || !validate(config_.retransmit)) {
        throw std::invalid_argument(
            "coordinator needs a positive vote timeout and a valid "
            "retransmit backoff");
    }
}

Effects Coordinator::on_message(const Message& message, SimTimeUs now)
{
    const MessageCheck check = check_message(message);
    if (check != MessageCheck::ok) {
        Effects effects;
        note(effects, "INVALID_MESSAGE", std::string{to_string(check)},
             message.trade_id, "Coordinator::on_message");
        return effects;
    }
    switch (message.type) {
    case MessageType::propose:
        return on_propose(std::get<ProposeBody>(message.body), now);
    case MessageType::vote:
        return on_vote(message);
    case MessageType::ack:
        return on_ack(message);
    case MessageType::outcome_query:
        return on_outcome_query(message);
    case MessageType::prepare:
    case MessageType::commit:
    case MessageType::abort:
        break;
    }
    // Unreachable for well-formed messages (route check rejects them), kept
    // so every type has an explicit branch.
    Effects effects;
    note(effects, "INVALID_MESSAGE", "UNEXPECTED_TYPE", message.trade_id,
         "Coordinator::on_message");
    return effects;
}

Effects Coordinator::on_propose(const ProposeBody& body, SimTimeUs now)
{
    Effects effects;
    TradeResult result = validate_and_assign(body.request, now, allocator_);
    if (const auto* reason = std::get_if<TradeRejectReason>(&result)) {
        note(effects, "REJECTED", std::string{to_string(*reason)},
             std::nullopt, "Coordinator::on_propose");
        return effects;
    }
    const Trade& trade = std::get<Trade>(result);
    const TradeId trade_id = trade.trade_id();

    // Validation guarantees valid_until_us > now, so the cap is >= 1.
    const SimTimeUs until_deadline = trade.valid_until_us() - now;
    const SimTimeUs vote_delay =
        std::min(config_.vote_timeout_us, until_deadline);

    effects.logs.push_back(Proposed{trade});
    effects.sends.push_back(Outgoing{make_prepare(trade, Leg::buyer)});
    effects.sends.push_back(Outgoing{make_prepare(trade, Leg::seller)});
    effects.timers.push_back(TimerRequest{
        vote_delay,
        TimerKind::vote_timeout,
        trade_id,
        TradeState::collecting,
        0,
    });
    note(effects, "STATE_CHANGE",
         transition(TradeState::none, TradeState::collecting), trade_id,
         "Coordinator::on_propose");
    trades_.emplace(trade_id, CoordinatorTrade{trade});
    return effects;
}

Effects Coordinator::on_vote(const Message& message)
{
    Effects effects;
    const TradeId trade_id = *message.trade_id;
    const auto it = trades_.find(trade_id);
    if (it == trades_.end()) {
        presumed_abort(message, effects, "Coordinator::on_vote");
        return effects;
    }
    CoordinatorTrade& record = it->second;
    const auto leg = party_leg(record.trade, message.from);
    if (!leg) {
        note(effects, "INVALID_MESSAGE", "NOT_A_PARTY", trade_id,
             "Coordinator::on_vote");
        return effects;
    }
    const VoteBody& vote = std::get<VoteBody>(message.body);

    if (record.state != TradeState::collecting) {
        // Late or duplicate vote: re-send the existing decision to the voter
        // once that decision is durable.
        if (record.decision_durable) {
            send_decision(record, message.from, effects);
        } else {
            note(effects, "DECISION_PENDING",
                 std::string{to_string(message.from)}, trade_id,
                 "Coordinator::on_vote");
        }
        return effects;
    }

    if (vote.vote == Vote::no) {
        decide(record, Decision::abort, vote.reason, effects);
        return effects;
    }
    if (*leg == Leg::buyer) {
        record.buyer_voted_yes = true;
    } else {
        record.seller_voted_yes = true;
    }
    note(effects, "VOTE_RECORDED", std::string{to_string(message.from)},
         trade_id, "Coordinator::on_vote");
    if (record.buyer_voted_yes && record.seller_voted_yes) {
        decide(record, Decision::commit, ReasonCode::none, effects);
    }
    return effects;
}

Effects Coordinator::on_ack(const Message& message)
{
    Effects effects;
    const TradeId trade_id = *message.trade_id;
    const auto it = trades_.find(trade_id);
    if (it == trades_.end()) {
        note(effects, "ACK_IGNORED", "UNKNOWN_TRADE", trade_id,
             "Coordinator::on_ack");
        return effects;
    }
    CoordinatorTrade& record = it->second;
    const auto leg = party_leg(record.trade, message.from);
    if (!leg) {
        note(effects, "INVALID_MESSAGE", "NOT_A_PARTY", trade_id,
             "Coordinator::on_ack");
        return effects;
    }
    if (!record.decision) {
        note(effects, "ACK_IGNORED", "NO_DECISION", trade_id,
             "Coordinator::on_ack");
        return effects;
    }
    const Decision acked = std::get<AckBody>(message.body).decision;
    if (acked != *record.decision) {
        note(effects, "INVARIANT_VIOLATION", "ACK_DECISION_MISMATCH",
             trade_id, "Coordinator::on_ack");
        return effects;
    }
    if (record.state == TradeState::done) {
        note(effects, "ACK_IGNORED", "DUPLICATE", trade_id,
             "Coordinator::on_ack");
        return effects;
    }
    if (*leg == Leg::buyer) {
        record.buyer_acked = true;
    } else {
        record.seller_acked = true;
    }
    if (record.buyer_acked && record.seller_acked) {
        effects.logs.push_back(End{trade_id});
        note(effects, "STATE_CHANGE",
             transition(record.state, TradeState::done), trade_id,
             "Coordinator::on_ack");
        record.state = TradeState::done;
    }
    return effects;
}

Effects Coordinator::on_outcome_query(const Message& message)
{
    Effects effects;
    const TradeId trade_id = *message.trade_id;
    const auto it = trades_.find(trade_id);
    if (it == trades_.end()) {
        presumed_abort(message, effects, "Coordinator::on_outcome_query");
        return effects;
    }
    const CoordinatorTrade& record = it->second;
    if (!party_leg(record.trade, message.from)) {
        note(effects, "INVALID_MESSAGE", "NOT_A_PARTY", trade_id,
             "Coordinator::on_outcome_query");
        return effects;
    }
    if (record.state == TradeState::collecting) {
        // No reply: the vote timeout will resolve it and the participant
        // will ask again.
        note(effects, "QUERY_DEFERRED", std::string{to_string(message.from)},
             trade_id, "Coordinator::on_outcome_query");
        return effects;
    }
    if (!record.decision_durable) {
        note(effects, "DECISION_PENDING",
             std::string{to_string(message.from)}, trade_id,
             "Coordinator::on_outcome_query");
        return effects;
    }
    send_decision(record, message.from, effects);
    return effects;
}

Effects Coordinator::on_timer(const TimerEvent& event, SimTimeUs)
{
    if (!event.trade_id) {
        return {};
    }
    switch (event.kind) {
    case TimerKind::vote_timeout:
        return on_vote_timeout(*event.trade_id);
    case TimerKind::retransmit:
        return on_retransmit(*event.trade_id, event.attempt);
    case TimerKind::decision_wait:
        break;
    }
    return {};
}

Effects Coordinator::on_vote_timeout(TradeId trade_id)
{
    Effects effects;
    const auto it = trades_.find(trade_id);
    if (it == trades_.end() || it->second.state != TradeState::collecting) {
        return effects;
    }
    decide(it->second, Decision::abort, ReasonCode::vote_timeout, effects);
    return effects;
}

Effects Coordinator::on_retransmit(TradeId trade_id, std::uint32_t attempt)
{
    Effects effects;
    const auto it = trades_.find(trade_id);
    if (it == trades_.end() ||
        (it->second.state != TradeState::committing &&
         it->second.state != TradeState::aborting)) {
        return effects;
    }
    const CoordinatorTrade& record = it->second;
    std::string targets;
    if (!record.buyer_acked) {
        send_decision(record, record.trade.buyer(), effects);
        targets += to_string(record.trade.buyer());
    }
    if (!record.seller_acked) {
        send_decision(record, record.trade.seller(), effects);
        if (!targets.empty()) {
            targets += ",";
        }
        targets += to_string(record.trade.seller());
    }
    note(effects, "RETRANSMIT", std::move(targets), trade_id,
         "Coordinator::on_retransmit");
    const std::uint32_t next =
        attempt == std::numeric_limits<std::uint32_t>::max() ? attempt
                                                             : attempt + 1;
    schedule_retransmit(record, next, effects);
    return effects;
}

void Coordinator::decide(CoordinatorTrade& record,
                         Decision decision,
                         ReasonCode reason,
                         Effects& effects) const
{
    const TradeId trade_id = record.trade.trade_id();
    const TradeState next = decided_state(decision);
    if (decision == Decision::commit) {
        effects.logs.push_back(DecisionCommit{
            trade_id, record.trade.buyer(), record.trade.seller()});
    } else {
        effects.logs.push_back(DecisionAbort{trade_id, reason});
    }
    record.decision = decision;
    record.abort_reason =
        decision == Decision::abort ? reason : ReasonCode::none;
    record.decision_durable = false;
    note(effects, "STATE_CHANGE", transition(record.state, next), trade_id,
         "Coordinator::decide");
    record.state = next;

    // Same batch as the decision record: released only after it is durable.
    send_decision(record, record.trade.buyer(), effects);
    send_decision(record, record.trade.seller(), effects);
    schedule_retransmit(record, 0, effects);
}

void Coordinator::send_decision(const CoordinatorTrade& record,
                                NodeId to,
                                Effects& effects) const
{
    effects.sends.push_back(Outgoing{make_decision(
        to, record.trade.trade_id(), *record.decision, record.abort_reason)});
}

void Coordinator::schedule_retransmit(const CoordinatorTrade& record,
                                      std::uint32_t attempt,
                                      Effects& effects) const
{
    // validate(config_.retransmit) holds (constructor), so this has a value.
    const SimTimeUs delay =
        backoff_delay_us(config_.retransmit, attempt).value_or(
            config_.retransmit.max_us);
    effects.timers.push_back(TimerRequest{
        delay,
        TimerKind::retransmit,
        record.trade.trade_id(),
        record.state,
        attempt,
    });
}

TradeState Coordinator::trade_state(TradeId trade_id) const
{
    const auto it = trades_.find(trade_id);
    return it == trades_.end() ? TradeState::none : it->second.state;
}

void Coordinator::on_log_durable(const LogRecord& record, SimTimeUs)
{
    if (!std::holds_alternative<DecisionCommit>(record) &&
        !std::holds_alternative<DecisionAbort>(record)) {
        return;
    }
    const auto it = trades_.find(record_trade_id(record));
    if (it != trades_.end()) {
        it->second.decision_durable = true;
    }
}

void Coordinator::on_crash()
{
    trades_.clear();
    allocator_ = TradeIdAllocator{};
}

Effects Coordinator::on_restart(std::span<const LogEntry> durable_log,
                                SimTimeUs)
{
    trades_.clear();
    allocator_ = TradeIdAllocator{};
    Effects effects;
    const CoordinatorReplay replay = replay_coordinator(durable_log);
    for (const ReplayError& error : replay.errors) {
        note(effects, "RECOVERY_ERROR", std::string{to_string(error.kind)},
             error.trade_id, "Coordinator::on_restart");
    }
    if (replay.max_trade_id) {
        allocator_.observe(*replay.max_trade_id);
    }
    for (const auto& [trade_id, replayed] : replay.trades) {
        if (!replayed.committed && !replayed.abort_reason) {
            note(effects, "RECOVERY_STEP", "FORGET_UNDECIDED", trade_id,
                 "Coordinator::on_restart");
            continue;
        }
        CoordinatorTrade record{replayed.trade};
        record.decision =
            replayed.committed ? Decision::commit : Decision::abort;
        record.abort_reason =
            replayed.committed ? ReasonCode::none : *replayed.abort_reason;
        record.decision_durable = true;
        if (replayed.ended) {
            record.state = TradeState::done;
            record.buyer_acked = true;
            record.seller_acked = true;
            note(effects, "RECOVERY_STEP", "RESTORE_DONE", trade_id,
                 "Coordinator::on_restart");
        } else {
            record.state = decided_state(*record.decision);
            note(effects, "RECOVERY_STEP", "RESEND_DECISION", trade_id,
                 "Coordinator::on_restart");
            send_decision(record, record.trade.buyer(), effects);
            send_decision(record, record.trade.seller(), effects);
            schedule_retransmit(record, 0, effects);
        }
        trades_.emplace(trade_id, std::move(record));
    }
    return effects;
}

const CoordinatorTrade* Coordinator::find(TradeId trade_id) const noexcept
{
    const auto it = trades_.find(trade_id);
    return it == trades_.end() ? nullptr : &it->second;
}

const std::map<TradeId, CoordinatorTrade>& Coordinator::trades()
    const noexcept
{
    return trades_;
}

} // namespace glassbox
