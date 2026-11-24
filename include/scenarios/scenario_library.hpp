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

// S9: the coordinator crashes AFTER_LOG_BEFORE_SEND on the deciding VOTE
// (the second VOTE of trade 1): DECISION_COMMIT is durable, no COMMIT
// leaves. Both participants stay in doubt, holding their locks and
// retrying OUTCOME_QUERY, until the coordinator restarts and resends
// COMMIT. Shows that 2PC is blocking.
inline constexpr SimTimeUs k_s9_restart_after_us = 5'000'000;
[[nodiscard]] ScenarioSetup make_s9_coordinator_crash_after_decision(
    std::uint64_t seed);

// S10: the coordinator crashes AFTER_FIRST_SEND on the deciding VOTE: the
// COMMIT to the buyer (first in the broadcast) leaves, the one to the
// seller is lost. BANK_A commits at once; BANK_B stays in doubt until the
// restart resends.
inline constexpr SimTimeUs k_s10_restart_after_us = 5'000'000;
[[nodiscard]] ScenarioSetup make_s10_coordinator_crash_mid_broadcast(
    std::uint64_t seed);

// S11: the first COMMIT to BANK_A is lost (drop_next). The coordinator's
// retransmit delivers it. BANK_A's decision-wait starts at 2 s here so the
// retransmit (1 s) arrives before BANK_A would ask.
[[nodiscard]] ScenarioSetup make_s11_lost_commit(std::uint64_t seed);

// S12: high jitter on COORD<->BANK_A and COORD<->BANK_B and duplicated
// PREPARE and COMMIT on both links. Messages arrive reordered and twice;
// the state tables must absorb it.
[[nodiscard]] ScenarioSetup make_s12_duplicates_and_reordering(
    std::uint64_t seed);

// S13: BANK_B (seller) holds k_s13_seller_bonds bonds, enough for one
// standard trade but not two. Trade 1 (BANK_A buys) and trade 2 (BANK_C
// buys) are submitted at t=0 in that order; both PREPAREs reach BANK_B at
// the same time and the first scheduled one wins the lock.
inline constexpr std::int64_t k_s13_seller_bonds = 40;
[[nodiscard]] ScenarioSetup make_s13_lock_conflict(std::uint64_t seed);

// S14: BANK_A buys from BANK_C. COORD<->BANK_C is partitioned at
// k_s14_partition_at_us (after BANK_C received PREPARE at 121,100, before its
// yes leaves at 122,200) and healed at k_s14_heal_at_us. The coordinator
// times out and aborts; after the heal BANK_C, in doubt, resolves by query.
// Partitions apply at send time (NetworkModel semantics).
inline constexpr SimTimeUs k_s14_partition_at_us = 122'000;
inline constexpr SimTimeUs k_s14_heal_at_us = 2'000'000;
[[nodiscard]] ScenarioSetup make_s14_partition(std::uint64_t seed);
[[nodiscard]] ScenarioSetup make_s14_with_partition(
    std::uint64_t seed,
    SimTimeUs partition_at_us,
    SimTimeUs heal_at_us);

} // namespace glassbox
