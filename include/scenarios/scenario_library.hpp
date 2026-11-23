#pragma once

// Scenario definitions S1-S14 (DESIGN.md section 8). Each function returns
// the setup for one scenario; the matching test asserts its outcome.

#include "scenarios/scenario.hpp"

#include <cstdint>

namespace glassbox {

// S1: normal config, BANK_A buys the standard trade from BANK_B at t=0.
[[nodiscard]] ScenarioSetup make_s1_happy_path(std::uint64_t seed);

// S2: BANK_A (buyer) holds one cent less than the price. BANK_A votes no
// (INSUFFICIENT_FUNDS); BANK_B locks bonds, then gets ABORT and releases.
[[nodiscard]] ScenarioSetup make_s2_buyer_lacks_funds(std::uint64_t seed);

// S3: BANK_B (seller) account is frozen. freeze_account is modeled as the
// genesis frozen flag, so it also holds after any restart (replay starts
// from genesis). BANK_B votes no (ACCOUNT_FROZEN); BANK_A releases its lock.
[[nodiscard]] ScenarioSetup make_s3_account_frozen(std::uint64_t seed);

} // namespace glassbox
