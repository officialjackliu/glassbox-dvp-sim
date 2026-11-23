#include "protocol/participant.hpp"

#include "domain/replay.hpp"

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

[[nodiscard]] std::string amount_text(Leg leg, const Trade& trade)
{
    return leg == Leg::buyer
               ? "CASH " + std::to_string(trade.cash_cents().value())
               : "BONDS " + std::to_string(trade.qty().value());
}

[[nodiscard]] ReasonCode lock_failure_reason(LedgerStatus status,
                                             Leg leg) noexcept
{
    switch (status) {
    case LedgerStatus::insufficient_funds:
        return ReasonCode::insufficient_funds;
    case LedgerStatus::insufficient_bonds:
        return ReasonCode::insufficient_bonds;
    case LedgerStatus::account_frozen:
        return ReasonCode::account_frozen;
    case LedgerStatus::ok:
    case LedgerStatus::invalid_amount:
    case LedgerStatus::unlock_exceeds_locked:
    case LedgerStatus::overflow:
        break;
    }
    // Any other refusal to lock is reported as lack of the leg's asset:
    // the safe answer is always "no".
    return leg == Leg::buyer ? ReasonCode::insufficient_funds
                             : ReasonCode::insufficient_bonds;
}

[[nodiscard]] LedgerOp lock_op(Leg leg, const Trade& trade) noexcept
{
    if (leg == Leg::buyer) {
        return LockCash{trade.cash_cents()};
    }
    return LockBonds{trade.qty()};
}

[[nodiscard]] LedgerOp unlock_op(Leg leg, const Trade& trade) noexcept
{
    if (leg == Leg::buyer) {
        return UnlockCash{trade.cash_cents()};
    }
    return UnlockBonds{trade.qty()};
}

[[nodiscard]] LedgerOp commit_op(Leg leg, const Trade& trade) noexcept
{
    if (leg == Leg::buyer) {
        return CommitBuyer{trade.cash_cents(), trade.qty()};
    }
    return CommitSeller{trade.qty(), trade.cash_cents()};
}

[[nodiscard]] std::string apply_text(Leg leg, const Trade& trade)
{
    const std::string cash = std::to_string(trade.cash_cents().value());
    const std::string qty = std::to_string(trade.qty().value());
    return leg == Leg::buyer ? "CASH -" + cash + " BONDS +" + qty
                             : "BONDS -" + qty + " CASH +" + cash;
}

[[nodiscard]] bool is_bank(NodeId node) noexcept
{
    return node == NodeId::bank_a || node == NodeId::bank_b ||
           node == NodeId::bank_c;
}

} // namespace

Participant::Participant(ParticipantConfig config)
    : config_(std::move(config)),
      ledger_(config_.genesis)
{
    if (!is_bank(config_.self)) {
        throw std::invalid_argument("participant must be a bank node");
    }
    if (!validate(config_.decision_wait)) {
        throw std::invalid_argument("invalid decision-wait backoff");
    }
    if (!config_.genesis.check_invariants()) {
        throw std::invalid_argument("genesis ledger breaks invariants");
    }
}

Effects Participant::on_message(const Message& message, SimTimeUs now)
{
    Effects effects;
    const MessageCheck check = check_message(message);
    if (check != MessageCheck::ok) {
        note(effects, "INVALID_MESSAGE", std::string{to_string(check)},
             message.trade_id, "Participant::on_message");
        return effects;
    }
    if (message.to != config_.self) {
        note(effects, "INVALID_MESSAGE", "WRONG_RECIPIENT", message.trade_id,
             "Participant::on_message");
        return effects;
    }
    switch (message.type) {
    case MessageType::prepare:
        return on_prepare(message, now);
    case MessageType::commit:
        return on_commit(message);
    case MessageType::abort:
        return on_abort(message);
    case MessageType::propose:
    case MessageType::vote:
    case MessageType::ack:
    case MessageType::outcome_query:
        break;
    }
    // Unreachable for well-formed messages addressed to a bank.
    note(effects, "INVALID_MESSAGE", "UNEXPECTED_TYPE", message.trade_id,
         "Participant::on_message");
    return effects;
}

