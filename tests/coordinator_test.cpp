// Task 12: coordinator state machine (DESIGN.md 5.3, 6.2, 6.4).
// Part 1 calls the pure node directly and asserts exact Effects.
// Part 2 runs the coordinator inside the Simulator with scripted participants.

#include "protocol/coordinator.hpp"
#include "sim/simulator.hpp"
#include "check.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using glassbox::Coordinator;
using glassbox::CoordinatorConfig;
using glassbox::Decision;
using glassbox::Effects;
using glassbox::Leg;
using glassbox::Message;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::ReasonCode;
using glassbox::TimerKind;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::Vote;

constexpr glassbox::SimTimeUs k_vote_timeout = 500'000;

[[nodiscard]] CoordinatorConfig config()
{
    return CoordinatorConfig{k_vote_timeout, {1'000'000, 8'000'000}};
}

[[nodiscard]] glassbox::TradeRequest request(
    glassbox::SimTimeUs valid_until = 30'000'000)
{
    return glassbox::TradeRequest{
        NodeId::bank_a,
        NodeId::bank_b,
        "UST_10Y",
        glassbox::Units{25},
        glassbox::Cents{2'487'500},
        valid_until,
        "client",
    };
}

[[nodiscard]] Effects propose(Coordinator& coordinator,
                              glassbox::SimTimeUs now = 0,
                              glassbox::SimTimeUs valid_until = 30'000'000)
{
    return coordinator.on_message(glassbox::make_propose(request(valid_until)),
                                  now);
}

[[nodiscard]] Message yes(NodeId from, TradeId id = TradeId{1})
{
    return glassbox::make_vote(from, id, Vote::yes, ReasonCode::none);
}

[[nodiscard]] Message no(NodeId from,
                         ReasonCode reason,
                         TradeId id = TradeId{1})
{
    return glassbox::make_vote(from, id, Vote::no, reason);
}

