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

constexpr std::array<ScenarioInfo, 13> k_registry{{
    {"S1", "Happy path", &make_s1_happy_path},
    {"S2", "Buyer lacks funds", &make_s2_buyer_lacks_funds},
    {"S3", "Account frozen", &make_s3_account_frozen},
    {"S4", "Slow link, tight timeout", &make_s4_slow_link_tight_timeout},
    {"S5", "Participant dead before voting", &make_s5_participant_down},
    {"S6", "In-doubt participant", &make_s6_in_doubt_participant},
    {"S7", "Crash after vote logged, before sending",
     &make_s7_crash_before_vote_sent},
    {"S8", "Crash before logging", &make_s8_crash_before_log},
    {"S9", "Coordinator crash after decision",
     &make_s9_coordinator_crash_after_decision},
    {"S10", "Coordinator crash mid-broadcast",
     &make_s10_coordinator_crash_mid_broadcast},
    {"S11", "Lost COMMIT", &make_s11_lost_commit},
    {"S12", "Duplicate and reordered messages",
     &make_s12_duplicates_and_reordering},
    {"S13", "Lock conflict", &make_s13_lock_conflict},
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
    for (const CrashSpec& crash : setup.crashes) {
        if (!simulator_.add_crash_spec(crash)) {
            throw std::invalid_argument("invalid crash spec");
        }
    }
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
