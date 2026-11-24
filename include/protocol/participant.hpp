#pragma once

// Participant state machine (DESIGN.md sections 5.4, 6.1-6.4).
//
// Pure node owning one LocalLedger. Voting yes is the promise: a yes vote
// shares its batch with the PREPARED record (structural write-ahead), and the
// trade stays PREPARING until the simulator reports that record durable, so a
// duplicate PREPARE can never trigger an early yes. After voting yes the
// participant never aborts on its own; it asks with OUTCOME_QUERY.
// Every refusal (vote no, ABORT before PREPARE) writes an ABORTED tombstone.

#include "sim/node.hpp"
#include "sim/timers.hpp"
#include "domain/local_ledger.hpp"
#include "domain/trade.hpp"
#include "protocol/messages.hpp"

#include <map>
#include <optional>

namespace glassbox {

// Which bank this is, its starting ledger, and how its outcome questions
// back off.
struct ParticipantConfig {
    NodeId self;
    LocalLedger genesis;
    BackoffPolicy decision_wait{};
};

// The bank's view of one trade: the trade terms (when known) and its state.
struct ParticipantTrade {
    // Absent for a tombstone written on ABORT-before-PREPARE and for trades
    // restored from a log as COMMITTED/ABORTED (not needed to answer).
    std::optional<Trade> trade;
    TradeState state = TradeState::none;

    [[nodiscard]] friend bool operator==(
        const ParticipantTrade&,
        const ParticipantTrade&) noexcept = default;
};

// One bank. On PREPARE it checks the trade (account not frozen, deadline,
// enough available cash or bonds), locks its leg and votes YES once PREPARED
// is durable, or writes ABORTED and votes NO. On COMMIT it settles its leg; on
// ABORT it releases the lock. Every reply is idempotent, so duplicates and
// retries are harmless.
class Participant final : public Node {
  public:
    // Throws std::invalid_argument if self is not a bank, the decision-wait
    // backoff is invalid, or the genesis ledger breaks its invariants.
    explicit Participant(ParticipantConfig config);

    // Node interface; see sim/node.hpp.
    [[nodiscard]] Effects on_message(const Message& message,
                                     SimTimeUs now) override;
    [[nodiscard]] Effects on_timer(const TimerEvent& event,
                                   SimTimeUs now) override;
    [[nodiscard]] TradeState trade_state(TradeId trade_id) const override;
    void on_log_durable(const LogRecord& record, SimTimeUs now) override;
    void on_crash() override;
    [[nodiscard]] Effects on_restart(std::span<const LogEntry> durable_log,
                                     SimTimeUs now) override;

    // Read-only state for tests and the player: which bank this is, its ledger,
    // and its trades.
    [[nodiscard]] NodeId self() const noexcept;
    [[nodiscard]] const LocalLedger& ledger() const noexcept;
    [[nodiscard]] const ParticipantTrade* find(TradeId trade_id)
        const noexcept;
    [[nodiscard]] const std::map<TradeId, ParticipantTrade>& trades()
        const noexcept;

  private:
    [[nodiscard]] Effects on_prepare(const Message& message, SimTimeUs now);
    [[nodiscard]] Effects on_commit(const Message& message);
    [[nodiscard]] Effects on_abort(const Message& message);
    [[nodiscard]] Effects on_decision_wait(TradeId trade_id,
                                           std::uint32_t attempt);

    [[nodiscard]] Leg leg_of(const Trade& trade) const noexcept;
    void schedule_decision_wait(TradeId trade_id,
                                std::uint32_t attempt,
                                Effects& effects) const;

    ParticipantConfig config_;
    LocalLedger ledger_;
    std::map<TradeId, ParticipantTrade> trades_;
};

} // namespace glassbox
