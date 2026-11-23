#include "cli/narrator.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <type_traits>
#include <variant>

namespace glassbox::cli {

namespace {

[[nodiscard]] const TraceRecord* find_kind(const Frame& f,
                                           std::string_view kind)
{
    for (const TraceRecord& r : f.records) {
        if (r.kind == kind) {
            return &r;
        }
    }
    return nullptr;
}

[[nodiscard]] const TraceRecord* find_prefix(const Frame& f,
                                             std::string_view prefix)
{
    for (const TraceRecord& r : f.records) {
        if (r.kind.starts_with(prefix)) {
            return &r;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string name(NodeId node)
{
    return std::string{glassbox::to_string(node)};
}

[[nodiscard]] std::string trade_name(std::optional<TradeId> id)
{
    return id ? "trade #" + std::to_string(id->value()) : "the trade";
}

// The node's handler for this event (the protocol function, not the
// simulator plumbing), or the first record's handler.
[[nodiscard]] std::string handler_of(const Frame& f)
{
    for (const TraceRecord& r : f.records) {
        if (r.handler.starts_with("Coordinator::") ||
            r.handler.starts_with("Participant::")) {
            return r.handler;
        }
    }
    return f.records.empty() ? std::string{} : f.records.front().handler;
}

[[nodiscard]] std::optional<Trade> find_trade(const Recording& rec, TradeId id)
{
    for (auto it = rec.frames.rbegin(); it != rec.frames.rend(); ++it) {
        const auto& coord = it->nodes[0].coordinator_trades;
        if (const auto found = coord.find(id); found != coord.end()) {
            return found->second.trade;
        }
        for (std::size_t b = 1; b < it->nodes.size(); ++b) {
            const auto& trades = it->nodes[b].trades;
            if (const auto found = trades.find(id);
                found != trades.end() && found->second.trade) {
                return found->second.trade;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] const CoordinatorTrade* coordinator_trade(const Frame& f,
                                                        TradeId id)
{
    const auto& trades = f.nodes[0].coordinator_trades;
    const auto it = trades.find(id);
    return it == trades.end() ? nullptr : &it->second;
}

[[nodiscard]] TradeState bank_state(const Frame& f, NodeId bank, TradeId id)
{
    const auto& trades = f.nodes[view_index(bank)].trades;
    const auto it = trades.find(id);
    return it == trades.end() ? TradeState::none : it->second.state;
}

[[nodiscard]] std::string link_text(NodeId from, NodeId to)
{
    const std::string_view a = city(from);
    const std::string_view b = city(to);
    if (a == b) {
        return "within " + std::string{a};
    }
    return std::string{a} + " → " + std::string{b};
}

[[nodiscard]] std::string reason_text(std::string_view code)
{
    if (code == "INSUFFICIENT_FUNDS") {
        return "not enough free cash";
    }
    if (code == "INSUFFICIENT_BONDS") {
        return "not enough free bonds";
    }
    if (code == "ACCOUNT_FROZEN") {
        return "account frozen";
    }
    if (code == "EXPIRED") {
        return "deadline passed";
    }
    if (code == "VOTE_TIMEOUT") {
        return "a vote did not arrive in time";
    }
    if (code == "ALREADY_ABORTED") {
        return "already aborted";
    }
    if (code == "UNKNOWN_TRADE") {
        return "unknown trade (presumed abort)";
    }
    return std::string{code};
}

[[nodiscard]] std::string timer_name(std::string_view kind)
{
    if (kind == "VOTE_TIMEOUT") {
        return "vote timer";
    }
    if (kind == "RETRANSMIT") {
        return "retransmit timer";
    }
    if (kind == "DECISION_WAIT") {
        return "decision-wait timer";
    }
    return std::string{kind};
}

[[nodiscard]] std::optional<Stage> stage_of(MessageType type)
{
    switch (type) {
    case MessageType::propose:
        return Stage::submit;
    case MessageType::prepare:
        return Stage::lock;
    case MessageType::vote:
        return Stage::vote;
    case MessageType::commit:
    case MessageType::abort:
        return Stage::settle;
    case MessageType::ack:
        return Stage::confirm;
    case MessageType::outcome_query:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_buyer(const std::optional<Trade>& trade, NodeId bank)
{
    return trade && trade->buyer() == bank;
}

// "$24,875.00 cash is" / "25 UST 10Y bonds are": for "... free again".
[[nodiscard]] std::string leg_is(const Trade& trade, NodeId bank)
{
    return trade.buyer() == bank
               ? dollars(trade.cash_cents()) + " cash is"
               : bonds_text(trade.qty().value()) + " are";
}

// "$24,875.00 cash" or "25 UST 10Y bonds": the leg this bank puts up.
[[nodiscard]] std::string leg_text(const Trade& trade, NodeId bank)
{
    return trade.buyer() == bank ? dollars(trade.cash_cents()) + " cash"
                                 : bonds_text(trade.qty().value());
}

// --- Stories ---------------------------------------------------------------

[[nodiscard]] Story submit_story(const Recording& rec, const Frame& f)
{
    Story s;
    s.stage = Stage::submit;
    s.code = "Coordinator::on_propose";
    if (const TraceRecord* rejected = find_kind(f, "REJECTED")) {
        s.headline = "COORD rejects the trade request: " +
                     rejected->detail.value_or("invalid");
        s.details.push_back("nothing is recorded and no bank is asked");
        s.tone = Tone::failure;
        return s;
    }
    const TraceRecord* opened = find_kind(f, "STATE_CHANGE");
    if (!opened || !opened->trade_id) {
        s.headline = "A trade request arrives at COORD";
        return s;
    }
    const TradeId id = *opened->trade_id;
    const auto trade = find_trade(rec, id);
    if (trade) {
        s.headline = "Trade #" + std::to_string(id.value()) +
                     " submitted: " + name(trade->buyer()) + " buys " +
                     bonds_text(trade->qty().value()) + " from " +
                     name(trade->seller()) + " for " +
                     dollars(trade->cash_cents());
    } else {
        s.headline = "Trade #" + std::to_string(id.value()) + " submitted";
    }
    s.details.push_back(
        "COORD checks the ticket: two different known banks, asset, "
        "quantity, price, deadline ✓");
    std::string opened_text = "COORD opens trade #" +
                              std::to_string(id.value()) +
                              " and starts collecting votes";
    for (const TimerSpan& t : rec.timers) {
        if (t.node == NodeId::coordinator && t.trade_id == id &&
            t.kind == "VOTE_TIMEOUT" && t.fires_at_us) {
            opened_text += " (vote timer: " + format_time(*t.fires_at_us) + ")";
            break;
        }
    }
    s.details.push_back(opened_text);
    s.details.push_back(
        "next: write PROPOSED to disk, then ask both banks to PREPARE");
    s.why = "Delivery versus payment: the bonds move one way and the cash "
            "the other, or nothing moves at all.";
    return s;
}

[[nodiscard]] Story action_story(const Frame& f)
{
    Story s;
    s.code = "Simulator::dispatch_script";
    std::visit(
        [&](const auto& action) {
            using A = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<A, ScriptSetPartition>) {
                const std::string link = name(action.a) + " ⇄ " + name(action.b);
                s.code = "NetworkModel::set_partition";
                if (action.partitioned) {
                    s.headline = "Network fault: the link " + link + " is cut";
                    s.details.push_back(
                        "messages sent on this link from now on are lost");
                    s.details.push_back(
                        "both nodes keep running; they just can't hear each "
                        "other");
                    s.why = "A partition is not a crash: nobody loses memory, "
                            "only messages.";
                    s.tone = Tone::failure;
                } else {
                    s.headline = "The link " + link + " is restored";
                    s.details.push_back("messages get through again");
                    s.tone = Tone::good;
                }
            } else if constexpr (std::is_same_v<A, ScriptSetLink>) {
                s.code = "NetworkModel::set_link";
                s.headline = "Network change: " + name(action.from) + " → " +
                             name(action.to) + " now takes " +
                             format_duration(action.base_latency_us);
                if (action.jitter_us > 0) {
                    s.headline +=
                        " (± " + format_duration(action.jitter_us) + ")";
                }
                if (action.drop_probability_ppm > 0) {
                    s.details.push_back(
                        "and loses " +
                        std::to_string(action.drop_probability_ppm / 10'000) +
                        "% of messages");
                }
                s.tone = Tone::warning;
            } else if constexpr (std::is_same_v<A, ScriptDropNext>) {
                s.headline = "Fault injected: the next " +
                             (action.message_type
                                  ? std::string{glassbox::to_string(
                                        *action.message_type)}
                                  : std::string{"message"}) +
                             " from " + name(action.from) + " to " +
                             name(action.to) + " will be lost";
                s.tone = Tone::warning;
            } else if constexpr (std::is_same_v<A, ScriptDuplicateNext>) {
                s.headline = "Fault injected: the next " +
                             std::string{glassbox::to_string(action.message_type)} +
                             " from " + name(action.from) + " to " +
                             name(action.to) + " will arrive twice";
                s.tone = Tone::warning;
            } else if constexpr (std::is_same_v<A, ScriptNodeDown>) {
                s.headline = name(action.node) +
                             " goes offline: it receives nothing until it is "
                             "back";
                s.tone = Tone::failure;
            } else if constexpr (std::is_same_v<A, ScriptNodeUp>) {
                s.headline = name(action.node) + " is back online";
                s.tone = Tone::good;
            } else {
                s.headline = "Scenario step";
            }
        },
        f.action->action);
    return s;
}

[[nodiscard]] Story prepare_story(const Recording& rec,
                                  const Frame& before,
                                  const Frame& f,
                                  const TraceRecord& deliver)
{
    Story s;
    const NodeId bank = *deliver.to;
    const std::optional<TradeId> id = deliver.trade_id;
    s.code = "Participant::on_prepare";
    s.stage = Stage::lock;
    const auto trade = id ? find_trade(rec, *id) : std::nullopt;
    const NodeView& was = before.nodes[view_index(bank)];

    const TraceRecord* lock = find_kind(f, "LOCK");
    const TraceRecord* refused = find_kind(f, "VOTE_NO");
    if ((lock || refused) && trade && was.balances) {
        const bool buyer = is_buyer(trade, bank);
        const std::int64_t need =
            buyer ? trade->cash_cents().value() : trade->qty().value();
        const std::int64_t free =
            buyer ? was.balances->cash_total - was.balances->cash_locked
                  : was.balances->bonds_total - was.balances->bonds_locked;
        const auto amount = [&](std::int64_t v) {
            return buyer ? dollars(Cents{v}) : with_commas(v);
        };
        const std::string what = buyer ? "free cash " : "free bonds ";
        const std::string deadline =
            " (valid until " + format_time(trade->valid_until_us()) + ")";
        const std::string reason =
            refused ? refused->detail.value_or("") : std::string{};

        if (reason == "ACCOUNT_FROZEN") {
            s.details.push_back("account frozen ✗");
        } else {
            s.details.push_back("account not frozen ✓");
            if (reason == "EXPIRED") {
                s.details.push_back("deadline passed ✗" + deadline);
            } else {
                s.details.push_back("deadline not passed ✓" + deadline);
                const bool enough = free >= need;
                s.details.push_back(what + amount(free) +
                                    (enough ? " ≥ " : " < ") + amount(need) +
                                    (enough ? " ✓" : " ✗"));
            }
        }
        if (lock) {
            s.headline = name(bank) + " checks the trade and locks " +
                         leg_text(*trade, bank);
            s.details.push_back("locks " +
                                (buyer ? dollars(trade->cash_cents())
                                       : bonds_text(trade->qty().value())) +
                                " → " + what + "now " + amount(free - need));
            s.details.push_back(
                "will vote YES as soon as its PREPARED record is on disk");
            s.why = buyer ? "Locking first means the same cash can't be "
                            "promised to two trades."
                          : "Locking first means the same bonds can't be "
                            "sold twice.";
        } else {
            s.headline = name(bank) + " refuses " + trade_name(id) + ": " +
                         reason_text(reason);
            s.details.push_back(
                "writes ABORTED to disk and votes NO; nothing is locked");
            s.why = "One NO is enough to cancel the trade: both legs settle "
                    "or neither does.";
            s.stage = Stage::vote;
            s.tone = Tone::failure;
        }
        return s;
    }
    s.tone = Tone::warning;
    s.stage.reset();
    if (find_kind(f, "DUPLICATE_IGNORED")) {
        s.headline = name(bank) + " gets PREPARE again and ignores it";
        s.details.push_back(
            "its YES vote is already queued behind the PREPARED disk write");
        return s;
    }
    const TradeState state = id ? bank_state(before, bank, *id) : TradeState::none;
    if (state == TradeState::aborted) {
        s.headline = name(bank) + " gets PREPARE again and repeats its NO";
    } else {
        s.headline = name(bank) + " gets PREPARE again and repeats its YES";
    }
    s.details.push_back("a duplicate never locks twice");
    return s;
}

[[nodiscard]] Story vote_story(const Frame& before,
                               const Frame& f,
                               const TraceRecord& deliver)
{
    Story s;
    const NodeId voter = *deliver.from;
    const std::optional<TradeId> id = deliver.trade_id;
    const std::string vote = deliver.detail.value_or("");
    s.code = handler_of(f);
    s.stage = Stage::vote;
    const TraceRecord* change = find_kind(f, "STATE_CHANGE");
    if (change && change->detail == "COLLECTING->COMMITTING") {
        s.headline = "Both banks voted YES → COORD decides COMMIT";
        s.code = "Coordinator::on_vote → Coordinator::decide";
        s.details.push_back("COORD records " + name(voter) +
                            "'s YES (2 of 2)");
        s.details.push_back("writes DECISION_COMMIT to disk before telling "
                            "anyone");
        s.why = "Once the decision is on disk it never changes, even if "
                "COORD crashes.";
        s.stage = Stage::decide;
        s.tone = Tone::good;
        return s;
    }
    if (change && change->detail == "COLLECTING->ABORTING") {
        const std::string reason =
            vote.starts_with("NO:") ? vote.substr(3) : vote;
        s.headline = name(voter) + " voted NO (" + reason_text(reason) +
                     ") → COORD decides ABORT";
        s.code = "Coordinator::on_vote → Coordinator::decide";
        s.details.push_back("writes DECISION_ABORT to disk, then tells both "
                            "banks to release their locks");
        s.stage = Stage::decide;
        s.tone = Tone::failure;
        return s;
    }
    if (find_kind(f, "VOTE_RECORDED")) {
        s.headline = "COORD records " + name(voter) + "'s YES (1 of 2)";
        if (id) {
            if (const CoordinatorTrade* t = coordinator_trade(f, *id)) {
                const NodeId other = t->trade.buyer() == voter
                                         ? t->trade.seller()
                                         : t->trade.buyer();
                s.details.push_back("still waiting for " + name(other));
            }
        }
        return s;
    }
    s.stage.reset();
    s.tone = Tone::warning;
    if (find_kind(f, "PRESUMED_ABORT")) {
        s.headline = "COORD has no record of " + trade_name(id) +
                     " → presumed abort: it answers ABORT";
        return s;
    }
    if (find_kind(f, "DECISION_PENDING")) {
        s.headline = "COORD gets a late vote from " + name(voter) +
                     "; its decision is not on disk yet, so it waits";
        return s;
    }
    std::string decision = "its decision";
    if (id) {
        if (const CoordinatorTrade* t = coordinator_trade(before, *id);
            t && t->decision) {
            decision = std::string{glassbox::to_string(*t->decision)};
        }
    }
    s.headline = "COORD gets a late vote from " + name(voter) +
                 " and repeats " + decision;
    return s;
}

[[nodiscard]] Story commit_story(const Recording& rec,
                                 const Frame& before,
                                 const Frame& f,
                                 const TraceRecord& deliver)
{
    Story s;
    const NodeId bank = *deliver.to;
    const std::optional<TradeId> id = deliver.trade_id;
    s.code = "Participant::on_commit";
    s.stage = Stage::settle;
    if (const TraceRecord* bad = find_kind(f, "INVARIANT_VIOLATION")) {
        s.headline = "Invariant violation at " + name(bank) + ": " +
                     bad->detail.value_or("");
        s.tone = Tone::failure;
        return s;
    }
    const auto trade = id ? find_trade(rec, *id) : std::nullopt;
    const NodeView& was = before.nodes[view_index(bank)];
    const NodeView& now = f.nodes[view_index(bank)];
    if (find_kind(f, "APPLY") && trade && was.balances && now.balances) {
        const std::string bonds = bonds_text(trade->qty().value());
        const std::string cash = dollars(trade->cash_cents());
        const std::string cash_line =
            "cash " + dollars(Cents{was.balances->cash_total}) + " → " +
            dollars(Cents{now.balances->cash_total});
        const std::string bond_line =
            "bonds " + with_commas(was.balances->bonds_total) + " → " +
            with_commas(now.balances->bonds_total);
        if (is_buyer(trade, bank)) {
            s.headline = name(bank) + " settles: pays " + cash +
                         " and receives " + bonds;
            s.details.push_back("the locked cash is paid out: " + cash_line);
            s.details.push_back(bond_line);
        } else {
            s.headline = name(bank) + " settles: delivers " + bonds +
                         " and receives " + cash;
            s.details.push_back("the locked bonds are delivered: " + bond_line);
            s.details.push_back(cash_line);
        }
        s.details.push_back("writes COMMITTED to disk, then sends ACK");
        s.why = "Both legs settle on the same decision: this is delivery "
                "versus payment.";
        s.tone = Tone::good;
        return s;
    }
    s.headline = name(bank) + " already settled; it ACKs the duplicate COMMIT";
    s.tone = Tone::warning;
    return s;
}

[[nodiscard]] Story abort_story(const Recording& rec,
                                const Frame& f,
                                const TraceRecord& deliver)
{
    Story s;
    const NodeId bank = *deliver.to;
    const std::optional<TradeId> id = deliver.trade_id;
    s.code = "Participant::on_abort";
    s.stage = Stage::settle;
    s.tone = Tone::warning;
    const auto trade = id ? find_trade(rec, *id) : std::nullopt;
    if (find_kind(f, "UNLOCK") && trade) {
        s.headline = name(bank) + " releases its lock: " +
                     leg_is(*trade, bank) + " free again";
        s.details.push_back("COORD's reason: " +
                            reason_text(deliver.detail.value_or("")));
        s.details.push_back("writes ABORTED to disk, then sends ACK");
        return s;
    }
    const TraceRecord* change = find_kind(f, "STATE_CHANGE");
    if (change && change->detail == "NONE->ABORTED") {
        s.headline = name(bank) + " never saw " + trade_name(id) +
                     "; it records ABORTED anyway";
        s.details.push_back("so a PREPARE that arrives late will be refused");
        s.details.push_back("then sends ACK");
        return s;
    }
    s.headline = name(bank) + " already aborted; it ACKs the duplicate ABORT";
    return s;
}

[[nodiscard]] Story ack_story(const Frame& f, const TraceRecord& deliver)
{
    Story s;
    const NodeId bank = *deliver.from;
    const std::optional<TradeId> id = deliver.trade_id;
    s.code = "Coordinator::on_ack";
    s.stage = Stage::confirm;
    const TraceRecord* change = find_kind(f, "STATE_CHANGE");
    if (change && change->detail && change->detail->ends_with("->DONE")) {
        s.headline = "Both banks confirmed → " + trade_name(id) +
                     " is complete";
        s.details.push_back("COORD writes END: nothing left to resend");
        s.tone = Tone::good;
        return s;
    }
    if (find_kind(f, "ACK_IGNORED")) {
        s.headline = "COORD ignores a repeated ACK from " + name(bank);
        s.stage.reset();
        return s;
    }
    s.headline = "COORD notes " + name(bank) + "'s ACK";
    if (id) {
        if (const CoordinatorTrade* t = coordinator_trade(f, *id)) {
            const NodeId other = t->trade.buyer() == bank ? t->trade.seller()
                                                          : t->trade.buyer();
            s.details.push_back("waiting for " + name(other) + " to confirm");
        }
    }
    return s;
}

[[nodiscard]] Story query_story(const Frame& f, const TraceRecord& deliver)
{
    Story s;
    const NodeId bank = *deliver.from;
    const std::optional<TradeId> id = deliver.trade_id;
    s.code = "Coordinator::on_outcome_query";
    s.tone = Tone::warning;
    if (find_kind(f, "QUERY_DEFERRED")) {
        s.headline = name(bank) + " asks for the outcome; COORD is still "
                                  "collecting votes, so no answer yet";
        return s;
    }
    if (find_kind(f, "DECISION_PENDING")) {
        s.headline = name(bank) + " asks for the outcome; the decision is "
                                  "not on disk yet, so no answer yet";
        return s;
    }
    if (find_kind(f, "PRESUMED_ABORT")) {
        s.headline = name(bank) + " asks about " + trade_name(id) +
                     "; COORD has no record → presumed abort: ABORT";
        return s;
    }
    std::string decision = "the decision";
    if (id) {
        if (const CoordinatorTrade* t = coordinator_trade(f, *id);
            t && t->decision) {
            decision = std::string{glassbox::to_string(*t->decision)};
        }
    }
    s.headline = name(bank) + " asks for the outcome; COORD answers " +
                 decision;
    s.details.push_back("the answer comes from the decision on COORD's disk");
    s.tone = Tone::good;
    return s;
}

[[nodiscard]] Story drop_story(const TraceRecord& drop)
{
    Story s;
    s.code = drop.handler;
    s.tone = Tone::failure;
    const std::string type = drop.msg_type
                                 ? std::string{glassbox::to_string(*drop.msg_type)}
                                 : std::string{"message"};
    const std::string from = drop.from ? name(*drop.from) : "?";
    const std::string to = drop.to ? name(*drop.to) : "?";
    s.headline = type + " from " + from + " to " + to + " is lost";
    const std::string reason =
        drop.reason ? *drop.reason
                    : (drop.kind == "MSG_DROP_NODE_DOWN" ? "NODE_DOWN" : "");
    if (reason == "PARTITION") {
        s.details.push_back("the link " + from + " ⇄ " + to + " is cut");
    } else if (reason == "NODE_DOWN") {
        s.details.push_back(to + " is down");
    } else if (reason == "DROP_RANDOM") {
        s.details.push_back("lost by the network (random drop)");
    } else if (reason == "DROP_NEXT") {
        s.details.push_back("lost on purpose (injected fault)");
    }
    if (drop.msg_type) {
        switch (*drop.msg_type) {
        case MessageType::prepare:
            s.details.push_back(to + " never hears of the trade; COORD's vote "
                                     "timer will run out");
            break;
        case MessageType::vote:
            s.details.push_back("COORD will not hear " + from + "'s vote");
            break;
        case MessageType::commit:
        case MessageType::abort:
            s.details.push_back("COORD resends it until " + to +
                                " confirms");
            break;
        case MessageType::ack:
            s.details.push_back("COORD will resend its decision");
            break;
        case MessageType::outcome_query:
            s.details.push_back(from + " will ask again later");
            break;
        case MessageType::propose:
            break;
        }
    }
    return s;
}

[[nodiscard]] Story log_story(const Recording& rec, const Frame& f)
{
    Story s;
    s.code = "Effects::flush";
    const TraceRecord* first = find_kind(f, "LOG_APPEND");
    const NodeId node = *first->node;
    const std::string kind = first->log_record.value_or("");
    const std::optional<TradeId> id = first->trade_id;
    const std::string who = name(node);
    if (kind == "PROPOSED") {
        s.headline = "COORD writes PROPOSED to disk";
        s.details.push_back(trade_name(id) +
                            " now survives a crash and its id is never reused");
        if (id) {
            if (const auto trade = find_trade(rec, *id)) {
                s.details.push_back("next: PREPARE to " + name(trade->buyer()) +
                                    " (buyer) and " + name(trade->seller()) +
                                    " (seller)");
            }
        }
        s.why = "Write-ahead rule: put it on disk first, then act on it.";
        s.stage = Stage::submit;
    } else if (kind == "PREPARED") {
        s.headline = who + "'s promise is on disk (PREPARED)";
        s.details.push_back("from now on " + who +
                            " may not back out on its own");
        s.details.push_back("after a crash it restores the lock from disk "
                            "and asks for the outcome");
        s.why = "A bank votes YES only after its promise is on disk.";
        s.stage = Stage::lock;
    } else if (kind == "DECISION_COMMIT") {
        s.headline = "COMMIT decision is on disk: the commit point";
        s.details.push_back(trade_name(id) +
                            " will settle now, even if COORD crashes");
        s.details.push_back("next: send COMMIT to both banks");
        s.why = "The decision is durable before anyone hears it, so it can "
                "never be taken back.";
        s.stage = Stage::decide;
        s.tone = Tone::good;
    } else if (kind == "DECISION_ABORT") {
        s.headline = "ABORT decision is on disk";
        if (id) {
            if (const CoordinatorTrade* t = coordinator_trade(f, *id)) {
                s.details.push_back(
                    "reason: " +
                    reason_text(glassbox::to_string(t->abort_reason)));
            }
        }
        s.details.push_back("next: send ABORT to both banks");
        s.stage = Stage::decide;
        s.tone = Tone::failure;
    } else if (kind == "COMMITTED") {
        s.headline = who + "'s settlement is on disk (COMMITTED)";
        s.details.push_back("next: send ACK to COORD");
        s.stage = Stage::settle;
        s.tone = Tone::good;
    } else if (kind == "ABORTED") {
        s.headline = who + " writes ABORTED to disk";
        s.details.push_back(trade_name(id) + " is closed for " + who);
        // Before COORD has decided, this is the bank's own NO.
        const CoordinatorTrade* t = id ? coordinator_trade(f, *id) : nullptr;
        s.stage = t && t->state == TradeState::collecting ? Stage::vote
                                                          : Stage::settle;
    } else if (kind == "END") {
        s.headline = "COORD writes END: " + trade_name(id) + " is finished";
        s.details.push_back("no more resends; leftover timers will be ignored");
        s.stage = Stage::confirm;
        s.tone = Tone::good;
    } else {
        s.headline = who + " writes " + kind + " to disk";
    }
    for (const TraceRecord& r : f.records) {
        if (r.kind == "LOG_APPEND" && &r != first) {
            s.details.push_back("also on disk: " + r.log_record.value_or(""));
        }
    }
    return s;
}

// The protocol function that ran when `sender` handled one event in this
// frame, or empty if the sender handled nothing here.
[[nodiscard]] std::string handled_by(const Frame& f, NodeId sender,
                                     std::optional<TradeId> trade,
                                     std::optional<MessageType> sends)
{
    const bool coord = sender == NodeId::coordinator;
    for (const TraceRecord& r : f.records) {
        if (r.trade_id && trade && *r.trade_id != *trade) {
            continue;
        }
        std::string handler;
        std::vector<MessageType> can_send;
        if (r.kind == "MSG_DELIVER" && r.to == sender && r.msg_type) {
            switch (*r.msg_type) {
            case MessageType::propose:
                handler = "Coordinator::on_propose";
                can_send = {MessageType::prepare};
                break;
            case MessageType::vote:
                handler = "Coordinator::on_vote";
                can_send = {MessageType::commit, MessageType::abort};
                break;
            case MessageType::outcome_query:
                handler = "Coordinator::on_outcome_query";
                can_send = {MessageType::commit, MessageType::abort};
                break;
            case MessageType::ack:
                handler = "Coordinator::on_ack";
                break;
            case MessageType::prepare:
                handler = "Participant::on_prepare";
                can_send = {MessageType::vote};
                break;
            case MessageType::commit:
                handler = "Participant::on_commit";
                can_send = {MessageType::ack};
                break;
            case MessageType::abort:
                handler = "Participant::on_abort";
                can_send = {MessageType::ack};
                break;
            }
        } else if (r.kind == "TIMER_FIRE" && r.node == sender) {
            const std::string kind = r.detail.value_or("");
            if (kind == "VOTE_TIMEOUT") {
                handler = "Coordinator::on_vote_timeout";
                can_send = {MessageType::abort};
            } else if (kind == "RETRANSMIT") {
                handler = "Coordinator::on_retransmit";
                can_send = {MessageType::commit, MessageType::abort};
            } else if (kind == "DECISION_WAIT") {
                handler = "Participant::on_decision_wait";
                can_send = {MessageType::outcome_query};
            }
        } else if (r.kind == "RESTART" && r.node == sender) {
            handler = coord ? "Coordinator::on_restart"
                            : "Participant::on_restart";
            can_send = coord ? std::vector<MessageType>{MessageType::commit,
                                                        MessageType::abort}
                             : std::vector<MessageType>{
                                   MessageType::outcome_query};
        } else {
            continue;
        }
        if (sends && std::find(can_send.begin(), can_send.end(), *sends) ==
                         can_send.end()) {
            return {};
        }
        // A decision taken while handling it shows as "... → decide".
        for (const TraceRecord& n : f.records) {
            if (n.handler == "Coordinator::decide") {
                return handler + " → Coordinator::decide";
            }
        }
        return handler;
    }
    return {};
}

// The handler whose batch this send belongs to: the sender's latest
// earlier event (for the same trade) that can produce this message type.
[[nodiscard]] std::string queued_by(const Recording& rec,
                                    std::size_t frame,
                                    const TraceRecord& send)
{
    for (std::size_t i = frame; i-- > 1;) {
        const std::string handler = handled_by(
            rec.frames[i], *send.from, send.trade_id, send.msg_type);
        if (!handler.empty()) {
            return handler;
        }
    }
    return {};
}

[[nodiscard]] Story send_story(const Recording& rec,
                               std::size_t index,
                               const TraceRecord& send)
{
    Story s;
    const NodeId from = *send.from;
    const NodeId to = *send.to;
    const MessageType type = *send.msg_type;
    const std::optional<TradeId> id = send.trade_id;
    const std::string detail = send.detail.value_or("");
    const auto trade = id ? find_trade(rec, *id) : std::nullopt;
    s.stage = stage_of(type);
    const std::string origin = queued_by(rec, index, send);
    s.code = origin.empty() ? send.handler : send.handler + " ← " + origin;

    switch (type) {
    case MessageType::prepare:
        s.headline = "COORD sends PREPARE to " + name(to) + " (" +
                     (detail == "BUYER" ? "buyer" : "seller") + " leg)";
        if (trade) {
            s.details.push_back("asks " + name(to) + " to lock " +
                                leg_text(*trade, to) + " and vote");
        }
        break;
    case MessageType::vote:
        if (detail == "YES") {
            s.headline = name(from) + " sends its YES vote to COORD";
            s.details.push_back("its PREPARED record is already on disk, so "
                                "the promise survives a crash");
        } else {
            const std::string reason =
                detail.starts_with("NO:") ? detail.substr(3) : detail;
            s.headline = name(from) + " sends its NO vote to COORD";
            s.details.push_back("reason: " + reason_text(reason));
            s.tone = Tone::failure;
        }
        break;
    case MessageType::commit:
        s.headline = "COORD sends COMMIT to " + name(to);
        s.details.push_back("the decision is on disk, so it is safe to "
                            "announce");
        break;
    case MessageType::abort:
        s.headline = "COORD sends ABORT to " + name(to);
        s.details.push_back("COORD's reason: " + reason_text(detail) + "; " +
                            name(to) + " must release any lock");
        s.tone = Tone::warning;
        break;
    case MessageType::ack:
        s.headline = name(from) + " sends ACK (" + detail + ") to COORD";
        s.details.push_back("lets COORD stop resending to " + name(from));
        break;
    case MessageType::outcome_query:
        s.headline = name(from) + " asks COORD: what was decided for " +
                     trade_name(id) + "?";
        s.details.push_back("a bank that voted YES may not decide alone; it "
                            "must ask");
        s.tone = Tone::warning;
        break;
    case MessageType::propose:
        s.headline = name(from) + " sends PROPOSE to " + name(to);
        break;
    }
    // Travel time from the recording (a lost message is dropped on arrival).
    for (const Flight& flight : rec.flights) {
        if (flight.msg_id == *send.msg_id &&
            flight.sent_at_us == send.sim_time_us) {
            if (flight.ends_at_us == k_open_end) {
                s.details.push_back("on its way, " + link_text(from, to));
            } else {
                s.details.push_back(
                    "travels " +
                    format_duration(flight.ends_at_us - flight.sent_at_us) +
                    ", " + link_text(from, to));
            }
            break;
        }
    }
    return s;
}

[[nodiscard]] Story timer_story(const Frame& before,
                                const Frame& f,
                                const TraceRecord& fire)
{
    Story s;
    const std::string kind = fire.detail.value_or("");
    const NodeId node = *fire.node;
    const std::optional<TradeId> id = fire.trade_id;
    s.code = handler_of(f);
    const TraceRecord* change = find_kind(f, "STATE_CHANGE");
    if (kind == "VOTE_TIMEOUT" && change &&
        change->detail == "COLLECTING->ABORTING") {
        s.headline = "Vote timer runs out → COORD decides ABORT";
        s.code = "Coordinator::on_vote_timeout → Coordinator::decide";
        if (id) {
            if (const CoordinatorTrade* t = coordinator_trade(before, *id)) {
                std::string missing;
                if (!t->buyer_voted_yes) {
                    missing = name(t->trade.buyer());
                }
                if (!t->seller_voted_yes) {
                    missing += (missing.empty() ? "" : " and ") +
                               name(t->trade.seller());
                }
                s.details.push_back("no vote from " + missing + " in time");
            }
        }
        s.details.push_back("writes DECISION_ABORT (VOTE_TIMEOUT) to disk, "
                            "then tells both banks");
        s.why = "Until it decides, COORD may abort on its own: silence "
                "counts as NO.";
        s.stage = Stage::decide;
        s.tone = Tone::failure;
        return s;
    }
    if (const TraceRecord* resend = find_kind(f, "RETRANSMIT")) {
        std::string decision = "its decision";
        if (id) {
            if (const CoordinatorTrade* t = coordinator_trade(f, *id);
                t && t->decision) {
                decision = std::string{glassbox::to_string(*t->decision)};
            }
        }
        std::string targets = resend->detail.value_or("");
        for (char& c : targets) {
            if (c == ',') {
                c = '+';
            }
        }
        s.headline = "No ACK yet from " + targets + " → COORD resends " +
                     decision;
        s.details.push_back("it keeps resending, waiting longer each time, "
                            "until every bank confirms");
        s.stage = Stage::settle;
        s.tone = Tone::warning;
        return s;
    }
    if (const TraceRecord* query = find_kind(f, "OUTCOME_QUERY")) {
        const std::string text = query->detail.value_or("0");
        std::uint32_t attempt = 0;
        static_cast<void>(
            std::from_chars(text.data(), text.data() + text.size(), attempt));
        s.headline = name(node) + " is in doubt and asks COORD for the outcome";
        s.details.push_back("it voted YES and holds its lock, but has heard "
                            "nothing");
        s.details.push_back("question #" +
                            std::to_string(std::uint64_t{attempt} + 1) +
                            "; the wait doubles each time");
        s.why = "A prepared bank may not decide alone: this is why two-phase "
                "commit can block.";
        s.tone = Tone::warning;
        return s;
    }
    s.headline = name(node) + "'s " + timer_name(kind) +
                 " fires; nothing to do";
    return s;
}

[[nodiscard]] Story ignored_story(const TraceRecord& ignored)
{
    Story s;
    const std::string who = ignored.node ? name(*ignored.node) : "?";
    s.headline = who + "'s " + timer_name(ignored.detail.value_or("")) +
                 " fires and is ignored";
    const std::string reason = ignored.reason.value_or("");
    if (reason == "state_mismatch") {
        s.details.push_back(trade_name(ignored.trade_id) +
                            " already moved past what this timer was "
                            "waiting for");
    } else if (reason == "stale_epoch") {
        s.details.push_back("it was set before " + who + " crashed");
    } else if (reason == "node_down") {
        s.details.push_back(who + " is down");
    }
    s.why = "Timers are never cancelled; when one fires, the simulator checks "
            "whether it still matters.";
    s.code = ignored.handler;
    return s;
}

[[nodiscard]] std::string crash_point_text(std::string_view point)
{
    if (point == "BEFORE_LOG") {
        return " before writing anything to disk";
    }
    if (point == "AFTER_LOG_BEFORE_SEND") {
        return " after writing to disk, before sending";
    }
    if (point == "AFTER_FIRST_SEND") {
        return " right after its first message left";
    }
    return {};
}

[[nodiscard]] Story crash_story(const Frame& f, const TraceRecord& crash)
{
    Story s;
    const NodeId node = *crash.node;
    const std::string who = name(node);
    const std::string point = crash.reason.value_or("");
    s.headline = who + " CRASHES" + crash_point_text(point);
    s.tone = Tone::failure;
    s.code = crash.handler;
    if (const TraceRecord* deliver = find_kind(f, "MSG_DELIVER");
        deliver && deliver->msg_type && deliver->from) {
        s.details.push_back("it was handling " +
                            std::string{glassbox::to_string(*deliver->msg_type)} +
                            " from " + name(*deliver->from));
    }
    s.details.push_back(node == NodeId::coordinator
                            ? "memory lost: votes, decisions in progress, "
                              "timers"
                            : "memory lost: open trades, locks, timers");
    const NodeView& view = f.nodes[view_index(node)];
    std::string disk;
    for (const std::string& line : view.disk) {
        disk += (disk.empty() ? "" : ", ") + line;
    }
    s.details.push_back("disk kept: " + (disk.empty() ? "nothing" : disk));
    s.details.push_back("messages sent to " + who +
                        " are lost until it restarts");
    if (point == "BEFORE_LOG") {
        s.why = "Nothing from this step reached disk or the network.";
    } else if (point == "AFTER_LOG_BEFORE_SEND") {
        s.why = "Its disk write finished, but none of its messages left.";
    } else if (point == "AFTER_FIRST_SEND") {
        s.why = "Its disk write finished and only its first message left.";
    }
    return s;
}

[[nodiscard]] Story restart_story(const Frame& f, const TraceRecord& restart)
{
    Story s;
    const NodeId node = *restart.node;
    const std::string who = name(node);
    s.headline = who + " restarts and rebuilds itself from disk";
    s.tone = Tone::good;
    s.code = handler_of(f);
    const NodeView& view = f.nodes[view_index(node)];
    std::string disk;
    for (const std::string& line : view.disk) {
        disk += (disk.empty() ? "" : ", ") + line;
    }
    s.details.push_back("replays its disk: " +
                        (disk.empty() ? std::string{"nothing"} : disk));
    bool any = false;
    for (const TraceRecord& r : f.records) {
        if (r.kind != "RECOVERY_STEP") {
            continue;
        }
        any = true;
        const std::string trade = trade_name(r.trade_id);
        const std::string step = r.detail.value_or("");
        if (step == "IN_DOUBT_QUERY") {
            std::string lock;
            if (view.balances) {
                lock = view.balances->cash_locked > 0
                           ? dollars(Cents{view.balances->cash_locked}) + " cash"
                           : bonds_text(view.balances->bonds_locked);
            }
            s.details.push_back(trade + " is IN DOUBT: lock restored (" +
                                lock + "), asks COORD for the outcome");
        } else if (step == "RESEND_DECISION") {
            s.details.push_back(trade + ": decision on disk but no END → "
                                        "resends it to both banks");
        } else if (step == "FORGET_UNDECIDED") {
            s.details.push_back(trade + ": no decision on disk → forgets "
                                        "it (presumed abort)");
        } else if (step == "RESTORE_DONE") {
            s.details.push_back(trade + " had already ended");
        } else {
            s.details.push_back(trade + ": " + step);
        }
    }
    if (const TraceRecord* error = find_kind(f, "RECOVERY_ERROR")) {
        s.details.push_back("recovery error: " + error->detail.value_or(""));
        s.tone = Tone::failure;
    }
    if (!any && disk.empty()) {
        s.details.push_back("nothing on disk: it starts clean and remembers "
                            "no trades");
    }
    s.why = "Everything in memory was lost; the disk is the only truth.";
    return s;
}

} // namespace

std::string_view to_string(Stage stage) noexcept
{
    switch (stage) {
    case Stage::submit:
        return "submit";
    case Stage::lock:
        return "lock";
    case Stage::vote:
        return "vote";
    case Stage::decide:
        return "decide";
    case Stage::settle:
        return "settle";
    case Stage::confirm:
        return "confirm";
    }
    return "?";
}

namespace {

[[nodiscard]] Story narrate_frame(const Recording& rec, std::size_t index)
{
    if (index == 0 || index >= rec.frames.size()) {
        Story s;
        s.headline = index == 0 ? "Ready: nothing has happened yet"
                                : "End of the recording";
        return s;
    }
    const Frame& f = rec.frames[index];
    const Frame& before = rec.frames[index - 1];

    if (const TraceRecord* crash = find_kind(f, "CRASH")) {
        return crash_story(f, *crash);
    }
    if (const TraceRecord* restart = find_kind(f, "RESTART")) {
        return restart_story(f, *restart);
    }
    if (f.action) {
        if (std::holds_alternative<ScriptSubmitTrade>(f.action->action)) {
            return submit_story(rec, f);
        }
        return action_story(f);
    }
    if (const TraceRecord* drop = find_prefix(f, "MSG_DROP")) {
        return drop_story(*drop);
    }
    if (const TraceRecord* deliver = find_kind(f, "MSG_DELIVER");
        deliver && deliver->msg_type && deliver->from && deliver->to) {
        switch (*deliver->msg_type) {
        case MessageType::prepare:
            return prepare_story(rec, before, f, *deliver);
        case MessageType::vote:
            return vote_story(before, f, *deliver);
        case MessageType::commit:
            return commit_story(rec, before, f, *deliver);
        case MessageType::abort:
            return abort_story(rec, f, *deliver);
        case MessageType::ack:
            return ack_story(f, *deliver);
        case MessageType::outcome_query:
            return query_story(f, *deliver);
        case MessageType::propose:
            return submit_story(rec, f);
        }
    }
    if (find_kind(f, "LOG_APPEND")) {
        return log_story(rec, f);
    }
    if (const TraceRecord* send = find_kind(f, "MSG_SEND");
        send && send->msg_type && send->from && send->to && send->msg_id) {
        return send_story(rec, index, *send);
    }
    if (const TraceRecord* fire = find_kind(f, "TIMER_FIRE");
        fire && fire->node) {
        return timer_story(before, f, *fire);
    }
    if (const TraceRecord* ignored = find_kind(f, "TIMER_IGNORED")) {
        return ignored_story(*ignored);
    }
    Story s;
    for (const TraceRecord& r : f.records) {
        s.headline += (s.headline.empty() ? "" : ", ") + r.kind;
    }
    s.code = handler_of(f);
    return s;
}

[[nodiscard]] std::size_t trade_count(const Recording& rec)
{
    std::set<TradeId> ids;
    for (const NodeView& view : rec.frames.back().nodes) {
        for (const auto& [id, trade] : view.coordinator_trades) {
            ids.insert(id);
        }
        for (const auto& [id, trade] : view.trades) {
            ids.insert(id);
        }
    }
    return ids.size();
}

} // namespace

Story narrate(const Recording& rec, std::size_t index)
{
    Story s = narrate_frame(rec, index);
    if (index == 0 || index >= rec.frames.size() || trade_count(rec) < 2 ||
        s.headline.find("rade #") != std::string::npos) {
        return s;
    }
    for (const TraceRecord& r : rec.frames[index].records) {
        if (r.trade_id) {
            s.headline = "Trade #" + std::to_string(r.trade_id->value()) +
                         " · " + s.headline;
            break;
        }
    }
    return s;
}

bool is_minor(const Frame& frame)
{
    if (frame.records.empty()) {
        return false;
    }
    for (const TraceRecord& r : frame.records) {
        if (r.kind != "TIMER_IGNORED") {
            return false;
        }
    }
    return true;
}

std::string with_commas(std::int64_t v)
{
    // Magnitude as unsigned so INT64_MIN does not overflow.
    const bool negative = v < 0;
    std::uint64_t m = negative ? 0 - static_cast<std::uint64_t>(v)
                               : static_cast<std::uint64_t>(v);
    std::string digits = std::to_string(m);
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[i];
        const std::size_t left = n - i - 1;
        if (left > 0 && left % 3 == 0) {
            out += ',';
        }
    }
    return negative ? "-" + out : out;
}

std::string dollars(Cents cents)
{
    const std::int64_t v = cents.value();
    const bool negative = v < 0;
    const std::uint64_t m = negative ? 0 - static_cast<std::uint64_t>(v)
                                     : static_cast<std::uint64_t>(v);
    const std::uint64_t whole = m / 100;
    const std::uint64_t part = m % 100;
    // whole < 2^64 / 100 < 2^63, so the cast is exact.
    const std::string text = with_commas(static_cast<std::int64_t>(whole));
    return std::string{negative ? "-$" : "$"} + text + "." +
           (part < 10 ? "0" : "") + std::to_string(part);
}

std::string format_time(SimTimeUs t)
{
    const bool negative = t < 0;
    const std::uint64_t m = negative ? 0 - static_cast<std::uint64_t>(t)
                                     : static_cast<std::uint64_t>(t);
    const std::uint64_t frac = m % 1'000;
    std::string f = std::to_string(frac);
    f.insert(0, 3 - f.size(), '0');
    return std::string{negative ? "-" : ""} +
           with_commas(static_cast<std::int64_t>(m / 1'000)) + "." + f + " ms";
}

std::string format_duration(SimTimeUs d)
{
    if (d < 0) {
        constexpr SimTimeUs lowest = std::numeric_limits<SimTimeUs>::min();
        return "-" + format_duration(
                         d == lowest ? std::numeric_limits<SimTimeUs>::max() : -d);
    }
    if (d < 1'000) {
        return std::to_string(d) + " µs";
    }
    if (d < 1'000'000) {
        return with_commas(d / 1'000) + "." + std::to_string((d % 1'000) / 100) +
               " ms";
    }
    std::string f = std::to_string((d % 1'000'000) / 1'000);
    f.insert(0, 3 - f.size(), '0');
    return with_commas(d / 1'000'000) + "." + f + " s";
}

std::string_view city(NodeId node)
{
    switch (node) {
    case NodeId::coordinator:
    case NodeId::bank_a:
        return "New York";
    case NodeId::bank_b:
        return "London";
    case NodeId::bank_c:
        return "Singapore";
    }
    return "?";
}

std::string bonds_text(std::int64_t qty)
{
    std::string asset{glassbox::to_string(Asset::ust_10y)};
    for (char& c : asset) {
        if (c == '_') {
            c = ' ';
        }
    }
    return with_commas(qty) + " " + asset + (qty == 1 ? " bond" : " bonds");
}

} // namespace glassbox::cli
