// Task 21: S9-S12 coordinator and communication failures, plus the
// network duplicate_next injector used by S12.

#include "scenarios/scenario_library.hpp"
#include "scenario_checks.hpp"

#include <map>
#include <string>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::MessageType;
using glassbox::NodeId;
using glassbox::TradeId;
using glassbox::TradeState;
using glassbox::Units;
using scenario_checks::count;
using scenario_checks::log_kinds;
using scenario_checks::notes;
using scenario_checks::sends;
using scenario_checks::times;
using Lines = std::vector<std::string>;
using Times = std::vector<glassbox::SimTimeUs>;

constexpr std::int64_t k_price = 2'487'500;

[[nodiscard]] Lines events(const glassbox::ScenarioRun& run,
                           const std::string& kind)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == kind) {
            out.push_back(std::to_string(r.sim_time_us) + " " +
                          std::string{glassbox::to_string(*r.node)} +
                          (r.reason ? " " + *r.reason : ""));
        }
    }
    return out;
}

[[nodiscard]] Lines drops(const glassbox::ScenarioRun& run)
{
    Lines out;
    for (const glassbox::TraceRecord& r : run.trace()) {
        if (r.kind == "MSG_DROP") {
            out.push_back(std::to_string(r.sim_time_us) + " " +
                          std::string{glassbox::to_string(*r.msg_type)} +
                          " " + std::string{glassbox::to_string(*r.from)} +
                          "->" + std::string{glassbox::to_string(*r.to)} +
                          " " + r.reason.value_or("-"));
        }
    }
    return out;
}

[[nodiscard]] glassbox::SimTimeUs durable_at(const glassbox::ScenarioRun& run,
                                             NodeId node,
                                             const std::string& kind)
{
    for (const glassbox::LogEntry& e : run.simulator().durable_log(node)) {
        if (glassbox::record_kind(e.record) == kind) {
            return e.durable_at_us;
        }
    }
    return -1;
}

void check_committed_everywhere(const glassbox::ScenarioRun& run)
{
    CHECK(run.nodes().coordinator->trade_state(TradeId{1}) ==
          TradeState::done);
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(log_kinds(run, NodeId::bank_a) == (Lines{"PREPARED", "COMMITTED"}));
    CHECK(log_kinds(run, NodeId::bank_b) == (Lines{"PREPARED", "COMMITTED"}));
    CHECK(log_kinds(run, NodeId::coordinator) ==
          (Lines{"PROPOSED", "DECISION_COMMIT", "END"}));
    CHECK(run.nodes().bank_a->ledger().cash_total() ==
          Cents{100'000'000 - k_price});
    CHECK(run.nodes().bank_a->ledger().bonds_total() == Units{25'025});
    CHECK(run.nodes().bank_b->ledger().cash_total() ==
          Cents{100'000'000 + k_price});
    CHECK(run.nodes().bank_b->ledger().bonds_total() == Units{24'975});
    CHECK(count(run, "INVALID_MESSAGE") == 0);
    scenario_checks::check_quiescent_invariants(run);
}

void test_s9_blocking_until_coordinator_restarts()
{
    glassbox::ScenarioRun run{
        glassbox::make_s9_coordinator_crash_after_decision(1)};

    // Mid-outage: the decision is durable but unannounced; both
    // participants are in doubt and keep their locks. This is the blocking.
    scenario_checks::run_until(run, 4'000'000);
    CHECK(events(run, "CRASH") == Lines{"73200 COORD AFTER_LOG_BEFORE_SEND"});
    CHECK(durable_at(run, NodeId::coordinator, "DECISION_COMMIT") == 73'200);
    CHECK(!run.simulator().node_up(NodeId::coordinator));
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_a->ledger().cash_locked() == Cents{k_price});
    CHECK(run.nodes().bank_b->ledger().bonds_locked() == Units{25});
    CHECK(count(run, "MSG_SEND", MessageType::commit) == 0);

    CHECK(run.run());
    CHECK(events(run, "RESTART") == Lines{"5073200 COORD"});
    // Every query during the outage is dropped at the down coordinator.
    CHECK(drops(run) ==
          (Lines{"1004300 OUTCOME_QUERY BANK_A->COORD NODE_DOWN",
                 "1072300 OUTCOME_QUERY BANK_B->COORD NODE_DOWN",
                 "3004400 OUTCOME_QUERY BANK_A->COORD NODE_DOWN",
                 "3072400 OUTCOME_QUERY BANK_B->COORD NODE_DOWN"}));
    // The stale vote timer died with the crash.
    CHECK(count(run, "TIMER_FIRE") == 4); // the four decision waits
    CHECK(notes(run, "RECOVERY_STEP", NodeId::coordinator) ==
          Lines{"5073200 RESEND_DECISION"});
    CHECK(sends(run, NodeId::coordinator, MessageType::commit) ==
          (Lines{"5073300 - ->BANK_A", "5073300 - ->BANK_B"}));
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::commit) ==
          Times{5'074'300});
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_b, MessageType::commit) ==
          Times{5'108'300});
    CHECK(durable_at(run, NodeId::coordinator, "END") == 5'145'400);
    check_committed_everywhere(run);
}

