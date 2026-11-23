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

} // namespace glassbox
