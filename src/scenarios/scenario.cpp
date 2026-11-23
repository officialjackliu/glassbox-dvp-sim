#include "scenarios/scenario.hpp"

#include "scenarios/scenario_library.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace glassbox {

LocalLedger make_ledger(std::int64_t cash_cents, std::int64_t bonds,
                        bool frozen)
{
    LedgerOutcome outcome =
        LocalLedger::genesis(Cents{cash_cents}, Units{bonds}, frozen);
    if (!outcome.ledger) {
        throw std::invalid_argument("invalid genesis ledger");
    }
    return *outcome.ledger;
}

BankGenesis default_genesis()
{
    return BankGenesis{
        make_ledger(k_default_cash_cents, k_default_bonds),
        make_ledger(k_default_cash_cents, k_default_bonds),
        make_ledger(k_default_cash_cents, k_default_bonds),
    };
}

TradeRequest standard_trade(NodeId buyer, NodeId seller,
                            SimTimeUs submitted_at)
{
    return TradeRequest{
        buyer,
        seller,
        "UST_10Y",
        Units{k_standard_qty},
        Cents{k_standard_price_cents},
        submitted_at + k_trade_deadline_us,
        "client",
    };
}

TimedAction submit(SimTimeUs at, TradeRequest request)
{
    return TimedAction{at,
                       ScriptActionEvent{ScriptSubmitTrade{std::move(request)}}};
}

namespace {

constexpr std::array<ScenarioInfo, 1> k_registry{{
    {"S1", "Happy path", &make_s1_happy_path},
}};

} // namespace

std::span<const ScenarioInfo> scenario_registry() noexcept
{
    return k_registry;
}

const ScenarioInfo* find_scenario(std::string_view id) noexcept
{
    for (const ScenarioInfo& info : k_registry) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

void ScenarioRun::CollectingSink::write(const TraceRecord& record)
{
    records.push_back(record);
    if (echo_) {
        echo_->write(record);
    }
}

ScenarioRun::ScenarioRun(const ScenarioSetup& setup, TraceSink* echo)
    : genesis_(setup.genesis),
      sink_(echo),
      simulator_(setup.config, sink_),
      nodes_(install_protocol_nodes(simulator_, setup.genesis))
{
    for (const TimedAction& step : setup.script) {
        if (!simulator_.schedule_script(step.at, step.action)) {
            throw std::invalid_argument("scenario script step not schedulable");
        }
    }
}

bool ScenarioRun::run(std::size_t max_events)
{
    return simulator_.run_bounded(max_events);
}

const std::vector<TraceRecord>& ScenarioRun::trace() const noexcept
{
    return sink_.records;
}

Simulator& ScenarioRun::simulator() noexcept
{
    return simulator_;
}

const Simulator& ScenarioRun::simulator() const noexcept
{
    return simulator_;
}

const ProtocolNodes& ScenarioRun::nodes() const noexcept
{
    return nodes_;
}

const BankGenesis& ScenarioRun::genesis() const noexcept
{
    return genesis_;
}

} // namespace glassbox
