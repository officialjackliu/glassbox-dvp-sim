#pragma once

// Glass-box player, step 3: how long things stay on screen.
//
// Slow motion. At 100% speed one wall-clock millisecond shows 25 us of
// simulated time, so a 35 ms New York -> London hop takes 1.4 s to cross the
// screen. Long gaps are compressed: at most 5 s while messages are on the
// wire, 1.5 s when nothing is (waiting for a timer). After each event the
// screen holds still so the explanation can be read. Speed scales every
// duration. Pacing only decides display time; it never changes the run.

#include "sim/core_types.hpp"

#include <array>
#include <cstdint>

namespace glassbox::cli {

inline constexpr std::array<int, 6> k_speed_percent{25, 50, 100, 200, 400, 800};
inline constexpr int k_default_speed_percent = 100;

// All at 100% speed.
inline constexpr SimTimeUs k_sim_us_per_wall_ms = 25;
inline constexpr std::int64_t k_max_travel_ms = 5'000; // messages on the wire
inline constexpr std::int64_t k_max_quiet_ms = 1'500;  // nothing on the wire
inline constexpr std::int64_t k_dwell_ms = 1'800;
inline constexpr std::int64_t k_minor_dwell_ms = 600;

// How to show one gap between events and the event after it.
struct Pace {
    std::int64_t travel_ms = 0; // animate the gap before the event
    std::int64_t dwell_ms = 0;  // hold after the event
    SimTimeUs skipped_us = 0;   // sim time compressed beyond the cap
};

// Returns how long to animate a gap of `gap_us` before an event and how long
// to hold the event. speed_percent is clamped to [25, 800]; a negative gap
// counts as 0.
[[nodiscard]] Pace pace(SimTimeUs gap_us,
                        bool messages_in_flight,
                        bool minor_event,
                        int speed_percent) noexcept;

// Returns the next speed up/down k_speed_percent (unchanged at the ends). A
// value between steps moves to the nearest step in that direction.
[[nodiscard]] int faster(int speed_percent) noexcept;
[[nodiscard]] int slower(int speed_percent) noexcept;

} // namespace glassbox::cli