void test_s10_one_commits_other_waits()
{
    glassbox::ScenarioRun run{
        glassbox::make_s10_coordinator_crash_mid_broadcast(1)};
    scenario_checks::run_until(run, 4'000'000);
    CHECK(events(run, "CRASH") == Lines{"73300 COORD AFTER_FIRST_SEND"});
    // Only the first COMMIT of the broadcast (to the buyer) left.
    CHECK(sends(run, NodeId::coordinator, MessageType::commit) ==
          Lines{"73300 - ->BANK_A"});
    CHECK(run.nodes().bank_a->trade_state(TradeId{1}) ==
          TradeState::committed);
    CHECK(run.nodes().bank_b->trade_state(TradeId{1}) == TradeState::prepared);
    CHECK(run.nodes().bank_b->ledger().bonds_locked() == Units{25});

    CHECK(run.run());
    CHECK(events(run, "RESTART") == Lines{"5073300 COORD"});
    CHECK(drops(run) ==
          (Lines{"76400 ACK BANK_A->COORD NODE_DOWN",
                 "1072300 OUTCOME_QUERY BANK_B->COORD NODE_DOWN",
                 "3072400 OUTCOME_QUERY BANK_B->COORD NODE_DOWN"}));
    // Restart resends to both; BANK_A's copy is a harmless duplicate.
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::commit) ==
          (Times{74'300, 5'074'400}));
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_b, MessageType::commit) ==
          Times{5'108'400});
    CHECK(sends(run, NodeId::bank_a, MessageType::ack) ==
          (Lines{"75400 COMMIT ->COORD", "5074500 COMMIT ->COORD"}));
    CHECK(notes(run, "APPLY", NodeId::bank_a) ==
          Lines{"74300 CASH -2487500 BONDS +25"});
    CHECK(durable_at(run, NodeId::coordinator, "END") == 5'145'500);
    check_committed_everywhere(run);
}

void test_s11_retransmit_delivers_lost_commit()
{
    glassbox::ScenarioRun run{glassbox::make_s11_lost_commit(1)};
    CHECK(run.run());
    CHECK(drops(run) == Lines{"74300 COMMIT COORD->BANK_A DROP_NEXT"});
    CHECK(notes(run, "RETRANSMIT", NodeId::coordinator) ==
          Lines{"1073300 BANK_A"});
    CHECK(times(run, "MSG_DELIVER", NodeId::bank_a, MessageType::commit) ==
          Times{1'074'400});
    CHECK(notes(run, "APPLY", NodeId::bank_a) ==
          Lines{"1074400 CASH -2487500 BONDS +25"});
    CHECK(durable_at(run, NodeId::coordinator, "END") == 1'077'500);
    // BANK_A never had to ask: its 2 s decision wait fires after it
    // committed and is ignored.
    CHECK(count(run, "MSG_SEND", MessageType::outcome_query) == 0);
    CHECK(run.simulator().config().timers.decision_wait.initial_us ==
          2'000'000);
    check_committed_everywhere(run);
}

struct S12Coverage {
    int duplicate_ignored = 0;  // copy arrived while PREPARING
    int late_yes_resend = 0;    // copy arrived after PREPARED/COMMITTED
    int commit_twice = 0;       // a participant got COMMIT more than once
    int copies_apart = 0;       // the two PREPARE copies arrived at
                                // different times (reordered by jitter)
};

