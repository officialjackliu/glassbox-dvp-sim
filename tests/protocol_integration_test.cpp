// Task 14: coordinator + three participants through the full event loop.
// Asserts message counts, per-node log sequences, exact timings, balances,
// write-ahead ordering in the trace, and byte-identical reruns.

#include "scenarios/protocol_system.hpp"
#include "sim/simulator.hpp"
#include "check.hpp"

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::LocalLedger;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::ReasonCode;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::Units;

constexpr std::int64_t k_cash = 100'000'000;
constexpr std::int64_t k_bonds = 25'000;
constexpr std::int64_t k_price = 2'487'500;
constexpr std::int64_t k_qty = 25;

class CollectorSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        records.push_back(record);
    }

    std::vector<glassbox::TraceRecord> records;
};

[[nodiscard]] LocalLedger ledger(std::int64_t cash, std::int64_t bonds)
{
    return *LocalLedger::genesis(Cents{cash}, Units{bonds}).ledger;
}

[[nodiscard]] glassbox::BankGenesis default_genesis()
{
    return glassbox::BankGenesis{
        ledger(k_cash, k_bonds),
        ledger(k_cash, k_bonds),
        ledger(k_cash, k_bonds),
    };
}

[[nodiscard]] glassbox::TradeRequest request(NodeId buyer,
                                             NodeId seller,
                                             std::int64_t qty = k_qty,
                                             std::int64_t price = k_price)
{
    return glassbox::TradeRequest{
        buyer,        seller, "UST_10Y", Units{qty}, Cents{price},
        30'000'000,   "client",
    };
}

struct Run {
    CollectorSink sink;
    glassbox::Simulator simulator;
    glassbox::ProtocolNodes nodes;

    explicit Run(const glassbox::BankGenesis& genesis = default_genesis(),
                 glassbox::SimulatorConfig config = {})
        : simulator(config, sink),
          nodes(glassbox::install_protocol_nodes(simulator, genesis))
    {
    }

    void submit(glassbox::SimTimeUs at, glassbox::TradeRequest trade)
    {
        CHECK(simulator
                  .schedule_script(at, glassbox::ScriptActionEvent{
                                           glassbox::ScriptSubmitTrade{
                                               std::move(trade)}})
                  .has_value());
    }

    [[nodiscard]] std::vector<std::string> log_kinds(NodeId node) const
    {
        std::vector<std::string> kinds;
        for (const glassbox::LogEntry& entry : simulator.durable_log(node)) {
            kinds.emplace_back(glassbox::record_kind(entry.record));
        }
        return kinds;
    }

    [[nodiscard]] int count(const std::string& kind,
                            std::optional<MessageType> type = std::nullopt,
                            std::optional<NodeId> node = std::nullopt) const
    {
        int n = 0;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == kind && (!type || record.msg_type == type) &&
                (!node || record.node == node)) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] int notes(const std::string& kind) const
    {
        return count(kind);
    }

    [[nodiscard]] std::optional<glassbox::SimTimeUs> durable_at(
        NodeId node,
        std::size_t index) const
    {
        const auto log = simulator.durable_log(node);
        if (index >= log.size()) {
            return std::nullopt;
        }
        return log[index].durable_at_us;
    }

    [[nodiscard]] std::vector<glassbox::SimTimeUs> delivered_at(
        NodeId to,
        MessageType type) const
    {
        std::vector<glassbox::SimTimeUs> times;
        for (const glassbox::TraceRecord& record : sink.records) {
            if (record.kind == "MSG_DELIVER" && record.node == to &&
                record.msg_type == type) {
                times.push_back(record.sim_time_us);
            }
        }
        return times;
    }
};

[[nodiscard]] std::size_t first_index(
    const std::vector<glassbox::TraceRecord>& records,
    const std::string& kind,
    NodeId node,
    TradeId trade_id,
    const std::string& log_record)
{
    for (std::size_t i = 0; i < records.size(); ++i) {
        const glassbox::TraceRecord& r = records[i];
        if (r.kind == kind && r.node == node && r.trade_id == trade_id &&
            r.log_record == log_record) {
            return i;
        }
    }
    return records.size();
}

