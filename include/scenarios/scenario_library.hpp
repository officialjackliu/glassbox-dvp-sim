#pragma once

// Scenario definitions S1-S14 (DESIGN.md section 8). Each function returns
// the setup for one scenario; the matching test asserts its outcome.

#include "scenarios/scenario.hpp"

#include <cstdint>

namespace glassbox {

// S1: normal config, BANK_A buys the standard trade from BANK_B at t=0.
[[nodiscard]] ScenarioSetup make_s1_happy_path(std::uint64_t seed);

} // namespace glassbox
