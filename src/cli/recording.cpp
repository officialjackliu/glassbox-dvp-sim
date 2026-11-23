#include "cli/recording.hpp"

#include <algorithm>
#include <map>
#include <variant>

namespace glassbox::cli {

namespace {

[[nodiscard]] NodeView view_of(const ScenarioRun& run, NodeId node)
{
    NodeView view;
    view.node = node;
    view.up = run.simulator().node_up(node);
    for (const LogEntry& entry : run.simulator().durable_log(node)) {
        view.disk.push_back(std::string{record_kind(entry.record)} + " t" +
                            std::to_string(record_trade_id(entry.record).value()));
    }
    if (node == NodeId::coordinator) {
        view.coordinator_trades = run.nodes().coordinator->trades();
        return view;
    }
    const Participant* bank = run.nodes().bank(node);
    view.balances = balance_view(bank->ledger());
    view.frozen = bank->ledger().frozen();
    view.trades = bank->trades();
    return view;
}

} // namespace

// Same fields as glassbox::snapshot(run), but the INVARIANT_VIOLATION count
// is passed in (kept incrementally) instead of rescanning the whole trace,
// which made long recordings quadratic. cli_recording_test checks the two
// agree after every event.
SystemSnapshot snapshot_counted(const ScenarioRun& run,
                                std::size_t invariant_events)
{
    SystemSnapshot s;
    const BankGenesis& g = run.genesis();
    s.genesis = {balance_view(g.bank_a), balance_view(g.bank_b),
                 balance_view(g.bank_c)};
    const auto banks = run.nodes().banks();
    for (std::size_t i = 0; i < banks.size(); ++i) {
        s.current[i] = balance_view(banks[i]->ledger());
        for (const auto& [id, trade] : banks[i]->trades()) {
            s.states[i].emplace(id, trade.state);
        }
    }
    for (const LogEntry& entry :
         run.simulator().durable_log(NodeId::coordinator)) {
        if (const auto* p = std::get_if<Proposed>(&entry.record)) {
            s.trades.push_back(TradeTerms{
                p->trade.trade_id(),
                p->trade.buyer(),
                p->trade.seller(),
                p->trade.qty().value(),
                p->trade.cash_cents().value(),
            });
        }
    }
    s.invariant_events = invariant_events;
    return s;
}

namespace {

[[nodiscard]] Frame capture(const ScenarioRun& run,
                            std::size_t invariant_events)
{
    Frame frame;
    frame.time_us = run.simulator().now_us();
    for (std::size_t i = 0; i < k_view_order.size(); ++i) {
        frame.nodes[i] = view_of(run, k_view_order[i]);
    }
    frame.violations = check_invariants(
        snapshot_counted(run, invariant_events), CheckMode::anytime);
    return frame;
}

// Script actions in the order the simulator runs them: by time, and in
// vector order at equal times (ScenarioRun schedules them in vector order).
[[nodiscard]] std::vector<ScriptActionEvent> firing_order(
    const ScenarioSetup& setup)
{
    std::vector<TimedAction> sorted = setup.script;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const TimedAction& a, const TimedAction& b) {
                         return a.at < b.at;
                     });
    std::vector<ScriptActionEvent> out;
    out.reserve(sorted.size());
    for (const TimedAction& action : sorted) {
        out.push_back(action.action);
    }
    return out;
}

[[nodiscard]] std::vector<Flight> pair_flights(const std::vector<Frame>& frames)
{
    std::vector<Flight> flights;
    std::map<std::uint64_t, std::vector<std::size_t>> by_id; // send order
    for (const Frame& frame : frames) {
        for (const TraceRecord& r : frame.records) {
            if (!r.msg_id || !r.msg_type || !r.from || !r.to) {
                continue;
            }
            if (r.kind == "MSG_SEND") {
                by_id[*r.msg_id].push_back(flights.size());
                flights.push_back(Flight{*r.msg_id, *r.msg_type, *r.from,
                                         *r.to, r.sim_time_us, k_open_end,
                                         r.detail, std::nullopt});
                continue;
            }
            const bool deliver = r.kind == "MSG_DELIVER";
            const bool drop = r.kind.starts_with("MSG_DROP");
            if (!deliver && !drop) {
                continue;
            }
            std::optional<std::string> reason;
            if (drop) {
                if (r.reason) {
                    reason = r.reason;
                } else if (r.kind == "MSG_DROP_NODE_DOWN") {
                    reason = "NODE_DOWN";
                } else if (r.kind == "MSG_DROP_NO_NODE") {
                    reason = "NO_NODE";
                } else {
                    reason = "DROPPED";
                }
            }
            const auto known = by_id.find(*r.msg_id);
            if (known == by_id.end()) {
                continue; // no send at all: a client PROPOSE never travels
            }
            // The earliest copy of this message still on the wire.
            std::optional<std::size_t> open;
            for (const std::size_t i : known->second) {
                if (flights[i].ends_at_us == k_open_end) {
                    open = i;
                    break;
                }
            }
            if (open) {
                flights[*open].ends_at_us = r.sim_time_us;
                flights[*open].drop_reason = reason;
            } else {
                // A duplicated message: another copy of an earlier send.
                Flight copy = flights[known->second.front()];
                copy.ends_at_us = r.sim_time_us;
                copy.drop_reason = reason;
                known->second.push_back(flights.size());
                flights.push_back(copy);
            }
        }
    }
    // Copies were appended when they ended; put them back in send order.
    std::stable_sort(flights.begin(), flights.end(),
                     [](const Flight& a, const Flight& b) {
                         return a.sent_at_us < b.sent_at_us;
                     });
    return flights;
}

