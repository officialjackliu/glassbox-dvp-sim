// Task 11: protocol message types, payload completeness, well-formedness
// checks, and routing of every message type through network + simulator.

#include "protocol/messages.hpp"
#include "sim/simulator.hpp"
#include "check.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace {

using glassbox::Decision;
using glassbox::Leg;
using glassbox::Message;
using glassbox::MessageCheck;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::ReasonCode;
using glassbox::TradeId;
using glassbox::Vote;

[[nodiscard]] glassbox::TradeRequest make_request()
{
    return glassbox::TradeRequest{
        NodeId::bank_a,
        NodeId::bank_b,
        "UST_10Y",
        glassbox::Units{25},
        glassbox::Cents{2'487'500},
        30'000'000,
        "client",
    };
}

[[nodiscard]] glassbox::Trade make_trade(TradeId id)
{
    glassbox::TradeIdAllocator allocator;
    if (id.value() > 1) {
        allocator.observe(TradeId{id.value() - 1});
    }
    glassbox::TradeResult result =
        glassbox::validate_and_assign(make_request(), 0, allocator);
    return std::get<glassbox::Trade>(result);
}

void test_builders_are_complete_and_well_formed()
{
    const glassbox::Trade trade = make_trade(TradeId{7});

    const Message propose = glassbox::make_propose(make_request());
    CHECK(propose.msg_id == 0);
    CHECK(propose.type == MessageType::propose);
    CHECK(propose.from == NodeId::coordinator);
    CHECK(propose.to == NodeId::coordinator);
    CHECK(!propose.trade_id.has_value());
    const auto& propose_body = std::get<glassbox::ProposeBody>(propose.body);
    CHECK(propose_body.request.buyer == NodeId::bank_a);
    CHECK(propose_body.request.seller == NodeId::bank_b);
    CHECK(propose_body.request.asset == "UST_10Y");
    CHECK(propose_body.request.qty == glassbox::Units{25});
    CHECK(propose_body.request.cash_cents == glassbox::Cents{2'487'500});
    CHECK(propose_body.request.valid_until_us == 30'000'000);
    CHECK(propose_body.request.proposer == "client");
    CHECK(glassbox::check_message(propose) == MessageCheck::ok);

    const Message buyer_prepare = glassbox::make_prepare(trade, Leg::buyer);
    CHECK(buyer_prepare.type == MessageType::prepare);
    CHECK(buyer_prepare.from == NodeId::coordinator);
    CHECK(buyer_prepare.to == NodeId::bank_a);
    CHECK(buyer_prepare.trade_id == TradeId{7});
    const auto& prepare_body =
        std::get<glassbox::PrepareBody>(buyer_prepare.body);
    CHECK(prepare_body.trade == trade);
    CHECK(prepare_body.leg == Leg::buyer);
    CHECK(glassbox::check_message(buyer_prepare) == MessageCheck::ok);

    const Message seller_prepare = glassbox::make_prepare(trade, Leg::seller);
    CHECK(seller_prepare.to == NodeId::bank_b);
    CHECK(std::get<glassbox::PrepareBody>(seller_prepare.body).leg ==
          Leg::seller);
    CHECK(glassbox::check_message(seller_prepare) == MessageCheck::ok);

    const Message yes = glassbox::make_vote(
        NodeId::bank_b, TradeId{7}, Vote::yes, ReasonCode::none);
    CHECK(yes.type == MessageType::vote);
    CHECK(yes.from == NodeId::bank_b);
    CHECK(yes.to == NodeId::coordinator);
    CHECK(yes.trade_id == TradeId{7});
    CHECK(std::get<glassbox::VoteBody>(yes.body).vote == Vote::yes);
    CHECK(std::get<glassbox::VoteBody>(yes.body).reason == ReasonCode::none);
    CHECK(glassbox::check_message(yes) == MessageCheck::ok);

    const Message no = glassbox::make_vote(
        NodeId::bank_a, TradeId{7}, Vote::no, ReasonCode::insufficient_funds);
    CHECK(std::get<glassbox::VoteBody>(no.body).vote == Vote::no);
    CHECK(std::get<glassbox::VoteBody>(no.body).reason ==
          ReasonCode::insufficient_funds);
    CHECK(glassbox::check_message(no) == MessageCheck::ok);

    const Message commit = glassbox::make_commit(NodeId::bank_c, TradeId{7});
    CHECK(commit.type == MessageType::commit);
    CHECK(commit.from == NodeId::coordinator);
    CHECK(commit.to == NodeId::bank_c);
    CHECK(commit.trade_id == TradeId{7});
    CHECK(std::holds_alternative<glassbox::CommitBody>(commit.body));
    CHECK(glassbox::check_message(commit) == MessageCheck::ok);

    const Message abort = glassbox::make_abort(
        NodeId::bank_a, TradeId{7}, ReasonCode::vote_timeout);
    CHECK(abort.type == MessageType::abort);
    CHECK(abort.to == NodeId::bank_a);
    CHECK(std::get<glassbox::AbortBody>(abort.body).reason ==
          ReasonCode::vote_timeout);
    CHECK(glassbox::check_message(abort) == MessageCheck::ok);

    const Message ack = glassbox::make_ack(
        NodeId::bank_a, TradeId{7}, Decision::abort);
    CHECK(ack.type == MessageType::ack);
    CHECK(ack.from == NodeId::bank_a);
    CHECK(ack.to == NodeId::coordinator);
    CHECK(std::get<glassbox::AckBody>(ack.body).decision == Decision::abort);
    CHECK(glassbox::check_message(ack) == MessageCheck::ok);

    const Message query =
        glassbox::make_outcome_query(NodeId::bank_b, TradeId{7});
    CHECK(query.type == MessageType::outcome_query);
    CHECK(query.from == NodeId::bank_b);
    CHECK(query.to == NodeId::coordinator);
    CHECK(std::holds_alternative<glassbox::OutcomeQueryBody>(query.body));
    CHECK(glassbox::check_message(query) == MessageCheck::ok);

    const Message decided_commit = glassbox::make_decision(
        NodeId::bank_b, TradeId{7}, Decision::commit, ReasonCode::none);
    CHECK(decided_commit.type == MessageType::commit);
    CHECK(decided_commit.to == NodeId::bank_b);
    const Message decided_abort = glassbox::make_decision(
        NodeId::bank_b, TradeId{7}, Decision::abort, ReasonCode::expired);
    CHECK(decided_abort.type == MessageType::abort);
    CHECK(std::get<glassbox::AbortBody>(decided_abort.body).reason ==
          ReasonCode::expired);
}