void run_s12(std::uint64_t seed, S12Coverage& coverage)
{
    glassbox::ScenarioRun run{glassbox::make_s12_duplicates_and_reordering(seed)};
    CHECK(run.run());
    check_committed_everywhere(run);
    CHECK(count(run, "MSG_DROP") == 0);
    for (NodeId bank : {NodeId::bank_a, NodeId::bank_b}) {
        const Times prepares =
            times(run, "MSG_DELIVER", bank, MessageType::prepare);
        CHECK(prepares.size() == 2);
        coverage.copies_apart += prepares[0] != prepares[1] ? 1 : 0;
        const std::size_t commits =
            times(run, "MSG_DELIVER", bank, MessageType::commit).size();
        CHECK(commits >= 2);
        coverage.commit_twice += commits >= 2 ? 1 : 0;
        const int votes = count(run, "MSG_SEND", MessageType::vote, bank);
        CHECK(votes == 1 || votes == 2);
        coverage.late_yes_resend += votes == 2 ? 1 : 0;
        coverage.duplicate_ignored +=
            static_cast<int>(notes(run, "DUPLICATE_IGNORED", bank).size());
        // A lock is taken exactly once per participant.
        CHECK(notes(run, "LOCK", bank).size() == 1);
        CHECK(notes(run, "APPLY", bank).size() == 1);
    }
}

void test_s12_duplicates_and_reordering()
{
    S12Coverage coverage;
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        run_s12(seed, coverage);
    }
    // The sweep exercised every duplicate path in the state tables.
    CHECK(coverage.duplicate_ignored > 0);
    CHECK(coverage.late_yes_resend > 0);
    CHECK(coverage.commit_twice == 600);
    CHECK(coverage.copies_apart > 500);

    // Deterministic per seed.
    glassbox::ScenarioRun a{glassbox::make_s12_duplicates_and_reordering(42)};
    glassbox::ScenarioRun b{glassbox::make_s12_duplicates_and_reordering(42)};
    CHECK(a.run());
    CHECK(b.run());
    CHECK(a.trace().size() == b.trace().size());
    for (std::size_t i = 0; i < a.trace().size(); ++i) {
        CHECK(a.trace()[i].sim_time_us == b.trace()[i].sim_time_us);
        CHECK(a.trace()[i].kind == b.trace()[i].kind);
        CHECK(a.trace()[i].detail == b.trace()[i].detail);
    }
}

// --- duplicate_next unit tests ---------------------------------------------