// Within one crash epoch a node has at most one pending timer per (trade,
// kind), so matching on epoch makes the pairing exact.
[[nodiscard]] std::vector<TimerSpan> pair_timers(const std::vector<Frame>& frames)
{
    std::vector<TimerSpan> timers;
    std::vector<std::uint64_t> epoch_of; // parallel to timers
    std::array<std::uint64_t, 4> crashes{};
    for (const Frame& frame : frames) {
        for (const TraceRecord& r : frame.records) {
            if (!r.node) {
                continue;
            }
            const std::size_t n = view_index(*r.node);
            if (r.kind == "CRASH") {
                ++crashes[n];
                continue;
            }
            if (!r.trade_id || !r.detail) {
                continue;
            }
            if (r.kind == "TIMER_SET") {
                timers.push_back(TimerSpan{*r.node, *r.trade_id, *r.detail,
                                           r.sim_time_us, std::nullopt});
                epoch_of.push_back(crashes[n]);
                continue;
            }
            if (r.kind != "TIMER_FIRE" && r.kind != "TIMER_IGNORED") {
                continue;
            }
            const bool stale = r.reason == "stale_epoch";
            const bool down = r.reason == "node_down";
            for (std::size_t i = 0; i < timers.size(); ++i) {
                TimerSpan& t = timers[i];
                if (t.fires_at_us || t.node != *r.node ||
                    t.trade_id != *r.trade_id || t.kind != *r.detail) {
                    continue;
                }
                const bool current = epoch_of[i] == crashes[n];
                if (!down && stale == current) {
                    continue;
                }
                t.fires_at_us = r.sim_time_us;
                break;
            }
        }
    }
    return timers;
}

} // namespace

std::size_t view_index(NodeId node) noexcept
{
    switch (node) {
    case NodeId::coordinator:
        return 0;
    case NodeId::bank_a:
        return 1;
    case NodeId::bank_b:
        return 2;
    case NodeId::bank_c:
        return 3;
    }
    return 0;
}

Recording record(const ScenarioSetup& setup, std::size_t max_events)
{
    Recording out;
    ScenarioRun run{setup};
    const std::vector<ScriptActionEvent> script = firing_order(setup);
    std::size_t next_action = 0;
    std::size_t seen = 0;
    std::size_t invariant_events = 0;

    out.frames.push_back(capture(run, invariant_events));
    for (std::size_t events = 0; events < max_events; ++events) {
        if (!run.simulator().step()) {
            out.quiesced = true;
            break;
        }
        const std::vector<TraceRecord>& trace = run.trace();
        for (std::size_t i = seen; i < trace.size(); ++i) {
            invariant_events += trace[i].kind == "INVARIANT_VIOLATION" ? 1 : 0;
        }
        Frame frame = capture(run, invariant_events);
        frame.records.assign(trace.begin() + static_cast<std::ptrdiff_t>(seen),
                             trace.end());
        seen = trace.size();
        for (const TraceRecord& r : frame.records) {
            if (r.kind == "SCRIPT_ACTION" && next_action < script.size()) {
                frame.action = script[next_action++];
            }
        }
        out.frames.push_back(std::move(frame));
    }
    if (!out.quiesced && !run.simulator().step()) {
        // Hit max_events exactly as the queue drained.
        out.quiesced = true;
    }
    out.flights = pair_flights(out.frames);
    out.timers = pair_timers(out.frames);
    return out;
}

std::vector<const Flight*> in_flight(const Recording& recording, SimTimeUs t)
{
    std::vector<const Flight*> out;
    for (const Flight& f : recording.flights) {
        if (f.sent_at_us <= t && t < f.ends_at_us) {
            out.push_back(&f);
        }
    }
    return out;
}

} // namespace glassbox::cli
