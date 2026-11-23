#pragma once

// Glass-box player, step 2: tell one event in plain English.
//
// narrate() reads a recorded frame (the trace records of one event plus the
// node views before and after it) and explains what happened, the checks or
// amounts involved, why the protocol does it at this moment, and which
// function ran. It only formats data that is in the recording.

#include "cli/recording.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glassbox::cli {

// The life of one trade, as shown in the stage bar.
enum class Stage : std::uint8_t {
    submit,  // client ticket, coordinator validates and records it
    lock,    // banks check and lock their leg
    vote,    // banks vote YES / NO
    decide,  // coordinator decides and writes the decision to disk
    settle,  // banks apply (COMMIT) or release (ABORT)
    confirm, // banks acknowledge, coordinator writes END
};

[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

enum class Tone : std::uint8_t {
    info,
    good,    // progress toward settlement
    warning, // retry, duplicate, waiting, in doubt
    failure, // crash, lost message, refusal, abort decision
};

struct Story {
    std::string headline;
    std::vector<std::string> details; // checks, amounts, what comes next
    std::string why;                  // the rule behind it, may be empty
    std::string code;                 // the function that ran
    std::optional<Stage> stage;
    Tone tone = Tone::info;
};

[[nodiscard]] Story narrate(const Recording& recording, std::size_t frame);

// Frames that only clean up (every record a TIMER_IGNORED). The player
// shows them briefly.
[[nodiscard]] bool is_minor(const Frame& frame);

// --- Formatting shared with the screen (integers only) ---------------------

[[nodiscard]] std::string dollars(Cents cents);        // "$24,875.00"
[[nodiscard]] std::string with_commas(std::int64_t v); // "25,000"
[[nodiscard]] std::string format_time(SimTimeUs t);    // "1,074.400 ms"
[[nodiscard]] std::string format_duration(SimTimeUs d); // "35.0 ms", "5.000 s"
[[nodiscard]] std::string_view city(NodeId node);      // "New York", ...
[[nodiscard]] std::string bonds_text(std::int64_t qty); // "25 UST 10Y bonds"

} // namespace glassbox::cli