[[nodiscard]] bool has_note(const Effects& effects,
                            const std::string& kind,
                            const std::string& detail)
{
    for (const glassbox::TraceNote& note : effects.notes) {
        if (note.kind == kind && note.detail == detail) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_decision(const Message& message,
                               NodeId to,
                               Decision decision,
                               ReasonCode reason,
                               TradeId id = TradeId{1})
{
    if (message.to != to || message.from != NodeId::coordinator ||
        message.trade_id != id) {
        return false;
    }
    if (decision == Decision::commit) {
        return message.type == MessageType::commit &&
               std::holds_alternative<glassbox::CommitBody>(message.body);
    }
    const auto* body = std::get_if<glassbox::AbortBody>(&message.body);
    return message.type == MessageType::abort && body &&
           body->reason == reason;
}

[[nodiscard]] glassbox::TimerEvent timer(TimerKind kind,
                                         TradeState expected,
                                         std::uint32_t attempt = 0,
                                         TradeId id = TradeId{1})
{
    return glassbox::TimerEvent{NodeId::coordinator, 0, kind, id, expected,
                                attempt};
}

// Drive one trade to COMMITTING with a durable decision.
void reach_committing(Coordinator& coordinator)
{
    static_cast<void>(propose(coordinator));
    static_cast<void>(coordinator.on_message(yes(NodeId::bank_a), 10));
    const Effects decided = coordinator.on_message(yes(NodeId::bank_b), 20);
    CHECK(decided.logs.size() == 1);
    coordinator.on_log_durable(decided.logs[0], 30);
}

void test_config_validation()
{
    bool threw = false;
    try {
        Coordinator bad{CoordinatorConfig{0, {1'000'000, 8'000'000}}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Coordinator bad{CoordinatorConfig{1, {2'000, 1'000}}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    Coordinator ok{CoordinatorConfig{1, {1'000, 1'000}}};
    CHECK(ok.trades().empty());
}

void test_propose_valid()
{
    Coordinator coordinator{config()};
    const Effects effects = propose(coordinator, 0);

    CHECK(effects.logs.size() == 1);
    const auto& proposed = std::get<glassbox::Proposed>(effects.logs[0]);
    CHECK(proposed.trade.trade_id() == TradeId{1});
    CHECK(proposed.trade.qty() == glassbox::Units{25});

    // Deterministic order: buyer PREPARE first, then seller.
    CHECK(effects.sends.size() == 2);
    const Message& to_buyer = effects.sends[0].message;
    const Message& to_seller = effects.sends[1].message;
    CHECK(to_buyer.type == MessageType::prepare);
    CHECK(to_buyer.to == NodeId::bank_a);
    CHECK(std::get<glassbox::PrepareBody>(to_buyer.body).leg == Leg::buyer);
    CHECK(std::get<glassbox::PrepareBody>(to_buyer.body).trade ==
          proposed.trade);
    CHECK(to_seller.type == MessageType::prepare);
    CHECK(to_seller.to == NodeId::bank_b);
    CHECK(std::get<glassbox::PrepareBody>(to_seller.body).leg ==
          Leg::seller);

    CHECK(effects.timers.size() == 1);
    CHECK(effects.timers[0].kind == TimerKind::vote_timeout);
    CHECK(effects.timers[0].delay_us == k_vote_timeout);
    CHECK(effects.timers[0].trade_id == TradeId{1});
    CHECK(effects.timers[0].expected_state == TradeState::collecting);
    CHECK(effects.timers[0].attempt == 0);

    CHECK(has_note(effects, "STATE_CHANGE", "NONE->COLLECTING"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::collecting);
    const auto* record = coordinator.find(TradeId{1});
    CHECK(record != nullptr);
    CHECK(!record->decision.has_value());
    CHECK(!record->buyer_voted_yes);
    CHECK(!record->seller_voted_yes);

    // Second trade gets the next id.
    const Effects second = propose(coordinator, 5);
    CHECK(std::get<glassbox::Proposed>(second.logs[0]).trade.trade_id() ==
          TradeId{2});
}

void test_propose_rejected_leaves_no_trace_in_state()
{
    Coordinator coordinator{config()};
    glassbox::TradeRequest same_party = request();
    same_party.seller = NodeId::bank_a;
    const Effects rejected = coordinator.on_message(
        glassbox::make_propose(same_party), 0);
    CHECK(rejected.logs.empty());
    CHECK(rejected.sends.empty());
    CHECK(rejected.timers.empty());
    CHECK(rejected.notes.size() == 1);
    CHECK(rejected.notes[0].kind == "REJECTED");
    CHECK(rejected.notes[0].detail == "SAME_PARTY");
    CHECK(coordinator.trades().empty());

    // Expired at exactly now (valid_until == now) is rejected.
    const Effects expired = propose(coordinator, 100, 100);
    CHECK(expired.logs.empty());
    CHECK(has_note(expired, "REJECTED", "EXPIRED"));
    CHECK(coordinator.trades().empty());

    // Rejections did not consume ids.
    const Effects accepted = propose(coordinator, 100, 101);
    CHECK(std::get<glassbox::Proposed>(accepted.logs[0]).trade.trade_id() ==
          TradeId{1});
}

void test_vote_timeout_is_capped_by_deadline()
{
    // deadline - now below, at, and above the configured timeout.
    const glassbox::SimTimeUs now = 1'000;
    {
        Coordinator coordinator{config()};
        const Effects e = propose(coordinator, now, now + k_vote_timeout - 1);
        CHECK(e.timers[0].delay_us == k_vote_timeout - 1);
    }
    {
        Coordinator coordinator{config()};
        const Effects e = propose(coordinator, now, now + k_vote_timeout);
        CHECK(e.timers[0].delay_us == k_vote_timeout);
    }
    {
        Coordinator coordinator{config()};
        const Effects e = propose(coordinator, now, now + k_vote_timeout + 1);
        CHECK(e.timers[0].delay_us == k_vote_timeout);
    }
    {
        Coordinator coordinator{config()};
        const Effects e = propose(coordinator, now, now + 1);
        CHECK(e.timers[0].delay_us == 1);
    }
}

void test_votes_yes_yes_commit()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));

    const Effects first = coordinator.on_message(yes(NodeId::bank_a), 10);
    CHECK(first.logs.empty());
    CHECK(first.sends.empty());
    CHECK(first.timers.empty());
    CHECK(has_note(first, "VOTE_RECORDED", "BANK_A"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::collecting);
    CHECK(coordinator.find(TradeId{1})->buyer_voted_yes);
    CHECK(!coordinator.find(TradeId{1})->seller_voted_yes);

    // Duplicate yes from the same voter does not decide.
    const Effects duplicate = coordinator.on_message(yes(NodeId::bank_a), 11);
    CHECK(duplicate.logs.empty());
    CHECK(duplicate.sends.empty());
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::collecting);

    const Effects decided = coordinator.on_message(yes(NodeId::bank_b), 20);
    CHECK(decided.logs.size() == 1);
    const auto& commit = std::get<glassbox::DecisionCommit>(decided.logs[0]);
    CHECK(commit.trade_id == TradeId{1});
    CHECK(commit.buyer == NodeId::bank_a);
    CHECK(commit.seller == NodeId::bank_b);
    CHECK(decided.sends.size() == 2);
    CHECK(is_decision(decided.sends[0].message, NodeId::bank_a,
                      Decision::commit, ReasonCode::none));
    CHECK(is_decision(decided.sends[1].message, NodeId::bank_b,
                      Decision::commit, ReasonCode::none));
    CHECK(decided.timers.size() == 1);
    CHECK(decided.timers[0].kind == TimerKind::retransmit);
    CHECK(decided.timers[0].delay_us == 1'000'000);
    CHECK(decided.timers[0].expected_state == TradeState::committing);
    CHECK(decided.timers[0].attempt == 0);
    CHECK(has_note(decided, "STATE_CHANGE", "COLLECTING->COMMITTING"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::committing);
    CHECK(coordinator.find(TradeId{1})->decision == Decision::commit);
    CHECK(coordinator.find(TradeId{1})->abort_reason == ReasonCode::none);
    CHECK(!coordinator.find(TradeId{1})->decision_durable);
}

void test_vote_no_aborts_with_reason()
{
    // Seller says no after buyer yes; the reason travels to both.
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    static_cast<void>(coordinator.on_message(yes(NodeId::bank_a), 10));
    const Effects decided = coordinator.on_message(
        no(NodeId::bank_b, ReasonCode::insufficient_bonds), 20);
    CHECK(decided.logs.size() == 1);
    const auto& abort = std::get<glassbox::DecisionAbort>(decided.logs[0]);
    CHECK(abort.trade_id == TradeId{1});
    CHECK(abort.reason == ReasonCode::insufficient_bonds);
    CHECK(decided.sends.size() == 2);
    CHECK(is_decision(decided.sends[0].message, NodeId::bank_a,
                      Decision::abort, ReasonCode::insufficient_bonds));
    CHECK(is_decision(decided.sends[1].message, NodeId::bank_b,
                      Decision::abort, ReasonCode::insufficient_bonds));
    CHECK(decided.timers.size() == 1);
    CHECK(decided.timers[0].expected_state == TradeState::aborting);
    CHECK(has_note(decided, "STATE_CHANGE", "COLLECTING->ABORTING"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::aborting);
    CHECK(coordinator.find(TradeId{1})->abort_reason ==
          ReasonCode::insufficient_bonds);

    // First vote no decides immediately (no need to wait for the other).
    Coordinator other{config()};
    static_cast<void>(propose(other));
    const Effects first_no = other.on_message(
        no(NodeId::bank_a, ReasonCode::insufficient_funds), 10);
    CHECK(other.trade_state(TradeId{1}) == TradeState::aborting);
    CHECK(std::get<glassbox::DecisionAbort>(first_no.logs[0]).reason ==
          ReasonCode::insufficient_funds);
}

void test_late_vote_waits_for_durable_decision()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    static_cast<void>(coordinator.on_message(yes(NodeId::bank_a), 10));
    const Effects decided = coordinator.on_message(yes(NodeId::bank_b), 20);

    // Decision record not yet durable: no reply may leave.
    const Effects early = coordinator.on_message(yes(NodeId::bank_a), 25);
    CHECK(early.sends.empty());
    CHECK(early.logs.empty());
    CHECK(has_note(early, "DECISION_PENDING", "BANK_A"));
    const Effects early_query = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_b, TradeId{1}), 25);
    CHECK(early_query.sends.empty());
    CHECK(has_note(early_query, "DECISION_PENDING", "BANK_B"));

    // Unrelated durable records do not mark the decision durable.
    coordinator.on_log_durable(glassbox::End{TradeId{1}}, 26);
    coordinator.on_log_durable(glassbox::DecisionCommit{
                                   TradeId{9}, NodeId::bank_a, NodeId::bank_b},
                               26);
    CHECK(!coordinator.find(TradeId{1})->decision_durable);

    coordinator.on_log_durable(decided.logs[0], 30);
    CHECK(coordinator.find(TradeId{1})->decision_durable);

    // After durability, a late vote gets the decision, only to the voter.
    const Effects late = coordinator.on_message(yes(NodeId::bank_a), 40);
    CHECK(late.logs.empty());
    CHECK(late.timers.empty());
    CHECK(late.sends.size() == 1);
    CHECK(is_decision(late.sends[0].message, NodeId::bank_a,
                      Decision::commit, ReasonCode::none));
    // A late "no" after commit also just gets COMMIT back.
    const Effects late_no = coordinator.on_message(
        no(NodeId::bank_b, ReasonCode::expired), 41);
    CHECK(late_no.sends.size() == 1);
    CHECK(is_decision(late_no.sends[0].message, NodeId::bank_b,
                      Decision::commit, ReasonCode::none));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::committing);
}

void test_votes_from_strangers_and_unknown_trades()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));

    const Effects stranger = coordinator.on_message(yes(NodeId::bank_c), 10);
    CHECK(stranger.sends.empty());
    CHECK(stranger.logs.empty());
    CHECK(has_note(stranger, "INVALID_MESSAGE", "NOT_A_PARTY"));
    CHECK(!coordinator.find(TradeId{1})->buyer_voted_yes);
    CHECK(!coordinator.find(TradeId{1})->seller_voted_yes);

    // Presumed abort for a trade the coordinator does not know.
    const Effects unknown =
        coordinator.on_message(yes(NodeId::bank_c, TradeId{77}), 10);
    CHECK(unknown.logs.empty());
    CHECK(unknown.timers.empty());
    CHECK(unknown.sends.size() == 1);
    CHECK(is_decision(unknown.sends[0].message, NodeId::bank_c,
                      Decision::abort, ReasonCode::unknown_trade,
                      TradeId{77}));
    CHECK(has_note(unknown, "PRESUMED_ABORT", "BANK_C"));
    CHECK(coordinator.find(TradeId{77}) == nullptr);
}

