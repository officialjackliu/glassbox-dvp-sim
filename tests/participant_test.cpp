// Task 13: participant state machine (DESIGN.md 5.4, 6.1-6.4).
// Part 1 calls the pure node directly and asserts exact Effects and ledger.
// Part 2 runs one participant inside the Simulator to check write-ahead
// timing, the PREPARING -> PREPARED transition, and in-doubt queries.

#include "protocol/participant.hpp"
#include "sim/simulator.hpp"
#include "check.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::Decision;
using glassbox::Effects;
using glassbox::Leg;
using glassbox::LocalLedger;
using glassbox::Message;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::Participant;
using glassbox::ParticipantConfig;
using glassbox::ReasonCode;
using glassbox::TimerKind;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::Units;
using glassbox::Vote;

constexpr std::int64_t k_cash = 2'487'500;
constexpr std::int64_t k_qty = 25;
constexpr glassbox::SimTimeUs k_deadline = 30'000'000;

[[nodiscard]] LocalLedger ledger(std::int64_t cash,
                                 std::int64_t bonds,
                                 bool frozen = false)
{
    return *LocalLedger::genesis(Cents{cash}, Units{bonds}, frozen).ledger;
}

[[nodiscard]] Participant participant(NodeId self,
                                      std::int64_t cash = 100'000'000,
                                      std::int64_t bonds = 25'000,
                                      bool frozen = false)
{
    return Participant{ParticipantConfig{
        self, ledger(cash, bonds, frozen), {1'000'000, 8'000'000}}};
}

[[nodiscard]] glassbox::Trade trade(TradeId id,
                                    std::int64_t qty = k_qty,
                                    std::int64_t cash = k_cash,
                                    glassbox::SimTimeUs valid_until =
                                        k_deadline)
{
    glassbox::TradeIdAllocator allocator;
    if (id.value() > 1) {
        allocator.observe(TradeId{id.value() - 1});
    }
    glassbox::TradeRequest request{
        NodeId::bank_a, NodeId::bank_b, "UST_10Y", Units{qty}, Cents{cash},
        valid_until, "client",
    };
    return std::get<glassbox::Trade>(
        glassbox::validate_and_assign(request, 0, allocator));
}

