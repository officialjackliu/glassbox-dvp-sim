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

// A scripted action and the simulated time it runs at.
struct TimedAction {
    SimTimeUs at;
    ScriptActionEvent action;
};

// Everything that defines a run: settings, starting ledgers, the script and
// the crash points. Plain data, so a front end may edit it before running
// (see cli/dials).
struct ScenarioSetup {
    SimulatorConfig config;
    BankGenesis genesis;
    // Scheduled in vector order, so same-time actions run in that order.
    std::vector<TimedAction> script;
    // One-shot crash points, registered before the run starts.
    std::vector<CrashSpec> crashes{};
};

// A registered scenario: its id ("S6"), a short name, and the function that
// builds its setup for a seed.
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

// Returns a ledger with these starting balances. Throws
// std::invalid_argument for negative amounts.
[[nodiscard]] LocalLedger make_ledger(std::int64_t cash_cents,
                                      std::int64_t bonds,
                                      bool frozen = false);
// Returns three banks with the default balances.
[[nodiscard]] BankGenesis default_genesis();
// Returns the standard trade ticket between `buyer` and `seller`, valid for
// k_trade_deadline_us after `submitted_at`.
[[nodiscard]] TradeRequest standard_trade(
    NodeId buyer = NodeId::bank_a,
    NodeId seller = NodeId::bank_b,
    SimTimeUs submitted_at = 0);
// Returns the script action that submits `request` to the coordinator at `at`.
[[nodiscard]] TimedAction submit(SimTimeUs at, TradeRequest request);

// Registry in id order (S1, S2, ...).
[[nodiscard]] std::span<const ScenarioInfo> scenario_registry() noexcept;
// Returns the scenario with this id ("S1" ... "S14"), or nullptr.
[[nodiscard]] const ScenarioInfo* find_scenario(std::string_view id) noexcept;

// Builds a Simulator with the protocol nodes from a ScenarioSetup, schedules
// its script and crash points, and collects the trace.
//
//   ScenarioRun run{make_s1_happy_path(1)};
//   if (run.run()) { inspect run.trace(), run.nodes() ... }
//
// Call run() once, or drive it with simulator().step() one event at a time
// (as the player does).
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

    // Accessors: every trace record so far, the simulator, the installed nodes,
    // and the starting ledgers.
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