void test_acks()
{
    Coordinator coordinator{config()};
    reach_committing(coordinator);

    // Wrong decision in ACK is an invariant violation; state unchanged.
    const Effects mismatch = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::abort), 50);
    CHECK(has_note(mismatch, "INVARIANT_VIOLATION", "ACK_DECISION_MISMATCH"));
    CHECK(!coordinator.find(TradeId{1})->buyer_acked);

    const Effects stranger = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_c, TradeId{1}, Decision::commit), 50);
    CHECK(has_note(stranger, "INVALID_MESSAGE", "NOT_A_PARTY"));

    const Effects first = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::commit), 60);
    CHECK(first.logs.empty());
    CHECK(first.sends.empty());
    CHECK(coordinator.find(TradeId{1})->buyer_acked);
    CHECK(!coordinator.find(TradeId{1})->seller_acked);
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::committing);

    // Duplicate ACK from the same party does not finish the trade.
    const Effects again = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::commit), 61);
    CHECK(again.logs.empty());
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::committing);

    const Effects last = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::commit), 70);
    CHECK(last.logs.size() == 1);
    CHECK(std::get<glassbox::End>(last.logs[0]).trade_id == TradeId{1});
    CHECK(last.sends.empty());
    CHECK(last.timers.empty());
    CHECK(has_note(last, "STATE_CHANGE", "COMMITTING->DONE"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::done);

    // ACK after DONE: no second END.
    const Effects after = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::commit), 80);
    CHECK(after.logs.empty());
    CHECK(has_note(after, "ACK_IGNORED", "DUPLICATE"));

    // ACK for unknown trade and ACK before any decision are ignored.
    const Effects unknown = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{50}, Decision::abort), 80);
    CHECK(unknown.sends.empty());
    CHECK(unknown.logs.empty());
    CHECK(has_note(unknown, "ACK_IGNORED", "UNKNOWN_TRADE"));

    Coordinator collecting{config()};
    static_cast<void>(propose(collecting));
    const Effects early = collecting.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::abort), 5);
    CHECK(has_note(early, "ACK_IGNORED", "NO_DECISION"));
    CHECK(collecting.trade_state(TradeId{1}) == TradeState::collecting);
    CHECK(!collecting.find(TradeId{1})->buyer_acked);
}

