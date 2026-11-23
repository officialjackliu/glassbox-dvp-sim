#include "sim/simulator.hpp"

#include "sim/virtual_clock.hpp"

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace glassbox {

Simulator::Simulator(const SimulatorConfig& config, TraceSink& trace_sink)
    : config_(config),
      trace_sink_(trace_sink),
      rng_(config.seed),
      network_(rng_)
{
    if (config.processing_delay_us < 0 ||
        config.log_flush_delay_us < 0) {
        throw std::invalid_argument(
            "processing and log flush delays must be non-negative");
    }
    if (config.timers.vote_timeout_us <= 0 ||
        !validate(config.timers.retransmit) ||
        !validate(config.timers.decision_wait)) {
        throw std::invalid_argument(
            "timer configuration must have positive vote timeout and valid backoffs");
    }
    node_up_.fill(true);
    epochs_.fill(0);
}

void Simulator::set_node(NodeId node, std::unique_ptr<Node> implementation)
{
    nodes_[node_index(node)] = std::move(implementation);
}

Node* Simulator::node(NodeId node) noexcept
{
    return nodes_[node_index(node)].get();
}

const Node* Simulator::node(NodeId node) const noexcept
{
    return nodes_[node_index(node)].get();
}

bool Simulator::node_up(NodeId node) const noexcept
{
    return node_up_[node_index(node)];
}

void Simulator::set_node_up(NodeId node, bool up) noexcept
{
    node_up_[node_index(node)] = up;
    network_.set_node_down(node, !up);
}

std::uint64_t Simulator::epoch(NodeId node) const noexcept
{
    return epochs_[node_index(node)];
}

const std::vector<LogRecord>& Simulator::durable_log(NodeId node) const noexcept
{
    return durable_logs_[node_index(node)];
}

SimTimeUs Simulator::now_us() const noexcept
{
    return clock_.now_us();
}

ScheduleResult Simulator::schedule_at(
    SimTimeUs time,
    EventKind kind,
    EventPayload payload)
{
    if (time < clock_.now_us()) {
        return {ScheduleStatus::past_time, std::nullopt};
    }
    const auto seq = queue_.schedule(time, kind, std::move(payload));
    if (!seq) {
        return {ScheduleStatus::queue_exhausted, std::nullopt};
    }
    return {ScheduleStatus::scheduled, seq};
}

std::optional<SequenceId> Simulator::schedule_script(
    SimTimeUs time,
    const ScriptActionEvent& action)
{
    return schedule_at(
        time,
        EventKind::script_action,
        action).seq;
}

std::optional<SequenceId> Simulator::schedule_message(
    NodeId from,
    NodeId to,
    MessageType type,
    std::uint64_t msg_id,
    SimTimeUs send_time,
    std::optional<TradeId> trade_id)
{
    if (send_time < clock_.now_us()) {
        return std::nullopt;
    }
    if (!node_up(from)) {
        return std::nullopt;
    }
    return network_.schedule_delivery(
        queue_,
        send_time,
        from,
        to,
        type,
        msg_id,
        trade_id).event_seq;
}

std::optional<MeasuredNs> Simulator::apply_effects(
    NodeId source,
    Effects effects,
    SimTimeUs now)
{
    if (!node_up(source)) {
        return std::nullopt;
    }

    const std::uint64_t source_epoch = epoch(source);

    // Crash-point seam:
    // Crash/recovery injection can run immediately after this batch is
    // classified and before the flush/send/timer events below are scheduled.
    const bool has_logs = !effects.logs.empty();
    const auto durable_time_result =
        checked_add_i64(now, config_.log_flush_delay_us);
    const auto processing_time_result =
        checked_add_i64(now, config_.processing_delay_us);
    if (!durable_time_result || !processing_time_result) {
        return std::nullopt;
    }
    const SimTimeUs durable_time = *durable_time_result;
    std::optional<SimTimeUs> send_time;
    if (has_logs) {
        send_time = checked_add_i64(
            *processing_time_result,
            config_.log_flush_delay_us);
    } else {
        send_time = *processing_time_result;
    }
    if (!send_time) {
        return std::nullopt;
    }

    if (*send_time >= 0) {
        if (has_logs) {
            std::vector<DeferredLogRecord> deferred_logs;
            deferred_logs.reserve(effects.logs.size());
            for (const LogRecord& record : effects.logs) {
                deferred_logs.push_back(
                    DeferredLogRecord{
                        record.kind,
                        record.trade_id,
                    });
            }
            const auto scheduled_log = schedule_at(
                durable_time,
                EventKind::flush_log,
                FlushLogEvent{
                    source,
                    source_epoch,
                    std::move(deferred_logs),
                });
            if (scheduled_log.status != ScheduleStatus::scheduled) {
                emit_trace(
                    SequenceId{0},
                    now,
                    "LOG_DEFER_REJECTED",
                    "Effects::flush",
                    0);
            }
        }

        for (Outgoing& outgoing : effects.sends) {
            const auto scheduled_send = schedule_at(
                *send_time,
                EventKind::release_send,
                ReleaseSendEvent{
                    source,
                    source_epoch,
                    outgoing.message,
                });
            if (scheduled_send.status != ScheduleStatus::scheduled) {
                emit_trace(
                    SequenceId{0},
                    now,
                    "SEND_DEFER_REJECTED",
                    "Effects::release",
                    0);
            }
        }
    }

    for (TimerRequest& timer : effects.timers) {
        if (timer.delay_us < 1) {
            emit_trace(
                SequenceId{0},
                now,
                "TIMER_REJECTED",
                "Effects::timer",
                0,
                "invalid_delay");
            continue;
        }
        const auto fire_time =
            checked_add_i64(*send_time, timer.delay_us);
        if (!fire_time) {
            emit_trace(
                SequenceId{0},
                now,
                "TIMER_REJECTED",
                "Effects::timer",
                0,
                "overflow");
            continue;
        }
        const auto scheduled_timer = schedule_at(
            *fire_time,
            EventKind::fire_timer,
            FireTimerEvent{
                source,
                source_epoch,
                timer.kind,
                timer.trade_id,
                timer.expected_state,
                timer.attempt,
            });
        if (scheduled_timer.status == ScheduleStatus::scheduled) {
            emit_trace(
                scheduled_timer.seq.value_or(SequenceId{0}),
                now,
                "TIMER_SET",
                "Effects::timer",
                0);
        } else {
            emit_trace(
                SequenceId{0},
                now,
                "TIMER_REJECTED",
                "Effects::timer",
                0,
                scheduled_timer.status == ScheduleStatus::past_time
                    ? "past_time"
                    : "queue_exhausted");
        }
    }

    for (TraceNote& note : effects.notes) {
        emit_trace(
            SequenceId{0},
            now,
            note.kind,
            "NodeEffect",
            0);
    }
    return 0;
}

void Simulator::crash_node(NodeId node)
{
    const std::size_t index = node_index(node);
    if (!node_up_[index]) {
        return;
    }
    ++epochs_[index];
    node_up_[index] = false;
    network_.set_node_down(node, true);

    Node* implementation = nodes_[index].get();
    if (implementation) {
        const auto start = std::chrono::steady_clock::now();
        implementation->on_crash();
        const auto finish = std::chrono::steady_clock::now();
        const MeasuredNs measured =
            static_cast<MeasuredNs>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finish - start).count());
        emit_trace(
            SequenceId{0},
            clock_.now_us(),
            "CRASH",
            "Node::on_crash",
            measured);
    }
}

