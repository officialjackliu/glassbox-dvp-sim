// Message builders, the well-formedness check run on every received message,
// and the short body descriptions shown in the trace.

#include "protocol/messages.hpp"

#include <type_traits>
#include <utility>

namespace glassbox {

namespace {

[[nodiscard]] constexpr bool is_bank(NodeId node) noexcept
{
    return node == NodeId::bank_a || node == NodeId::bank_b ||
           node == NodeId::bank_c;
}

[[nodiscard]] constexpr bool coordinator_to_bank(
    const Message& message) noexcept
{
    return message.from == NodeId::coordinator && is_bank(message.to);
}

[[nodiscard]] constexpr bool bank_to_coordinator(
    const Message& message) noexcept
{
    return is_bank(message.from) && message.to == NodeId::coordinator;
}

[[nodiscard]] Message make_message(NodeId from,
                                   NodeId to,
                                   MessageType type,
                                   std::optional<TradeId> trade_id,
                                   MessageBody body)
{
    return Message{
        0,
        from,
        to,
        type,
        trade_id,
        std::move(body),
    };
}

} // namespace

std::optional<MessageType> body_type(const MessageBody& body) noexcept
{
    return std::visit(
        [](const auto& alternative) -> std::optional<MessageType> {
            using Body = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<Body, ProposeBody>) {
                return MessageType::propose;
            } else if constexpr (std::is_same_v<Body, PrepareBody>) {
                return MessageType::prepare;
            } else if constexpr (std::is_same_v<Body, VoteBody>) {
                return MessageType::vote;
            } else if constexpr (std::is_same_v<Body, CommitBody>) {
                return MessageType::commit;
            } else if constexpr (std::is_same_v<Body, AbortBody>) {
                return MessageType::abort;
            } else if constexpr (std::is_same_v<Body, AckBody>) {
                return MessageType::ack;
            } else if constexpr (std::is_same_v<Body, OutcomeQueryBody>) {
                return MessageType::outcome_query;
            } else {
                return std::nullopt;
            }
        },
        body);
}

std::optional<std::string> describe_body(const MessageBody& body)
{
    if (const auto* propose = std::get_if<ProposeBody>(&body)) {
        std::string text{to_string(propose->request.buyer)};
        text += "<-";
        text += to_string(propose->request.seller);
        return text;
    }
    if (const auto* prepare = std::get_if<PrepareBody>(&body)) {
        return std::string{to_string(prepare->leg)};
    }
    if (const auto* vote = std::get_if<VoteBody>(&body)) {
        if (vote->vote == Vote::yes) {
            return std::string{"YES"};
        }
        return "NO:" + std::string{to_string(vote->reason)};
    }
    if (const auto* abort = std::get_if<AbortBody>(&body)) {
        return std::string{to_string(abort->reason)};
    }
    if (const auto* ack = std::get_if<AckBody>(&body)) {
        return std::string{to_string(ack->decision)};
    }
    return std::nullopt;
}

NodeId leg_party(const Trade& trade, Leg leg) noexcept
{
    return leg == Leg::buyer ? trade.buyer() : trade.seller();
}

MessageCheck check_message(const Message& message) noexcept
{
    const auto encoded = body_type(message.body);
    if (!encoded) {
        return MessageCheck::missing_body;
    }
    if (*encoded != message.type) {
        return MessageCheck::body_type_mismatch;
    }

    if (message.type == MessageType::propose) {
        if (message.trade_id) {
            return MessageCheck::unexpected_trade_id;
        }
    } else if (!message.trade_id) {
        return MessageCheck::missing_trade_id;
    }

    const auto* prepare = std::get_if<PrepareBody>(&message.body);
    if (prepare && prepare->trade.trade_id() != *message.trade_id) {
        return MessageCheck::trade_id_mismatch;
    }

    switch (message.type) {
    case MessageType::propose:
        if (message.from != NodeId::coordinator ||
            message.to != NodeId::coordinator) {
            return MessageCheck::invalid_route;
        }
        break;
    case MessageType::prepare:
        if (!coordinator_to_bank(message) ||
            leg_party(prepare->trade, prepare->leg) != message.to) {
            return MessageCheck::invalid_route;
        }
        break;
    case MessageType::commit:
    case MessageType::abort:
        if (!coordinator_to_bank(message)) {
            return MessageCheck::invalid_route;
        }
        break;
    case MessageType::vote:
    case MessageType::ack:
    case MessageType::outcome_query:
        if (!bank_to_coordinator(message)) {
            return MessageCheck::invalid_route;
        }
        break;
    }

    if (const auto* vote = std::get_if<VoteBody>(&message.body)) {
        const bool reason_ok =
            vote->vote == Vote::yes
                ? vote->reason == ReasonCode::none
                : vote->reason != ReasonCode::none;
        if (!reason_ok) {
            return MessageCheck::invalid_vote_reason;
        }
    }
    if (const auto* abort = std::get_if<AbortBody>(&message.body)) {
        if (abort->reason == ReasonCode::none) {
            return MessageCheck::invalid_abort_reason;
        }
    }
    return MessageCheck::ok;
}

Message make_propose(TradeRequest request)
{
    return make_message(NodeId::coordinator,
                        NodeId::coordinator,
                        MessageType::propose,
                        std::nullopt,
                        ProposeBody{std::move(request)});
}

Message make_prepare(const Trade& trade, Leg leg)
{
    return make_message(NodeId::coordinator,
                        leg_party(trade, leg),
                        MessageType::prepare,
                        trade.trade_id(),
                        PrepareBody{trade, leg});
}

Message make_vote(NodeId from,
                  TradeId trade_id,
                  Vote vote,
                  ReasonCode reason)
{
    return make_message(from,
                        NodeId::coordinator,
                        MessageType::vote,
                        trade_id,
                        VoteBody{vote, reason});
}

Message make_commit(NodeId to, TradeId trade_id)
{
    return make_message(NodeId::coordinator,
                        to,
                        MessageType::commit,
                        trade_id,
                        CommitBody{});
}

Message make_abort(NodeId to, TradeId trade_id, ReasonCode reason)
{
    return make_message(NodeId::coordinator,
                        to,
                        MessageType::abort,
                        trade_id,
                        AbortBody{reason});
}

Message make_decision(NodeId to,
                      TradeId trade_id,
                      Decision decision,
                      ReasonCode abort_reason)
{
    if (decision == Decision::commit) {
        return make_commit(to, trade_id);
    }
    return make_abort(to, trade_id, abort_reason);
}

Message make_ack(NodeId from, TradeId trade_id, Decision decision)
{
    return make_message(from,
                        NodeId::coordinator,
                        MessageType::ack,
                        trade_id,
                        AckBody{decision});
}

Message make_outcome_query(NodeId from, TradeId trade_id)
{
    return make_message(from,
                        NodeId::coordinator,
                        MessageType::outcome_query,
                        trade_id,
                        OutcomeQueryBody{});
}

} // namespace glassbox
