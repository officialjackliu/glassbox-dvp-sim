// Slow-motion timing for the player; integer arithmetic only.

#include "cli/pacing.hpp"

#include <algorithm>

namespace glassbox::cli {

Pace pace(SimTimeUs gap_us,
          bool messages_in_flight,
          bool minor_event,
          int speed_percent) noexcept
{
    const std::int64_t speed = std::clamp(speed_percent, k_speed_percent.front(),
                                          k_speed_percent.back());
    const SimTimeUs gap = std::max<SimTimeUs>(gap_us, 0);
    const std::int64_t cap_ms =
        messages_in_flight ? k_max_travel_ms : k_max_quiet_ms;
    const SimTimeUs cap_us = cap_ms * k_sim_us_per_wall_ms;
    const SimTimeUs shown_us = std::min(gap, cap_us);

    Pace p;
    p.skipped_us = gap - shown_us;
    // shown_us / 25 <= 5,000 and the dwell constants are small, so the
    // scaling below cannot overflow.
    p.travel_ms = (shown_us / k_sim_us_per_wall_ms) * 100 / speed;
    p.dwell_ms = (minor_event ? k_minor_dwell_ms : k_dwell_ms) * 100 / speed;
    return p;
}

int faster(int speed_percent) noexcept
{
    for (const int step : k_speed_percent) {
        if (step > speed_percent) {
            return step;
        }
    }
    return k_speed_percent.back();
}

int slower(int speed_percent) noexcept
{
    for (auto it = k_speed_percent.rbegin(); it != k_speed_percent.rend(); ++it) {
        if (*it < speed_percent) {
            return *it;
        }
    }
    return k_speed_percent.front();
}

} // namespace glassbox::cli
