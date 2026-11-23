#pragma once

// Scenario framework (DESIGN.md section 8).
//
// A scenario is data: simulator config, bank genesis ledgers, and a timed
// script (trade submissions, link changes, faults). ScenarioRun builds the
// simulator and protocol nodes from it, runs to quiescence (bounded), and
// keeps the full trace for assertions and the CLI. Expected outcomes live in
// the scenario tests, not here.

#include "faults/crash_spec.hpp"
#include "scenarios/protocol_system.hpp"
#include "sim/simulator.hpp"
#include "sim/trace.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace glassbox {

struct TimedAction {
    SimTimeUs at;
    ScriptActionEvent action;
};

struct ScenarioSetup {
    SimulatorConfig config;
    BankGenesis genesis;
    // Scheduled in vector order, so same-time actions run in that order.
    std::vector<TimedAction> script;
    // One-shot crash points, registered before the run starts.
    std::vector<CrashSpec> crashes{};
};

struct ScenarioInfo {
    std::string_view id;
    std::string_view name;
    ScenarioSetup (*make)(std::uint64_t seed);
};

// Shared defaults: every bank starts with 1,000,000.00 cash and 25,000
// bonds; the standard trade is BANK_A buying 25 UST_10Y from BANK_B for
// 24,875.00 with a 30 s deadline (DESIGN.md section 6.2).
inline constexpr std::int64_t k_default_cash_cents = 100'000'000;
inline constexpr std::int64_t k_default_bonds = 25'000;
inline constexpr std::int64_t k_standard_qty = 25;
inline constexpr std::int64_t k_standard_price_cents = 2'487'500;
inline constexpr SimTimeUs k_trade_deadline_us = 30'000'000;

[[nodiscard]] LocalLedger make_ledger(std::int64_t cash_cents,
                                      std::int64_t bonds,
                                      bool frozen = false);
[[nodiscard]] BankGenesis default_genesis();
[[nodiscard]] TradeRequest standard_trade(
    NodeId buyer = NodeId::bank_a,
    NodeId seller = NodeId::bank_b,
    SimTimeUs submitted_at = 0);
[[nodiscard]] TimedAction submit(SimTimeUs at, TradeRequest request);

// Registry in id order (S1, S2, ...).
[[nodiscard]] std::span<const ScenarioInfo> scenario_registry() noexcept;
[[nodiscard]] const ScenarioInfo* find_scenario(std::string_view id) noexcept;

class ScenarioRun {
  public:
    // echo, when given, receives every trace record as it is produced.
    explicit ScenarioRun(const ScenarioSetup& setup,
                         TraceSink* echo = nullptr);
    ScenarioRun(const ScenarioRun&) = delete;
    ScenarioRun& operator=(const ScenarioRun&) = delete;

    // Runs until the event queue drains or max_events have been processed.
    // Returns true on quiescence.
    [[nodiscard]] bool run(std::size_t max_events = 1'000'000);

    [[nodiscard]] const std::vector<TraceRecord>& trace() const noexcept;
    [[nodiscard]] Simulator& simulator() noexcept;
    [[nodiscard]] const Simulator& simulator() const noexcept;
    [[nodiscard]] const ProtocolNodes& nodes() const noexcept;
    [[nodiscard]] const BankGenesis& genesis() const noexcept;

  private:
    class CollectingSink : public TraceSink {
      public:
        explicit CollectingSink(TraceSink* echo) : echo_(echo) {}
        void write(const TraceRecord& record) override;
        std::vector<TraceRecord> records;

      private:
        TraceSink* echo_;
    };

    BankGenesis genesis_;
    CollectingSink sink_;
    Simulator simulator_;
    ProtocolNodes nodes_;
};

} // namespace glassbox