void test_abort_acks_finish_trade()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    static_cast<void>(coordinator.on_message(
        no(NodeId::bank_a, ReasonCode::account_frozen), 10));
    static_cast<void>(coordinator.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::abort), 20));
    const Effects last = coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::abort), 30);
    CHECK(last.logs.size() == 1);
    CHECK(std::holds_alternative<glassbox::End>(last.logs[0]));
    CHECK(has_note(last, "STATE_CHANGE", "ABORTING->DONE"));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::done);
}

void test_vote_timeout_timer()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    static_cast<void>(coordinator.on_message(yes(NodeId::bank_a), 10));
    const Effects timeout = coordinator.on_timer(
        timer(TimerKind::vote_timeout, TradeState::collecting), 501'100);
    CHECK(timeout.logs.size() == 1);
    CHECK(std::get<glassbox::DecisionAbort>(timeout.logs[0]).reason ==
          ReasonCode::vote_timeout);
    CHECK(timeout.sends.size() == 2);
    CHECK(is_decision(timeout.sends[0].message, NodeId::bank_a,
                      Decision::abort, ReasonCode::vote_timeout));
    CHECK(is_decision(timeout.sends[1].message, NodeId::bank_b,
                      Decision::abort, ReasonCode::vote_timeout));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::aborting);

    // Fired again (e.g. called directly) after deciding: no effect.
    const Effects again = coordinator.on_timer(
        timer(TimerKind::vote_timeout, TradeState::collecting), 501'200);
    CHECK(again.logs.empty());
    CHECK(again.sends.empty());
    CHECK(again.timers.empty());

    // Unknown trade or no trade id: no effect.
    const Effects unknown = coordinator.on_timer(
        timer(TimerKind::vote_timeout, TradeState::collecting, 0,
              TradeId{9}),
        0);
    CHECK(unknown.logs.empty());
    glassbox::TimerEvent no_id =
        timer(TimerKind::vote_timeout, TradeState::collecting);
    no_id.trade_id.reset();
    CHECK(coordinator.on_timer(no_id, 0).logs.empty());
    // DECISION_WAIT is a participant timer; the coordinator ignores it.
    CHECK(coordinator.on_timer(
                  timer(TimerKind::decision_wait, TradeState::aborting), 0)
              .sends.empty());
}

void test_retransmit_timer_and_backoff()
{
    Coordinator coordinator{config()};
    reach_committing(coordinator);
    static_cast<void>(coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::commit), 60));

    const Effects first = coordinator.on_timer(
        timer(TimerKind::retransmit, TradeState::committing, 0), 1'000'000);
    CHECK(first.logs.empty());
    CHECK(first.sends.size() == 1); // only the non-acked seller
    CHECK(is_decision(first.sends[0].message, NodeId::bank_b,
                      Decision::commit, ReasonCode::none));
    CHECK(first.timers.size() == 1);
    CHECK(first.timers[0].kind == TimerKind::retransmit);
    CHECK(first.timers[0].attempt == 1);
    CHECK(first.timers[0].delay_us == 2'000'000);
    CHECK(first.timers[0].expected_state == TradeState::committing);
    CHECK(has_note(first, "RETRANSMIT", "BANK_B"));

    const std::vector<std::pair<std::uint32_t, glassbox::SimTimeUs>> steps{
        {1, 4'000'000}, {2, 8'000'000}, {3, 8'000'000}, {40, 8'000'000}};
    for (const auto& [attempt, delay] : steps) {
        const Effects e = coordinator.on_timer(
            timer(TimerKind::retransmit, TradeState::committing, attempt), 0);
        CHECK(e.timers.size() == 1);
        CHECK(e.timers[0].attempt == attempt + 1);
        CHECK(e.timers[0].delay_us == delay);
    }
    // Attempt counter saturates instead of wrapping.
    const Effects saturated = coordinator.on_timer(
        timer(TimerKind::retransmit, TradeState::committing,
              std::numeric_limits<std::uint32_t>::max()),
        0);
    CHECK(saturated.timers[0].attempt ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK(saturated.timers[0].delay_us == 8'000'000);

    // Only the seller acked: resend to the buyer only.
    Coordinator seller_acked{config()};
    reach_committing(seller_acked);
    static_cast<void>(seller_acked.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::commit), 60));
    const Effects buyer_only = seller_acked.on_timer(
        timer(TimerKind::retransmit, TradeState::committing), 0);
    CHECK(buyer_only.sends.size() == 1);
    CHECK(is_decision(buyer_only.sends[0].message, NodeId::bank_a,
                      Decision::commit, ReasonCode::none));
    CHECK(has_note(buyer_only, "RETRANSMIT", "BANK_A"));

    // No ACKs yet: both parties, buyer first.
    Coordinator fresh{config()};
    reach_committing(fresh);
    const Effects both = fresh.on_timer(
        timer(TimerKind::retransmit, TradeState::committing), 0);
    CHECK(both.sends.size() == 2);
    CHECK(both.sends[0].message.to == NodeId::bank_a);
    CHECK(both.sends[1].message.to == NodeId::bank_b);
    CHECK(has_note(both, "RETRANSMIT", "BANK_A,BANK_B"));

    // Retransmit in COLLECTING or DONE does nothing.
    Coordinator collecting{config()};
    static_cast<void>(propose(collecting));
    CHECK(collecting.on_timer(
                  timer(TimerKind::retransmit, TradeState::collecting), 0)
              .sends.empty());
    static_cast<void>(coordinator.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::commit), 70));
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::done);
    const Effects done = coordinator.on_timer(
        timer(TimerKind::retransmit, TradeState::committing), 0);
    CHECK(done.sends.empty());
    CHECK(done.timers.empty());
}

