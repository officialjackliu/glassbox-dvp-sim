#pragma once

// Single-threaded discrete-event simulator.
//
// The simulator owns virtual time, the event queue, the one seeded RNG through
// NetworkModel, node registry, node epochs, durable logs, node-up state, and
// the trace sink. Node implementations remain pure and communicate only via
// Message, TimerEvent, and Effects.

#include "faults/crash_spec.hpp"
#include "sim/deterministic_rng.hpp"
#include "sim/effects.hpp"
#include "sim/network_model.hpp"
#include "sim/node.hpp"
#include "sim/trace.hpp"
#include "sim/timers.hpp"
#include "sim/virtual_clock.hpp"
#include "domain/durable_log.hpp"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace glassbox {

// Protocol timer settings (DESIGN.md section 6.2). Backoff policies double the
// delay on each attempt up to their maximum.
struct TimerConfig {
    SimTimeUs vote_timeout_us = 500'000;
    BackoffPolicy retransmit{
        1'000'000,
        8'000'000,
    };
    BackoffPolicy decision_wait{
        1'000'000,
        8'000'000,
    };
};

// Simulator settings: the RNG seed (same seed, same run), how long a handler
// takes before its messages leave, how long a disk write takes, and timers.
struct SimulatorConfig {
    std::uint64_t seed = DeterministicRng::default_seed;
    SimTimeUs processing_delay_us = 100;
    SimTimeUs log_flush_delay_us = 1'000;
    TimerConfig timers;
};

// Result of putting an event on the queue.
enum class ScheduleStatus : std::uint8_t {
    scheduled,
    past_time,
    queue_exhausted,
};

// The status and, when scheduled, the event's sequence number.
struct ScheduleResult {
    ScheduleStatus status = ScheduleStatus::queue_exhausted;
    std::optional<SequenceId> seq;
};

// The discrete-event loop. It owns everything that is not a node: virtual
// time, the event queue, the network, the single seeded RNG, each node's
// durable log, up/down state and crash epoch, and the trace sink. Nodes are
// pure state machines installed with set_node(); the simulator delivers their
// messages and timers and applies the Effects they return, enforcing the
// write-ahead rule (a batch's messages leave only after its log records are
// durable).
//
//   RecordingSink sink;
//   Simulator sim{SimulatorConfig{}, sink};
//   sim.set_node(NodeId::coordinator, std::make_unique<Coordinator>(...));
//   ...
//   while (sim.step()) {}
//
// Not thread-safe; everything runs on one thread.
class Simulator {
  public:
    // `trace_sink` receives every trace record and must outlive the simulator.
    Simulator(const SimulatorConfig& config, TraceSink& trace_sink);

    // Returns the settings the simulator was built with.
    [[nodiscard]] const SimulatorConfig& config() const noexcept;

    // Installs (or replaces) the state machine for `node`. node() returns it, or
    // nullptr if none is installed.
    void set_node(NodeId node, std::unique_ptr<Node> implementation);
    [[nodiscard]] Node* node(NodeId node) noexcept;
    [[nodiscard]] const Node* node(NodeId node) const noexcept;

    // Whether `node` is up. A node that is down receives nothing and its timers
    // are ignored; set_node_up() changes this without a crash (memory kept).
    [[nodiscard]] bool node_up(NodeId node) const noexcept;
    void set_node_up(NodeId node, bool up) noexcept;

    // Returns how many times `node` has crashed. Deferred work stamped with an
    // older epoch is ignored.
    [[nodiscard]] std::uint64_t epoch(NodeId node) const noexcept;
    // Returns `node`'s durable log, which survives its crashes.
    [[nodiscard]] std::span<const LogEntry> durable_log(
        NodeId node) const noexcept;

    // Returns the current simulated time.
    [[nodiscard]] SimTimeUs now_us() const noexcept;
    // Pops and handles the earliest event. Returns false if there was none.
    [[nodiscard]] bool step();
    // Handles events until the queue is empty. Use run_bounded() for runs that
    // might never quiesce.
    void run();
    // Processes at most max_events events. Returns true if the queue drained
    // (quiescence), false if the bound was hit first.
    [[nodiscard]] bool run_bounded(std::size_t max_events);

    // Schedules a scripted action (fault, network change, trade submission) at
    // `time`. Returns its sequence number, or std::nullopt if it could not be
    // scheduled.
    [[nodiscard]] std::optional<SequenceId> schedule_script(
        SimTimeUs time,
        const ScriptActionEvent& action);