void test_duplicate_next_network()
{
    using glassbox::DeterministicRng;
    using glassbox::EventQueue;
    using glassbox::NetworkModel;
    const glassbox::LinkConfig jittery{10'000, 5'000, 0};

    // Copy keeps msg_id and takes the next draw: same times as two sends.
    EventQueue dup_queue;
    DeterministicRng dup_rng{5};
    NetworkModel dup{dup_rng};
    dup.set_link(NodeId::coordinator, NodeId::bank_a, jittery);
    dup.duplicate_next(NodeId::coordinator, NodeId::bank_a,
                       MessageType::commit);
    glassbox::Message m = glassbox::make_commit(NodeId::bank_a, TradeId{1});
    m.msg_id = 77;
    const auto first = dup.schedule_delivery(dup_queue, 100, m);
    CHECK(first.delivered);
    CHECK(dup_queue.size() == 2);

    EventQueue twin_queue;
    DeterministicRng twin_rng{5};
    NetworkModel twin{twin_rng};
    twin.set_link(NodeId::coordinator, NodeId::bank_a, jittery);
    const auto t1 = twin.schedule_delivery(twin_queue, 100, m);
    const auto t2 = twin.schedule_delivery(twin_queue, 100, m);
    CHECK(first.deliver_time_us == t1.deliver_time_us);

    std::vector<glassbox::SimTimeUs> dup_times;
    while (auto e = dup_queue.pop()) {
        const auto& d = std::get<glassbox::DeliverMessageEvent>(e->payload);
        CHECK(d.message.msg_id == 77);
        CHECK(d.message.type == MessageType::commit);
        dup_times.push_back(d.deliver_at_us);
    }
    std::vector<glassbox::SimTimeUs> twin_times;
    while (auto e = twin_queue.pop()) {
        twin_times.push_back(
            std::get<glassbox::DeliverMessageEvent>(e->payload).deliver_at_us);
    }
    CHECK(dup_times == twin_times);
    CHECK(t1.deliver_time_us != t2.deliver_time_us); // jitter differs

    // One-shot, and only the matching type and direction.
    EventQueue q;
    DeterministicRng rng{1};
    NetworkModel net{rng};
    net.duplicate_next(NodeId::coordinator, NodeId::bank_a,
                       MessageType::prepare);
    static_cast<void>(net.schedule_delivery(
        q, 0, glassbox::make_commit(NodeId::bank_a, TradeId{1})));
    CHECK(q.size() == 1); // wrong type
    static_cast<void>(net.schedule_delivery(
        q, 0, glassbox::make_ack(NodeId::bank_a, TradeId{1},
                                 glassbox::Decision::commit)));
    CHECK(q.size() == 2); // wrong direction
    glassbox::Message other_destination =
        glassbox::make_commit(NodeId::bank_b, TradeId{1});
    other_destination.type = MessageType::prepare;
    static_cast<void>(net.schedule_delivery(q, 0, other_destination));
    CHECK(q.size() == 3); // same sender and type, other destination
    glassbox::Message prepare = glassbox::make_commit(NodeId::bank_a,
                                                      TradeId{1});
    prepare.type = MessageType::prepare;
    static_cast<void>(net.schedule_delivery(q, 0, prepare));
    CHECK(q.size() == 5); // duplicated
    static_cast<void>(net.schedule_delivery(q, 0, prepare));
    CHECK(q.size() == 6); // consumed

    // A duplicated message on a partitioned link: both copies dropped.
    EventQueue pq;
    DeterministicRng prng{1};
    NetworkModel part{prng};
    part.set_partition(NodeId::coordinator, NodeId::bank_b, true);
    part.duplicate_next(NodeId::coordinator, NodeId::bank_b,
                        MessageType::commit);
    const auto dropped = part.schedule_delivery(
        pq, 0, glassbox::make_commit(NodeId::bank_b, TradeId{1}));
    CHECK(!dropped.delivered);
    CHECK(pq.size() == 2);
    while (auto e = pq.pop()) {
        CHECK(e->kind == glassbox::EventKind::drop_message);
    }
}

void test_registry()
{
    CHECK(glassbox::find_scenario("S9")->make ==
          &glassbox::make_s9_coordinator_crash_after_decision);
    CHECK(glassbox::find_scenario("S10")->make ==
          &glassbox::make_s10_coordinator_crash_mid_broadcast);
    CHECK(glassbox::find_scenario("S11")->make ==
          &glassbox::make_s11_lost_commit);
    CHECK(glassbox::find_scenario("S12")->make ==
          &glassbox::make_s12_duplicates_and_reordering);
    const auto s9 = glassbox::make_s9_coordinator_crash_after_decision(1);
    CHECK(s9.crashes.size() == 1);
    CHECK(s9.crashes[0].node == NodeId::coordinator);
    CHECK(s9.crashes[0].occurrence == 2);
    CHECK(std::get<MessageType>(s9.crashes[0].trigger) == MessageType::vote);
    CHECK(glassbox::make_s10_coordinator_crash_mid_broadcast(1)
              .crashes[0]
              .point == glassbox::CrashPoint::after_first_send);
}

void test_s9_s10_outcome_independent_of_restart_delay()
{
    for (glassbox::SimTimeUs delay : {1'000, 900'000, 2'000'000, 12'000'000}) {
        for (auto make : {&glassbox::make_s9_coordinator_crash_after_decision,
                          &glassbox::make_s10_coordinator_crash_mid_broadcast}) {
            glassbox::ScenarioSetup setup = make(1);
            setup.crashes[0].restart_after_us = delay;
            glassbox::ScenarioRun run{setup};
            CHECK(run.run());
            check_committed_everywhere(run);
        }
    }
}

} // namespace

int main()
{
    test_registry();
    test_duplicate_next_network();
    test_s9_blocking_until_coordinator_restarts();
    test_s10_one_commits_other_waits();
    test_s11_retransmit_delivers_lost_commit();
    test_s12_duplicates_and_reordering();
    test_s9_s10_outcome_independent_of_restart_delay();
    return 0;
}