void test_body_type_and_leg_party()
{
    CHECK(!glassbox::body_type(glassbox::MessageBody{}).has_value());
    CHECK(glassbox::body_type(glassbox::ProposeBody{make_request()}) ==
          MessageType::propose);
    CHECK(glassbox::body_type(
              glassbox::PrepareBody{make_trade(TradeId{1}), Leg::buyer}) ==
          MessageType::prepare);
    CHECK(glassbox::body_type(
              glassbox::VoteBody{Vote::yes, ReasonCode::none}) ==
          MessageType::vote);
    CHECK(glassbox::body_type(glassbox::CommitBody{}) == MessageType::commit);
    CHECK(glassbox::body_type(
              glassbox::AbortBody{ReasonCode::expired}) ==
          MessageType::abort);
    CHECK(glassbox::body_type(glassbox::AckBody{Decision::commit}) ==
          MessageType::ack);
    CHECK(glassbox::body_type(glassbox::OutcomeQueryBody{}) ==
          MessageType::outcome_query);

    const glassbox::Trade trade = make_trade(TradeId{1});
    CHECK(glassbox::leg_party(trade, Leg::buyer) == NodeId::bank_a);
    CHECK(glassbox::leg_party(trade, Leg::seller) == NodeId::bank_b);
}

void test_check_message_rejections()
{
    const glassbox::Trade trade = make_trade(TradeId{3});

    // Missing body.
    {
        Message message = glassbox::make_commit(NodeId::bank_a, TradeId{3});
        message.body = std::monostate{};
        CHECK(glassbox::check_message(message) == MessageCheck::missing_body);
    }
    // Every type paired with a different body is rejected.
    {
        const std::vector<Message> samples{
            glassbox::make_propose(make_request()),
            glassbox::make_prepare(trade, Leg::buyer),
            glassbox::make_vote(NodeId::bank_a, TradeId{3}, Vote::yes,
                                ReasonCode::none),
            glassbox::make_commit(NodeId::bank_a, TradeId{3}),
            glassbox::make_abort(NodeId::bank_a, TradeId{3},
                                 ReasonCode::expired),
            glassbox::make_ack(NodeId::bank_a, TradeId{3}, Decision::commit),
            glassbox::make_outcome_query(NodeId::bank_a, TradeId{3}),
        };
        int mismatches = 0;
        for (const Message& typed : samples) {
            for (const Message& other : samples) {
                if (typed.type == other.type) {
                    continue;
                }
                Message wrong = typed;
                wrong.body = other.body;
                CHECK(glassbox::check_message(wrong) ==
                      MessageCheck::body_type_mismatch);
                ++mismatches;
            }
        }
        CHECK(mismatches == 42);
    }
    // PROPOSE must not carry a trade id; every other type must.
    {
        Message propose = glassbox::make_propose(make_request());
        propose.trade_id = TradeId{1};
        CHECK(glassbox::check_message(propose) ==
              MessageCheck::unexpected_trade_id);

        const std::vector<Message> needs_id{
            glassbox::make_prepare(trade, Leg::buyer),
            glassbox::make_vote(NodeId::bank_a, TradeId{3}, Vote::yes,
                                ReasonCode::none),
            glassbox::make_commit(NodeId::bank_a, TradeId{3}),
            glassbox::make_abort(NodeId::bank_a, TradeId{3},
                                 ReasonCode::expired),
            glassbox::make_ack(NodeId::bank_a, TradeId{3}, Decision::commit),
            glassbox::make_outcome_query(NodeId::bank_a, TradeId{3}),
        };
        for (Message message : needs_id) {
            message.trade_id.reset();
            CHECK(glassbox::check_message(message) ==
                  MessageCheck::missing_trade_id);
        }
    }
    // PREPARE trade id must equal the embedded trade's id (both sides).
    {
        Message prepare = glassbox::make_prepare(trade, Leg::buyer);
        prepare.trade_id = TradeId{4};
        CHECK(glassbox::check_message(prepare) ==
              MessageCheck::trade_id_mismatch);
        prepare.trade_id = TradeId{2};
        CHECK(glassbox::check_message(prepare) ==
              MessageCheck::trade_id_mismatch);
        prepare.trade_id = TradeId{3};
        CHECK(glassbox::check_message(prepare) == MessageCheck::ok);
    }
    // Routes.
    {
        Message propose = glassbox::make_propose(make_request());
        propose.from = NodeId::bank_a;
        CHECK(glassbox::check_message(propose) == MessageCheck::invalid_route);
        propose.from = NodeId::coordinator;
        propose.to = NodeId::bank_a;
        CHECK(glassbox::check_message(propose) == MessageCheck::invalid_route);

        Message prepare = glassbox::make_prepare(trade, Leg::buyer);
        prepare.to = NodeId::bank_b; // seller does not own the buyer leg
        CHECK(glassbox::check_message(prepare) == MessageCheck::invalid_route);
        prepare.to = NodeId::bank_c;
        CHECK(glassbox::check_message(prepare) == MessageCheck::invalid_route);
        prepare.to = NodeId::coordinator;
        CHECK(glassbox::check_message(prepare) == MessageCheck::invalid_route);
        prepare.to = NodeId::bank_a;
        prepare.from = NodeId::bank_b;
        CHECK(glassbox::check_message(prepare) == MessageCheck::invalid_route);

        for (Message decision :
             {glassbox::make_commit(NodeId::bank_a, TradeId{3}),
              glassbox::make_abort(NodeId::bank_a, TradeId{3},
                                   ReasonCode::expired)}) {
            decision.to = NodeId::coordinator;
            CHECK(glassbox::check_message(decision) ==
                  MessageCheck::invalid_route);
            decision.to = NodeId::bank_b;
            decision.from = NodeId::bank_a;
            CHECK(glassbox::check_message(decision) ==
                  MessageCheck::invalid_route);
        }

        for (Message upstream :
             {glassbox::make_vote(NodeId::bank_a, TradeId{3}, Vote::yes,
                                  ReasonCode::none),
              glassbox::make_ack(NodeId::bank_a, TradeId{3},
                                 Decision::commit),
              glassbox::make_outcome_query(NodeId::bank_a, TradeId{3})}) {
            upstream.to = NodeId::bank_b;
            CHECK(glassbox::check_message(upstream) ==
                  MessageCheck::invalid_route);
            upstream.to = NodeId::coordinator;
            upstream.from = NodeId::coordinator;
            CHECK(glassbox::check_message(upstream) ==
                  MessageCheck::invalid_route);
        }
    }
    // Vote reasons: yes requires none, no requires a reason.
    {
        Message yes = glassbox::make_vote(
            NodeId::bank_a, TradeId{3}, Vote::yes, ReasonCode::expired);
        CHECK(glassbox::check_message(yes) ==
              MessageCheck::invalid_vote_reason);
        Message no = glassbox::make_vote(
            NodeId::bank_a, TradeId{3}, Vote::no, ReasonCode::none);
        CHECK(glassbox::check_message(no) ==
              MessageCheck::invalid_vote_reason);
    }
    // Abort reason must be present.
    {
        Message abort = glassbox::make_abort(
            NodeId::bank_a, TradeId{3}, ReasonCode::none);
        CHECK(glassbox::check_message(abort) ==
              MessageCheck::invalid_abort_reason);
    }
    // Check order: earliest defect wins.
    {
        Message message = glassbox::make_vote(
            NodeId::bank_a, TradeId{3}, Vote::yes, ReasonCode::expired);
        message.to = NodeId::bank_b;
        CHECK(glassbox::check_message(message) ==
              MessageCheck::invalid_route);
        message.trade_id.reset();
        CHECK(glassbox::check_message(message) ==
              MessageCheck::missing_trade_id);
        message.body = glassbox::CommitBody{};
        CHECK(glassbox::check_message(message) ==
              MessageCheck::body_type_mismatch);
        message.body = std::monostate{};
        CHECK(glassbox::check_message(message) ==
              MessageCheck::missing_body);
    }
}

