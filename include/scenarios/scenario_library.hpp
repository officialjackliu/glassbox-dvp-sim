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

// S4: BANK_A buys from BANK_C; the COORD<->BANK_C link is slowed to
// k_s4_slow_link_us each way (> vote_timeout / 2). With the default 500 ms
// vote timeout the coordinator aborts before BANK_C's yes arrives (false
// abort). make_s4_with_vote_timeout() is the same setup with another
// timeout, e.g. a raised one under which the same trade commits.
inline constexpr SimTimeUs k_s4_slow_link_us = 300'000;
[[nodiscard]] ScenarioSetup make_s4_slow_link_tight_timeout(
    std::uint64_t seed);
[[nodiscard]] ScenarioSetup make_s4_with_vote_timeout(
    std::uint64_t seed,
    SimTimeUs vote_timeout_us);

// S5: BANK_B is node_down from t=0 (before PREPARE) and comes back up at
// k_s5_node_up_us. The coordinator times out and aborts; BANK_A releases;
// after BANK_B is up, a retransmitted ABORT reaches it and it writes a
// tombstone and ACKs.
inline constexpr SimTimeUs k_s5_node_up_us = 3'000'000;
[[nodiscard]] ScenarioSetup make_s5_participant_down(std::uint64_t seed);

} // namespace glassbox