void Simulator::restart_node(NodeId node)
{
    const std::size_t index = node_index(node);
    if (node_up_[index]) {
        return;
    }
    node_up_[index] = true;
    network_.set_node_down(node, false);

    Node* implementation = nodes_[index].get();
    if (!implementation) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    Effects effects = implementation->on_restart(
        durable_logs_[index],
        clock_.now_us());
    const auto finish = std::chrono::steady_clock::now();
    const MeasuredNs measured =
        static_cast<MeasuredNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start).count());
    emit_trace(
        SequenceId{0},
        clock_.now_us(),
        "RESTART",
        "Node::on_restart",
        measured);
    static_cast<void>(apply_effects(node, std::move(effects),
                                    clock_.now_us()));
}

bool Simulator::step()
{
    auto event = clock_.pop_next(queue_);
    if (!event) {
        return false;
    }
    dispatch(*event);
    return true;
}

void Simulator::run()
{
    while (step()) {}
}

void Simulator::dispatch(const Event& event)
{
    switch (event.kind) {
    case EventKind::deliver_message:
        dispatch_deliver(
            std::get<DeliverMessageEvent>(event.payload),
            event.seq);
        break;
    case EventKind::drop_message:
        dispatch_drop(
            std::get<DropMessageEvent>(event.payload),
            event.seq);
        break;
    case EventKind::flush_log:
        dispatch_flush(
            std::get<FlushLogEvent>(event.payload),
            event.seq);
        break;
    case EventKind::release_send:
        dispatch_release(
            std::get<ReleaseSendEvent>(event.payload),
            event.seq);
        break;
    case EventKind::fire_timer:
        dispatch_timer(
            std::get<FireTimerEvent>(event.payload),
            event.seq);
        break;
    case EventKind::node_crash:
        dispatch_crash(
            std::get<NodeCrashEvent>(event.payload),
            event.seq);
        break;
    case EventKind::node_restart:
        dispatch_restart(
            std::get<NodeRestartEvent>(event.payload),
            event.seq);
        break;
    case EventKind::script_action:
        dispatch_script(
            std::get<ScriptActionEvent>(event.payload),
            event.seq);
        break;
    }
}