void test_string_tables()
{
    CHECK(glassbox::to_string(MessageType::propose) == "PROPOSE");
    CHECK(glassbox::to_string(MessageType::prepare) == "PREPARE");
    CHECK(glassbox::to_string(MessageType::vote) == "VOTE");
    CHECK(glassbox::to_string(MessageType::commit) == "COMMIT");
    CHECK(glassbox::to_string(MessageType::abort) == "ABORT");
    CHECK(glassbox::to_string(MessageType::ack) == "ACK");
    CHECK(glassbox::to_string(MessageType::outcome_query) == "OUTCOME_QUERY");

    CHECK(glassbox::to_string(Vote::yes) == "YES");
    CHECK(glassbox::to_string(Vote::no) == "NO");
    CHECK(glassbox::to_string(Decision::commit) == "COMMIT");
    CHECK(glassbox::to_string(Decision::abort) == "ABORT");
    CHECK(glassbox::to_string(Leg::buyer) == "BUYER");
    CHECK(glassbox::to_string(Leg::seller) == "SELLER");

    CHECK(glassbox::to_string(MessageCheck::ok) == "OK");
    CHECK(glassbox::to_string(MessageCheck::missing_body) == "MISSING_BODY");
    CHECK(glassbox::to_string(MessageCheck::body_type_mismatch) ==
          "BODY_TYPE_MISMATCH");
    CHECK(glassbox::to_string(MessageCheck::missing_trade_id) ==
          "MISSING_TRADE_ID");
    CHECK(glassbox::to_string(MessageCheck::unexpected_trade_id) ==
          "UNEXPECTED_TRADE_ID");
    CHECK(glassbox::to_string(MessageCheck::trade_id_mismatch) ==
          "TRADE_ID_MISMATCH");
    CHECK(glassbox::to_string(MessageCheck::invalid_route) ==
          "INVALID_ROUTE");
    CHECK(glassbox::to_string(MessageCheck::invalid_vote_reason) ==
          "INVALID_VOTE_REASON");
    CHECK(glassbox::to_string(MessageCheck::invalid_abort_reason) ==
          "INVALID_ABORT_REASON");

    CHECK(glassbox::to_string(ReasonCode::none) == "NONE");
    CHECK(glassbox::to_string(ReasonCode::insufficient_funds) ==
          "INSUFFICIENT_FUNDS");
    CHECK(glassbox::to_string(ReasonCode::insufficient_bonds) ==
          "INSUFFICIENT_BONDS");
    CHECK(glassbox::to_string(ReasonCode::account_frozen) ==
          "ACCOUNT_FROZEN");
    CHECK(glassbox::to_string(ReasonCode::expired) == "EXPIRED");
    CHECK(glassbox::to_string(ReasonCode::vote_timeout) == "VOTE_TIMEOUT");
    CHECK(glassbox::to_string(ReasonCode::already_aborted) ==
          "ALREADY_ABORTED");
    CHECK(glassbox::to_string(ReasonCode::unknown_trade) == "UNKNOWN_TRADE");

    using glassbox::TradeState;
    CHECK(glassbox::to_string(TradeState::none) == "NONE");
    CHECK(glassbox::to_string(TradeState::collecting) == "COLLECTING");
    CHECK(glassbox::to_string(TradeState::committing) == "COMMITTING");
    CHECK(glassbox::to_string(TradeState::aborting) == "ABORTING");
    CHECK(glassbox::to_string(TradeState::done) == "DONE");
    CHECK(glassbox::to_string(TradeState::preparing) == "PREPARING");
    CHECK(glassbox::to_string(TradeState::prepared) == "PREPARED");
    CHECK(glassbox::to_string(TradeState::committed) == "COMMITTED");
    CHECK(glassbox::to_string(TradeState::aborted) == "ABORTED");

    using glassbox::TimerKind;
    CHECK(glassbox::to_string(TimerKind::vote_timeout) == "VOTE_TIMEOUT");
    CHECK(glassbox::to_string(TimerKind::retransmit) == "RETRANSMIT");
    CHECK(glassbox::to_string(TimerKind::decision_wait) == "DECISION_WAIT");

    using glassbox::DropReason;
    CHECK(glassbox::to_string(DropReason::random) == "DROP_RANDOM");
    CHECK(glassbox::to_string(DropReason::partition) == "PARTITION");
    CHECK(glassbox::to_string(DropReason::node_down) == "NODE_DOWN");
    CHECK(glassbox::to_string(DropReason::next_message) == "DROP_NEXT");
}

