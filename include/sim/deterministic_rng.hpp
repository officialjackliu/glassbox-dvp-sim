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

// The run's only source of randomness: a seeded mt19937_64 with integer-only
// draws. Same seed, same sequence, so a scenario replays identically. One per
// simulator; never global.
class DeterministicRng {
  public:
    using result_type = std::uint64_t;
    static constexpr result_type default_seed = 1;

    // Seeds the engine with `seed`.
    explicit DeterministicRng(result_type seed) noexcept
        : engine_(seed),
          seed_(seed)
    {
    }

    // Returns the seed the engine started from.
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

    // Returns a uniform integer in [0, upper_bound).
    [[nodiscard]] std::uint64_t uniform_u64(std::uint64_t upper_bound) noexcept
    {
        // Produces an integer in [0, upper_bound) using rejection sampling.
        // This avoids implementation-defined standard-library distributions.
        if (upper_bound == 0) {
            return 0;
        }
        const std::uint64_t threshold =
            (std::numeric_limits<std::uint64_t>::max() - upper_bound + 1) %
            upper_bound;
        std::uint64_t value = engine_();
        while (value < threshold) {
            value = engine_();
        }
        return value % upper_bound;
    }

    // Returns a uniform integer in [lower, upper].
    [[nodiscard]] std::int64_t uniform_i64(std::int64_t lower,
                                           std::int64_t upper) noexcept
    {
        // Produces an integer in [lower, upper]. Invalid ranges return lower;
        // a single-value range still consumes one engine draw.
        if (lower > upper) {
            return lower;
        }
        if (lower == upper) {
            static_cast<void>(uniform_u64(1));
            return lower;
        }
        if (lower >= 0) {
            const std::uint64_t width_minus_one =
                static_cast<std::uint64_t>(upper) -
                static_cast<std::uint64_t>(lower);
            const std::uint64_t offset =
                uniform_u64(width_minus_one + 1);
            return lower + static_cast<std::int64_t>(offset);
        }
        if (upper < 0) {
            const std::uint64_t lower_magnitude =
                static_cast<std::uint64_t>(-(lower + 1)) + 1;
            const std::uint64_t upper_magnitude =
                static_cast<std::uint64_t>(-(upper + 1)) + 1;
            const std::uint64_t negative_count =
                lower_magnitude - upper_magnitude + 1;
            const std::uint64_t offset = uniform_u64(negative_count);
            return lower + static_cast<std::int64_t>(offset);
        }

        const std::uint64_t negative_count =
            static_cast<std::uint64_t>(-(lower + 1)) + 1;
        const std::uint64_t positive_count =
            static_cast<std::uint64_t>(upper) + 1;
        const std::uint64_t span = negative_count + positive_count;
        if (span == 0) {
            const std::uint64_t raw = engine_();
            if (raw <
                (std::uint64_t{1} << 63)) {
                return static_cast<std::int64_t>(raw);
            }
            const std::uint64_t offset =
                raw - (std::uint64_t{1} << 63);
            return std::numeric_limits<std::int64_t>::min() +
                   static_cast<std::int64_t>(offset);
        }
        const std::uint64_t offset = uniform_u64(span);
        if (offset < negative_count) {
            return lower + static_cast<std::int64_t>(offset);
        }
        return static_cast<std::int64_t>(
            offset - negative_count);
    }

    // Restarts the sequence from the seed.
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
    // Always consume one PPM-sized drop draw, regardless of the configured
    // probability, so configuration changes do not shift later jitter draws.
    const std::uint64_t draw =
        rng.uniform_u64(drop_probability_denominator);
    if (probability_ppm == 0) {
        return false;
    }
    if (probability_ppm >= drop_probability_denominator) {
        return true;
    }
    return draw < probability_ppm;
}

// Returns `base_latency_us` plus a random jitter in [-jitter_us, +jitter_us],
// at least 1 us. Always takes exactly one draw.
[[nodiscard]] inline SimTimeUs draw_jittered_latency(
    DeterministicRng& rng,
    SimTimeUs base_latency_us,
    SimTimeUs jitter_us) noexcept
{
    // Called by the network model before a message is scheduled. It draws
    // exactly one signed jitter value, then clamps the result to one microsecond.
    // Always consume one jitter draw. Zero or negative jitter consumes a
    // single-value draw, preserving the same draw sequence as a jittered link.
    const SimTimeUs jitter =
        jitter_us <= 0
            ? rng.uniform_i64(0, 0)
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

// The two random outcomes for one message: its latency and whether it is lost.
struct DeterministicNetworkDraw {
    SimTimeUs latency_us;
    bool dropped;
};

// Draws latency and loss for one message on a link (jitter first, then drop).
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
