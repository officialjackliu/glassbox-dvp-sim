#pragma once

// Coordinator state machine (DESIGN.md sections 5.3, 6.2, 6.4).
//
// Pure node: every handler takes a message or timer plus virtual time and
// returns Effects. Per-trade state is volatile and rebuilt from the durable
// log on restart; trades without a logged decision are forgotten (presumed
// abort). A decision is never announced before its record is durable: the
// first broadcast shares a batch with the decision record (structural
// write-ahead), and later replies wait for on_log_durable().

#include "sim/node.hpp"
#include "sim/timers.hpp"
#include "domain/trade.hpp"
#include "protocol/messages.hpp"

#include <map>
#include <optional>

namespace glassbox {

struct CoordinatorConfig {
    SimTimeUs vote_timeout_us = 500'000;
    BackoffPolicy retransmit{};
};

struct CoordinatorTrade {
    Trade trade;
    TradeState state = TradeState::collecting;
    bool buyer_voted_yes = false;
    bool seller_voted_yes = false;
    std::optional<Decision> decision{};
    ReasonCode abort_reason = ReasonCode::none;
    bool decision_durable = false;
    bool buyer_acked = false;
    bool seller_acked = false;

    [[nodiscard]] friend bool operator==(
        const CoordinatorTrade&,
        const CoordinatorTrade&) noexcept = default;
};

class Coordinator final : public Node {
  public:
    // Throws std::invalid_argument for a non-positive vote timeout or an
    // invalid retransmit backoff.
    explicit Coordinator(CoordinatorConfig config);

    [[nodiscard]] Effects on_message(const Message& message,
                                     SimTimeUs now) override;
    [[nodiscard]] Effects on_timer(const TimerEvent& event,
                                   SimTimeUs now) override;
    [[nodiscard]] TradeState trade_state(TradeId trade_id) const override;
    void on_log_durable(const LogRecord& record, SimTimeUs now) override;
    void on_crash() override;
    [[nodiscard]] Effects on_restart(std::span<const LogEntry> durable_log,
                                     SimTimeUs now) override;

    // Read-only inspection for tests and state views.
    [[nodiscard]] const CoordinatorTrade* find(TradeId trade_id) const noexcept;
    [[nodiscard]] const std::map<TradeId, CoordinatorTrade>& trades()
        const noexcept;

  private:
    [[nodiscard]] Effects on_propose(const ProposeBody& body, SimTimeUs now);
    [[nodiscard]] Effects on_vote(const Message& message);
    [[nodiscard]] Effects on_ack(const Message& message);
    [[nodiscard]] Effects on_outcome_query(const Message& message);
    [[nodiscard]] Effects on_vote_timeout(TradeId trade_id);
    [[nodiscard]] Effects on_retransmit(TradeId trade_id,
                                        std::uint32_t attempt);

    void decide(CoordinatorTrade& record,
                Decision decision,
                ReasonCode reason,
                Effects& effects) const;
    void send_decision(const CoordinatorTrade& record,
                       NodeId to,
                       Effects& effects) const;
    void schedule_retransmit(const CoordinatorTrade& record,
                             std::uint32_t attempt,
                             Effects& effects) const;

    CoordinatorConfig config_;
    TradeIdAllocator allocator_;
    std::map<TradeId, CoordinatorTrade> trades_;
};

} // namespace glassbox
