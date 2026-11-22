#pragma once

// Scenario-local pseudo-random generation and reusable network draws.
//
// One DeterministicRng per run owns the mt19937_64 engine. Jitter and drop
// helpers consume it in a fixed order so the same seed and scenario produce
// identical simulation behavior.

#include "sim/core_types.hpp"

#include <cstdint>
#include <limits>
#include <random>

namespace glassbox {

class DeterministicRng {
  public:
    using result_type = std::uint64_t;
    static constexpr result_type default_seed = 1;

    explicit DeterministicRng(result_type seed) noexcept
        : engine_(seed),
          seed_(seed)
    {
    }

    [[nodiscard]] constexpr result_type seed() const noexcept
    {
        return seed_;
    }

    // Returns the next raw engine draw. It is used for tests and low-level
    // callers that need a stable sequence without hiding RNG state in a global.
    [[nodiscard]] result_type next_u64() noexcept
    {
        return engine_();
    }

    [[nodiscard]] std::uint64_t uniform_u64(std::uint64_t upper_bound) noexcept
    {
        // Produces an integer in [0, upper_bound). Network drop probability
        // and probability helpers call this with an explicit PPM denominator.
        if (upper_bound == 0) {
            return 0;
        }
        std::uniform_int_distribution<std::uint64_t> distribution(
            0,
            upper_bound - 1);
        return distribution(engine_);
    }

    [[nodiscard]] std::int64_t uniform_i64(std::int64_t lower,
                                           std::int64_t upper) noexcept
    {
        // Produces an inclusive integer range for jitter. Invalid or reversed
        // ranges return lower, keeping callers from needing floating point.
        if (lower >= upper) {
            return lower;
        }
        std::uniform_int_distribution<std::int64_t> distribution(lower, upper);
        return distribution(engine_);
    }

    void reset() noexcept
    {
        // Restores the scenario seed. Tests and replay code call reset when they
        // need to compare complete runs or independent network draw sequences.
        engine_.seed(seed_);
    }

  private:
    std::mt19937_64 engine_;
    result_type seed_;
};

inline constexpr std::uint32_t drop_probability_denominator = 1'000'000;

// Converts a PPM probability into a deterministic boolean. The caller supplies
// the scenario RNG; draw_drop is later used while scheduling message delivery.
[[nodiscard]] inline bool draw_drop(DeterministicRng& rng,
                                    std::uint32_t probability_ppm) noexcept
{
    if (probability_ppm == 0) {
        return false;
    }
    if (probability_ppm >= drop_probability_denominator) {
        return true;
    }

    return rng.uniform_u64(drop_probability_denominator) < probability_ppm;
}

[[nodiscard]] inline SimTimeUs draw_jittered_latency(
    DeterministicRng& rng,
    SimTimeUs base_latency_us,
    SimTimeUs jitter_us) noexcept
{
    // Called by the network model before a message is scheduled. It draws
    // exactly one signed jitter value, then clamps the result to one microsecond.
    const SimTimeUs jitter =
        jitter_us <= 0
            ? 0
            : rng.uniform_i64(-jitter_us, jitter_us);

    if (jitter >= 0) {
        const SimTimeUs max_latency =
            std::numeric_limits<SimTimeUs>::max();
        if (base_latency_us > max_latency - jitter) {
            return max_latency;
        }
    } else {
        if (base_latency_us > 0 &&
            base_latency_us < -jitter &&
            jitter != std::numeric_limits<SimTimeUs>::min()) {
            return 1;
        }
    }

    const SimTimeUs latency = base_latency_us + jitter;
    return latency < 1 ? 1 : latency;
}

struct DeterministicNetworkDraw {
    SimTimeUs latency_us;
    bool dropped;
};

[[nodiscard]] inline DeterministicNetworkDraw draw_network_delivery(
    DeterministicRng& rng,
    SimTimeUs base_latency_us,
    SimTimeUs jitter_us,
    std::uint32_t drop_probability_ppm) noexcept
{
    // Fixed draw order: jitter is sampled first, then drop. Network scheduling
    // calls this helper so scenario traces remain reproducible for a given seed.
    const SimTimeUs latency_us =
        draw_jittered_latency(rng, base_latency_us, jitter_us);
    const bool dropped =
        draw_drop(rng, drop_probability_ppm);
    return {latency_us, dropped};
}

} // namespace glassbox
