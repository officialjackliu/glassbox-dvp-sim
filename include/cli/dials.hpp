#pragma once

// Glass-box player, step 5: --set edits to a scenario before it runs, so a
// viewer can change the system and watch the outcome change.
//
//   vote_timeout=30ms          how long COORD waits for votes (us, ms, s)
//   latency.COORD-BANK_B=80ms  one-way delay of a link, both directions
//   drop.COORD-BANK_B=20%      share of messages a link loses, both
//                              directions (0-100%)
//   restart_after=2s           how long crashed nodes stay down (scenarios
//                              with a crash point only)
//
// Link settings are added to the script as ordinary network actions at
// t = 0, after the scenario's own, so the player shows them like any other
// event. Jitter and the other setting of the link are kept.

#include "scenarios/scenario.hpp"

#include <cstdint>
#include <string_view>

namespace glassbox::cli {

enum class DialStatus : std::uint8_t {
    ok,
    unknown_key, // also: a key this scenario has nothing to apply to
    bad_value,   // not a number with a unit, or a bad node pair
    out_of_range,
};

[[nodiscard]] std::string_view to_string(DialStatus status) noexcept;

// Applies one "key=value". On failure the setup is unchanged.
[[nodiscard]] DialStatus apply_dial(ScenarioSetup& setup,
                                    std::string_view assignment);

} // namespace glassbox::cli
