#pragma once

// Glass-box player, step 4: draw one moment of a recording as text lines.
//
// Layout, top to bottom: clock and playback status; each trade's ticket and
// stage bar; one panel per involved node (state, balances and locks, trade
// states, votes and acks, pending timers, durable log); the messages on the
// wire with their progress; the story of the latest event; the safety
// checks. Pure: same inputs, same lines (tests compare them exactly).

#include "cli/guide.hpp"
#include "cli/recording.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace glassbox::cli {

struct Moment {
    std::size_t frame = 0;   // the latest event shown
    SimTimeUs now_us = 0;    // clamped to [this event, next event]
    int speed_percent = 100;
    bool paused = false;
};

struct ScreenOptions {
    int width = 100; // clamped to [80, 200]
    bool color = true;
};

[[nodiscard]] std::vector<std::string> draw(const Recording& recording,
                                            const Moment& moment,
                                            const ScreenOptions& options);

// The card shown before a scenario plays: what happens, the trades, the
// nodes with their starting balances, and what to watch for.
[[nodiscard]] std::vector<std::string> draw_intro(const Recording& recording,
                                                  const ScenarioGuide& guide,
                                                  const ScreenOptions& options);

// Columns a string takes on screen: UTF-8 code points, ANSI color escapes
// excluded (every character the screen uses is one column wide).
[[nodiscard]] std::size_t visible_width(std::string_view text) noexcept;

} // namespace glassbox::cli
