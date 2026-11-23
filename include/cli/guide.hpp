#pragma once

// Glass-box player: the intro card for each scenario (what it demonstrates
// and what to watch for), and the guided tour order.

#include <array>
#include <string_view>

namespace glassbox::cli {

struct ScenarioGuide {
    std::string_view id;
    std::string_view title;
    std::string_view what_it_shows;
    std::string_view watch_for;
};

// nullptr for an unknown id.
[[nodiscard]] const ScenarioGuide* find_guide(std::string_view id) noexcept;

// make tour: a success, a refusal, a crashed bank, a crashed coordinator,
// a network partition.
inline constexpr std::array<std::string_view, 5> k_tour{"S1", "S2", "S6", "S9",
                                                        "S14"};

} // namespace glassbox::cli
