#include "scenarios/scenario_library.hpp"

namespace glassbox {

namespace {

[[nodiscard]] ScenarioSetup base_setup(std::uint64_t seed)
{
    ScenarioSetup setup{SimulatorConfig{}, default_genesis(), {}};
    setup.config.seed = seed;
    return setup;
}

[[nodiscard]] TimedAction set_link(SimTimeUs at,
                                   NodeId from,
                                   NodeId to,
                                   SimTimeUs latency_us)
{
    return TimedAction{at, ScriptActionEvent{ScriptSetLink{
                               from, to, latency_us, 0, 0}}};
}

} // namespace

ScenarioSetup make_s1_happy_path(std::uint64_t seed)
{
    ScenarioSetup setup = base_setup(seed);
    setup.script.push_back(submit(0, standard_trade()));
    return setup;
}

ScenarioSetup make_s2_buyer_lacks_funds(std::uint64_t seed)
{
    ScenarioSetup setup = base_setup(seed);
    setup.genesis.bank_a =
        make_ledger(k_standard_price_cents - 1, k_default_bonds);
    setup.script.push_back(submit(0, standard_trade()));
    return setup;
}

ScenarioSetup make_s3_account_frozen(std::uint64_t seed)
{
    ScenarioSetup setup = base_setup(seed);
    setup.genesis.bank_b =
        make_ledger(k_default_cash_cents, k_default_bonds, true);
    setup.script.push_back(submit(0, standard_trade()));
    return setup;
}

ScenarioSetup make_s4_with_vote_timeout(std::uint64_t seed,
                                        SimTimeUs vote_timeout_us)
{
    ScenarioSetup setup = base_setup(seed);
    setup.config.timers.vote_timeout_us = vote_timeout_us;
    setup.script.push_back(set_link(0, NodeId::coordinator, NodeId::bank_c,
                                    k_s4_slow_link_us));
    setup.script.push_back(set_link(0, NodeId::bank_c, NodeId::coordinator,
                                    k_s4_slow_link_us));
    setup.script.push_back(
        submit(0, standard_trade(NodeId::bank_a, NodeId::bank_c)));
    return setup;
}

ScenarioSetup make_s4_slow_link_tight_timeout(std::uint64_t seed)
{
    return make_s4_with_vote_timeout(seed,
                                     SimulatorConfig{}.timers.vote_timeout_us);
}

ScenarioSetup make_s5_participant_down(std::uint64_t seed)
{
    ScenarioSetup setup = base_setup(seed);
    setup.script.push_back(TimedAction{
        0, ScriptActionEvent{ScriptNodeDown{NodeId::bank_b}}});
    setup.script.push_back(submit(0, standard_trade()));
    setup.script.push_back(TimedAction{
        k_s5_node_up_us, ScriptActionEvent{ScriptNodeUp{NodeId::bank_b}}});
    return setup;
}

ScenarioSetup make_s13_lock_conflict(std::uint64_t seed)
{
    ScenarioSetup setup = base_setup(seed);
    setup.genesis.bank_b =
        make_ledger(k_default_cash_cents, k_s13_seller_bonds);
    setup.script.push_back(
        submit(0, standard_trade(NodeId::bank_a, NodeId::bank_b)));
    setup.script.push_back(
        submit(0, standard_trade(NodeId::bank_c, NodeId::bank_b)));
    return setup;
}

} // namespace glassbox
