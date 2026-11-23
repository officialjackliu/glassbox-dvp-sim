#include "cli/summary.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace glassbox::cli {

namespace {

[[nodiscard]] std::optional<std::size_t> bank_slot(NodeId node) noexcept
{
    switch (node) {
    case NodeId::bank_a:
        return 0;
    case NodeId::bank_b:
        return 1;
    case NodeId::bank_c:
        return 2;
    case NodeId::coordinator:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_outcome(const TraceRecord& r)
{
    return r.kind == "STATE_CHANGE" && r.detail &&
           (r.detail->ends_with("->COMMITTED") ||
            r.detail->ends_with("->ABORTED"));
}

} // namespace

RunSummary summarize(const Recording& rec)
{
    RunSummary s;
    const SimTimeUs last = rec.frames.back().time_us;
    using Key = std::pair<std::size_t, TradeId>; // bank slot, trade
    std::map<TradeId, SimTimeUs> opened;
    std::map<TradeId, SimTimeUs> last_outcome;
    std::map<TradeId, SimTimeUs> ended;
    std::map<Key, SimTimeUs> locked_since;
    std::map<Key, SimTimeUs> doubt_since;

    for (std::size_t i = 1; i < rec.frames.size(); ++i) {
        const Frame& f = rec.frames[i];
        s.violations += f.violations.size();
        for (const TraceRecord& r : f.records) {
            const SimTimeUs t = r.sim_time_us;
            if (r.kind == "MSG_SEND") {
                ++s.sent;
            } else if (r.kind.starts_with("MSG_DROP")) {
                ++s.dropped;
            } else if (r.kind == "RETRANSMIT" && r.detail) {
                s.retransmits += 1 + static_cast<int>(std::count(
                                         r.detail->begin(), r.detail->end(), ','));
            }
            if (r.kind == "CRASH" && r.node && bank_slot(*r.node)) {
                // A lock not yet backed by PREPARED on disk dies with the
                // bank's memory; a durable one is restored on restart.
                const std::size_t slot = *bank_slot(*r.node);
                for (auto it = locked_since.begin(); it != locked_since.end();) {
                    if (it->first.first == slot && !doubt_since.contains(it->first)) {
                        s.lock_hold_us[slot] += t - it->second;
                        it = locked_since.erase(it);
                    } else {
                        ++it;
                    }
                }
                continue;
            }
            if (!r.node || !r.trade_id) {
                continue;
            }
            const TradeId id = *r.trade_id;
            if (*r.node == NodeId::coordinator) {
                if (r.kind == "STATE_CHANGE" && r.detail == "NONE->COLLECTING") {
                    opened.try_emplace(id, t);
                } else if (r.kind == "LOG_APPEND" && r.log_record == "END") {
                    ended.try_emplace(id, t);
                }
                continue;
            }
            const auto slot = bank_slot(*r.node);
            if (!slot) {
                continue;
            }
            const Key key{*slot, id};
            if (r.kind == "LOCK") {
                locked_since.try_emplace(key, t);
            } else if (r.kind == "UNLOCK" || r.kind == "APPLY") {
                if (const auto it = locked_since.find(key);
                    it != locked_since.end()) {
                    s.lock_hold_us[*slot] += t - it->second;
                    locked_since.erase(it);
                }
            } else if (r.kind == "LOG_APPEND" && r.log_record == "PREPARED") {
                doubt_since.try_emplace(key, t);
            }
            if (is_outcome(r)) {
                last_outcome[id] = t; // records are in time order
                if (const auto it = doubt_since.find(key); it != doubt_since.end()) {
                    s.longest_in_doubt_us =
                        std::max(s.longest_in_doubt_us, t - it->second);
                    doubt_since.erase(it);
                }
            }
        }
    }
    // Still open at the end of the recording.
    for (const auto& [key, since] : locked_since) {
        s.lock_hold_us[key.first] += last - since;
    }
    for (const auto& [key, since] : doubt_since) {
        s.longest_in_doubt_us = std::max(s.longest_in_doubt_us, last - since);
    }

    for (const auto& [id, t] : rec.frames.back().nodes[0].coordinator_trades) {
        if (!t.decision) {
            continue;
        }
        TradeOutcome o;
        o.trade_id = id;
        o.buyer = t.trade.buyer();
        o.seller = t.trade.seller();
        o.qty = t.trade.qty().value();
        o.cash = t.trade.cash_cents();
        o.decision = *t.decision;
        o.reason = t.abort_reason;
        const SimTimeUs start = opened.contains(id) ? opened.at(id) : 0;
        if (last_outcome.contains(id)) {
            o.settled_after_us = last_outcome.at(id) - start;
        } else if (ended.contains(id)) {
            o.settled_after_us = ended.at(id) - start;
        }
        s.trades.push_back(o);
    }
    s.checks_run = rec.frames.size() - 1;
    return s;
}

} // namespace glassbox::cli
