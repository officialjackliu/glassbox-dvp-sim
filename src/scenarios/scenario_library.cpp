#include "scenarios/scenario_library.hpp"

namespace glassbox {

namespace {

[[nodiscard]] ScenarioSetup base_setup(std::uint64_t seed)
{
    ScenarioSetup setup{SimulatorConfig{}, default_genesis(), {}};
    setup.config.seed = seed;
    return setup;
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

} // namespace glassbox