// --- Integration: every message type through network + simulator ---------

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }

    std::vector<glassbox::TraceRecord> records;
};

// Records every delivered message; on a scripted trigger, emits a fixed
// batch of outgoing messages.
class RecordingNode : public glassbox::Node {
  public:
    glassbox::Effects on_message(const Message& message,
                                 glassbox::SimTimeUs) override
    {
        received.push_back(message);
        glassbox::Effects effects;
        if (message.type == MessageType::propose) {
            for (const Message& out : to_send) {
                effects.sends.push_back(glassbox::Outgoing{out});
            }
        }
        return effects;
    }

    glassbox::Effects on_timer(const glassbox::TimerEvent&,
                               glassbox::SimTimeUs) override
    {
        return {};
    }

    glassbox::TradeState trade_state(TradeId) const override
    {
        return glassbox::TradeState::none;
    }

    void on_crash() override {}

    glassbox::Effects on_restart(std::span<const glassbox::LogEntry>,
                                 glassbox::SimTimeUs) override
    {
        return {};
    }

    std::vector<Message> received;
    std::vector<Message> to_send;
};

void test_all_message_types_route_with_bodies_and_unique_ids()
{
    CollectorSink sink;
    glassbox::SimulatorConfig config;
    glassbox::Simulator simulator{config, sink};

    const glassbox::Trade trade = make_trade(TradeId{5});
    auto coordinator = std::make_unique<RecordingNode>();
    RecordingNode* coord = coordinator.get();
    coord->to_send = {
        glassbox::make_prepare(trade, Leg::buyer),
        glassbox::make_commit(NodeId::bank_a, TradeId{5}),
        glassbox::make_abort(NodeId::bank_a, TradeId{5},
                             ReasonCode::insufficient_bonds),
    };
    auto bank_a = std::make_unique<RecordingNode>();
    RecordingNode* a = bank_a.get();
    simulator.set_node(NodeId::coordinator, std::move(coordinator));
    simulator.set_node(NodeId::bank_a, std::move(bank_a));

    // PROPOSE is injected by script; it triggers the coordinator's sends.
    CHECK(simulator.schedule_script(
              0,
              glassbox::ScriptActionEvent{
                  glassbox::ScriptSubmitTrade{make_request()}})
              .has_value());

    // Upstream messages are released by BANK_A at time 10 via apply_effects.
    simulator.run();
    glassbox::Effects upstream;
    upstream.sends.push_back(glassbox::Outgoing{glassbox::make_vote(
        NodeId::bank_a, TradeId{5}, Vote::no, ReasonCode::account_frozen)});
    upstream.sends.push_back(glassbox::Outgoing{
        glassbox::make_ack(NodeId::bank_a, TradeId{5}, Decision::abort)});
    upstream.sends.push_back(glassbox::Outgoing{
        glassbox::make_outcome_query(NodeId::bank_a, TradeId{5})});
    // A node-chosen nonzero msg_id is kept as is.
    upstream.sends.back().message.msg_id = 900;
    CHECK(simulator.apply_effects(NodeId::bank_a, std::move(upstream),
                                  simulator.now_us())
              .has_value());
    simulator.run();

    // Coordinator saw PROPOSE then VOTE, ACK, OUTCOME_QUERY with bodies.
    CHECK(coord->received.size() == 4);
    CHECK(coord->received[0].type == MessageType::propose);
    CHECK(std::get<glassbox::ProposeBody>(coord->received[0].body)
              .request.qty == glassbox::Units{25});
    CHECK(coord->received[1].type == MessageType::vote);
    CHECK(std::get<glassbox::VoteBody>(coord->received[1].body).reason ==
          ReasonCode::account_frozen);
    CHECK(coord->received[2].type == MessageType::ack);
    CHECK(std::get<glassbox::AckBody>(coord->received[2].body).decision ==
          Decision::abort);
    CHECK(coord->received[3].type == MessageType::outcome_query);
    CHECK(coord->received[3].msg_id == 900);

    // BANK_A saw PREPARE, COMMIT, ABORT with intact bodies.
    CHECK(a->received.size() == 3);
    CHECK(a->received[0].type == MessageType::prepare);
    CHECK(std::get<glassbox::PrepareBody>(a->received[0].body).trade ==
          trade);
    CHECK(a->received[1].type == MessageType::commit);
    CHECK(a->received[2].type == MessageType::abort);
    CHECK(std::get<glassbox::AbortBody>(a->received[2].body).reason ==
          ReasonCode::insufficient_bonds);
    for (const Message& message : a->received) {
        CHECK(message.from == NodeId::coordinator);
        CHECK(message.to == NodeId::bank_a);
        CHECK(message.trade_id == TradeId{5});
        CHECK(glassbox::check_message(message) == MessageCheck::ok);
    }
    for (const Message& message : coord->received) {
        CHECK(glassbox::check_message(message) == MessageCheck::ok);
    }

    // Simulator-assigned msg_ids: PROPOSE=1, then sends 2,3,4 in release
    // order, then 5,6 for VOTE and ACK; node-chosen 900 kept.
    CHECK(coord->received[0].msg_id == 1);
    CHECK(a->received[0].msg_id == 2);
    CHECK(a->received[1].msg_id == 3);
    CHECK(a->received[2].msg_id == 4);
    CHECK(coord->received[1].msg_id == 5);
    CHECK(coord->received[2].msg_id == 6);

    // Trace: every MSG_SEND has a unique msg_id and full message context,
    // and every send has exactly one matching MSG_DELIVER.
    std::set<std::uint64_t> send_ids;
    std::map<MessageType, int> sends_by_type;
    int delivers = 0;
    for (const glassbox::TraceRecord& record : sink.records) {
        if (record.kind == "MSG_SEND") {
            CHECK(record.msg_id.has_value());
            CHECK(record.msg_type.has_value());
            CHECK(record.from.has_value());
            CHECK(record.to.has_value());
            CHECK(record.trade_id == TradeId{5});
            CHECK(record.node == record.from);
            CHECK(send_ids.insert(*record.msg_id).second);
            ++sends_by_type[*record.msg_type];
        }
        if (record.kind == "MSG_DELIVER") {
            ++delivers;
        }
    }
    CHECK(send_ids.size() == 6);
    CHECK(delivers == 7); // 6 sends + the injected PROPOSE
    CHECK(sends_by_type[MessageType::prepare] == 1);
    CHECK(sends_by_type[MessageType::commit] == 1);
    CHECK(sends_by_type[MessageType::abort] == 1);
    CHECK(sends_by_type[MessageType::vote] == 1);
    CHECK(sends_by_type[MessageType::ack] == 1);
    CHECK(sends_by_type[MessageType::outcome_query] == 1);
    CHECK(sends_by_type[MessageType::propose] == 0);
}

