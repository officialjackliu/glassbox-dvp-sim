#pragma once

// Protocol wire messages (DESIGN.md section 5.1).
//
// A Message carries routing/trace fields plus one typed body. Nodes build
// messages with the make_* helpers and leave msg_id at 0; the simulator assigns
// a unique msg_id when the send is released. check_message() is the single
// definition of a well-formed message and is used by nodes on receipt.

#include "sim/core_types.hpp"
#include "domain/log_records.hpp"
#include "domain/trade.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace glassbox {

// The seven protocol messages. PROPOSE: client to coordinator. PREPARE:
// coordinator asks a bank to lock its leg. VOTE: bank answers YES or NO.
// COMMIT / ABORT: the decision. ACK: bank confirms it applied the decision.
// OUTCOME_QUERY: a bank in doubt asks for the decision.
enum class MessageType : std::uint8_t {
    propose,
    prepare,
    vote,
    commit,
    abort,
    ack,
    outcome_query,
};

// Returns the trace spelling of `type`, e.g. "OUTCOME_QUERY".
[[nodiscard]] constexpr std::string_view to_string(MessageType type) noexcept
{
    switch (type) {
    case MessageType::propose:
        return "PROPOSE";
    case MessageType::prepare:
        return "PREPARE";
    case MessageType::vote:
        return "VOTE";
    case MessageType::commit:
        return "COMMIT";
    case MessageType::abort:
        return "ABORT";
    case MessageType::ack:
        return "ACK";
    case MessageType::outcome_query:
        return "OUTCOME_QUERY";
    }
    return "UNKNOWN";
}

// A bank's answer to PREPARE.
enum class Vote : std::uint8_t {
    yes,
    no,
};

// Returns "YES" or "NO".
[[nodiscard]] constexpr std::string_view to_string(Vote vote) noexcept
{
    return vote == Vote::yes ? "YES" : "NO";
}

// The coordinator's decision for a trade.
enum class Decision : std::uint8_t {
    commit,
    abort,
};

// Returns "COMMIT" or "ABORT".
[[nodiscard]] constexpr std::string_view to_string(Decision decision) noexcept
{
    return decision == Decision::commit ? "COMMIT" : "ABORT";
}

// Message bodies, one per MessageType.
//
// PROPOSE: the unvalidated trade request.
struct ProposeBody {
    TradeRequest request;
};

// PREPARE: the validated trade and which leg the receiving bank owns.
struct PrepareBody {
    Trade trade;
    Leg leg;
};

// VOTE: YES, or NO with the reason the bank refused.
struct VoteBody {
    Vote vote;
    ReasonCode reason;
};

// COMMIT carries no data.
struct CommitBody {};

// ABORT: why the coordinator aborted (passed on to the bank).
struct AbortBody {
    ReasonCode reason;
};

// ACK: which decision the bank applied.
struct AckBody {
    Decision decision;
};

// OUTCOME_QUERY carries no data.
struct OutcomeQueryBody {};

// std::monostate means "no body" and is only valid for legacy/test messages
// that never reach a protocol node; check_message() rejects it.
using MessageBody = std::variant<
    std::monostate,
    ProposeBody,
    PrepareBody,
    VoteBody,
    CommitBody,
    AbortBody,
    AckBody,
    OutcomeQueryBody>;

// One message on the wire. `trade_id` is set for every type except PROPOSE
// (the coordinator assigns the id on receipt).
struct Message {
    std::uint64_t msg_id = 0;
    NodeId from = NodeId::coordinator;
    NodeId to = NodeId::coordinator;
    MessageType type = MessageType::prepare;
    std::optional<TradeId> trade_id;
    MessageBody body{};
};

// Result of check_message(): ok, or the first problem found.
enum class MessageCheck : std::uint8_t {
    ok,
    missing_body,
    body_type_mismatch,
    missing_trade_id,
    unexpected_trade_id,
    trade_id_mismatch,
    invalid_route,
    invalid_vote_reason,
    invalid_abort_reason,
};

// Returns the trace spelling of `check`, e.g. "INVALID_ROUTE".
[[nodiscard]] constexpr std::string_view to_string(MessageCheck check) noexcept
{
    switch (check) {
    case MessageCheck::ok:
        return "OK";
    case MessageCheck::missing_body:
        return "MISSING_BODY";
    case MessageCheck::body_type_mismatch:
        return "BODY_TYPE_MISMATCH";
    case MessageCheck::missing_trade_id:
        return "MISSING_TRADE_ID";
    case MessageCheck::unexpected_trade_id:
        return "UNEXPECTED_TRADE_ID";
    case MessageCheck::trade_id_mismatch:
        return "TRADE_ID_MISMATCH";
    case MessageCheck::invalid_route:
        return "INVALID_ROUTE";
    case MessageCheck::invalid_vote_reason:
        return "INVALID_VOTE_REASON";
    case MessageCheck::invalid_abort_reason:
        return "INVALID_ABORT_REASON";
    }
    return "UNKNOWN";
}

// Returns the MessageType that a body alternative encodes, or nullopt for
// std::monostate.
[[nodiscard]] std::optional<MessageType> body_type(
    const MessageBody& body) noexcept;

// Fixed check order: body present, body matches type, trade_id presence,
// trade_id matches PREPARE trade, route (sender/receiver roles and leg party),
// vote reason, abort reason.
[[nodiscard]] MessageCheck check_message(const Message& message) noexcept;

// Short text for trace/CLI: PREPARE -> leg, VOTE -> "YES" or "NO:<reason>",
// ABORT -> reason, ACK -> decision, PROPOSE -> "<buyer><-<seller>".
// nullopt for bodies with no payload (COMMIT, OUTCOME_QUERY, none).
[[nodiscard]] std::optional<std::string> describe_body(
    const MessageBody& body);

// The party that owns a leg of a trade.
[[nodiscard]] NodeId leg_party(const Trade& trade, Leg leg) noexcept;

// Builders for each message type. They fill routing, type, trade id and body
// consistently, so check_message() accepts the result; msg_id stays 0 until
// the simulator releases the send. make_decision() builds COMMIT or ABORT
// from a Decision.
[[nodiscard]] Message make_propose(TradeRequest request);
[[nodiscard]] Message make_prepare(const Trade& trade, Leg leg);
[[nodiscard]] Message make_vote(NodeId from,
                                TradeId trade_id,
                                Vote vote,
                                ReasonCode reason);
[[nodiscard]] Message make_commit(NodeId to, TradeId trade_id);
[[nodiscard]] Message make_abort(NodeId to,
                                 TradeId trade_id,
                                 ReasonCode reason);
[[nodiscard]] Message make_decision(NodeId to,
                                    TradeId trade_id,
                                    Decision decision,
                                    ReasonCode abort_reason);
[[nodiscard]] Message make_ack(NodeId from,
                               TradeId trade_id,
                               Decision decision);
[[nodiscard]] Message make_outcome_query(NodeId from, TradeId trade_id);

} // namespace glassbox
