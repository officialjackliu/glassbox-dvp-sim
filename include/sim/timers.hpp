#pragma once

// Exponential backoff for the protocol's retry timers: the coordinator
// resending its decision and a participant asking for an outcome wait
// initial_us, then twice that, and so on up to max_us.

#include "sim/core_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace glassbox {

// First delay and the cap that repeated delays grow towards.
struct BackoffPolicy {
    SimTimeUs initial_us = 1'000'000;
    SimTimeUs max_us = 8'000'000;
};

// Returns true if initial_us is positive and max_us >= initial_us.
[[nodiscard]] inline bool validate(const BackoffPolicy& policy) noexcept
{
    return policy.initial_us > 0 &&
           policy.max_us >= policy.initial_us;
}

// Returns min(max_us, initial_us * 2^attempt) without overflowing, or
// std::nullopt for an invalid policy. Attempt 0 is the first wait.
[[nodiscard]] inline std::optional<SimTimeUs> backoff_delay_us(
    const BackoffPolicy& policy,
    std::uint32_t attempt) noexcept
{
    if (!validate(policy)) {
        return std::nullopt;
    }
    if (policy.initial_us >= policy.max_us) {
        return policy.max_us;
    }

    SimTimeUs result = policy.initial_us;
    for (std::uint32_t i = 0; i < attempt && i < 63; ++i) {
        if (result >= policy.max_us) {
            return policy.max_us;
        }
        if (result > policy.max_us / 2) {
            return policy.max_us;
        }
        result *= 2;
        if (result > policy.max_us) {
            return policy.max_us;
        }
    }
    return result;
}

} // namespace glassbox
