#pragma once

#include "sim/core_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace glassbox {

struct BackoffPolicy {
    SimTimeUs initial_us = 1'000'000;
    SimTimeUs max_us = 8'000'000;
};

[[nodiscard]] inline bool validate(const BackoffPolicy& policy) noexcept
{
    return policy.initial_us > 0 &&
           policy.max_us >= policy.initial_us;
}

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