    // Sends a body-less message of `type` through the network model at
    // `send_time` (used by lower-level tests). Returns the delivery or drop
    // event's sequence number.
    [[nodiscard]] std::optional<SequenceId> schedule_message(
        NodeId from,
        NodeId to,
        MessageType type,
        std::uint64_t msg_id,
        SimTimeUs send_time,
        std::optional<TradeId> trade_id = std::nullopt);

    // Sends a complete message (with body) through the network model.
    [[nodiscard]] std::optional<SequenceId> schedule_message(
        Message message,
        SimTimeUs send_time);

    // Applies one Effects batch structurally:
    // 1. logs become durable at now + log_flush_delay_us;
    // 2. sends are released at now + processing_delay_us
    //    plus log_flush_delay_us if that batch contains logs.
    // A registered CrashSpec that matches the event being handled cuts the
    // batch short at its crash point (faults/crash_spec.hpp).
    [[nodiscard]] std::optional<MeasuredNs> apply_effects(
        NodeId source,
        Effects effects,
        SimTimeUs now);

    // reason, when given, is traced on the CRASH record.
    // Crashes `node` now: increments its epoch, wipes its memory, keeps its disk.
    // `reason`, when given, is traced on the CRASH record.
    void crash_node(NodeId node,
                    std::optional<std::string> reason = std::nullopt);
    // Restarts a crashed `node` and lets it rebuild itself from its durable log.
    void restart_node(NodeId node);

    // Registers a one-shot crash point. Returns false (and registers
    // nothing) if occurrence is 0 or restart_after_us is negative.
    [[nodiscard]] bool add_crash_spec(const CrashSpec& spec);
    // Returns how many registered crash points have not fired yet.
    [[nodiscard]] std::size_t pending_crash_specs() const noexcept;

    // Direct access to the internals, for tests.
    VirtualClock& clock() noexcept;
    EventQueue& events() noexcept;
    NetworkModel& network() noexcept;

  private:
    [[nodiscard]] ScheduleResult schedule_at(
        SimTimeUs time,
        EventKind kind,
        EventPayload payload);

    void dispatch(const Event& event);
    void dispatch_deliver(const DeliverMessageEvent& payload,
                          SequenceId seq);
    void dispatch_drop(const DropMessageEvent& payload,
                       SequenceId seq);
    void dispatch_flush(const FlushLogEvent& payload,
                        SequenceId seq);
    void dispatch_release(const ReleaseSendEvent& payload,
                          SequenceId seq);
    void dispatch_timer(const FireTimerEvent& payload,
                        SequenceId seq);
    void dispatch_crash(const NodeCrashEvent& payload,
                        SequenceId seq);
    void dispatch_restart(const NodeRestartEvent& payload,
                          SequenceId seq);
    void dispatch_script(const ScriptActionEvent& payload,
                         SequenceId seq);
    // Returns the spec that fires for this event (and consumes it), if any.
    [[nodiscard]] std::optional<CrashSpec> take_crash_spec(
        NodeId node,
        const CrashTrigger& trigger,
        std::optional<TradeId> trade_id);
    [[nodiscard]] std::optional<MeasuredNs> apply_effects_impl(
        NodeId source,
        Effects effects,
        SimTimeUs now,
        const std::optional<CrashSpec>& crash);
    void schedule_spec_crash(const CrashSpec& spec, SimTimeUs at);

    void deliver_to_node(NodeId target,
                         const Message& message,
                         SequenceId seq);
    [[nodiscard]] std::uint64_t next_msg_id() noexcept;

    void emit(TraceRecord record);
    [[nodiscard]] TraceRecord message_trace(
        const std::string& kind,
        const std::string& handler,
        NodeId node,
        const Message& message) const;

    void emit_trace(SequenceId seq,
                    SimTimeUs now,
                    const std::string& kind,
                    const std::string& handler,
                    MeasuredNs measured_ns,
                    std::optional<std::string> reason = std::nullopt);

    [[nodiscard]] static std::size_t node_index(NodeId node) noexcept;

    SimulatorConfig config_;
    TraceSink& trace_sink_;
    DeterministicRng rng_;
    VirtualClock clock_;
    EventQueue queue_;
    NetworkModel network_;
    std::array<std::unique_ptr<Node>, k_node_count> nodes_{};
    std::array<bool, k_node_count> node_up_{};
    std::array<std::uint64_t, k_node_count> epochs_{};
    std::array<DurableLog, k_node_count> durable_logs_{};
    std::uint64_t trace_seq_ = 0;
    std::uint64_t next_msg_id_ = 1;
    struct ArmedCrash {
        CrashSpec spec;
        std::uint32_t matches = 0;
    };
    std::vector<ArmedCrash> crash_specs_;
};

} // namespace glassbox
