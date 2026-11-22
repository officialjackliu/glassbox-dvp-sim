#include "sim/deterministic_rng.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    glassbox::DeterministicRng first{42};
    glassbox::DeterministicRng second{42};

    assert(first.seed() == 42);
    assert(second.seed() == 42);

    for (int i = 0; i < 1000; ++i) {
        assert(first.next_u64() == second.next_u64());
    }

    first.reset();
    second.reset();
    std::array<std::uint64_t, 32> first_values{};
    std::array<std::uint64_t, 32> second_values{};
    for (std::size_t i = 0; i < first_values.size(); ++i) {
        first_values[i] = first.next_u64();
        second_values[i] = second.next_u64();
    }
    assert(first_values == second_values);

    for (int i = 0; i < 10000; ++i) {
        const auto jitter = first.uniform_i64(-100, 100);
        assert(jitter >= -100);
        assert(jitter <= 100);

        const auto latency =
            glassbox::draw_jittered_latency(first, 500, 100);
        assert(latency >= 1);
        assert(latency >= 400);
        assert(latency <= 600);
    }

    first.reset();
    const auto zero_drop = glassbox::draw_network_delivery(first, 100, 10, 0);
    assert(zero_drop.latency_us >= 90);
    assert(zero_drop.latency_us <= 110);
    assert(!zero_drop.dropped);

    first.reset();
    second.reset();
    for (int i = 0; i < 100; ++i) {
        const auto a =
            glassbox::draw_network_delivery(first, 100, 50, 250000);
        const auto b =
            glassbox::draw_network_delivery(second, 100, 50, 250000);
        assert(a.latency_us == b.latency_us);
        assert(a.dropped == b.dropped);
    }

    first.reset();
    second.reset();
    for (int i = 0; i < 100; ++i) {
        const auto jitter_only =
            glassbox::draw_jittered_latency(first, 100, 50);
        assert(jitter_only >= 50);
        assert(jitter_only <= 150);
        assert(jitter_only ==
               glassbox::draw_jittered_latency(second, 100, 50));
    }

    glassbox::DeterministicRng different_seed{43};
    assert(different_seed.next_u64() !=
           glassbox::DeterministicRng{42}.next_u64());

    glassbox::DeterministicRng extremes{99};
    for (int i = 0; i < 100; ++i) {
        const auto extreme_latency =
            glassbox::draw_jittered_latency(
                extremes,
                std::numeric_limits<glassbox::SimTimeUs>::max(),
                1000);
        assert(extreme_latency >=
               std::numeric_limits<glassbox::SimTimeUs>::max() - 1000);
        assert(extreme_latency <=
               std::numeric_limits<glassbox::SimTimeUs>::max());
    }

    return 0;
}
