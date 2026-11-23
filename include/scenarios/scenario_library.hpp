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

// S6: BANK_A crashes AFTER_FIRST_SEND on PREPARE (its yes vote leaves) and
// restarts k_s6_restart_after_us later. The coordinator commits; BANK_A
// recovers in doubt, asks with OUTCOME_QUERY, and applies the COMMIT.
inline constexpr SimTimeUs k_s6_restart_after_us = 5'000'000;
[[nodiscard]] ScenarioSetup make_s6_in_doubt_participant(std::uint64_t seed);

// S7: BANK_A crashes AFTER_LOG_BEFORE_SEND on PREPARE (PREPARED durable, no
// vote sent). The coordinator times out and aborts; BANK_A restarts in
// doubt, queries, and gets ABORT.
inline constexpr SimTimeUs k_s7_restart_after_us = 2'000'000;
[[nodiscard]] ScenarioSetup make_s7_crash_before_vote_sent(
    std::uint64_t seed);

// S8: BANK_A crashes BEFORE_LOG on PREPARE: no record, no lock survive. The
// coordinator times out and aborts; BANK_A restarts clean and later writes
// a tombstone when a retransmitted ABORT reaches it.
inline constexpr SimTimeUs k_s8_restart_after_us = 2'000'000;
[[nodiscard]] ScenarioSetup make_s8_crash_before_log(std::uint64_t seed);

// S13: BANK_B (seller) holds k_s13_seller_bonds bonds, enough for one
// standard trade but not two. Trade 1 (BANK_A buys) and trade 2 (BANK_C
// buys) are submitted at t=0 in that order; both PREPAREs reach BANK_B at
// the same time and the first scheduled one wins the lock.
inline constexpr std::int64_t k_s13_seller_bonds = 40;
[[nodiscard]] ScenarioSetup make_s13_lock_conflict(std::uint64_t seed);

} // namespace glassbox
