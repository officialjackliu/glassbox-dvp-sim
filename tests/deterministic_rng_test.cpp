#include "sim/deterministic_rng.hpp"
#include "check.hpp"

#include <array>
#include <limits>

int main()
{
    glassbox::DeterministicRng first{42};
    glassbox::DeterministicRng second{42};

    CHECK(first.seed() == 42);
    CHECK(second.seed() == 42);

    for (int i = 0; i < 1000; ++i) {
        CHECK(first.next_u64() == second.next_u64());
    }

    first.reset();
    second.reset();
    std::array<std::uint64_t, 32> first_values{};
    std::array<std::uint64_t, 32> second_values{};
    for (std::size_t i = 0; i < first_values.size(); ++i) {
        first_values[i] = first.next_u64();
        second_values[i] = second.next_u64();
    }
    CHECK(first_values == second_values);

    first.reset();
    for (int i = 0; i < 10000; ++i) {
        const auto jitter = first.uniform_i64(-100, 100);
        CHECK(jitter >= -100);
        CHECK(jitter <= 100);

        const auto latency =
            glassbox::draw_jittered_latency(first, 500, 100);
        CHECK(latency >= 400);
        CHECK(latency <= 600);
    }

    first.reset();
    for (int i = 0; i < 1000; ++i) {
        const auto full_range = first.uniform_i64(
            std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::max());
        static_cast<void>(full_range);
    }

    first.reset();
    const auto zero_drop = glassbox::draw_network_delivery(first, 100, 10, 0);
    CHECK(zero_drop.latency_us >= 90);
    CHECK(zero_drop.latency_us <= 110);
    CHECK(!zero_drop.dropped);

    first.reset();
    second.reset();
    for (int i = 0; i < 100; ++i) {
        const auto a =
            glassbox::draw_network_delivery(first, 100, 50, 250000);
        const auto b =
            glassbox::draw_network_delivery(second, 100, 50, 250000);
        CHECK(a.latency_us == b.latency_us);
        CHECK(a.dropped == b.dropped);
    }

    // Configuration-independent draw order:
    // 0 ppm and 1 ppm both consume jitter and drop draws.
    first.reset();
    second.reset();
    const auto zero = glassbox::draw_network_delivery(first, 100, 100, 0);
    const auto one = glassbox::draw_network_delivery(second, 100, 100, 1);
    CHECK(one.latency_us >= 1);
    CHECK(one.latency_us <= 200);
    const auto zero_next =
        glassbox::draw_jittered_latency(first, 100, 100);
    const auto one_next =
        glassbox::draw_jittered_latency(second, 100, 100);
    CHECK(zero_next == one_next);
    CHECK(!zero.dropped);

    first.reset();
    second.reset();
    const auto no_jitter =
        glassbox::draw_network_delivery(first, 100, 0, 0);
    CHECK(no_jitter.latency_us == 100);
    CHECK(!no_jitter.dropped);

    second.reset();
    static_cast<void>(second.uniform_i64(0, 0));
    static_cast<void>(second.uniform_u64(
        glassbox::drop_probability_denominator));
    CHECK(first.next_u64() == second.next_u64());

    glassbox::DeterministicRng different_seed{43};
    CHECK(different_seed.next_u64() !=
          glassbox::DeterministicRng{42}.next_u64());

    glassbox::DeterministicRng extremes{99};
    for (int i = 0; i < 100; ++i) {
        const auto extreme_latency =
            glassbox::draw_jittered_latency(
                extremes,
                std::numeric_limits<glassbox::SimTimeUs>::max(),
                1000);
        CHECK(extreme_latency >=
              std::numeric_limits<glassbox::SimTimeUs>::max() - 1000);
        CHECK(extreme_latency <=
              std::numeric_limits<glassbox::SimTimeUs>::max());
    }

    return 0;
}
