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

const SimulatorConfig& Simulator::config() const noexcept
{
    return config_;
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

std::span<const LogEntry> Simulator::durable_log(NodeId node) const noexcept
{
    return durable_logs_[node_index(node)].entries();
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
    return schedule_message(
        Message{
            msg_id,
            from,
            to,
            type,
            trade_id,
            std::monostate{},
        },
        send_time);
}

std::optional<SequenceId> Simulator::schedule_message(
    Message message,
    SimTimeUs send_time)
{
    if (send_time < clock_.now_us()) {
        return std::nullopt;
    }
    if (!node_up(message.from)) {
        return std::nullopt;
    }
    return network_.schedule_delivery(
        queue_,
        send_time,
        std::move(message)).event_seq;
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
            std::vector<LogRecord> deferred_logs;
            deferred_logs.reserve(effects.logs.size());
            deferred_logs = effects.logs;
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
            TraceRecord record;
            record.sim_time_us = now;
            record.kind = "TIMER_SET";
            record.handler = "Effects::timer";
            record.node = source;
            record.trade_id = timer.trade_id;
            record.detail = std::string{to_string(timer.kind)};
            emit(std::move(record));
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
        TraceRecord record;
        record.sim_time_us = now;
        record.kind = std::move(note.kind);
        record.handler = std::move(note.handler);
        record.node = source;
        record.trade_id = note.trade_id;
        if (!note.detail.empty()) {
            record.detail = std::move(note.detail);
        }
        emit(std::move(record));
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
        TraceRecord record;
        record.sim_time_us = clock_.now_us();
        record.kind = "CRASH";
        record.handler = "Node::on_crash";
        record.measured_ns = measured;
        record.node = node;
        emit(std::move(record));
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
        durable_logs_[index].entries(),
        clock_.now_us());
    const auto finish = std::chrono::steady_clock::now();
    const MeasuredNs measured =
        static_cast<MeasuredNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start).count());
    TraceRecord record;
    record.sim_time_us = clock_.now_us();
    record.kind = "RESTART";
    record.handler = "Node::on_restart";
    record.measured_ns = measured;
    record.node = node;
    emit(std::move(record));
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
    deliver_to_node(payload.target, payload.message, seq);
}

void Simulator::deliver_to_node(NodeId target,
                                const Message& message,
                                SequenceId seq)
{
    static_cast<void>(seq);
    if (!node_up(target)) {
        emit(message_trace("MSG_DROP_NODE_DOWN",
                           "NetworkArrival",
                           target,
                           message));
        return;
    }

    Node* implementation = nodes_[node_index(target)].get();
    if (!implementation) {
        emit(message_trace("MSG_DROP_NO_NODE",
                           "NetworkArrival",
                           target,
                           message));
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    Effects effects =
        implementation->on_message(message, clock_.now_us());
    const auto finish = std::chrono::steady_clock::now();
    const MeasuredNs measured =
        static_cast<MeasuredNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start).count());
    TraceRecord record = message_trace("MSG_DELIVER",
                                       "Node::on_message",
                                       target,
                                       message);
    record.measured_ns = measured;
    emit(std::move(record));
    static_cast<void>(apply_effects(
        target,
        std::move(effects),
        clock_.now_us()));
}

void Simulator::dispatch_drop(const DropMessageEvent& payload,
                              SequenceId seq)
{
    static_cast<void>(seq);
    TraceRecord record = message_trace("MSG_DROP",
                                       "NetworkModel",
                                       payload.message.to,
                                       payload.message);
    record.reason = std::string{to_string(payload.reason)};
    emit(std::move(record));
}