// Write-ahead in the trace: a node's first VOTE / ACK / COMMIT / ABORT
// for a trade is sent only after the record that backs it is durable.
void check_write_ahead(const Run& run)
{
    const auto& records = run.sink.records;
    std::map<std::pair<NodeId, std::int64_t>, std::map<MessageType, bool>>
        seen;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const glassbox::TraceRecord& r = records[i];
        if (r.kind != "MSG_SEND" || !r.trade_id || !r.node || !r.msg_type) {
            continue;
        }
        auto& first = seen[{*r.node, r.trade_id->value()}];
        if (first[*r.msg_type]) {
            continue; // duplicates/retransmits may answer from memory
        }
        first[*r.msg_type] = true;
        const NodeId node = *r.node;
        const TradeId trade_id = *r.trade_id;
        std::vector<std::string> backing;
        switch (*r.msg_type) {
        case MessageType::vote:
            backing = {"PREPARED", "ABORTED"};
            break;
        case MessageType::ack:
            backing = {"COMMITTED", "ABORTED"};
            break;
        case MessageType::commit:
            backing = {"DECISION_COMMIT"};
            break;
        case MessageType::abort:
            backing = {"DECISION_ABORT"};
            break;
        case MessageType::prepare:
            backing = {"PROPOSED"};
            break;
        case MessageType::propose:
        case MessageType::outcome_query:
            continue;
        }
        bool backed = false;
        for (const std::string& kind : backing) {
            backed = backed ||
                     first_index(records, "LOG_APPEND", node, trade_id, kind) <
                         i;
        }
        CHECK(backed);
    }
}

void check_conservation_and_no_locks(const Run& run,
                                     const glassbox::BankGenesis& genesis)
{
    const std::array<LocalLedger, 3> before{genesis.bank_a, genesis.bank_b,
                                            genesis.bank_c};
    const std::array<LocalLedger, 3> after{run.nodes.bank_a->ledger(),
                                           run.nodes.bank_b->ledger(),
                                           run.nodes.bank_c->ledger()};
    CHECK(glassbox::totals(before) == glassbox::totals(after));
    for (const LocalLedger& l : after) {
        CHECK(l.cash_locked() == Cents{0});
        CHECK(l.bonds_locked() == Units{0});
        CHECK(l.check_invariants());
    }
}

