#pragma once

// Single-threaded discrete-event simulator.
//
// The simulator owns virtual time, the event queue, the one seeded RNG through
// NetworkModel, node registry, node epochs, durable logs, node-up state, and
// the trace sink. Node implementations remain pure and communicate only via
// Message, TimerEvent, and Effects.

#include "sim/deterministic_rng.hpp"
#include "sim/effects.hpp"
#include "sim/network_model.hpp"
#include "sim/node.hpp"
#include "sim/trace.hpp"
#include "sim/virtual_clock.hpp"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace glassbox {

struct SimulatorConfig {
    std::uint64_t seed = DeterministicRng::default_seed;
    SimTimeUs processing_delay_us = 100;
    SimTimeUs log_flush_delay_us = 1'000;
};

class Simulator {
  public:
    Simulator(const SimulatorConfig& config, TraceSink& trace_sink);

    void set_node(NodeId node, std::unique_ptr<Node> implementation);
    [[nodiscard]] Node* node(NodeId node) noexcept;
    [[nodiscard]] const Node* node(NodeId node) const noexcept;

    [[nodiscard]] bool node_up(NodeId node) const noexcept;
    void set_node_up(NodeId node, bool up) noexcept;

    [[nodiscard]] std::uint64_t epoch(NodeId node) const noexcept;
    [[nodiscard]] const std::vector<LogRecord>& durable_log(
        NodeId node) const noexcept;

    [[nodiscard]] SimTimeUs now_us() const noexcept;
    [[nodiscard]] bool step();
    void run();

    [[nodiscard]] std::optional<SequenceId> schedule_script(
        SimTimeUs time,
        const ScriptActionEvent& action);

    [[nodiscard]] std::optional<SequenceId> schedule_message(
        NodeId from,
        NodeId to,
        MessageType type,
        std::uint64_t msg_id,
        SimTimeUs send_time,
        std::optional<TradeId> trade_id = std::nullopt);

    // Applies one Effects batch structurally:
    // 1. logs become durable at now + log_flush_delay_us;
    // 2. sends are released at now + processing_delay_us
    //    plus log_flush_delay_us if that batch contains logs.
    // Crash-point injection can be added here as a pre/post batch hook.
    [[nodiscard]] std::optional<MeasuredNs> apply_effects(
        NodeId source,
        Effects effects,
        SimTimeUs now);

    void crash_node(NodeId node);
    void restart_node(NodeId node);

    VirtualClock& clock() noexcept;
    EventQueue& events() noexcept;
    NetworkModel& network() noexcept;

  private:
    [[nodiscard]] std::optional<SequenceId> schedule_at(
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

    void emit_trace(SequenceId seq,
                    SimTimeUs now,
                    const std::string& kind,
                    const std::string& handler,
                    MeasuredNs measured_ns);

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
    std::array<std::vector<LogRecord>, k_node_count> durable_logs_{};
    std::uint64_t trace_seq_ = 0;
};

} // namespace glassbox