void test_full_message_overload_keeps_draw_order()
{
    // Same seed: the field overload and the full-message overload must give
    // the same delivery times (one jitter + one drop draw each).
    glassbox::EventQueue field_queue;
    glassbox::DeterministicRng field_rng{99};
    glassbox::NetworkModel field_network{field_rng};
    glassbox::EventQueue full_queue;
    glassbox::DeterministicRng full_rng{99};
    glassbox::NetworkModel full_network{full_rng};
    for (glassbox::NetworkModel* network : {&field_network, &full_network}) {
        network->set_link(NodeId::coordinator, NodeId::bank_c,
                          glassbox::LinkConfig{120'000, 5'000, 0});
    }
    for (int i = 0; i < 20; ++i) {
        const auto field = field_network.schedule_delivery(
            field_queue, 1'000 * i, NodeId::coordinator, NodeId::bank_c,
            MessageType::commit, 0, TradeId{1});
        const auto full = full_network.schedule_delivery(
            full_queue, 1'000 * i,
            glassbox::make_commit(NodeId::bank_c, TradeId{1}));
        CHECK(field.status == full.status);
        CHECK(field.deliver_time_us == full.deliver_time_us);
    }
    // drop_next matches on the full message's type.
    full_network.drop_next(NodeId::coordinator, NodeId::bank_c,
                           MessageType::abort);
    const auto commit = full_network.schedule_delivery(
        full_queue, 50'000, glassbox::make_commit(NodeId::bank_c, TradeId{1}));
    CHECK(commit.delivered);
    const auto abort = full_network.schedule_delivery(
        full_queue, 50'000,
        glassbox::make_abort(NodeId::bank_c, TradeId{1},
                             ReasonCode::expired));
    CHECK(!abort.delivered);
    CHECK(abort.drop_reason == glassbox::DropReason::next_message);
}

void test_drop_trace_carries_reason_and_message()
{
    CollectorSink sink;
    glassbox::SimulatorConfig config;
    glassbox::Simulator simulator{config, sink};
    simulator.network().set_partition(NodeId::coordinator, NodeId::bank_b,
                                      true);
    glassbox::Effects effects;
    effects.sends.push_back(glassbox::Outgoing{
        glassbox::make_commit(NodeId::bank_b, TradeId{2})});
    CHECK(simulator.apply_effects(NodeId::coordinator, std::move(effects), 0)
              .has_value());
    simulator.run();
    int drops = 0;
    for (const glassbox::TraceRecord& record : sink.records) {
        if (record.kind == "MSG_DROP") {
            ++drops;
            CHECK(record.reason == "PARTITION");
            CHECK(record.msg_type == MessageType::commit);
            CHECK(record.from == NodeId::coordinator);
            CHECK(record.to == NodeId::bank_b);
            CHECK(record.trade_id == TradeId{2});
            CHECK(record.msg_id == 1);
            CHECK(record.sim_time_us == 100 + 35'000);
        }
    }
    CHECK(drops == 1);
}

void test_submit_to_down_coordinator_is_dropped()
{
    CollectorSink sink;
    glassbox::SimulatorConfig config;
    glassbox::Simulator simulator{config, sink};
    auto coordinator = std::make_unique<RecordingNode>();
    RecordingNode* coord = coordinator.get();
    simulator.set_node(NodeId::coordinator, std::move(coordinator));
    simulator.set_node_up(NodeId::coordinator, false);
    CHECK(simulator.schedule_script(
              5,
              glassbox::ScriptActionEvent{
                  glassbox::ScriptSubmitTrade{make_request()}})
              .has_value());
    simulator.run();
    CHECK(coord->received.empty());
    CHECK(sink.records.size() == 2);
    CHECK(sink.records[0].kind == "SCRIPT_ACTION");
    CHECK(sink.records[1].kind == "MSG_DROP_NODE_DOWN");
    CHECK(sink.records[1].msg_type == MessageType::propose);
    CHECK(sink.records[1].sim_time_us == 5);
}

} // namespace

int main()
{
    test_builders_are_complete_and_well_formed();
    test_body_type_and_leg_party();
    test_check_message_rejections();
    test_string_tables();
    test_all_message_types_route_with_bodies_and_unique_ids();
    test_full_message_overload_keeps_draw_order();
    test_drop_trace_carries_reason_and_message();
    test_submit_to_down_coordinator_is_dropped();
    return 0;
}