void Simulator::dispatch_deliver(const DeliverMessageEvent& payload,
                                 SequenceId seq)
{
    if (!node_up(payload.target)) {
        emit_trace(
            seq,
            clock_.now_us(),
            "MSG_DROP_NODE_DOWN",
            "NetworkArrival",
            0);
        return;
    }

    Node* implementation = nodes_[node_index(payload.target)].get();
    if (!implementation) {
        emit_trace(
            seq,
            clock_.now_us(),
            "MSG_DROP_NO_NODE",
            "NetworkArrival",
            0);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    Effects effects =
        implementation->on_message(payload.message, clock_.now_us());
    const auto finish = std::chrono::steady_clock::now();
    const MeasuredNs measured =
        static_cast<MeasuredNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start).count());
    emit_trace(
        seq,
        clock_.now_us(),
        "MSG_DELIVER",
        "Node::on_message",
        measured);
    static_cast<void>(apply_effects(
        payload.target,
        std::move(effects),
        clock_.now_us()));
}

void Simulator::dispatch_drop(const DropMessageEvent& payload,
                              SequenceId seq)
{
    static_cast<void>(payload);
    emit_trace(
        seq,
        clock_.now_us(),
        "MSG_DROP",
        "NetworkModel",
        0);
}

void Simulator::dispatch_flush(const FlushLogEvent& payload,
                               SequenceId seq)
{
    const std::size_t index = node_index(payload.node);
    if (!node_up_[index] ||
        epochs_[index] != payload.epoch) {
        return;
    }
    std::vector<LogRecord>& log = durable_logs_[index];
    for (const DeferredLogRecord& record : payload.records) {
        log.push_back(
            LogRecord{
                record.kind,
                record.trade_id,
            });
        emit_trace(
            seq,
            clock_.now_us(),
            "LOG_APPEND",
            "Effects::flush",
            0);
    }
}

void Simulator::dispatch_release(const ReleaseSendEvent& payload,
                                 SequenceId seq)
{
    const std::size_t index = node_index(payload.node);
    if (!node_up_[index] ||
        epochs_[index] != payload.epoch) {
        return;
    }
    static_cast<void>(schedule_message(
        payload.node,
        payload.send.to,
        payload.send.type,
        payload.send.msg_id,
        clock_.now_us(),
        payload.send.trade_id));
    emit_trace(
        seq,
        clock_.now_us(),
        "MSG_SEND",
        "Effects::release",
        0);
}