void Simulator::dispatch_flush(const FlushLogEvent& payload,
                               SequenceId seq)
{
    const std::size_t index = node_index(payload.node);
    if (!node_up_[index] ||
        epochs_[index] != payload.epoch) {
        return;
    }
    static_cast<void>(seq);
    DurableLog& log = durable_logs_[index];
    Node* implementation = nodes_[index].get();
    for (const LogRecord& record : payload.records) {
        const auto entry =
            log.append(record, clock_.now_us());
        if (!entry) {
            continue;
        }
        TraceRecord trace;
        trace.sim_time_us = clock_.now_us();
        trace.kind = "LOG_APPEND";
        trace.handler = "Effects::flush";
        trace.node = payload.node;
        trace.trade_id = record_trade_id(record);
        trace.log_record = std::string{record_kind(record)};
        emit(std::move(trace));
        if (implementation) {
            implementation->on_log_durable(record, clock_.now_us());
        }
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
    static_cast<void>(seq);
    Message message = payload.send;
    message.from = payload.node;
    if (message.msg_id == 0) {
        message.msg_id = next_msg_id();
    }
    TraceRecord record = message_trace("MSG_SEND",
                                       "Effects::release",
                                       payload.node,
                                       message);
    static_cast<void>(schedule_message(
        std::move(message),
        clock_.now_us()));
    emit(std::move(record));
}

void Simulator::dispatch_timer(const FireTimerEvent& payload,
                               SequenceId seq)
{
    static_cast<void>(seq);
    const auto timer_trace =
        [&](const char* kind, std::optional<std::string> reason) {
            TraceRecord record;
            record.sim_time_us = clock_.now_us();
            record.kind = kind;
            record.handler = "Simulator::dispatch_timer";
            record.reason = std::move(reason);
            record.node = payload.node;
            record.trade_id = payload.trade_id;
            record.detail = std::string{to_string(payload.kind)};
            return record;
        };
    const std::size_t index = node_index(payload.node);
    if (!node_up_[index]) {
        emit(timer_trace("TIMER_IGNORED", "node_down"));
        return;
    }
    if (epochs_[index] != payload.epoch) {
        emit(timer_trace("TIMER_IGNORED", "stale_epoch"));
        return;
    }
    Node* implementation = nodes_[index].get();
    if (!implementation) {
        return;
    }
    if (payload.trade_id &&
        implementation->trade_state(*payload.trade_id) !=
            payload.expected_state) {
        emit(timer_trace("TIMER_IGNORED", "state_mismatch"));
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
    TraceRecord record = timer_trace("TIMER_FIRE", std::nullopt);
    record.handler = "Node::on_timer";
    record.measured_ns = measured;
    emit(std::move(record));
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
    // A submitted trade is delivered after the SCRIPT_ACTION record so the
    // trace reads "client submits" -> "coordinator receives PROPOSE".
    if (const auto* submit =
            std::get_if<ScriptSubmitTrade>(&payload.action)) {
        Message propose = make_propose(submit->request);
        propose.msg_id = next_msg_id();
        deliver_to_node(NodeId::coordinator, propose, seq);
    }
}

void Simulator::emit_trace(SequenceId seq,
                          SimTimeUs now,
                          const std::string& kind,
                          const std::string& handler,
                          MeasuredNs measured_ns,
                          std::optional<std::string> reason)
{
    static_cast<void>(seq);
    TraceRecord record;
    record.sim_time_us = now;
    record.kind = kind;
    record.handler = handler;
    record.measured_ns = measured_ns;
    record.reason = std::move(reason);
    emit(std::move(record));
}

void Simulator::emit(TraceRecord record)
{
    ++trace_seq_;
    record.seq = trace_seq_;
    trace_sink_.write(record);
}

TraceRecord Simulator::message_trace(const std::string& kind,
                                     const std::string& handler,
                                     NodeId node,
                                     const Message& message) const
{
    TraceRecord record;
    record.sim_time_us = clock_.now_us();
    record.kind = kind;
    record.handler = handler;
    record.node = node;
    record.trade_id = message.trade_id;
    record.msg_type = message.type;
    record.msg_id = message.msg_id;
    record.from = message.from;
    record.to = message.to;
    return record;
}

std::uint64_t Simulator::next_msg_id() noexcept
{
    return next_msg_id_++;
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