void test_happy_path_s1()
{
    Run run;
    run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
    run.simulator.run();

    // Final states.
    CHECK(run.nodes.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK(run.nodes.bank_a->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(run.nodes.bank_b->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(run.nodes.bank_c->trades().empty());

    // Per-node durable log sequences.
    CHECK((run.log_kinds(NodeId::coordinator) ==
           std::vector<std::string>{"PROPOSED", "DECISION_COMMIT", "END"}));
    CHECK((run.log_kinds(NodeId::bank_a) ==
           std::vector<std::string>{"PREPARED", "COMMITTED"}));
    CHECK((run.log_kinds(NodeId::bank_b) ==
           std::vector<std::string>{"PREPARED", "COMMITTED"}));
    CHECK(run.log_kinds(NodeId::bank_c).empty());

    // Message counts by type: 2 of each phase message, nothing else.
    CHECK(run.count("MSG_SEND", MessageType::prepare) == 2);
    CHECK(run.count("MSG_SEND", MessageType::vote) == 2);
    CHECK(run.count("MSG_SEND", MessageType::commit) == 2);
    CHECK(run.count("MSG_SEND", MessageType::ack) == 2);
    CHECK(run.count("MSG_SEND", MessageType::abort) == 0);
    CHECK(run.count("MSG_SEND", MessageType::outcome_query) == 0);
    CHECK(run.count("MSG_SEND") == 8);
    CHECK(run.count("MSG_DELIVER") == 9); // 8 + PROPOSE
    CHECK(run.count("MSG_DELIVER", MessageType::propose) == 1);
    CHECK(run.count("MSG_DROP") == 0);
    CHECK(run.count("LOG_APPEND") == 7);

    // Exact timings (default links: COORD-A 1 ms, COORD-B 35 ms;
    // processing 0.1 ms, log flush 1 ms).
    CHECK(run.durable_at(NodeId::coordinator, 0) == 1'000);
    CHECK(run.delivered_at(NodeId::bank_a, MessageType::prepare) ==
          std::vector<glassbox::SimTimeUs>{2'100});
    CHECK(run.delivered_at(NodeId::bank_b, MessageType::prepare) ==
          std::vector<glassbox::SimTimeUs>{36'100});
    CHECK(run.durable_at(NodeId::bank_a, 0) == 3'100);
    CHECK(run.durable_at(NodeId::bank_b, 0) == 37'100);
    CHECK(run.delivered_at(NodeId::coordinator, MessageType::vote) ==
          (std::vector<glassbox::SimTimeUs>{4'200, 72'200}));
    CHECK(run.durable_at(NodeId::coordinator, 1) == 73'200);
    CHECK(run.delivered_at(NodeId::bank_a, MessageType::commit) ==
          std::vector<glassbox::SimTimeUs>{74'300});
    CHECK(run.delivered_at(NodeId::bank_b, MessageType::commit) ==
          std::vector<glassbox::SimTimeUs>{108'300});
    CHECK(run.durable_at(NodeId::bank_a, 1) == 75'300);
    CHECK(run.durable_at(NodeId::bank_b, 1) == 109'300);
    CHECK(run.delivered_at(NodeId::coordinator, MessageType::ack) ==
          (std::vector<glassbox::SimTimeUs>{76'400, 144'400}));
    CHECK(run.durable_at(NodeId::coordinator, 2) == 145'400);

    // Balances moved by exactly the trade.
    const LocalLedger& a = run.nodes.bank_a->ledger();
    const LocalLedger& b = run.nodes.bank_b->ledger();
    CHECK(a.cash_total() == Cents{k_cash - k_price});
    CHECK(a.bonds_total() == Units{k_bonds + k_qty});
    CHECK(b.cash_total() == Cents{k_cash + k_price});
    CHECK(b.bonds_total() == Units{k_bonds - k_qty});
    CHECK(run.nodes.bank_c->ledger() == ledger(k_cash, k_bonds));
    check_conservation_and_no_locks(run, default_genesis());

    // Timers: vote timeout, retransmit, and both decision waits fire into
    // finished trades and are ignored by the state check.
    CHECK(run.count("TIMER_SET") == 4);
    CHECK(run.count("TIMER_FIRE") == 0);
    int state_mismatch = 0;
    for (const glassbox::TraceRecord& r : run.sink.records) {
        if (r.kind == "TIMER_IGNORED") {
            CHECK(r.reason == "state_mismatch");
            ++state_mismatch;
        }
    }
    CHECK(state_mismatch == 4);

    CHECK(run.notes("INVARIANT_VIOLATION") == 0);
    CHECK(run.notes("INVALID_MESSAGE") == 0);
    CHECK(run.notes("REJECTED") == 0);
    check_write_ahead(run);

    // State-change notes carry the handler that caused them.
    std::vector<std::string> transitions;
    for (const glassbox::TraceRecord& r : run.sink.records) {
        if (r.kind == "STATE_CHANGE") {
            transitions.push_back(std::string{glassbox::to_string(*r.node)} +
                                  ":" + *r.detail + "@" + r.handler);
        }
    }
    CHECK((transitions ==
           std::vector<std::string>{
               "COORD:NONE->COLLECTING@Coordinator::on_propose",
               "BANK_A:NONE->PREPARING@Participant::on_prepare",
               "BANK_B:NONE->PREPARING@Participant::on_prepare",
               "COORD:COLLECTING->COMMITTING@Coordinator::decide",
               "BANK_A:PREPARED->COMMITTED@Participant::on_commit",
               "BANK_B:PREPARED->COMMITTED@Participant::on_commit",
               "COORD:COMMITTING->DONE@Coordinator::on_ack",
           }));
}

void test_vote_no_releases_seller_lock()
{
    glassbox::BankGenesis genesis = default_genesis();
    genesis.bank_a = ledger(k_price - 1, 0); // buyer one cent short
    Run run{genesis};
    run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
    run.simulator.run();

    CHECK(run.nodes.coordinator->trade_state(TradeId{1}) == TradeState::done);
    CHECK(run.nodes.bank_a->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK(run.nodes.bank_b->trade_state(TradeId{1}) == TradeState::aborted);
    CHECK((run.log_kinds(NodeId::coordinator) ==
           std::vector<std::string>{"PROPOSED", "DECISION_ABORT", "END"}));
    CHECK(std::get<glassbox::DecisionAbort>(
              run.simulator.durable_log(NodeId::coordinator)[1].record)
              .reason == ReasonCode::insufficient_funds);
    CHECK((run.log_kinds(NodeId::bank_a) ==
           std::vector<std::string>{"ABORTED"}));
    CHECK((run.log_kinds(NodeId::bank_b) ==
           std::vector<std::string>{"PREPARED", "ABORTED"}));
    // No balance moved and the seller's lock is released.
    CHECK(run.nodes.bank_a->ledger() == genesis.bank_a);
    CHECK(run.nodes.bank_b->ledger() == genesis.bank_b);
    check_conservation_and_no_locks(run, genesis);
    CHECK(run.count("MSG_SEND", MessageType::commit) == 0);
    CHECK(run.notes("INVARIANT_VIOLATION") == 0);
    check_write_ahead(run);
}

void test_lost_commit_resolved_by_outcome_query()
{
    // BANK_B never gets the first COMMIT. Its DECISION_WAIT (37,200 + 1 s)
    // fires before the coordinator's retransmit (73,300 + 1 s).
    Run run;
    run.simulator.network().drop_next(NodeId::coordinator, NodeId::bank_b,
                                      MessageType::commit);
    run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
    run.simulator.run();

    CHECK(run.count("MSG_DROP") == 1);
    CHECK(run.count("MSG_SEND", MessageType::outcome_query, NodeId::bank_b) ==
          1);
    CHECK(run.count("MSG_SEND", MessageType::outcome_query, NodeId::bank_a) ==
          0);
    // Query sent 1,037,300, answered at the coordinator 1,072,300; the
    // reply reaches BANK_B at 1,107,400, before the retransmit (1,108,400).
    CHECK(run.delivered_at(NodeId::coordinator,
                           MessageType::outcome_query) ==
          std::vector<glassbox::SimTimeUs>{1'072'300});
    CHECK(run.delivered_at(NodeId::bank_b, MessageType::commit) ==
          (std::vector<glassbox::SimTimeUs>{1'107'400, 1'108'400}));

    CHECK(run.nodes.bank_b->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(run.nodes.coordinator->trade_state(TradeId{1}) == TradeState::done);
    // Duplicate COMMIT is harmless: one COMMITTED record, one END.
    CHECK((run.log_kinds(NodeId::bank_b) ==
           std::vector<std::string>{"PREPARED", "COMMITTED"}));
    CHECK((run.log_kinds(NodeId::coordinator) ==
           std::vector<std::string>{"PROPOSED", "DECISION_COMMIT", "END"}));
    CHECK(run.durable_at(NodeId::coordinator, 2) == 1'144'500);
    CHECK(run.notes("ACK_IGNORED") == 1);
    CHECK(run.notes("INVARIANT_VIOLATION") == 0);
    check_conservation_and_no_locks(run, default_genesis());
    check_write_ahead(run);
}

void test_concurrent_trades_on_different_pairs()
{
    Run run;
    run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
    run.submit(0, request(NodeId::bank_c, NodeId::bank_a, 10, 990'000));
    run.simulator.run();

    for (TradeId id : {TradeId{1}, TradeId{2}}) {
        CHECK(run.nodes.coordinator->trade_state(id) == TradeState::done);
    }
    CHECK(run.nodes.bank_a->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(run.nodes.bank_b->trade_state(TradeId{1}) == TradeState::committed);
    CHECK(run.nodes.bank_c->trade_state(TradeId{2}) == TradeState::committed);
    CHECK(run.nodes.bank_a->trade_state(TradeId{2}) == TradeState::committed);
    CHECK(run.count("MSG_SEND") == 16);
    CHECK((run.log_kinds(NodeId::bank_a) ==
           std::vector<std::string>{"PREPARED", "PREPARED", "COMMITTED",
                                    "COMMITTED"}));
    // BANK_A: buys 25 for 2,487,500, sells 10 for 990,000.
    CHECK(run.nodes.bank_a->ledger().cash_total() ==
          Cents{k_cash - k_price + 990'000});
    CHECK(run.nodes.bank_a->ledger().bonds_total() ==
          Units{k_bonds + k_qty - 10});
    CHECK(run.nodes.bank_c->ledger().cash_total() == Cents{k_cash - 990'000});
    CHECK(run.nodes.bank_c->ledger().bonds_total() == Units{k_bonds + 10});
    check_conservation_and_no_locks(run, default_genesis());
    CHECK(run.notes("INVARIANT_VIOLATION") == 0);
    check_write_ahead(run);
}

void test_wiring_uses_simulator_timer_config()
{
    // Distinct values so a swapped or dropped setting changes the timings.
    {
        glassbox::SimulatorConfig config;
        config.timers.vote_timeout_us = 20'000;
        Run run{default_genesis(), config};
        run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
        run.simulator.run();
        // BANK_B's yes would arrive at 72,200; the 20 ms timeout fires at
        // 1,100 + 20,000 = 21,100 and DECISION_ABORT is durable at 22,100.
        const auto log = run.simulator.durable_log(NodeId::coordinator);
        CHECK(std::get<glassbox::DecisionAbort>(log[1].record).reason ==
              ReasonCode::vote_timeout);
        CHECK(log[1].durable_at_us == 22'100);
        check_conservation_and_no_locks(run, default_genesis());
    }
    {
        glassbox::SimulatorConfig config;
        config.timers.retransmit = glassbox::BackoffPolicy{300'000, 600'000};
        config.timers.decision_wait =
            glassbox::BackoffPolicy{200'000, 400'000};
        Run run{default_genesis(), config};
        run.simulator.network().drop_next(NodeId::coordinator, NodeId::bank_b,
                                          MessageType::commit);
        run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
        run.simulator.run();
        // BANK_B asks at 37,200 + 200,000 (+100 processing); the reply
        // arrives at 307,400 and its ACK reaches the coordinator at 343,500,
        // before the 300 ms retransmit (373,300) fires. END is durable at
        // 344,500.
        std::vector<glassbox::SimTimeUs> queries;
        for (const glassbox::TraceRecord& r : run.sink.records) {
            if (r.kind == "MSG_SEND" &&
                r.msg_type == MessageType::outcome_query) {
                queries.push_back(r.sim_time_us);
            }
        }
        CHECK(queries == std::vector<glassbox::SimTimeUs>{237'300});
        CHECK(run.delivered_at(NodeId::bank_b, MessageType::commit) ==
              std::vector<glassbox::SimTimeUs>{307'400});
        CHECK(run.durable_at(NodeId::coordinator, 2) == 344'500);
        CHECK(run.nodes.bank_b->trade_state(TradeId{1}) ==
              TradeState::committed);
    }
}

void test_rejected_propose_sends_nothing()
{
    Run run;
    run.submit(0, request(NodeId::bank_b, NodeId::bank_b));
    run.simulator.run();
    CHECK(run.notes("REJECTED") == 1);
    CHECK(run.count("MSG_SEND") == 0);
    for (NodeId node : {NodeId::coordinator, NodeId::bank_a, NodeId::bank_b,
                        NodeId::bank_c}) {
        CHECK(run.simulator.durable_log(node).empty());
    }
    CHECK(run.nodes.coordinator->trades().empty());
    for (const glassbox::TraceRecord& r : run.sink.records) {
        if (r.kind == "REJECTED") {
            CHECK(r.detail == "SAME_PARTY");
            CHECK(r.node == NodeId::coordinator);
            CHECK(r.handler == "Coordinator::on_propose");
        }
    }
}

[[nodiscard]] std::string render(const glassbox::TraceRecord& r)
{
    // Every field except measured_ns.
    std::string s = std::to_string(r.seq) + "|" +
                    std::to_string(r.sim_time_us) + "|" + r.kind + "|" +
                    r.handler + "|" + r.reason.value_or("-") + "|";
    s += r.node ? std::string{glassbox::to_string(*r.node)} : "-";
    s += "|" + (r.trade_id ? std::to_string(r.trade_id->value()) : "-");
    s += "|";
    s += r.msg_type ? std::string{glassbox::to_string(*r.msg_type)} : "-";
    s += "|" + (r.msg_id ? std::to_string(*r.msg_id) : "-") + "|";
    s += r.from ? std::string{glassbox::to_string(*r.from)} : "-";
    s += "|";
    s += r.to ? std::string{glassbox::to_string(*r.to)} : "-";
    s += "|" + r.log_record.value_or("-") + "|" + r.detail.value_or("-");
    return s;
}

[[nodiscard]] std::vector<std::string> traced_run(std::uint64_t seed)
{
    glassbox::SimulatorConfig config;
    config.seed = seed;
    Run run{default_genesis(), config};
    // Jitter and random drops make the RNG matter.
    for (NodeId a : {NodeId::coordinator, NodeId::bank_a, NodeId::bank_b}) {
        for (NodeId b : {NodeId::coordinator, NodeId::bank_a,
                         NodeId::bank_b}) {
            if (a != b) {
                run.simulator.network().set_link(
                    a, b, glassbox::LinkConfig{20'000, 15'000, 100'000});
            }
        }
    }
    run.submit(0, request(NodeId::bank_a, NodeId::bank_b));
    run.submit(5'000, request(NodeId::bank_b, NodeId::bank_a, 3, 300'000));
    run.simulator.run();
    // Whatever happened, the protocol must stay atomic.
    for (TradeId id : {TradeId{1}, TradeId{2}}) {
        const TradeState a = run.nodes.bank_a->trade_state(id);
        const TradeState b = run.nodes.bank_b->trade_state(id);
        CHECK((a == TradeState::committed) == (b == TradeState::committed));
    }
    check_conservation_and_no_locks(run, default_genesis());
    CHECK(run.notes("INVARIANT_VIOLATION") == 0);
    std::vector<std::string> lines;
    for (const glassbox::TraceRecord& r : run.sink.records) {
        lines.push_back(render(r));
    }
    return lines;
}

void test_determinism_under_jitter_and_drops()
{
    const auto first = traced_run(7);
    const auto second = traced_run(7);
    CHECK(first == second);
    CHECK(first.size() > 40);
    // A different seed takes a different path (sanity check on the RNG).
    const auto other = traced_run(8);
    CHECK(other != first);
    // Many seeds: atomicity, conservation, no leaked locks every time.
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        static_cast<void>(traced_run(seed));
    }
}

} // namespace

int main()
{
    test_happy_path_s1();
    test_vote_no_releases_seller_lock();
    test_lost_commit_resolved_by_outcome_query();
    test_concurrent_trades_on_different_pairs();
    test_wiring_uses_simulator_timer_config();
    test_rejected_propose_sends_nothing();
    test_determinism_under_jitter_and_drops();
    return 0;
}