void Simulator::dispatch_timer(const FireTimerEvent& payload,
                               SequenceId seq)
{
    const std::size_t index = node_index(payload.node);
    if (!node_up_[index]) {
        emit_trace(
            seq,
            clock_.now_us(),
            "TIMER_IGNORED",
            "Simulator::dispatch_timer",
            0,
            "node_down");
        return;
    }
    if (epochs_[index] != payload.epoch) {
        emit_trace(
            seq,
            clock_.now_us(),
            "TIMER_IGNORED",
            "Simulator::dispatch_timer",
            0,
            "stale_epoch");
        return;
    }
    Node* implementation = nodes_[index].get();
    if (!implementation) {
        return;
    }
    if (payload.trade_id &&
        implementation->trade_state(*payload.trade_id) !=
            payload.expected_state) {
        emit_trace(
            seq,
            clock_.now_us(),
            "TIMER_IGNORED",
            "Simulator::dispatch_timer",
            0,
            "state_mismatch");
        return;
    }
    TimerEvent timer{
        payload.node,
        payload.epoch,
        payload.kind,
        payload.trade_id,
        payload.expected_state,
        payload.attempt,
    };
    const auto start = std::chrono::steady_clock::now();
    Effects effects =
        implementation->on_timer(timer, clock_.now_us());
    const auto finish = std::chrono::steady_clock::now();
    const MeasuredNs measured =
        static_cast<MeasuredNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start).count());
    emit_trace(
        seq,
        clock_.now_us(),
        "TIMER_FIRE",
        "Node::on_timer",
        measured);
    static_cast<void>(apply_effects(
        payload.node,
        std::move(effects),
        clock_.now_us()));
}

void Simulator::dispatch_crash(const NodeCrashEvent& payload,
                               SequenceId)
{
    crash_node(payload.node);
}

void Simulator::dispatch_restart(const NodeRestartEvent& payload,
                                 SequenceId)
{
    restart_node(payload.node);
}

void Simulator::dispatch_script(const ScriptActionEvent& payload,
                                SequenceId seq)
{
    std::visit([&](const auto& action) {
        using Action = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<Action, ScriptSetLink>) {
            network_.set_link(
                action.from,
                action.to,
                LinkConfig{
                    action.base_latency_us,
                    action.jitter_us,
                    action.drop_probability_ppm,
                });
        } else if constexpr (std::is_same_v<Action, ScriptSetPartition>) {
            network_.set_partition(
                action.a,
                action.b,
                action.partitioned);
        } else if constexpr (std::is_same_v<Action, ScriptDropNext>) {
            if (action.message_type) {
                network_.drop_next(
                    action.from,
                    action.to,
                    *action.message_type);
            } else {
                network_.drop_next(action.from, action.to);
            }
        } else if constexpr (std::is_same_v<Action, ScriptCrash>) {
            crash_node(action.node);
        } else if constexpr (std::is_same_v<Action, ScriptRestart>) {
            restart_node(action.node);
        } else if constexpr (std::is_same_v<Action, ScriptNodeDown>) {
            set_node_up(action.node, false);
        } else if constexpr (std::is_same_v<Action, ScriptNodeUp>) {
            set_node_up(action.node, true);
        }
    }, payload.action);
    emit_trace(
        seq,
        clock_.now_us(),
        "SCRIPT_ACTION",
        "Simulator::dispatch_script",
        0);
}

void Simulator::emit_trace(SequenceId seq,
                          SimTimeUs now,
                          const std::string& kind,
                          const std::string& handler,
                          MeasuredNs measured_ns,
                          std::optional<std::string> reason)
{
    static_cast<void>(seq);
    ++trace_seq_;
    trace_sink_.write(
        TraceRecord{
            trace_seq_,
            now,
            kind,
            handler,
            measured_ns,
            reason,
        });
}

std::size_t Simulator::node_index(NodeId node) noexcept
{
    return static_cast<std::size_t>(node);
}

VirtualClock& Simulator::clock() noexcept
{
    return clock_;
}

EventQueue& Simulator::events() noexcept
{
    return queue_;
}

NetworkModel& Simulator::network() noexcept
{
    return network_;
}

} // namespace glassbox