[[nodiscard]] Message prepare_for(NodeId self, const glassbox::Trade& t)
{
    return glassbox::make_prepare(
        t, self == NodeId::bank_a ? Leg::buyer : Leg::seller);
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

[[nodiscard]] bool is_vote(const Message& message,
                           NodeId from,
                           Vote vote,
                           ReasonCode reason,
                           TradeId id = TradeId{1})
{
    const auto* body = std::get_if<glassbox::VoteBody>(&message.body);
    return message.type == MessageType::vote && message.from == from &&
           message.to == NodeId::coordinator && message.trade_id == id &&
           body && body->vote == vote && body->reason == reason;
}

[[nodiscard]] bool is_ack(const Message& message,
                          NodeId from,
                          Decision decision,
                          TradeId id = TradeId{1})
{
    const auto* body = std::get_if<glassbox::AckBody>(&message.body);
    return message.type == MessageType::ack && message.from == from &&
           message.trade_id == id && body && body->decision == decision;
}

[[nodiscard]] bool only_note(const Effects& effects)
{
    return effects.logs.empty() && effects.sends.empty() &&
           effects.timers.empty();
}

// Prepare and mark durable: state PREPARED.
void reach_prepared(Participant& p, const glassbox::Trade& t)
{
    const Effects e = p.on_message(prepare_for(p.self(), t), 0);
    CHECK(e.logs.size() == 1);
    p.on_log_durable(e.logs[0], 1'000);
    CHECK(p.trade_state(t.trade_id()) == TradeState::prepared);
}

void test_config_validation()
{
    bool threw = false;
    try {
        static_cast<void>(participant(NodeId::coordinator));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Participant bad{ParticipantConfig{NodeId::bank_a, ledger(1, 1),
                                          {0, 1}}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    const Participant ok = participant(NodeId::bank_c);
    CHECK(ok.self() == NodeId::bank_c);
    CHECK(ok.trades().empty());
    CHECK(ok.ledger() == ledger(100'000'000, 25'000));
}

void test_prepare_buyer_locks_cash_and_votes_yes()
{
    Participant p = participant(NodeId::bank_a);
    const glassbox::Trade t = trade(TradeId{1});
    const Effects e = p.on_message(prepare_for(NodeId::bank_a, t), 10);

    CHECK(e.logs.size() == 1);
    const auto& prepared = std::get<glassbox::Prepared>(e.logs[0]);
    CHECK(prepared.trade == t);
    CHECK(prepared.leg == Leg::buyer);
    CHECK(e.sends.size() == 1);
    CHECK(is_vote(e.sends[0].message, NodeId::bank_a, Vote::yes,
                  ReasonCode::none));
    CHECK(e.timers.size() == 1);
    CHECK(e.timers[0].kind == TimerKind::decision_wait);
    CHECK(e.timers[0].delay_us == 1'000'000);
    CHECK(e.timers[0].trade_id == TradeId{1});
    CHECK(e.timers[0].expected_state == TradeState::prepared);
    CHECK(e.timers[0].attempt == 0);
    CHECK(has_note(e, "LOCK", "CASH 2487500"));
    CHECK(has_note(e, "STATE_CHANGE", "NONE->PREPARING"));

    CHECK(p.trade_state(TradeId{1}) == TradeState::preparing);
    CHECK(p.ledger().cash_locked() == Cents{k_cash});
    CHECK(p.ledger().cash_total() == Cents{100'000'000});
    CHECK(p.ledger().bonds_locked() == Units{0});
    CHECK(p.ledger().available_cash() == Cents{100'000'000 - k_cash});
}

void test_prepare_seller_locks_bonds()
{
    Participant p = participant(NodeId::bank_b);
    const glassbox::Trade t = trade(TradeId{1});
    const Effects e = p.on_message(prepare_for(NodeId::bank_b, t), 10);
    CHECK(std::get<glassbox::Prepared>(e.logs[0]).leg == Leg::seller);
    CHECK(is_vote(e.sends[0].message, NodeId::bank_b, Vote::yes,
                  ReasonCode::none));
    CHECK(has_note(e, "LOCK", "BONDS 25"));
    CHECK(p.ledger().bonds_locked() == Units{k_qty});
    CHECK(p.ledger().cash_locked() == Cents{0});
}

// Expect a refusal: tombstone + VOTE no(reason), ledger unchanged.
void expect_refusal(Participant& p,
                    const glassbox::Trade& t,
                    glassbox::SimTimeUs now,
                    ReasonCode reason)
{
    const LocalLedger before = p.ledger();
    const Effects e = p.on_message(prepare_for(p.self(), t), now);
    CHECK(e.logs.size() == 1);
    CHECK(std::get<glassbox::Aborted>(e.logs[0]).trade_id == t.trade_id());
    CHECK(e.sends.size() == 1);
    CHECK(is_vote(e.sends[0].message, p.self(), Vote::no, reason,
                  t.trade_id()));
    CHECK(e.timers.empty());
    CHECK(has_note(e, "VOTE_NO", std::string{glassbox::to_string(reason)}));
    CHECK(has_note(e, "STATE_CHANGE", "NONE->ABORTED"));
    CHECK(p.ledger() == before);
    CHECK(p.trade_state(t.trade_id()) == TradeState::aborted);
}

void test_prepare_balance_boundaries()
{
    // Buyer: available == amount passes; amount - 1 fails.
    {
        Participant exact = participant(NodeId::bank_a, k_cash, 0);
        const Effects e =
            exact.on_message(prepare_for(NodeId::bank_a, trade(TradeId{1})), 0);
        CHECK(is_vote(e.sends[0].message, NodeId::bank_a, Vote::yes,
                      ReasonCode::none));
        CHECK(exact.ledger().available_cash() == Cents{0});
    }
    {
        Participant short_by_one = participant(NodeId::bank_a, k_cash - 1, 0);
        expect_refusal(short_by_one, trade(TradeId{1}), 0,
                       ReasonCode::insufficient_funds);
    }
    // Seller: available == qty passes; qty - 1 fails.
    {
        Participant exact = participant(NodeId::bank_b, 0, k_qty);
        const Effects e =
            exact.on_message(prepare_for(NodeId::bank_b, trade(TradeId{1})), 0);
        CHECK(is_vote(e.sends[0].message, NodeId::bank_b, Vote::yes,
                      ReasonCode::none));
        CHECK(exact.ledger().available_bonds() == Units{0});
    }
    {
        Participant short_by_one = participant(NodeId::bank_b, 0, k_qty - 1);
        expect_refusal(short_by_one, trade(TradeId{1}), 0,
                       ReasonCode::insufficient_bonds);
    }
}

void test_prepare_check_order_and_deadline()
{
    const glassbox::Trade t = trade(TradeId{1}, k_qty, k_cash, 1'000);
    // Frozen wins over expired and over insufficient funds.
    {
        Participant p = participant(NodeId::bank_a, 0, 0, true);
        expect_refusal(p, t, 5'000, ReasonCode::account_frozen);
    }
    // Frozen with enough funds and a live deadline still refuses.
    {
        Participant p = participant(NodeId::bank_a, 100'000'000, 0, true);
        expect_refusal(p, t, 0, ReasonCode::account_frozen);
    }
    // Expired wins over insufficient funds.
    {
        Participant p = participant(NodeId::bank_a, 0, 0);
        expect_refusal(p, t, 1'000, ReasonCode::expired);
    }
    // Deadline boundary: now == valid_until is expired, one before is not.
    {
        Participant p = participant(NodeId::bank_a);
        expect_refusal(p, t, 1'000, ReasonCode::expired);
    }
    {
        Participant p = participant(NodeId::bank_a);
        const Effects e = p.on_message(prepare_for(NodeId::bank_a, t), 999);
        CHECK(is_vote(e.sends[0].message, NodeId::bank_a, Vote::yes,
                      ReasonCode::none));
    }
}

void test_duplicate_prepare_before_and_after_durable()
{
    Participant p = participant(NodeId::bank_a);
    const glassbox::Trade t = trade(TradeId{1});
    const Effects first = p.on_message(prepare_for(NodeId::bank_a, t), 0);

    // PREPARING: the yes is queued behind the flush; never send another.
    const Effects early = p.on_message(prepare_for(NodeId::bank_a, t), 10);
    CHECK(only_note(early));
    CHECK(has_note(early, "DUPLICATE_IGNORED", "VOTE_PENDING"));
    CHECK(p.ledger().cash_locked() == Cents{k_cash}); // not locked twice

    // Unrelated durable records do not complete the prepare.
    p.on_log_durable(glassbox::Aborted{TradeId{1}}, 20);
    p.on_log_durable(glassbox::Prepared{trade(TradeId{2}), Leg::buyer}, 20);
    CHECK(p.trade_state(TradeId{1}) == TradeState::preparing);

    p.on_log_durable(first.logs[0], 1'000);
    CHECK(p.trade_state(TradeId{1}) == TradeState::prepared);

    // PREPARED: resend yes, nothing else.
    const Effects again = p.on_message(prepare_for(NodeId::bank_a, t), 2'000);
    CHECK(again.logs.empty());
    CHECK(again.timers.empty());
    CHECK(again.sends.size() == 1);
    CHECK(is_vote(again.sends[0].message, NodeId::bank_a, Vote::yes,
                  ReasonCode::none));
    CHECK(p.ledger().cash_locked() == Cents{k_cash});
}

void test_prepare_after_final_states()
{
    // ABORTED: VOTE no(ALREADY_ABORTED), no new lock.
    Participant p = participant(NodeId::bank_a, 0, 0);
    const glassbox::Trade t = trade(TradeId{1});
    expect_refusal(p, t, 0, ReasonCode::insufficient_funds);
    const Effects late = p.on_message(prepare_for(NodeId::bank_a, t), 10);
    CHECK(late.logs.empty());
    CHECK(late.sends.size() == 1);
    CHECK(is_vote(late.sends[0].message, NodeId::bank_a, Vote::no,
                  ReasonCode::already_aborted));
    CHECK(p.trade_state(TradeId{1}) == TradeState::aborted);

    // COMMITTED: still truthfully yes; ledger untouched.
    Participant c = participant(NodeId::bank_a);
    reach_prepared(c, t);
    static_cast<void>(c.on_message(
        glassbox::make_commit(NodeId::bank_a, TradeId{1}), 3'000));
    const LocalLedger after_commit = c.ledger();
    const Effects dup = c.on_message(prepare_for(NodeId::bank_a, t), 4'000);
    CHECK(dup.logs.empty());
    CHECK(is_vote(dup.sends[0].message, NodeId::bank_a, Vote::yes,
                  ReasonCode::none));
    CHECK(c.ledger() == after_commit);
}

void test_commit_applies_each_leg()
{
    const glassbox::Trade t = trade(TradeId{1});
    {
        Participant buyer = participant(NodeId::bank_a);
        reach_prepared(buyer, t);
        const Effects e = buyer.on_message(
            glassbox::make_commit(NodeId::bank_a, TradeId{1}), 3'000);
        CHECK(e.logs.size() == 1);
        const auto& committed = std::get<glassbox::Committed>(e.logs[0]);
        CHECK(committed.trade_id == TradeId{1});
        CHECK(committed.leg == Leg::buyer);
        CHECK(e.sends.size() == 1);
        CHECK(is_ack(e.sends[0].message, NodeId::bank_a, Decision::commit));
        CHECK(e.timers.empty());
        CHECK(has_note(e, "APPLY", "CASH -2487500 BONDS +25"));
        CHECK(has_note(e, "STATE_CHANGE", "PREPARED->COMMITTED"));
        CHECK(buyer.trade_state(TradeId{1}) == TradeState::committed);
        CHECK(buyer.ledger().cash_total() == Cents{100'000'000 - k_cash});
        CHECK(buyer.ledger().cash_locked() == Cents{0});
        CHECK(buyer.ledger().bonds_total() == Units{25'000 + k_qty});
        CHECK(buyer.ledger().bonds_locked() == Units{0});

        // Duplicate COMMIT: ACK again, ledger unchanged.
        const LocalLedger before = buyer.ledger();
        const Effects dup = buyer.on_message(
            glassbox::make_commit(NodeId::bank_a, TradeId{1}), 4'000);
        CHECK(dup.logs.empty());
        CHECK(dup.sends.size() == 1);
        CHECK(is_ack(dup.sends[0].message, NodeId::bank_a, Decision::commit));
        CHECK(buyer.ledger() == before);
    }
    {
        Participant seller = participant(NodeId::bank_b);
        reach_prepared(seller, t);
        const Effects e = seller.on_message(
            glassbox::make_commit(NodeId::bank_b, TradeId{1}), 3'000);
        CHECK(std::get<glassbox::Committed>(e.logs[0]).leg == Leg::seller);
        CHECK(is_ack(e.sends[0].message, NodeId::bank_b, Decision::commit));
        CHECK(has_note(e, "APPLY", "BONDS -25 CASH +2487500"));
        CHECK(seller.ledger().bonds_total() == Units{25'000 - k_qty});
        CHECK(seller.ledger().bonds_locked() == Units{0});
        CHECK(seller.ledger().cash_total() == Cents{100'000'000 + k_cash});
        CHECK(seller.ledger().cash_locked() == Cents{0});
    }
}

void expect_commit_violation(Participant& p, const std::string& detail)
{
    const LocalLedger before = p.ledger();
    const TradeState state_before = p.trade_state(TradeId{1});
    const Effects e = p.on_message(
        glassbox::make_commit(p.self(), TradeId{1}), 5'000);
    CHECK(only_note(e));
    CHECK(e.notes.size() == 1);
    CHECK(e.notes[0].kind == "INVARIANT_VIOLATION");
    CHECK(e.notes[0].detail == detail);
    CHECK(e.notes[0].trade_id == TradeId{1});
    CHECK(e.notes[0].handler == "Participant::on_commit");
    CHECK(p.ledger() == before);
    CHECK(p.trade_state(TradeId{1}) == state_before);
}

void test_commit_in_illegal_states()
{
    const glassbox::Trade t = trade(TradeId{1});
    {
        Participant p = participant(NodeId::bank_a);
        expect_commit_violation(p, "COMMIT_IN_NONE");
        CHECK(p.trades().empty());
    }
    {
        Participant p = participant(NodeId::bank_a);
        static_cast<void>(p.on_message(prepare_for(NodeId::bank_a, t), 0));
        expect_commit_violation(p, "COMMIT_IN_PREPARING");
        CHECK(p.ledger().cash_locked() == Cents{k_cash});
    }
    {
        Participant p = participant(NodeId::bank_a, 0, 0);
        expect_refusal(p, t, 0, ReasonCode::insufficient_funds);
        expect_commit_violation(p, "COMMIT_IN_ABORTED");
    }
}

void test_abort_paths()
{
    const glassbox::Trade t = trade(TradeId{1});
    const LocalLedger genesis = ledger(100'000'000, 25'000);
    // PREPARED -> release lock, log ABORTED, ACK.
    {
        Participant p = participant(NodeId::bank_b);
        reach_prepared(p, t);
        const Effects e = p.on_message(
            glassbox::make_abort(NodeId::bank_b, TradeId{1},
                                 ReasonCode::insufficient_funds),
            3'000);
        CHECK(e.logs.size() == 1);
        CHECK(std::get<glassbox::Aborted>(e.logs[0]).trade_id == TradeId{1});
        CHECK(e.sends.size() == 1);
        CHECK(is_ack(e.sends[0].message, NodeId::bank_b, Decision::abort));
        CHECK(has_note(e, "UNLOCK", "BONDS 25"));
        CHECK(has_note(e, "STATE_CHANGE", "PREPARED->ABORTED"));
        CHECK(p.ledger() == genesis);
        CHECK(p.trade_state(TradeId{1}) == TradeState::aborted);

        // Duplicate ABORT: ACK only.
        const Effects dup = p.on_message(
            glassbox::make_abort(NodeId::bank_b, TradeId{1},
                                 ReasonCode::insufficient_funds),
            4'000);
        CHECK(dup.logs.empty());
        CHECK(dup.sends.size() == 1);
        CHECK(is_ack(dup.sends[0].message, NodeId::bank_b, Decision::abort));
        CHECK(p.ledger() == genesis);

        // A late durable notification never resurrects the trade.
        p.on_log_durable(glassbox::Prepared{t, Leg::seller}, 5'000);
        CHECK(p.trade_state(TradeId{1}) == TradeState::aborted);
    }
    // PREPARING -> same cleanup.
    {
        Participant p = participant(NodeId::bank_a);
        static_cast<void>(p.on_message(prepare_for(NodeId::bank_a, t), 0));
        const Effects e = p.on_message(
            glassbox::make_abort(NodeId::bank_a, TradeId{1},
                                 ReasonCode::vote_timeout),
            500);
        CHECK(has_note(e, "UNLOCK", "CASH 2487500"));
        CHECK(has_note(e, "STATE_CHANGE", "PREPARING->ABORTED"));
        CHECK(p.ledger() == genesis);
        p.on_log_durable(glassbox::Prepared{t, Leg::buyer}, 1'000);
        CHECK(p.trade_state(TradeId{1}) == TradeState::aborted);
    }
    // Never seen -> tombstone, ACK; a delayed PREPARE is then refused.
    {
        Participant p = participant(NodeId::bank_a);
        const Effects e = p.on_message(
            glassbox::make_abort(NodeId::bank_a, TradeId{1},
                                 ReasonCode::vote_timeout),
            0);
        CHECK(e.logs.size() == 1);
        CHECK(std::get<glassbox::Aborted>(e.logs[0]).trade_id == TradeId{1});
        CHECK(is_ack(e.sends[0].message, NodeId::bank_a, Decision::abort));
        CHECK(has_note(e, "STATE_CHANGE", "NONE->ABORTED"));
        CHECK(p.trade_state(TradeId{1}) == TradeState::aborted);
        CHECK(!p.find(TradeId{1})->trade.has_value());
        CHECK(p.ledger() == genesis);

        const Effects late = p.on_message(prepare_for(NodeId::bank_a, t), 10);
        CHECK(late.logs.empty());
        CHECK(is_vote(late.sends[0].message, NodeId::bank_a, Vote::no,
                      ReasonCode::already_aborted));
        CHECK(p.ledger() == genesis);
    }
    // COMMITTED -> invariant violation, nothing changes.
    {
        Participant p = participant(NodeId::bank_a);
        reach_prepared(p, t);
        static_cast<void>(p.on_message(
            glassbox::make_commit(NodeId::bank_a, TradeId{1}), 3'000));
        const LocalLedger before = p.ledger();
        const Effects e = p.on_message(
            glassbox::make_abort(NodeId::bank_a, TradeId{1},
                                 ReasonCode::vote_timeout),
            4'000);
        CHECK(only_note(e));
        CHECK(has_note(e, "INVARIANT_VIOLATION", "ABORT_AFTER_COMMIT"));
        CHECK(p.ledger() == before);
        CHECK(p.trade_state(TradeId{1}) == TradeState::committed);
    }
}

[[nodiscard]] glassbox::TimerEvent decision_wait(std::uint32_t attempt,
                                                 TradeId id = TradeId{1})
{
    return glassbox::TimerEvent{NodeId::bank_a, 0, TimerKind::decision_wait,
                                id, TradeState::prepared, attempt};
}

void test_decision_wait_queries_and_backs_off()
{
    Participant p = participant(NodeId::bank_a);
    reach_prepared(p, trade(TradeId{1}));
    const LocalLedger before = p.ledger();

    const Effects first = p.on_timer(decision_wait(0), 1'001'100);
    CHECK(first.logs.empty());
    CHECK(first.sends.size() == 1);
    const Message& query = first.sends[0].message;
    CHECK(query.type == MessageType::outcome_query);
    CHECK(query.from == NodeId::bank_a);
    CHECK(query.to == NodeId::coordinator);
    CHECK(query.trade_id == TradeId{1});
    CHECK(first.timers.size() == 1);
    CHECK(first.timers[0].kind == TimerKind::decision_wait);
    CHECK(first.timers[0].attempt == 1);
    CHECK(first.timers[0].delay_us == 2'000'000);
    CHECK(first.timers[0].expected_state == TradeState::prepared);
    CHECK(has_note(first, "OUTCOME_QUERY", "0"));
    // Never aborts on its own: lock and state are kept.
    CHECK(p.trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(p.ledger() == before);

    const std::vector<std::pair<std::uint32_t, glassbox::SimTimeUs>> steps{
        {1, 4'000'000}, {2, 8'000'000}, {3, 8'000'000}, {60, 8'000'000}};
    for (const auto& [attempt, delay] : steps) {
        const Effects e = p.on_timer(decision_wait(attempt), 0);
        CHECK(e.timers[0].attempt == attempt + 1);
        CHECK(e.timers[0].delay_us == delay);
    }
    const Effects saturated = p.on_timer(
        decision_wait(std::numeric_limits<std::uint32_t>::max()), 0);
    CHECK(saturated.timers[0].attempt ==
          std::numeric_limits<std::uint32_t>::max());

    // Not PREPARED (unknown, or already committed): nothing.
    CHECK(only_note(p.on_timer(decision_wait(0, TradeId{9}), 0)));
    static_cast<void>(p.on_message(
        glassbox::make_commit(NodeId::bank_a, TradeId{1}), 10));
    const Effects after = p.on_timer(decision_wait(0), 20);
    CHECK(after.sends.empty());
    CHECK(after.timers.empty());
    // Other timer kinds and timers without a trade are ignored.
    glassbox::TimerEvent retransmit = decision_wait(0);
    retransmit.kind = TimerKind::retransmit;
    CHECK(p.on_timer(retransmit, 0).sends.empty());
    glassbox::TimerEvent no_id = decision_wait(0);
    no_id.trade_id.reset();
    CHECK(p.on_timer(no_id, 0).sends.empty());
}

void test_invalid_messages_are_ignored()
{
    Participant p = participant(NodeId::bank_a);
    const glassbox::Trade t = trade(TradeId{1});
    // Well-formed but addressed to BANK_B.
    const Effects wrong = p.on_message(
        glassbox::make_prepare(t, Leg::seller), 0);
    CHECK(only_note(wrong));
    CHECK(has_note(wrong, "INVALID_MESSAGE", "WRONG_RECIPIENT"));
    CHECK(p.trades().empty());
    // Malformed: ABORT without a reason.
    const Effects malformed = p.on_message(
        glassbox::make_abort(NodeId::bank_a, TradeId{1}, ReasonCode::none),
        0);
    CHECK(only_note(malformed));
    CHECK(has_note(malformed, "INVALID_MESSAGE", "INVALID_ABORT_REASON"));
    CHECK(p.trades().empty());
    // A VOTE delivered to a participant (route check fails).
    const Effects vote = p.on_message(
        glassbox::make_vote(NodeId::bank_b, TradeId{1}, Vote::yes,
                            ReasonCode::none),
        0);
    CHECK(only_note(vote));
    CHECK(p.ledger() == ledger(100'000'000, 25'000));
}

void test_concurrent_trades_contend_for_bonds()
{
    // Seller can cover one 25-unit trade out of 40 bonds, not two.
    Participant p = participant(NodeId::bank_b, 0, 40);
    const Effects first =
        p.on_message(prepare_for(NodeId::bank_b, trade(TradeId{1})), 0);
    CHECK(is_vote(first.sends[0].message, NodeId::bank_b, Vote::yes,
                  ReasonCode::none));
    const Effects second =
        p.on_message(prepare_for(NodeId::bank_b, trade(TradeId{2})), 1);
    CHECK(is_vote(second.sends[0].message, NodeId::bank_b, Vote::no,
                  ReasonCode::insufficient_bonds, TradeId{2}));
    CHECK(p.ledger().bonds_locked() == Units{25});
    // Releasing the first frees the bonds for a third trade.
    static_cast<void>(p.on_message(
        glassbox::make_abort(NodeId::bank_b, TradeId{1},
                             ReasonCode::vote_timeout),
        2));
    const Effects third =
        p.on_message(prepare_for(NodeId::bank_b, trade(TradeId{3})), 3);
    CHECK(is_vote(third.sends[0].message, NodeId::bank_b, Vote::yes,
                  ReasonCode::none, TradeId{3}));
    CHECK(p.ledger().bonds_locked() == Units{25});
}

[[nodiscard]] std::vector<glassbox::LogEntry> log_of(
    const std::vector<glassbox::LogRecord>& records)
{
    std::vector<glassbox::LogEntry> entries;
    std::uint64_t lsn = 1;
    for (const glassbox::LogRecord& record : records) {
        entries.push_back(glassbox::LogEntry{
            lsn, static_cast<glassbox::SimTimeUs>(lsn * 10), record});
        ++lsn;
    }
    return entries;
}

void test_crash_and_restart()
{
    const glassbox::Trade t1 = trade(TradeId{1});
    const glassbox::Trade t2 = trade(TradeId{2}, 10, 1'000);
    const glassbox::Trade t3 = trade(TradeId{3}, 5, 500);
    const auto log = log_of({
        glassbox::Prepared{t1, Leg::buyer},
        glassbox::Committed{TradeId{1}, Leg::buyer},
        glassbox::Prepared{t2, Leg::buyer},
        glassbox::Prepared{t3, Leg::buyer},
        glassbox::Aborted{TradeId{3}},
        glassbox::Aborted{TradeId{4}},
    });

    Participant p = participant(NodeId::bank_a);
    reach_prepared(p, t1);
    p.on_crash();
    CHECK(p.trades().empty());
    CHECK(p.ledger() == ledger(100'000'000, 25'000));

    const Effects e = p.on_restart(log, 50'000);
    CHECK(e.logs.empty());
    // Only the in-doubt trade 2 queries, immediately and with a timer.
    CHECK(e.sends.size() == 1);
    CHECK(e.sends[0].message.type == MessageType::outcome_query);
    CHECK(e.sends[0].message.trade_id == TradeId{2});
    CHECK(e.timers.size() == 1);
    CHECK(e.timers[0].trade_id == TradeId{2});
    CHECK(e.timers[0].kind == TimerKind::decision_wait);
    CHECK(e.timers[0].attempt == 0);
    CHECK(e.timers[0].delay_us == 1'000'000);
    CHECK(has_note(e, "RECOVERY_STEP", "IN_DOUBT_QUERY"));

    CHECK(p.trade_state(TradeId{1}) == TradeState::committed);
    CHECK(p.trade_state(TradeId{2}) == TradeState::prepared);
    CHECK(p.trade_state(TradeId{3}) == TradeState::aborted);
    CHECK(p.trade_state(TradeId{4}) == TradeState::aborted);
    CHECK(p.find(TradeId{2})->trade == t2);
    // Ledger: t1 applied, t2 locked, t3 released.
    CHECK(p.ledger().cash_total() == Cents{100'000'000 - k_cash});
    CHECK(p.ledger().bonds_total() == Units{25'000 + k_qty});
    CHECK(p.ledger().cash_locked() == Cents{1'000});

    // The restored in-doubt trade completes normally.
    const Effects commit = p.on_message(
        glassbox::make_commit(NodeId::bank_a, TradeId{2}), 60'000);
    CHECK(is_ack(commit.sends[0].message, NodeId::bank_a, Decision::commit,
                 TradeId{2}));
    CHECK(p.ledger().cash_locked() == Cents{0});
    CHECK(p.ledger().cash_total() == Cents{100'000'000 - k_cash - 1'000});

    // Idempotent: restarting twice equals restarting once.
    Participant once = participant(NodeId::bank_a);
    static_cast<void>(once.on_restart(log, 0));
    Participant twice = participant(NodeId::bank_a);
    static_cast<void>(twice.on_restart(log, 0));
    static_cast<void>(twice.on_restart(log, 0));
    CHECK(once.trades() == twice.trades());
    CHECK(once.ledger() == twice.ledger());

    // Replay errors surface as notes; the bad record is skipped.
    Participant broken = participant(NodeId::bank_a);
    const Effects errors = broken.on_restart(
        log_of({glassbox::Committed{TradeId{7}, Leg::buyer}}), 0);
    CHECK(has_note(errors, "RECOVERY_ERROR", "COMMITTED_WITHOUT_PREPARED"));
    CHECK(broken.trades().empty());
    CHECK(broken.ledger() == ledger(100'000'000, 25'000));
}

// --- Part 2: one participant inside the simulator -------------------------

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }

    std::vector<glassbox::TraceRecord> records;
};

void test_simulator_write_ahead_and_in_doubt_queries()
{
    // No coordinator node: the participant's vote and queries reach nobody
    // and it must stay PREPARED, holding its lock, asking with backoff.
    CollectorSink sink;
    glassbox::Simulator simulator{glassbox::SimulatorConfig{}, sink};
    auto node = std::make_unique<Participant>(ParticipantConfig{
        NodeId::bank_a, ledger(100'000'000, 0), {1'000'000, 8'000'000}});
    Participant* bank_a = node.get();
    simulator.set_node(NodeId::bank_a, std::move(node));

    Effects inject;
    inject.sends.push_back(
        glassbox::Outgoing{glassbox::make_prepare(trade(TradeId{1}),
                                                  Leg::buyer)});
    CHECK(simulator.apply_effects(NodeId::coordinator, std::move(inject), 0)
              .has_value());

    // PREPARE released at 100, arrives at 1,100. PREPARED durable at 2,100,
    // vote released at 2,200. Stop just before the flush.
    while (simulator.events().peek() &&
           simulator.events().peek()->time_us < 2'100) {
        CHECK(simulator.step());
    }
    CHECK(bank_a->trade_state(TradeId{1}) == TradeState::preparing);
    CHECK(simulator.durable_log(NodeId::bank_a).empty());
    while (simulator.events().peek() &&
           simulator.events().peek()->time_us <= 2'100) {
        CHECK(simulator.step());
    }
    CHECK(bank_a->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(simulator.durable_log(NodeId::bank_a).size() == 1);
    CHECK(simulator.durable_log(NodeId::bank_a)[0].durable_at_us == 2'100);

    // Run until 20 s: queries at 2,200 + 1 s, + 2 s, + 4 s, + 8 s.
    while (simulator.events().peek() &&
           simulator.events().peek()->time_us < 20'000'000) {
        CHECK(simulator.step());
    }
    std::vector<glassbox::SimTimeUs> vote_sends;
    std::vector<glassbox::SimTimeUs> query_sends;
    for (const glassbox::TraceRecord& record : sink.records) {
        if (record.kind == "MSG_SEND" && record.node == NodeId::bank_a) {
            if (record.msg_type == MessageType::vote) {
                vote_sends.push_back(record.sim_time_us);
            }
            if (record.msg_type == MessageType::outcome_query) {
                query_sends.push_back(record.sim_time_us);
            }
        }
    }
    CHECK(vote_sends.size() == 1);
    CHECK(vote_sends[0] == 2'200); // strictly after the PREPARED flush
    CHECK((query_sends == std::vector<glassbox::SimTimeUs>{
                              1'002'300, 3'002'400, 7'002'500,
                              15'002'600}));
    CHECK(bank_a->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(bank_a->ledger().cash_locked() == Cents{k_cash});
}

void test_simulator_crash_before_prepare_flush_loses_lock()
{
    // Crash between PREPARE handling (1,100) and its flush (2,100): no
    // record, no vote, and after restart no lock and no trade.
    CollectorSink sink;
    glassbox::Simulator simulator{glassbox::SimulatorConfig{}, sink};
    auto node = std::make_unique<Participant>(ParticipantConfig{
        NodeId::bank_a, ledger(100'000'000, 0), {1'000'000, 8'000'000}});
    Participant* bank_a = node.get();
    simulator.set_node(NodeId::bank_a, std::move(node));
    Effects inject;
    inject.sends.push_back(
        glassbox::Outgoing{glassbox::make_prepare(trade(TradeId{1}),
                                                  Leg::buyer)});
    CHECK(simulator.apply_effects(NodeId::coordinator, std::move(inject), 0)
              .has_value());
    CHECK(simulator
              .schedule_script(1'500, glassbox::ScriptActionEvent{
                                          glassbox::ScriptCrash{
                                              NodeId::bank_a}})
              .has_value());
    CHECK(simulator
              .schedule_script(5'000, glassbox::ScriptActionEvent{
                                          glassbox::ScriptRestart{
                                              NodeId::bank_a}})
              .has_value());
    simulator.run();
    CHECK(simulator.durable_log(NodeId::bank_a).empty());
    CHECK(bank_a->trades().empty());
    CHECK(bank_a->ledger() == ledger(100'000'000, 0));
    for (const glassbox::TraceRecord& record : sink.records) {
        CHECK(!(record.kind == "MSG_SEND" && record.node == NodeId::bank_a));
    }
}

} // namespace

int main()
{
    test_config_validation();
    test_prepare_buyer_locks_cash_and_votes_yes();
    test_prepare_seller_locks_bonds();
    test_prepare_balance_boundaries();
    test_prepare_check_order_and_deadline();
    test_duplicate_prepare_before_and_after_durable();
    test_prepare_after_final_states();
    test_commit_applies_each_leg();
    test_commit_in_illegal_states();
    test_abort_paths();
    test_decision_wait_queries_and_backs_off();
    test_invalid_messages_are_ignored();
    test_concurrent_trades_contend_for_bonds();
    test_crash_and_restart();
    test_simulator_write_ahead_and_in_doubt_queries();
    test_simulator_crash_before_prepare_flush_loses_lock();
    return 0;
}
