#include "sim/timers.hpp"
#include "check.hpp"

#include <cstdint>
#include <limits>

int main()
{
    glassbox::BackoffPolicy policy{1'000, 8'000};
    CHECK(glassbox::validate(policy));

    CHECK(glassbox::backoff_delay_us(policy, 0).value_or(-1) == 1'000);
    CHECK(glassbox::backoff_delay_us(policy, 1).value_or(-1) == 2'000);
    CHECK(glassbox::backoff_delay_us(policy, 2).value_or(-1) == 4'000);
    CHECK(glassbox::backoff_delay_us(policy, 3).value_or(-1) == 8'000);
    CHECK(glassbox::backoff_delay_us(policy, 200).value_or(-1) == 8'000);
    CHECK(glassbox::backoff_delay_us(policy,
                                     std::numeric_limits<std::uint32_t>::max())
              .value_or(-1) == 8'000);

    glassbox::BackoffPolicy exact{5'000, 5'000};
    CHECK(glassbox::validate(exact));
    CHECK(glassbox::backoff_delay_us(exact, 0).value_or(-1) == 5'000);
    CHECK(glassbox::backoff_delay_us(exact,
                                     std::numeric_limits<std::uint32_t>::max())
              .value_or(-1) == 5'000);

    CHECK(!glassbox::validate(glassbox::BackoffPolicy{0, 8'000}));
    CHECK(!glassbox::validate(glassbox::BackoffPolicy{9'000, 8'000}));
    CHECK(!glassbox::backoff_delay_us(
        glassbox::BackoffPolicy{0, 8'000}, 1));

    for (std::uint32_t attempt = 0; attempt <= 200; ++attempt) {
        const auto delay =
            glassbox::backoff_delay_us(policy, attempt);
        CHECK(delay.has_value());
        CHECK(*delay >= 1'000);
        CHECK(*delay <= 8'000);
    }

    // Large range verifies the doubling loop continues well beyond 63.
    glassbox::BackoffPolicy large{1, 1'000'000'000'000};
    CHECK(glassbox::validate(large));
    CHECK(glassbox::backoff_delay_us(large, 39).value_or(-1) ==
          549'755'813'888);
    CHECK(glassbox::backoff_delay_us(large, 40).value_or(-1) ==
          1'000'000'000'000);
    CHECK(glassbox::backoff_delay_us(large, 41).value_or(-1) ==
          1'000'000'000'000);

    // Near INT64_MAX verifies the max/2 guard and final cap.
    glassbox::BackoffPolicy extreme{
        1,
        std::numeric_limits<glassbox::SimTimeUs>::max(),
    };
    CHECK(glassbox::validate(extreme));
    for (std::uint32_t attempt : {40u, 62u,
                                  std::numeric_limits<std::uint32_t>::max()}) {
        const auto delay =
            glassbox::backoff_delay_us(extreme, attempt);
        CHECK(delay.has_value());
        CHECK(*delay >= 1);
        CHECK(*delay <=
              std::numeric_limits<glassbox::SimTimeUs>::max());
    }
    return 0;
}