Effects Participant::on_prepare(const Message& message, SimTimeUs now)
{
    Effects effects;
    const PrepareBody& body = std::get<PrepareBody>(message.body);
    const Trade& trade = body.trade;
    const TradeId trade_id = trade.trade_id();
    const Leg leg = body.leg;
    const NodeId self = config_.self;

    const auto it = trades_.find(trade_id);
    if (it != trades_.end()) {
        switch (it->second.state) {
        case TradeState::preparing:
            // The yes vote is already queued behind the PREPARED flush.
            note(effects, "DUPLICATE_IGNORED", "VOTE_PENDING", trade_id,
                 "Participant::on_prepare");
            return effects;
        case TradeState::prepared:
        case TradeState::committed:
            effects.sends.push_back(Outgoing{
                make_vote(self, trade_id, Vote::yes, ReasonCode::none)});
            return effects;
        case TradeState::aborted:
            effects.sends.push_back(Outgoing{make_vote(
                self, trade_id, Vote::no, ReasonCode::already_aborted)});
            return effects;
        case TradeState::none:
        case TradeState::collecting:
        case TradeState::committing:
        case TradeState::aborting:
        case TradeState::done:
            break;
        }
    }

    // Fixed check order: frozen, expired, available >= leg amount.
    std::optional<ReasonCode> refusal;
    if (ledger_.frozen()) {
        refusal = ReasonCode::account_frozen;
    } else if (is_expired(trade, now)) {
        refusal = ReasonCode::expired;
    } else {
        const LedgerStatus status = glassbox::apply(ledger_, lock_op(leg, trade));
        if (status != LedgerStatus::ok) {
            refusal = lock_failure_reason(status, leg);
        }
    }

    if (refusal) {
        effects.logs.push_back(Aborted{trade_id});
        effects.sends.push_back(
            Outgoing{make_vote(self, trade_id, Vote::no, *refusal)});
        note(effects, "VOTE_NO", std::string{to_string(*refusal)}, trade_id,
             "Participant::on_prepare");
        note(effects, "STATE_CHANGE",
             transition(TradeState::none, TradeState::aborted), trade_id,
             "Participant::on_prepare");
        trades_.insert_or_assign(
            trade_id, ParticipantTrade{trade, TradeState::aborted});
        return effects;
    }

    effects.logs.push_back(Prepared{trade, leg});
    effects.sends.push_back(
        Outgoing{make_vote(self, trade_id, Vote::yes, ReasonCode::none)});
    schedule_decision_wait(trade_id, 0, effects);
    note(effects, "LOCK", amount_text(leg, trade), trade_id,
         "Participant::on_prepare");
    note(effects, "STATE_CHANGE",
         transition(TradeState::none, TradeState::preparing), trade_id,
         "Participant::on_prepare");
    trades_.insert_or_assign(trade_id,
                             ParticipantTrade{trade, TradeState::preparing});
    return effects;
}

Effects Participant::on_commit(const Message& message)
{
    Effects effects;
    const TradeId trade_id = *message.trade_id;
    const auto it = trades_.find(trade_id);
    const TradeState state =
        it == trades_.end() ? TradeState::none : it->second.state;

    if (state == TradeState::committed) {
        effects.sends.push_back(Outgoing{
            make_ack(config_.self, trade_id, Decision::commit)});
        return effects;
    }
    if (state != TradeState::prepared) {
        // COMMIT is only legal after a durable yes vote.
        std::string detail = "COMMIT_IN_";
        detail += to_string(state);
        note(effects, "INVARIANT_VIOLATION", std::move(detail), trade_id,
             "Participant::on_commit");
        return effects;
    }

    const Trade& trade = *it->second.trade;
    const Leg leg = leg_of(trade);
    const LedgerStatus status = glassbox::apply(ledger_, commit_op(leg, trade));
    if (status != LedgerStatus::ok) {
        note(effects, "INVARIANT_VIOLATION", "COMMIT_LEDGER_FAILED", trade_id,
             "Participant::on_commit");
        return effects;
    }
    effects.logs.push_back(Committed{trade_id, leg});
    effects.sends.push_back(
        Outgoing{make_ack(config_.self, trade_id, Decision::commit)});
    note(effects, "APPLY", apply_text(leg, trade), trade_id,
         "Participant::on_commit");
    note(effects, "STATE_CHANGE",
         transition(TradeState::prepared, TradeState::committed), trade_id,
         "Participant::on_commit");
    it->second.state = TradeState::committed;
    return effects;
}

Effects Participant::on_abort(const Message& message)
{
    Effects effects;
    const TradeId trade_id = *message.trade_id;
    const auto it = trades_.find(trade_id);
    const TradeState state =
        it == trades_.end() ? TradeState::none : it->second.state;

    switch (state) {
    case TradeState::aborted:
        effects.sends.push_back(Outgoing{
            make_ack(config_.self, trade_id, Decision::abort)});
        return effects;
    case TradeState::committed:
        note(effects, "INVARIANT_VIOLATION", "ABORT_AFTER_COMMIT", trade_id,
             "Participant::on_abort");
        return effects;
    case TradeState::preparing:
    case TradeState::prepared: {
        const Trade& trade = *it->second.trade;
        const Leg leg = leg_of(trade);
        const LedgerStatus status = glassbox::apply(ledger_, unlock_op(leg, trade));
        if (status != LedgerStatus::ok) {
            note(effects, "INVARIANT_VIOLATION", "UNLOCK_FAILED", trade_id,
                 "Participant::on_abort");
            return effects;
        }
        effects.logs.push_back(Aborted{trade_id});
        effects.sends.push_back(Outgoing{
            make_ack(config_.self, trade_id, Decision::abort)});
        note(effects, "UNLOCK", amount_text(leg, trade), trade_id,
             "Participant::on_abort");
        note(effects, "STATE_CHANGE", transition(state, TradeState::aborted),
             trade_id, "Participant::on_abort");
        it->second.state = TradeState::aborted;
        return effects;
    }
    case TradeState::none:
    case TradeState::collecting:
    case TradeState::committing:
    case TradeState::aborting:
    case TradeState::done:
        break;
    }

    // Never seen: write a tombstone so a delayed PREPARE is refused.
    effects.logs.push_back(Aborted{trade_id});
    effects.sends.push_back(
        Outgoing{make_ack(config_.self, trade_id, Decision::abort)});
    note(effects, "STATE_CHANGE",
         transition(TradeState::none, TradeState::aborted), trade_id,
         "Participant::on_abort");
    trades_.insert_or_assign(
        trade_id, ParticipantTrade{std::nullopt, TradeState::aborted});
    return effects;
}