void test_outcome_query()
{
    Coordinator coordinator{config()};
    // Unknown -> presumed abort.
    const Effects unknown = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{3}), 0);
    CHECK(unknown.sends.size() == 1);
    CHECK(is_decision(unknown.sends[0].message, NodeId::bank_a,
                      Decision::abort, ReasonCode::unknown_trade,
                      TradeId{3}));

    // COLLECTING -> no reply.
    static_cast<void>(propose(coordinator));
    const Effects collecting = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{1}), 1);
    CHECK(collecting.sends.empty());
    CHECK(has_note(collecting, "QUERY_DEFERRED", "BANK_A"));

    // Non-party asking about a known trade -> ignored.
    const Effects stranger = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_c, TradeId{1}), 1);
    CHECK(stranger.sends.empty());
    CHECK(has_note(stranger, "INVALID_MESSAGE", "NOT_A_PARTY"));

    // Decided and durable -> decision.
    static_cast<void>(coordinator.on_message(yes(NodeId::bank_a), 10));
    const Effects decided = coordinator.on_message(yes(NodeId::bank_b), 20);
    coordinator.on_log_durable(decided.logs[0], 30);
    const Effects answer = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_b, TradeId{1}), 40);
    CHECK(answer.sends.size() == 1);
    CHECK(is_decision(answer.sends[0].message, NodeId::bank_b,
                      Decision::commit, ReasonCode::none));

    // DONE still answers from the kept decision.
    static_cast<void>(coordinator.on_message(
        glassbox::make_ack(NodeId::bank_a, TradeId{1}, Decision::commit), 50));
    static_cast<void>(coordinator.on_message(
        glassbox::make_ack(NodeId::bank_b, TradeId{1}, Decision::commit), 60));
    const Effects done = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{1}), 70);
    CHECK(done.sends.size() == 1);
    CHECK(is_decision(done.sends[0].message, NodeId::bank_a,
                      Decision::commit, ReasonCode::none));
}

void test_invalid_messages_are_ignored()
{
    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    Message malformed = yes(NodeId::bank_a);
    malformed.body = glassbox::AckBody{Decision::commit};
    const Effects e = coordinator.on_message(malformed, 1);
    CHECK(e.sends.empty());
    CHECK(e.logs.empty());
    CHECK(e.notes.size() == 1);
    CHECK(e.notes[0].kind == "INVALID_MESSAGE");
    CHECK(e.notes[0].detail == "BODY_TYPE_MISMATCH");
    CHECK(!coordinator.find(TradeId{1})->buyer_voted_yes);

    // A PREPARE addressed to the coordinator fails the route check.
    const Effects prepare = coordinator.on_message(
        glassbox::Message{0, NodeId::coordinator, NodeId::coordinator,
                          MessageType::prepare, TradeId{1},
                          glassbox::PrepareBody{
                              coordinator.find(TradeId{1})->trade,
                              Leg::buyer}},
        1);
    CHECK(has_note(prepare, "INVALID_MESSAGE", "INVALID_ROUTE"));
}

[[nodiscard]] std::vector<glassbox::LogEntry> log_of(
    const std::vector<glassbox::LogRecord>& records)
{
    std::vector<glassbox::LogEntry> entries;
    std::uint64_t lsn = 1;
    for (const glassbox::LogRecord& record : records) {
        entries.push_back(glassbox::LogEntry{lsn, static_cast<glassbox::SimTimeUs>(lsn), record});
        ++lsn;
    }
    return entries;
}