Effects Participant::on_timer(const TimerEvent& event, SimTimeUs)
{
    if (!event.trade_id || event.kind != TimerKind::decision_wait) {
        return {};
    }
    return on_decision_wait(*event.trade_id, event.attempt);
}

Effects Participant::on_decision_wait(TradeId trade_id,
                                      std::uint32_t attempt)
{
    Effects effects;
    if (trade_state(trade_id) != TradeState::prepared) {
        return effects;
    }
    // In doubt: ask, never abort unilaterally.
    effects.sends.push_back(
        Outgoing{make_outcome_query(config_.self, trade_id)});
    note(effects, "OUTCOME_QUERY", std::to_string(attempt), trade_id,
         "Participant::on_decision_wait");
    const std::uint32_t next =
        attempt == std::numeric_limits<std::uint32_t>::max() ? attempt
                                                             : attempt + 1;
    schedule_decision_wait(trade_id, next, effects);
    return effects;
}

void Participant::schedule_decision_wait(TradeId trade_id,
                                         std::uint32_t attempt,
                                         Effects& effects) const
{
    // validate(config_.decision_wait) holds (constructor).
    const SimTimeUs delay =
        backoff_delay_us(config_.decision_wait, attempt)
            .value_or(config_.decision_wait.max_us);
    effects.timers.push_back(TimerRequest{
        delay,
        TimerKind::decision_wait,
        trade_id,
        TradeState::prepared,
        attempt,
    });
}

Leg Participant::leg_of(const Trade& trade) const noexcept
{
    return trade.buyer() == config_.self ? Leg::buyer : Leg::seller;
}

TradeState Participant::trade_state(TradeId trade_id) const
{
    const auto it = trades_.find(trade_id);
    return it == trades_.end() ? TradeState::none : it->second.state;
}

void Participant::on_log_durable(const LogRecord& record, SimTimeUs)
{
    if (!std::holds_alternative<Prepared>(record)) {
        return;
    }
    const auto it = trades_.find(record_trade_id(record));
    if (it != trades_.end() && it->second.state == TradeState::preparing) {
        it->second.state = TradeState::prepared;
    }
}

void Participant::on_crash()
{
    trades_.clear();
    ledger_ = config_.genesis;
}

Effects Participant::on_restart(std::span<const LogEntry> durable_log,
                                SimTimeUs)
{
    Effects effects;
    const ParticipantReplay replay =
        replay_participant(config_.genesis, config_.self, durable_log);
    for (const ReplayError& error : replay.errors) {
        note(effects, "RECOVERY_ERROR", std::string{to_string(error.kind)},
             error.trade_id, "Participant::on_restart");
    }
    ledger_ = replay.ledger;
    trades_.clear();
    for (const auto& [trade_id, state] : replay.states) {
        trades_.emplace(trade_id, ParticipantTrade{std::nullopt, state});
    }
    for (const Trade& trade : replay.in_doubt) {
        const TradeId trade_id = trade.trade_id();
        trades_.insert_or_assign(
            trade_id, ParticipantTrade{trade, TradeState::prepared});
        effects.sends.push_back(
            Outgoing{make_outcome_query(config_.self, trade_id)});
        schedule_decision_wait(trade_id, 0, effects);
        note(effects, "RECOVERY_STEP", "IN_DOUBT_QUERY", trade_id,
             "Participant::on_restart");
    }
    return effects;
}

NodeId Participant::self() const noexcept
{
    return config_.self;
}

const LocalLedger& Participant::ledger() const noexcept
{
    return ledger_;
}

const ParticipantTrade* Participant::find(TradeId trade_id) const noexcept
{
    const auto it = trades_.find(trade_id);
    return it == trades_.end() ? nullptr : &it->second;
}

const std::map<TradeId, ParticipantTrade>& Participant::trades()
    const noexcept
{
    return trades_;
}

} // namespace glassbox