void test_crash_and_restart()
{
    // Build a log with: trade 1 committed (no END), trade 2 undecided,
    // trade 3 aborted and ended, trade 4 aborted (no END).
    Coordinator source{config()};
    std::vector<glassbox::Trade> trades;
    for (int i = 0; i < 4; ++i) {
        const Effects e = propose(source, i);
        trades.push_back(std::get<glassbox::Proposed>(e.logs[0]).trade);
    }
    const auto log = log_of({
        glassbox::Proposed{trades[0]},
        glassbox::Proposed{trades[1]},
        glassbox::DecisionCommit{TradeId{1}, NodeId::bank_a, NodeId::bank_b},
        glassbox::Proposed{trades[2]},
        glassbox::DecisionAbort{TradeId{3}, ReasonCode::vote_timeout},
        glassbox::End{TradeId{3}},
        glassbox::Proposed{trades[3]},
        glassbox::DecisionAbort{TradeId{4}, ReasonCode::expired},
    });

    Coordinator coordinator{config()};
    static_cast<void>(propose(coordinator));
    coordinator.on_crash();
    CHECK(coordinator.trades().empty());
    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::none);

    const Effects restart = coordinator.on_restart(log, 1'000);
    CHECK(restart.logs.empty());
    // Trade 1: resend COMMIT to both; trade 4: resend ABORT(EXPIRED) to both.
    CHECK(restart.sends.size() == 4);
    CHECK(is_decision(restart.sends[0].message, NodeId::bank_a,
                      Decision::commit, ReasonCode::none, TradeId{1}));
    CHECK(is_decision(restart.sends[1].message, NodeId::bank_b,
                      Decision::commit, ReasonCode::none, TradeId{1}));
    CHECK(is_decision(restart.sends[2].message, NodeId::bank_a,
                      Decision::abort, ReasonCode::expired, TradeId{4}));
    CHECK(is_decision(restart.sends[3].message, NodeId::bank_b,
                      Decision::abort, ReasonCode::expired, TradeId{4}));
    CHECK(restart.timers.size() == 2);
    CHECK(restart.timers[0].trade_id == TradeId{1});
    CHECK(restart.timers[0].expected_state == TradeState::committing);
    CHECK(restart.timers[0].delay_us == 1'000'000);
    CHECK(restart.timers[1].trade_id == TradeId{4});
    CHECK(restart.timers[1].expected_state == TradeState::aborting);

    CHECK(coordinator.trade_state(TradeId{1}) == TradeState::committing);
    CHECK(coordinator.trade_state(TradeId{2}) == TradeState::none);
    CHECK(coordinator.trade_state(TradeId{3}) == TradeState::done);
    CHECK(coordinator.trade_state(TradeId{4}) == TradeState::aborting);
    CHECK(coordinator.find(TradeId{1})->decision_durable);
    CHECK(coordinator.find(TradeId{3})->abort_reason ==
          ReasonCode::vote_timeout);
    CHECK(coordinator.find(TradeId{4})->abort_reason == ReasonCode::expired);
    CHECK(has_note(restart, "RECOVERY_STEP", "FORGET_UNDECIDED"));
    CHECK(has_note(restart, "RECOVERY_STEP", "RESTORE_DONE"));
    CHECK(has_note(restart, "RECOVERY_STEP", "RESEND_DECISION"));

    // Forgotten trade 2 is now presumed abort.
    const Effects query = coordinator.on_message(
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{2}), 2'000);
    CHECK(is_decision(query.sends[0].message, NodeId::bank_a,
                      Decision::abort, ReasonCode::unknown_trade,
                      TradeId{2}));

    // A new trade never reuses an id seen in the log.
    const Effects next = propose(coordinator, 3'000);
    CHECK(std::get<glassbox::Proposed>(next.logs[0]).trade.trade_id() ==
          TradeId{5});

    // Restart is idempotent.
    Coordinator again{config()};
    static_cast<void>(again.on_restart(log, 1'000));
    Coordinator twice{config()};
    static_cast<void>(twice.on_restart(log, 1'000));
    static_cast<void>(twice.on_restart(log, 1'000));
    CHECK(again.trades() == twice.trades());
    CHECK(again.trades().size() == 3);

    // Replay errors surface as RECOVERY_ERROR notes.
    Coordinator broken{config()};
    const Effects errors = broken.on_restart(
        log_of({glassbox::End{TradeId{8}}}), 0);
    CHECK(has_note(errors, "RECOVERY_ERROR", "DECISION_WITHOUT_PROPOSED"));
    CHECK(broken.trades().empty());
}

// --- Part 2: inside the simulator with scripted participants -------------

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }

    std::vector<glassbox::TraceRecord> records;
};

// Answers PREPARE with a configured vote (or stays silent) and ACKs every
// decision. Keeps no ledger: this isolates the coordinator.
class ScriptedParticipant : public glassbox::Node {
  public:
    ScriptedParticipant(NodeId self, std::optional<Vote> vote,
                        ReasonCode reason = ReasonCode::none)
        : self_(self), vote_(vote), reason_(reason)
    {
    }

    Effects on_message(const Message& message,
                       glassbox::SimTimeUs now) override
    {
        received.push_back(message);
        received_at.push_back(now);
        Effects effects;
        if (message.type == MessageType::prepare && vote_) {
            effects.sends.push_back(glassbox::Outgoing{glassbox::make_vote(
                self_, *message.trade_id, *vote_,
                *vote_ == Vote::no ? reason_ : ReasonCode::none)});
        }
        if (message.type == MessageType::commit ||
            message.type == MessageType::abort) {
            effects.sends.push_back(glassbox::Outgoing{glassbox::make_ack(
                self_, *message.trade_id,
                message.type == MessageType::commit ? Decision::commit
                                                    : Decision::abort)});
        }
        return effects;
    }

    Effects on_timer(const glassbox::TimerEvent&,
                     glassbox::SimTimeUs) override
    {
        return {};
    }

    TradeState trade_state(TradeId) const override
    {
        return TradeState::none;
    }

    void on_crash() override {}

    Effects on_restart(std::span<const glassbox::LogEntry>,
                       glassbox::SimTimeUs) override
    {
        return {};
    }

    [[nodiscard]] int count(MessageType type) const
    {
        int n = 0;
        for (const Message& message : received) {
            n += message.type == type ? 1 : 0;
        }
        return n;
    }

    std::vector<Message> received;
    std::vector<glassbox::SimTimeUs> received_at;

  private:
    NodeId self_;
    std::optional<Vote> vote_;
    ReasonCode reason_;
};

struct Harness {
    CollectorSink sink;
    glassbox::Simulator simulator;
    Coordinator* coordinator = nullptr;
    ScriptedParticipant* buyer = nullptr;
    ScriptedParticipant* seller = nullptr;

    Harness(std::optional<Vote> buyer_vote,
            std::optional<Vote> seller_vote,
            ReasonCode reason = ReasonCode::none)
        : simulator(glassbox::SimulatorConfig{}, sink)
    {
        auto c = std::make_unique<Coordinator>(config());
        coordinator = c.get();
        simulator.set_node(NodeId::coordinator, std::move(c));
        auto a = std::make_unique<ScriptedParticipant>(
            NodeId::bank_a, buyer_vote, reason);
        buyer = a.get();
        simulator.set_node(NodeId::bank_a, std::move(a));
        auto b = std::make_unique<ScriptedParticipant>(
            NodeId::bank_b, seller_vote, reason);
        seller = b.get();
        simulator.set_node(NodeId::bank_b, std::move(b));
        CHECK(simulator
                  .schedule_script(0, glassbox::ScriptActionEvent{
                                          glassbox::ScriptSubmitTrade{
                                              request()}})
                  .has_value());
    }

    [[nodiscard]] std::vector<std::string> coordinator_log() const
    {
        std::vector<std::string> kinds;
        for (const glassbox::LogEntry& entry :
             simulator.durable_log(NodeId::coordinator)) {
            kinds.emplace_back(glassbox::record_kind(entry.record));
        }
        return kinds;
    }

    [[nodiscard]] int count(const std::string& kind,
                            std::optional<std::string> reason =
                                std::nullopt) const
    {
        int n = 0;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == kind && (!reason || record.reason == reason)) {
                ++n;
            }
        }
        return n;
    }

    void run_until(glassbox::SimTimeUs time)
    {
        while (true) {
            const auto next = simulator.events().peek();
            if (!next || next->time_us >= time) {
                return;
            }
            CHECK(simulator.step());
        }
    }
};

void test_integration_commit_with_exact_timing()
{
    Harness h{Vote::yes, Vote::yes};
    h.simulator.run();

    CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK((h.coordinator_log() ==
           std::vector<std::string>{"PROPOSED", "DECISION_COMMIT", "END"}));
    const auto log = h.simulator.durable_log(NodeId::coordinator);
    // PROPOSE at 0 -> PROPOSED durable at 1,000; PREPAREs released 1,100.
    // BANK_B (35 ms away) votes: arrives 1,100+35,000+100+35,000 = 71,200.
    // DECISION_COMMIT durable at 72,200; COMMIT released at 72,300.
    // BANK_B ACK arrives 72,300+35,000+100+35,000 = 142,400; END at 143,400.
    CHECK(log[0].durable_at_us == 1'000);
    CHECK(log[1].durable_at_us == 72'200);
    CHECK(log[2].durable_at_us == 143'400);
    CHECK(log[0].lsn == 1);
    CHECK(log[2].lsn == 3);

    CHECK(h.buyer->count(MessageType::prepare) == 1);
    CHECK(h.buyer->count(MessageType::commit) == 1);
    CHECK(h.seller->count(MessageType::prepare) == 1);
    CHECK(h.seller->count(MessageType::commit) == 1);
    CHECK(h.buyer->received_at[0] == 2'100);
    CHECK(h.seller->received_at[0] == 36'100);
    CHECK(h.buyer->received_at[1] == 73'300);
    CHECK(h.seller->received_at[1] == 107'300);

    // Vote timeout (501,100) and retransmit (1,072,300) fire into a
    // finished trade and are ignored by the state check.
    CHECK(h.count("TIMER_IGNORED", "state_mismatch") == 2);
    CHECK(h.count("TIMER_FIRE") == 0);
    CHECK(h.count("INVARIANT_VIOLATION") == 0);
}

void test_integration_vote_no_abort()
{
    Harness h{Vote::no, Vote::yes, ReasonCode::insufficient_funds};
    h.simulator.run();
    CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK((h.coordinator_log() ==
           std::vector<std::string>{"PROPOSED", "DECISION_ABORT", "END"}));
    const auto log = h.simulator.durable_log(NodeId::coordinator);
    CHECK(std::get<glassbox::DecisionAbort>(log[1].record).reason ==
          ReasonCode::insufficient_funds);
    // Buyer's no arrives at 3,200; DECISION_ABORT durable at 4,200.
    CHECK(log[1].durable_at_us == 4'200);
    CHECK(h.buyer->count(MessageType::abort) == 1);
    CHECK(h.buyer->count(MessageType::commit) == 0);
    CHECK(h.seller->count(MessageType::commit) == 0);
    // Seller gets the broadcast ABORT (arrives 39,300), then its late yes
    // (arrives 71,200, before its ACK at 74,400) is answered with ABORT.
    CHECK(h.seller->count(MessageType::abort) == 2);
    CHECK(h.seller->received_at[1] == 39'300);
    CHECK(h.seller->received_at[2] == 71'200 + 100 + 35'000);
}

void test_integration_vote_timeout_and_late_vote()
{
    // Seller is 35 ms away; shrink the timeout below the round trip.
    CollectorSink sink;
    glassbox::Simulator simulator{glassbox::SimulatorConfig{}, sink};
    auto c = std::make_unique<Coordinator>(
        CoordinatorConfig{50'000, {1'000'000, 8'000'000}});
    Coordinator* coordinator = c.get();
    simulator.set_node(NodeId::coordinator, std::move(c));
    auto a = std::make_unique<ScriptedParticipant>(NodeId::bank_a, Vote::yes);
    auto b = std::make_unique<ScriptedParticipant>(NodeId::bank_b, Vote::yes);
    ScriptedParticipant* seller = b.get();
    simulator.set_node(NodeId::bank_a, std::move(a));
    simulator.set_node(NodeId::bank_b, std::move(b));
    CHECK(simulator
              .schedule_script(0, glassbox::ScriptActionEvent{
                                      glassbox::ScriptSubmitTrade{request()}})
              .has_value());
    simulator.run();

    // Timeout fires at 1,100 + 50,000 = 51,100, before the seller's yes
    // (71,200): false abort. The late yes gets ABORT(VOTE_TIMEOUT) again.
    CHECK(coordinator->trade_state(TradeId{1}) == TradeState::done);
    const auto log = simulator.durable_log(NodeId::coordinator);
    CHECK(log.size() == 3);
    CHECK(std::get<glassbox::DecisionAbort>(log[1].record).reason ==
          ReasonCode::vote_timeout);
    CHECK(log[1].durable_at_us == 52'100);
    CHECK(seller->count(MessageType::abort) == 2);
    for (const Message& message : seller->received) {
        if (message.type == MessageType::abort) {
            CHECK(std::get<glassbox::AbortBody>(message.body).reason ==
                  ReasonCode::vote_timeout);
        }
    }
    int fired = 0;
    for (const glassbox::TraceRecord& record : sink.records) {
        if (record.kind == "TIMER_FIRE" &&
            record.detail == "VOTE_TIMEOUT") {
            ++fired;
            CHECK(record.sim_time_us == 51'100);
        }
    }
    CHECK(fired == 1);
}

void test_integration_retransmit_after_lost_commit()
{
    Harness h{Vote::yes, Vote::yes};
    h.simulator.network().drop_next(NodeId::coordinator, NodeId::bank_b,
                                    MessageType::commit);
    h.simulator.run();
    CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK(h.seller->count(MessageType::commit) == 1);
    // Retransmit fires at 72,300 + 1,000,000, is released 100 us later and
    // reaches BANK_B 35 ms after that; only BANK_B is resent.
    CHECK(h.seller->received_at.back() == 1'072'300 + 100 + 35'000);
    CHECK(h.buyer->count(MessageType::commit) == 1);
    CHECK(h.count("MSG_DROP", "DROP_NEXT") == 1);
    CHECK((h.coordinator_log() ==
           std::vector<std::string>{"PROPOSED", "DECISION_COMMIT", "END"}));
}

void test_integration_on_log_durable_is_called_live()
{
    // Lose the first COMMIT to BANK_A, then have BANK_A ask at 200 ms.
    // The coordinator can only answer if the simulator reported the
    // decision record durable.
    Harness h{Vote::yes, Vote::yes};
    h.simulator.network().drop_next(NodeId::coordinator, NodeId::bank_a,
                                    MessageType::commit);
    h.run_until(200'000);
    CHECK(h.coordinator->find(TradeId{1})->decision_durable);
    Effects query;
    query.sends.push_back(glassbox::Outgoing{
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{1})});
    CHECK(h.simulator.apply_effects(NodeId::bank_a, std::move(query),
                                    h.simulator.now_us())
              .has_value());
    h.simulator.run();
    CHECK(h.buyer->count(MessageType::commit) == 1);
    CHECK(h.buyer->received_at.back() < 1'000'000);
    CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::done);
}

void test_integration_crash_before_and_after_decision_flush()
{
    // Decision is taken at 71,200 and becomes durable at 72,200.
    {
        Harness h{Vote::yes, Vote::yes};
        CHECK(h.simulator
                  .schedule_script(71'500, glassbox::ScriptActionEvent{
                                               glassbox::ScriptCrash{
                                                   NodeId::coordinator}})
                  .has_value());
        CHECK(h.simulator
                  .schedule_script(80'000, glassbox::ScriptActionEvent{
                                               glassbox::ScriptRestart{
                                                   NodeId::coordinator}})
                  .has_value());
        h.simulator.run();
        // Lost before flush: no decision survives, trade forgotten, no
        // COMMIT ever left.
        CHECK((h.coordinator_log() == std::vector<std::string>{"PROPOSED"}));
        CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::none);
        CHECK(h.buyer->count(MessageType::commit) == 0);
        CHECK(h.seller->count(MessageType::commit) == 0);
    }
    {
        Harness h{Vote::yes, Vote::yes};
        CHECK(h.simulator
                  .schedule_script(72'250, glassbox::ScriptActionEvent{
                                               glassbox::ScriptCrash{
                                                   NodeId::coordinator}})
                  .has_value());
        CHECK(h.simulator
                  .schedule_script(80'000, glassbox::ScriptActionEvent{
                                               glassbox::ScriptRestart{
                                                   NodeId::coordinator}})
                  .has_value());
        h.simulator.run();
        // Durable decision, sends lost with the crash (released at 72,300).
        // Restart at 80,000 resends COMMIT; both ACK; END is written.
        CHECK((h.coordinator_log() ==
               std::vector<std::string>{"PROPOSED", "DECISION_COMMIT",
                                        "END"}));
        CHECK(h.coordinator->trade_state(TradeId{1}) == TradeState::done);
        CHECK(h.buyer->count(MessageType::commit) == 1);
        CHECK(h.seller->count(MessageType::commit) == 1);
        CHECK(h.buyer->received_at.back() == 80'000 + 100 + 1'000);
    }
}

} // namespace

int main()
{
    test_config_validation();
    test_propose_valid();
    test_propose_rejected_leaves_no_trace_in_state();
    test_vote_timeout_is_capped_by_deadline();
    test_votes_yes_yes_commit();
    test_vote_no_aborts_with_reason();
    test_late_vote_waits_for_durable_decision();
    test_votes_from_strangers_and_unknown_trades();
    test_acks();
    test_abort_acks_finish_trade();
    test_vote_timeout_timer();
    test_retransmit_timer_and_backoff();
    test_outcome_query();
    test_invalid_messages_are_ignored();
    test_crash_and_restart();
    test_integration_commit_with_exact_timing();
    test_integration_vote_no_abort();
    test_integration_vote_timeout_and_late_vote();
    test_integration_retransmit_after_lost_commit();
    test_integration_on_log_durable_is_called_live();
    test_integration_crash_before_and_after_decision_flush();
    return 0;
}
